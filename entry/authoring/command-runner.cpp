#include "command-runner.hpp"

#include "json-writer.hpp"

#include <authoring-edit.hpp>
#include <preview.hpp>
#include <project-persistence.hpp>
#include <source-ingestion.hpp>

#include <annotation/authoring-document.hpp>
#include <annotation/content-hash.hpp>
#include <annotation/recognition.hpp>
#include <annotation/recognition-runtime.hpp>
#include <annotation/resource.hpp>

#include <core/error/contracts.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/safety/annotations.hpp>
#include <core/safety/checked-access.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/space.hpp>
#include <domain/time.hpp>

#include <engine/runtime-loader.hpp>

#include <image/pixels.hpp>
#include <image/png.hpp>

#include <vision/bgra-image.hpp>
#include <vision/frame-analysis.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uf::authoring
{
    namespace
    {
        [[nodiscard]]
        auto invalid(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        // Resource identity is derived rather than random. An agent that reruns
        // a command has to land on the same project, and a name reused by
        // accident has to collide on its id and be refused by
        // AuthoringDocument::create rather than quietly adding a twin the author
        // can no longer tell apart. sha256 over a kind tag, the project id and
        // the name, truncated to the sixteen bytes a ResourceId carries.
        [[nodiscard]]
        auto derivedResourceId(
            std::string_view kind,
            std::string_view projectId,
            std::string_view name
        ) -> Result<annotation::ResourceId>
        {
            auto seed = std::string{};
            seed += kind;
            seed += '\0';
            seed += projectId;
            seed += '\0';
            seed += name;

            UF_TRY_VALUE(hash, annotation::sha256(std::as_bytes(std::span{seed})));
            return annotation::ResourceId::fromBytes(
                std::as_bytes(hash.bytes().first<16>())
            );
        }

        // A source is identified by the first half of the canonical PNG's own
        // content hash, so the same screenshot always resolves to the same
        // source and ingesting it twice is a lookup rather than a second copy.
        [[nodiscard]]
        auto sourceIdOf(annotation::ContentHash const& hash) -> annotation::SourceId
        {
            return annotation::SourceId{
                annotation::ResourceId::fromBytes(
                    std::as_bytes(hash.bytes().first<16>())
                )
            };
        }

        // importSourcePng needs an id before it has computed the canonical hash
        // the real id is derived from. It stores nothing, so this value exists
        // for that one call and is replaced before anything sees it.
        [[nodiscard]]
        auto placeholderSourceId() -> annotation::SourceId
        {
            auto const zero = std::array<std::byte, 16>{};
            return annotation::SourceId{annotation::ResourceId::fromBytes(zero)};
        }

        // The three enumerations this tool answers with, each as one total
        // mapping. A ternary would file a future enumerator with whichever
        // branch it fell into; a switch over a scoped enum makes the compiler
        // name the case that was forgotten.
        [[nodiscard]]
        auto readLayoutName(annotation::ReadLayout layout) -> std::string_view
        {
            switch (layout)
            {
            case annotation::ReadLayout::SingleLine:
                return "single_line";
            case annotation::ReadLayout::Block:
                return "block";
            }
            UF_UNREACHABLE_MSG("unknown ReadLayout value");
        }

        [[nodiscard]]
        auto charsetName(annotation::CharsetRestriction charset) -> std::string_view
        {
            switch (charset)
            {
            case annotation::CharsetRestriction::Digits:
                return "digits";
            }
            UF_UNREACHABLE_MSG("unknown CharsetRestriction value");
        }

        [[nodiscard]]
        auto signatureRoleName(annotation::SignatureRole role) -> std::string_view
        {
            switch (role)
            {
            case annotation::SignatureRole::Required:
                return "required";
            case annotation::SignatureRole::Forbidden:
                return "forbidden";
            }
            UF_UNREACHABLE_MSG("unknown SignatureRole value");
        }

        [[nodiscard]]
        auto holdingName(annotation::Holding holding) -> std::string_view
        {
            switch (holding)
            {
            case annotation::Holding::Owned:
                return "owned";
            case annotation::Holding::Referenced:
                return "referenced";
            }
            UF_UNREACHABLE_MSG("unknown Holding value");
        }

        [[nodiscard]]
        auto rectJson(PixelRect const& rect) -> std::string
        {
            auto const members = std::array{
                JsonMember{.key = "x", .value = jsonUnsigned(rect.x())},
                JsonMember{.key = "y", .value = jsonUnsigned(rect.y())},
                JsonMember{.key = "width", .value = jsonUnsigned(rect.width())},
                JsonMember{.key = "height", .value = jsonUnsigned(rect.height())},
            };
            return jsonObject(members);
        }

        [[nodiscard]]
        auto colourKeyJson(
            std::optional<annotation::ColourKey> const& key
        ) -> std::string
        {
            if (!key)
            {
                return jsonNull();
            }
            auto const members = std::array{
                JsonMember{.key = "red", .value = jsonUnsigned(key->red())},
                JsonMember{.key = "green", .value = jsonUnsigned(key->green())},
                JsonMember{.key = "blue", .value = jsonUnsigned(key->blue())},
                JsonMember{
                    .key   = "tolerance",
                    .value = jsonUnsigned(key->tolerance()),
                },
            };
            return jsonObject(members);
        }

        [[nodiscard]]
        auto fingerprintJson(annotation::ProjectFingerprint fingerprint) -> std::string
        {
            auto const members = std::array{
                JsonMember{
                    .key   = "width",
                    .value = jsonUnsigned(fingerprint.width()),
                },
                JsonMember{
                    .key   = "height",
                    .value = jsonUnsigned(fingerprint.height()),
                },
                JsonMember{.key = "dpi_x", .value = jsonUnsigned(fingerprint.dpiX())},
                JsonMember{.key = "dpi_y", .value = jsonUnsigned(fingerprint.dpiY())},
            };
            return jsonObject(members);
        }

        // Each capability as its own payload or null, rather than as a flat bag
        // of fields. That keeps the structural fact the capability set was
        // introduced for visible in the answer too: an element without read has
        // nowhere to put OCR parameters, and one without interact has nowhere to
        // put a click offset.
        [[nodiscard]]
        auto capabilitiesJson(
            annotation::ElementCapabilities const& capabilities
        ) -> std::string
        {
            auto interact = jsonNull();
            if (auto const& declared = capabilities.interact())
            {
                auto offset = jsonNull();
                if (auto const& click = declared->clickOffset)
                {
                    auto const point = std::array{
                        JsonMember{.key = "x", .value = jsonUnsigned(click->x())},
                        JsonMember{.key = "y", .value = jsonUnsigned(click->y())},
                    };
                    offset = jsonObject(point);
                }
                auto const inner = std::array{
                    JsonMember{
                        .key   = "click_offset",
                        .value = std::move(offset),
                    },
                };
                interact = jsonObject(inner);
            }

            auto read = jsonNull();
            if (auto const& declared = capabilities.read())
            {
                auto const inner = std::array{
                    JsonMember{
                        .key   = "layout",
                        .value = jsonString(readLayoutName(declared->layout)),
                    },
                    JsonMember{
                        .key   = "charset",
                        .value = declared->charset
                            ? jsonString(charsetName(*declared->charset))
                            : jsonNull(),
                    },
                };
                read = jsonObject(inner);
            }

            auto const members = std::array{
                JsonMember{
                    .key   = "identify",
                    .value = capabilities.hasIdentify()
                        ? jsonObject({})
                        : jsonNull(),
                },
                JsonMember{.key = "interact", .value = std::move(interact)},
                JsonMember{.key = "read", .value = std::move(read)},
            };
            return jsonObject(members);
        }

        // The page side of the same three, with the one datum a page adds:
        // whether its identify evidence points for the page or against it.
        [[nodiscard]]
        auto exercisedJson(
            annotation::ExercisedCapabilities const& exercised
        ) -> std::string
        {
            auto identify = jsonNull();
            if (auto const& declared = exercised.identify())
            {
                auto const inner = std::array{
                    JsonMember{
                        .key   = "role",
                        .value = jsonString(signatureRoleName(declared->role)),
                    },
                };
                identify = jsonObject(inner);
            }

            auto const members = std::array{
                JsonMember{.key = "identify", .value = std::move(identify)},
                JsonMember{
                    .key   = "interact",
                    .value = exercised.hasInteract() ? jsonObject({}) : jsonNull(),
                },
                JsonMember{
                    .key   = "read",
                    .value = exercised.hasRead() ? jsonObject({}) : jsonNull(),
                },
            };
            return jsonObject(members);
        }

        [[nodiscard]]
        auto variantJson(annotation::Variant const& variant) -> std::string
        {
            auto const members = std::array{
                JsonMember{
                    .key   = "name",
                    .value = jsonString(variant.name().value()),
                },
                JsonMember{
                    .key   = "source_id",
                    .value = jsonString(variant.sourceId().value().toString()),
                },
                JsonMember{
                    .key   = "template_rect",
                    .value = rectJson(variant.templateRect()),
                },
                JsonMember{
                    .key   = "min_similarity_bp",
                    .value = jsonUnsigned(variant.threshold().basisPoints()),
                },
                JsonMember{
                    .key   = "colour_key",
                    .value = colourKeyJson(variant.colourKey()),
                },
            };
            return jsonObject(members);
        }

        // An empty list is a legal and meaningful answer: it says this rectangle
        // is located by the page being recognised rather than by pixels of its
        // own, which is why nothing here invents a placeholder appearance.
        [[nodiscard]]
        auto variantsJson(
            std::span<annotation::Variant const> variants
        ) -> std::string
        {
            auto encoded = std::vector<std::string>{};
            encoded.reserve(variants.size());
            for (auto const& variant : variants)
            {
                encoded.emplace_back(variantJson(variant));
            }
            return jsonArray(encoded);
        }

        [[nodiscard]]
        auto elementJson(annotation::Element const& element) -> std::string
        {
            auto const members = std::array{
                JsonMember{
                    .key   = "id",
                    .value = jsonString(element.id().value().toString()),
                },
                JsonMember{
                    .key   = "name",
                    .value = jsonString(element.name().value()),
                },
                JsonMember{
                    .key   = "capabilities",
                    .value = capabilitiesJson(element.capabilities()),
                },
                JsonMember{
                    .key   = "search_roi",
                    .value = rectJson(element.searchRoi()),
                },
                JsonMember{
                    .key   = "variants",
                    .value = variantsJson(element.variants()),
                },
            };
            return jsonObject(members);
        }

        // The runtime's view of one element: the same identity and capabilities,
        // and appearances stripped of the two facts that stop at authoring --
        // the screen a template was cut from, and the colour key that produced
        // its mask, which the runtime reads off the template's alpha channel.
        [[nodiscard]]
        auto runtimeRecognizerJson(
            annotation::RecognizerDefinition const& recognizer
        ) -> std::string
        {
            auto variants = std::vector<std::string>{};
            variants.reserve(recognizer.variants().size());
            for (auto const& variant : recognizer.variants())
            {
                auto const entry = std::array{
                    JsonMember{
                        .key   = "name",
                        .value = jsonString(variant.name.value()),
                    },
                    JsonMember{
                        .key   = "template_rect",
                        .value = rectJson(variant.templateRect),
                    },
                    JsonMember{
                        .key   = "min_similarity_bp",
                        .value = jsonUnsigned(variant.threshold.basisPoints()),
                    },
                };
                variants.emplace_back(jsonObject(entry));
            }

            auto const members = std::array{
                JsonMember{
                    .key   = "id",
                    .value = jsonString(recognizer.id().value().toString()),
                },
                JsonMember{
                    .key   = "name",
                    .value = jsonString(recognizer.name().value()),
                },
                JsonMember{
                    .key   = "capabilities",
                    .value = capabilitiesJson(recognizer.capabilities()),
                },
                JsonMember{
                    .key   = "search_roi",
                    .value = rectJson(recognizer.searchRoi()),
                },
                JsonMember{.key = "variants", .value = jsonArray(variants)},
            };
            return jsonObject(members);
        }

        [[nodiscard]]
        auto elementNameJson(
            annotation::AuthoringDocument const& document,
            annotation::ElementId id
        ) -> std::string
        {
            auto const* p_element = document.findElement(id);
            return p_element == nullptr
                ? jsonString(id.value().toString())
                : jsonString(p_element->name().value());
        }

        // One page's use of one element. `search_roi` and `variant` are null
        // when the page refines neither, because absent means "the element's
        // own" rather than a value this could fill in.
        [[nodiscard]]
        auto referenceJson(
            annotation::AuthoringDocument const& document,
            annotation::PageReference const& reference
        ) -> std::string
        {
            auto const members = std::array{
                JsonMember{
                    .key   = "element",
                    .value = elementNameJson(document, reference.elementId),
                },
                JsonMember{
                    .key   = "holding",
                    .value = jsonString(holdingName(reference.holding)),
                },
                JsonMember{
                    .key   = "exercised",
                    .value = exercisedJson(reference.exercised),
                },
                JsonMember{
                    .key   = "search_roi",
                    .value = reference.searchRoi
                        ? rectJson(*reference.searchRoi)
                        : jsonNull(),
                },
                JsonMember{
                    .key   = "variant",
                    .value = reference.variant
                        ? jsonString(reference.variant->value())
                        : jsonNull(),
                },
            };
            return jsonObject(members);
        }

        [[nodiscard]]
        auto pageJson(
            annotation::AuthoringDocument const& document,
            annotation::PageSignature const& page
        ) -> std::string
        {
            // The signature is derived from the references below rather than
            // authored beside them, so it is reported as what it is: the answer
            // the model computed from the rows that follow.
            auto required = std::vector<std::string>{};
            for (auto const id : page.required())
            {
                required.emplace_back(elementNameJson(document, id));
            }
            auto forbidden = std::vector<std::string>{};
            for (auto const id : page.forbidden())
            {
                forbidden.emplace_back(elementNameJson(document, id));
            }

            auto references = std::vector<std::string>{};
            for (auto const& reference : document.references())
            {
                if (reference.pageId == page.id())
                {
                    references.emplace_back(referenceJson(document, reference));
                }
            }

            auto const members = std::array{
                JsonMember{
                    .key   = "id",
                    .value = jsonString(page.id().value().toString()),
                },
                JsonMember{.key = "name", .value = jsonString(page.name().value())},
                JsonMember{.key = "required", .value = jsonArray(required)},
                JsonMember{.key = "forbidden", .value = jsonArray(forbidden)},
                JsonMember{.key = "references", .value = jsonArray(references)},
            };
            return jsonObject(members);
        }

        [[nodiscard]]
        auto sourceJson(annotation::AuthoringSource const& source) -> std::string
        {
            auto const members = std::array{
                JsonMember{
                    .key   = "id",
                    .value = jsonString(source.id().value().toString()),
                },
                JsonMember{
                    .key   = "content_hash",
                    .value = jsonString(source.contentHash().hex()),
                },
                JsonMember{
                    .key   = "relative_path",
                    .value = jsonString(source.relativePath()),
                },
                JsonMember{
                    .key   = "fingerprint",
                    .value = fingerprintJson(source.fingerprint()),
                },
            };
            return jsonObject(members);
        }

        [[nodiscard]]
        auto successJson(
            std::string_view command,
            std::span<JsonMember const> members
        ) -> std::string
        {
            auto document = std::vector<JsonMember>{};
            document.reserve(members.size() + 2U);
            document.emplace_back(
                JsonMember{.key = "ok", .value = jsonBoolean(true)}
            );
            document.emplace_back(
                JsonMember{.key = "command", .value = jsonString(command)}
            );
            for (auto const& member : members)
            {
                document.emplace_back(member);
            }
            return jsonObject(document);
        }

        // The project as one CLI edit sees it: the draft the edit rewrites, and
        // the source bytes the save hands back to the compiler. They travel
        // together because ingesting a screenshot adds to both at once, and
        // because saveAndGenerateAuthoringProject needs every source's bytes,
        // not only the newly added one's.
        struct EditSession final
        {
            workbench::AuthoringDraft                     draft;
            std::vector<annotation::AuthoringSourceAsset> assets{};
        };

        [[nodiscard]]
        auto openSession(std::filesystem::path const& root) -> Result<EditSession>
        {
            UF_TRY_VALUE(loaded, workbench::loadAuthoringProject(root));
            return EditSession{
                .draft  = workbench::makeAuthoringDraft(loaded.document),
                .assets = std::move(loaded.sources),
            };
        }

        [[nodiscard]]
        auto commitSession(
            std::filesystem::path const& root,
            EditSession const& session
        ) -> Result<annotation::AuthoringDocument>
        {
            UF_TRY_VALUE(
                document,
                workbench::buildAuthoringDocument(session.draft)
            );
            UF_TRY(
                workbench::saveAndGenerateAuthoringProject(
                    root,
                    document,
                    session.assets
                )
            );
            return document;
        }

        struct ResolvedSource final
        {
            EditSession          session;
            annotation::SourceId id;
            bool                 ingested{};
        };

        // Resolves what --source named. A value ContentHash::parse accepts is
        // the hash of a screen the project already holds; anything else is a
        // path to a PNG to ingest.
        //
        // Ingestion goes through workbench::importSourcePng, which re-encodes
        // the image into the project's canonical PNG form, so the same
        // screenshot saved by two different encoders lands on one hash and one
        // installed file. That hash is also the source's identity here, which
        // makes re-ingesting an image already in the project a lookup rather
        // than a second copy under a second id. The bytes are only added to the
        // session; saveAndGenerateAuthoringProject is what installs them at
        // assets/sources/<hash>.png, so this CLI never writes an asset path.
        [[nodiscard]]
        auto resolveSource(
            EditSession session,
            std::string const& spec
        ) -> Result<ResolvedSource>
        {
            if (auto const parsed = annotation::ContentHash::parse(spec); parsed)
            {
                auto const found = std::ranges::find(
                    session.draft.sources,
                    *parsed,
                    &workbench::EditableSource::contentHash
                );
                if (found == session.draft.sources.end())
                {
                    return invalid(
                        std::format(
                            "no source with content hash {} is part of this project",
                            parsed->hex()
                        )
                    );
                }
                auto const id = found->id;
                return ResolvedSource{
                    .session  = std::move(session),
                    .id       = id,
                    .ingested = false,
                };
            }

            // Imported at the PROJECT's density, not at a file default. A PNG
            // carries no display density, so the only density that can be right
            // is the one the project was authored at -- and AuthoringDocument
            // refuses a source whose fingerprint differs, so a fixed 96 would
            // make every high-DPI project unauthorable rather than merely
            // approximate. The mismatch check below stays: it now catches a
            // genuine extent difference instead of a density this code chose.
            auto const path = std::filesystem::path{spec};
            UF_TRY_VALUE_CONTEXT(
                ingested,
                workbench::importSourcePng(
                    placeholderSourceId(),
                    path,
                    session.draft.fingerprint.dpiX()
                ),
                std::format("--source \"{}\"", spec)
            );
            auto const id = sourceIdOf(ingested.spec.contentHash);

            auto const existing = std::ranges::find(
                session.draft.sources,
                id,
                &workbench::EditableSource::id
            );
            if (existing != session.draft.sources.end())
            {
                return ResolvedSource{
                    .session  = std::move(session),
                    .id       = id,
                    .ingested = false,
                };
            }

            // AuthoringDocument::create refuses this too, but only as "every
            // authoring source must match the project fingerprint". Saying which
            // file and which two resolutions is the difference between an agent
            // fixing the command and an agent guessing.
            if (ingested.spec.fingerprint != session.draft.fingerprint)
            {
                return invalid(
                    std::format(
                        "screenshot \"{}\" is {}x{} but the project is authored at {}x{}",
                        spec,
                        ingested.spec.fingerprint.width(),
                        ingested.spec.fingerprint.height(),
                        session.draft.fingerprint.width(),
                        session.draft.fingerprint.height()
                    )
                );
            }

            session.draft.sources.emplace_back(
                workbench::EditableSource{
                    .id          = id,
                    .contentHash = ingested.spec.contentHash,
                    .fingerprint = ingested.spec.fingerprint,
                    .provenance  = ingested.spec.provenance,
                }
            );
            session.assets.emplace_back(
                annotation::AuthoringSourceAsset{
                    .id       = id,
                    .pngBytes = std::move(ingested.asset.pngBytes),
                }
            );
            return ResolvedSource{
                .session  = std::move(session),
                .id       = id,
                .ingested = true,
            };
        }

        [[nodiscard]]
        auto findPageByName(
            workbench::AuthoringDraft const& draft,
            std::string const& name
        ) -> Result<annotation::PageId>
        {
            auto const found = std::ranges::find(
                draft.pages,
                name,
                &workbench::EditablePage::name
            );
            if (found == draft.pages.end())
            {
                return invalid(
                    std::format("no page named \"{}\" is part of this project", name)
                );
            }
            return found->id;
        }

        // The CLI names an element; createPageFromSource mints its own
        // placeholder name because a canvas caller has none to give yet.
        // Renaming in the draft rather than teaching it a name keeps the linkage
        // it performs -- an element, a page, the reference deriving the
        // signature and the regression case -- as the one implementation of it.
        // An unusable name is refused by ResourceName::create when the draft is
        // built.
        [[nodiscard]]
        auto renameRecognizer(
            workbench::AuthoringDraft draft,
            annotation::ElementId id,
            std::string name
        ) -> Result<workbench::AuthoringDraft>
        {
            auto const found = std::ranges::find(
                draft.recognizers,
                id,
                &workbench::EditableRecognizer::id
            );
            if (found == draft.recognizers.end())
            {
                return invalid("the element just added is not part of the draft");
            }
            found->name = std::move(name);
            return draft;
        }

        [[nodiscard]]
        auto renamePage(
            workbench::AuthoringDraft draft,
            annotation::PageId id,
            std::string name
        ) -> Result<workbench::AuthoringDraft>
        {
            auto const found = std::ranges::find(
                draft.pages,
                id,
                &workbench::EditablePage::id
            );
            if (found == draft.pages.end())
            {
                return invalid("the page just created is not part of the draft");
            }
            found->name = std::move(name);
            return draft;
        }

        // The element half of what --capability said, and the page half. They
        // are two types in the model on purpose -- what a page adds to identify
        // has no meaning on an element, and the OCR parameters have none on a
        // page -- so the one flag set is mapped across twice rather than stored
        // once and reinterpreted.
        [[nodiscard]]
        auto declaredCapabilitiesOf(
            DrawnCapabilities const& drawn
        ) -> workbench::EditableCapabilities
        {
            auto declared = workbench::EditableCapabilities{};
            if (drawn.identify)
            {
                declared.identify = annotation::Identify{};
            }
            if (drawn.interact)
            {
                declared.interact = workbench::EditableInteract{};
            }
            if (drawn.read)
            {
                declared.read = annotation::Read{};
            }
            return declared;
        }

        [[nodiscard]]
        auto exercisedCapabilitiesOf(
            DrawnCapabilities const& drawn
        ) -> workbench::EditableExercised
        {
            auto exercised = workbench::EditableExercised{};
            if (drawn.identify)
            {
                exercised.identify = annotation::ExercisedIdentify{
                    .role = *drawn.identify,
                };
            }
            if (drawn.interact)
            {
                exercised.interact = annotation::ExercisedInteract{};
            }
            if (drawn.read)
            {
                exercised.read = annotation::ExercisedRead{};
            }
            return exercised;
        }

        [[nodiscard]]
        auto searchRoiOf(
            ElementDraw const& draw,
            annotation::ProjectFingerprint fingerprint
        ) -> Result<PixelRect>
        {
            if (draw.searchRoi)
            {
                return *draw.searchRoi;
            }
            // Without --search-roi the matcher may look anywhere on the screen.
            // Narrowing it is a measurement the author has to make; guessing one
            // would silently decide where a template is allowed to be found.
            return PixelRect::create(
                0,
                0,
                fingerprint.width(),
                fingerprint.height()
            );
        }

        [[nodiscard]]
        auto drawJson(
            annotation::AuthoringDocument const& document,
            annotation::ElementId id,
            bool ingested
        ) -> Result<std::string>
        {
            auto const* p_element = document.findElement(id);
            if (p_element == nullptr)
            {
                return invalid("the element just authored is not in the document");
            }
            auto const members = std::array{
                JsonMember{.key = "element", .value = elementJson(*p_element)},
                JsonMember{
                    .key   = "source_ingested",
                    .value = jsonBoolean(ingested),
                },
            };
            return jsonObject(members);
        }

        // The frame a match runs against, built from a PNG exactly as the live
        // path builds one from a captured window: BGRA8, one packed row stride,
        // and an identity transform, so the recognizer compares the same pixels
        // it would compare on screen.
        [[nodiscard]]
        auto frameFromPng(
            std::filesystem::path const& path,
            annotation::ProjectFingerprint fingerprint
        ) -> Result<Frame>
        {
            UF_TRY_VALUE_CONTEXT(
                decoded,
                image::loadPng(path),
                std::format("--frame \"{}\"", path.string())
            );
            if (
                decoded.width != fingerprint.width()
                || decoded.height != fingerprint.height()
            )
            {
                return invalid(
                    std::format(
                        "frame \"{}\" is {}x{} but the project is authored at {}x{}",
                        path.string(),
                        decoded.width,
                        decoded.height,
                        fingerprint.width(),
                        fingerprint.height()
                    )
                );
            }

            UF_TRY_VALUE(bgra, image::rgba8ToBgra8(std::move(decoded.pixels)));
            UF_TRY_VALUE(
                transform,
                CoordinateTransform::create(
                    Point<DesktopSpace>{0.0F, 0.0F},
                    static_cast<float>(fingerprint.width()),
                    static_cast<float>(fingerprint.height()),
                    fingerprint.width(),
                    fingerprint.height()
                )
            );

            auto const width = checkedCast<std::size_t>(fingerprint.width());
            UF_CHECK(width.has_value());
            auto const stride = checkedMultiply(
                width.value_or(std::size_t{0}),
                bytesPerPixel(PixelFormat::Bgra8)
            );
            UF_CHECK(stride.has_value());

            auto const p_pixels = std::shared_ptr<FrameBuffer const>{
                std::make_shared<FrameBuffer>(std::move(bgra))
            };
            return Frame::create(
                FrameId{1},
                CaptureSessionId{1},
                TargetGeneration::fromValue(1),
                MonotonicInstant::fromTimePoint(MonotonicInstant::TimePoint{}),
                fingerprint.width(),
                fingerprint.height(),
                stride.value_or(std::size_t{0}),
                PixelFormat::Bgra8,
                p_pixels,
                transform
            );
        }

        [[nodiscard]]
        auto stopFailure(
            annotation::PageRecognitionStop const& stop
        ) -> std::unexpected<Error>
        {
            return fail(
                annotation::searchStopKind(stop.reason),
                std::format(
                    "recognition stopped on {}: {}",
                    stop.recognizerId.value().toString(),
                    annotation::searchStopDescription(stop.reason)
                )
            );
        }

        struct MatchOutcome final
        {
            annotation::AnchorEvidence evidence;

            // The page a click target was located on. Absent for the anchor
            // pass, which runs before any page is known -- that is what makes
            // one search serve every page.
            std::optional<annotation::PageId> pageId{};

            uint64 pixelComparisons{};
        };

        // Which of the runtime's two entry points measures this element.
        enum class MatchPath : uint8
        {
            PageSignature,
            ActionTarget,
            Unreachable,
        };

        // Identify wins when an element declares both. The anchor pass is global
        // and page-independent, so it is the measurement that needs no page
        // argument at all, and a reference exercising identify may not refine
        // the search region -- so an element that also interacts is searched
        // over the same rectangle of the same region either way.
        [[nodiscard]]
        auto matchPathOf(
            annotation::ElementCapabilities const& capabilities
        ) noexcept -> MatchPath
        {
            if (capabilities.hasIdentify())
            {
                return MatchPath::PageSignature;
            }
            if (capabilities.hasInteract())
            {
                return MatchPath::ActionTarget;
            }
            return MatchPath::Unreachable;
        }

        [[nodiscard]]
        auto pageNameList(
            annotation::RecognitionCatalog const& catalog,
            std::span<annotation::PageId const> ids
        ) -> std::string
        {
            auto listed = std::string{};
            for (auto const id : ids)
            {
                if (!listed.empty())
                {
                    listed += ", ";
                }
                auto const* p_page = catalog.findPage(id);
                listed += p_page == nullptr
                    ? id.value().toString()
                    : p_page->name().value();
            }
            return listed;
        }

        // Which page a click target is located on. It is a page-scoped question
        // now: the refined search region and the pinned appearance both live on
        // the page's reference, so there is no page-less entry point to fall
        // back on. The references answer it when only one page clicks the
        // element; nothing but --page can choose when several do.
        [[nodiscard]]
        auto interactPageOf(
            annotation::RecognitionCatalog const& catalog,
            annotation::RecognizerDefinition const& recognizer,
            std::optional<std::string> const& requested
        ) -> Result<annotation::PageId>
        {
            auto pages = std::vector<annotation::PageId>{};
            for (auto const& reference : catalog.references())
            {
                if (
                    reference.elementId == recognizer.id()
                    && reference.exercised.hasInteract()
                    && !std::ranges::contains(pages, reference.pageId)
                )
                {
                    pages.emplace_back(reference.pageId);
                }
            }

            if (requested)
            {
                auto const named = std::ranges::find_if(
                    catalog.pages(),
                    [&requested](annotation::PageSignature const& page)
                    {
                        return page.name().value() == *requested;
                    }
                );
                if (named == catalog.pages().end())
                {
                    return invalid(
                        std::format(
                            "no page named \"{}\" is part of this project",
                            *requested
                        )
                    );
                }
                if (!std::ranges::contains(pages, named->id()))
                {
                    return invalid(
                        std::format(
                            "page \"{}\" does not click \"{}\"; the pages that "
                            "do are: {}",
                            *requested,
                            recognizer.name().value(),
                            pageNameList(catalog, pages)
                        )
                    );
                }
                return named->id();
            }

            if (pages.empty())
            {
                return invalid(
                    std::format(
                        "no page clicks \"{}\", so there is no page to locate "
                        "it on",
                        recognizer.name().value()
                    )
                );
            }
            if (pages.size() > 1U)
            {
                return invalid(
                    std::format(
                        "\"{}\" is clicked on more than one page, so --page has "
                        "to name which: {}",
                        recognizer.name().value(),
                        pageNameList(catalog, pages)
                    )
                );
            }
            return pages.front();
        }

        [[nodiscard]]
        auto matchActionTarget(
            annotation::RecognitionRuntime const& runtime,
            annotation::RecognizerDefinition const& recognizer,
            Frame const& frame,
            annotation::RecognitionPolicy const& policy,
            annotation::PageId pageId
        ) -> Result<MatchOutcome>
        {
            UF_TRY_VALUE(
                attempt,
                runtime.evaluateActionTarget(
                    frame,
                    runtime.manifest().catalog().fingerprint(),
                    pageId,
                    recognizer.id(),
                    policy
                )
            );
            if (
                auto const* p_stop = std::get_if<annotation::PageRecognitionStop>(
                    &attempt.result
                )
            )
            {
                return stopFailure(*p_stop);
            }
            auto const* p_evidence = std::get_if<annotation::AnchorEvidence>(
                &attempt.result
            );
            UF_CHECK(p_evidence != nullptr);
            return MatchOutcome{
                .evidence         = *p_evidence,
                .pageId           = pageId,
                .pixelComparisons = attempt.completedPixelComparisons,
            };
        }

        [[nodiscard]]
        auto matchPageAnchor(
            annotation::RecognitionRuntime const& runtime,
            annotation::RecognizerDefinition const& recognizer,
            Frame const& frame,
            annotation::RecognitionPolicy const& policy
        ) -> Result<MatchOutcome>
        {
            UF_TRY_VALUE(
                attempt,
                runtime.evaluatePage(
                    frame,
                    runtime.manifest().catalog().fingerprint(),
                    policy
                )
            );
            auto const found = std::ranges::find(
                attempt.completedAnchorEvidence,
                recognizer.id(),
                &annotation::AnchorEvidence::recognizerId
            );
            if (found == attempt.completedAnchorEvidence.end())
            {
                // The page evaluation stopped before it reached this anchor, so
                // there is no evidence to report and the stop is the answer.
                if (
                    auto const* p_stop = std::get_if<annotation::PageRecognitionStop>(
                        &attempt.result
                    )
                )
                {
                    return stopFailure(*p_stop);
                }
                return invalid(
                    std::format(
                        "\"{}\" produced no evidence in this page evaluation",
                        recognizer.name().value()
                    )
                );
            }
            return MatchOutcome{
                .evidence         = *found,
                .pixelComparisons = attempt.completedPixelComparisons,
            };
        }

        // An element that identifies is only ever searched as part of resolving
        // a page, so that is how it is measured; one that is clicked has its own
        // single evaluation on the page that clicks it. An element that is only
        // read has neither entry point -- reading happens inside a task that
        // already resolved the page -- so it is refused with the reason.
        [[nodiscard]]
        auto matchRecognizer(
            annotation::RecognitionRuntime const& runtime,
            annotation::RecognizerDefinition const& recognizer,
            Frame const& frame,
            annotation::RecognitionPolicy const& policy,
            std::optional<std::string> const& requestedPage
        ) -> Result<MatchOutcome>
        {
            auto const& catalog = runtime.manifest().catalog();
            switch (matchPathOf(recognizer.capabilities()))
            {
            case MatchPath::PageSignature:
                if (requestedPage)
                {
                    return invalid(
                        std::format(
                            "--page does not apply to \"{}\": it identifies a "
                            "page, and the anchor pass runs before any page is "
                            "known",
                            recognizer.name().value()
                        )
                    );
                }
                return matchPageAnchor(runtime, recognizer, frame, policy);
            case MatchPath::ActionTarget:
            {
                UF_TRY_VALUE(
                    pageId,
                    interactPageOf(catalog, recognizer, requestedPage)
                );
                return matchActionTarget(
                    runtime,
                    recognizer,
                    frame,
                    policy,
                    pageId
                );
            }
            case MatchPath::Unreachable:
                return invalid(
                    std::format(
                        "\"{}\" is only read, which the runtime does inside a "
                        "task that has already resolved the page",
                        recognizer.name().value()
                    )
                );
            }
            UF_UNREACHABLE_MSG("unknown MatchPath value");
        }

        [[nodiscard]]
        auto runInitProject(InitProject const& command) -> Result<std::string>
        {
            UF_TRY_VALUE(projectId, annotation::ProjectId::create(command.projectId));
            UF_TRY_VALUE(
                document,
                annotation::AuthoringDocument::create(
                    projectId,
                    command.fingerprint,
                    {},
                    {},
                    {},
                    {},
                    {}
                )
            );
            UF_TRY(
                workbench::saveAndGenerateAuthoringProject(command.root, document, {})
            );

            auto const members = std::array{
                JsonMember{
                    .key   = "root",
                    .value = jsonString(command.root.string()),
                },
                JsonMember{
                    .key   = "project_id",
                    .value = jsonString(projectId.value()),
                },
                JsonMember{
                    .key   = "fingerprint",
                    .value = fingerprintJson(command.fingerprint),
                },
            };
            return successJson("project init", members);
        }

        [[nodiscard]]
        auto runShowProject(ShowProject const& command) -> Result<std::string>
        {
            UF_TRY_VALUE(loaded, workbench::loadAuthoringProject(command.root));
            auto const& document = loaded.document;
            auto const& catalog  = document.catalog();

            auto sources = std::vector<std::string>{};
            for (auto const& source : document.sources())
            {
                sources.emplace_back(sourceJson(source));
            }
            auto elements = std::vector<std::string>{};
            for (auto const& element : document.elements())
            {
                elements.emplace_back(elementJson(element));
            }
            auto pages = std::vector<std::string>{};
            for (auto const& page : catalog.pages())
            {
                pages.emplace_back(pageJson(document, page));
            }

            // The names `match` accepts, read back out of the generated manifest
            // rather than off the document above. One element compiles to one
            // runtime recognizer now, so the two lists share their names; what
            // this still answers is whether the published manifest loads at all,
            // and what the compiler made of each appearance.
            UF_TRY_VALUE(runtime, engine::loadRuntimeProject(command.root));
            auto recognizers = std::vector<std::string>{};
            for (auto const& recognizer : runtime.runtime.manifest().catalog().recognizers())
            {
                recognizers.emplace_back(runtimeRecognizerJson(recognizer));
            }

            auto const members = std::array{
                JsonMember{
                    .key   = "root",
                    .value = jsonString(command.root.string()),
                },
                JsonMember{
                    .key   = "project_id",
                    .value = jsonString(catalog.projectId().value()),
                },
                JsonMember{
                    .key   = "fingerprint",
                    .value = fingerprintJson(catalog.fingerprint()),
                },
                JsonMember{.key = "sources", .value = jsonArray(sources)},
                JsonMember{.key = "elements", .value = jsonArray(elements)},
                JsonMember{.key = "pages", .value = jsonArray(pages)},
                JsonMember{
                    .key   = "runtime_recognizers",
                    .value = jsonArray(recognizers),
                },
            };
            return successJson("project show", members);
        }

        [[nodiscard]]
        auto runSaveProject(SaveProject const& command) -> Result<std::string>
        {
            UF_TRY_VALUE(session, openSession(command.root));
            UF_TRY_VALUE(document, commitSession(command.root, session));

            auto const members = std::array{
                JsonMember{
                    .key   = "root",
                    .value = jsonString(command.root.string()),
                },
                JsonMember{
                    .key   = "sources",
                    .value = jsonUnsigned(document.sources().size()),
                },
                JsonMember{
                    .key   = "elements",
                    .value = jsonUnsigned(document.elements().size()),
                },
                JsonMember{
                    .key   = "pages",
                    .value = jsonUnsigned(document.catalog().pages().size()),
                },
            };
            return successJson("project save", members);
        }

        [[nodiscard]]
        auto runCreatePage(CreatePage const& command) -> Result<std::string>
        {
            UF_TRY_VALUE(opened, openSession(command.root));
            UF_TRY_VALUE(resolved, resolveSource(std::move(opened), command.anchor.source));
            auto session = std::move(resolved.session);

            // A copy rather than a borrow into the draft: the draft is moved
            // into createPageFromSource below, and a reference into it would
            // outlive the owner it names.
            auto const projectId = session.draft.projectId.value();
            UF_TRY_VALUE(
                pageId,
                derivedResourceId("page", projectId, command.page)
            );
            UF_TRY_VALUE(
                anchorId,
                derivedResourceId("element", projectId, command.anchor.name)
            );
            UF_TRY_VALUE(
                regressionId,
                derivedResourceId("regression", projectId, command.page)
            );
            UF_TRY_VALUE(
                searchRoi,
                searchRoiOf(command.anchor, session.draft.fingerprint)
            );

            UF_TRY_VALUE(
                created,
                workbench::createPageFromSource(
                    std::move(session.draft),
                    workbench::NewPageSpec{
                        .pageId                = annotation::PageId{pageId},
                        .anchorId              = annotation::ElementId{anchorId},
                        .regressionId          = annotation::RegressionId{regressionId},
                        .sourceId              = resolved.id,
                        .templateRect          = command.anchor.templateRect,
                        .searchRoi             = searchRoi,
                        .similarityBasisPoints = command.anchor.threshold.basisPoints(),
                    }
                )
            );
            UF_TRY_VALUE(
                namedPage,
                renamePage(
                    std::move(created.draft),
                    annotation::PageId{pageId},
                    command.page
                )
            );
            UF_TRY_VALUE(
                namedAnchor,
                renameRecognizer(
                    std::move(namedPage),
                    annotation::ElementId{anchorId},
                    command.anchor.name
                )
            );
            UF_TRY_VALUE(
                keyed,
                workbench::setElementColourKey(
                    std::move(namedAnchor),
                    annotation::ElementId{anchorId},
                    command.anchor.colourKey
                )
            );

            session.draft = std::move(keyed);
            UF_TRY_VALUE(document, commitSession(command.root, session));
            UF_TRY_VALUE(
                drawn,
                drawJson(
                    document,
                    annotation::ElementId{anchorId},
                    resolved.ingested
                )
            );

            auto const pageMembers = std::array{
                JsonMember{
                    .key   = "id",
                    .value = jsonString(pageId.toString()),
                },
                JsonMember{.key = "name", .value = jsonString(command.page)},
            };
            auto const members = std::array{
                JsonMember{.key = "page", .value = jsonObject(pageMembers)},
                JsonMember{.key = "authored", .value = drawn},
            };
            return successJson("page create", members);
        }

        [[nodiscard]]
        auto runAddElement(AddElement const& command) -> Result<std::string>
        {
            UF_TRY_VALUE(opened, openSession(command.root));
            UF_TRY_VALUE(resolved, resolveSource(std::move(opened), command.draw.source));
            auto session = std::move(resolved.session);

            UF_TRY_VALUE(pageId, findPageByName(session.draft, command.page));
            UF_TRY_VALUE(
                elementResourceId,
                derivedResourceId(
                    "element",
                    session.draft.projectId.value(),
                    command.draw.name
                )
            );
            UF_TRY_VALUE(
                searchRoi,
                searchRoiOf(command.draw, session.draft.fingerprint)
            );

            // The element and the page's use of it, appended as one edit. It is
            // written here rather than through workbench::addPageMember because
            // that helper commits exactly one PageMemberKind, and the case this
            // whole model exists for -- one patch of pixels that both names its
            // page and can be clicked -- is not one of the three kinds. The
            // helper's other work is already done above: this caller knows the
            // name, minted the id, resolved the source and found the page.
            //
            // Holding is Owned with nothing to decide: a drawn rectangle belongs
            // to the page it was drawn on, and borrowing is `page reference`.
            auto const elementId  = annotation::ElementId{elementResourceId};
            auto const appearance = workbench::EditableVariant{
                .name                  = std::string{workbench::k_defaultVariantName},
                .sourceId              = resolved.id,
                .templateRect          = command.draw.templateRect,
                .similarityBasisPoints = command.draw.threshold.basisPoints(),
                .colourKey             = command.draw.colourKey,
            };
            session.draft.recognizers.emplace_back(
                workbench::EditableRecognizer{
                    .id           = elementId,
                    .name         = command.draw.name,
                    .capabilities = declaredCapabilitiesOf(command.capabilities),
                    .searchRoi    = searchRoi,
                    .variants     = {appearance},
                }
            );
            session.draft.references.emplace_back(
                workbench::EditableReference{
                    .pageId    = pageId,
                    .elementId = elementId,
                    .holding   = annotation::Holding::Owned,
                    .exercised = exercisedCapabilitiesOf(command.capabilities),
                }
            );

            UF_TRY_VALUE(document, commitSession(command.root, session));
            UF_TRY_VALUE(drawn, drawJson(document, elementId, resolved.ingested));

            auto const members = std::array{
                JsonMember{.key = "page", .value = jsonString(command.page)},
                JsonMember{.key = "authored", .value = drawn},
            };
            return successJson("page add", members);
        }

        // The element named, and what this page will do with it, kept apart from
        // the draft it was read out of: the draft is moved into the edit below,
        // and a borrow into it would outlive the owner it names.
        struct NamedElement final
        {
            annotation::ElementId id;
            PixelRect             searchRoi;
        };

        [[nodiscard]]
        auto findElementByName(
            workbench::AuthoringDraft const& draft,
            std::string const& name
        ) -> Result<NamedElement>
        {
            auto const found = std::ranges::find(
                draft.recognizers,
                name,
                &workbench::EditableRecognizer::name
            );
            if (found == draft.recognizers.end())
            {
                return invalid(
                    std::format(
                        "no element named \"{}\" is part of this project",
                        name
                    )
                );
            }
            return NamedElement{
                .id        = found->id,
                .searchRoi = found->searchRoi,
            };
        }

        [[nodiscard]]
        auto runReferenceElement(
            ReferenceElement const& command
        ) -> Result<std::string>
        {
            UF_TRY_VALUE(session, openSession(command.root));
            UF_TRY_VALUE(pageId, findPageByName(session.draft, command.page));
            UF_TRY_VALUE(
                element,
                findElementByName(session.draft, command.element)
            );

            // Seeded from the element's own region when --search-roi is absent,
            // which is what "this page did not refine it" means. Narrowing it is
            // a measurement the author makes; widening it here would enlarge
            // both the search cost and the surface for a false match.
            UF_TRY_VALUE(
                referenced,
                workbench::referenceElementOnPage(
                    std::move(session.draft),
                    workbench::ReferenceElementSpec{
                        .elementId = element.id,
                        .pageId    = pageId,
                        .searchRoi = command.searchRoi.value_or(element.searchRoi),
                    }
                )
            );

            session.draft = std::move(referenced.draft);
            UF_TRY_VALUE(document, commitSession(command.root, session));

            auto const* p_reference = document.catalog().findReference(
                pageId,
                element.id
            );
            if (p_reference == nullptr)
            {
                return invalid("the reference just made is not in the document");
            }

            // How many pages this element now serves. It is the number the whole
            // verb exists to raise above one, and the number an author has to
            // know before correcting the element's pixels: one edit lands on
            // every page counted here.
            auto reached = std::vector<annotation::PageId>{};
            for (auto const& reference : document.references())
            {
                if (
                    reference.elementId == element.id
                    && !std::ranges::contains(reached, reference.pageId)
                )
                {
                    reached.emplace_back(reference.pageId);
                }
            }

            auto const members = std::array{
                JsonMember{.key = "page", .value = jsonString(command.page)},
                JsonMember{
                    .key   = "element",
                    .value = jsonString(command.element),
                },
                JsonMember{
                    .key   = "reference",
                    .value = referenceJson(document, *p_reference),
                },
                JsonMember{
                    .key   = "pages_referencing",
                    .value = jsonUnsigned(reached.size()),
                },
            };
            return successJson("page reference", members);
        }

        [[nodiscard]]
        auto runMatchRecognizer(
            MatchRecognizer const& command
        ) -> Result<std::string>
        {
            UF_TRY_VALUE(loaded, engine::loadRuntimeProject(command.root));
            auto const& catalog    = loaded.runtime.manifest().catalog();
            auto const recognizers = catalog.recognizers();

            auto const found = std::ranges::find_if(
                recognizers,
                [&command](annotation::RecognizerDefinition const& recognizer)
                {
                    return recognizer.name().value() == command.recognizer;
                }
            );
            if (found == recognizers.end())
            {
                return invalid(
                    std::format(
                        "no recognizer named \"{}\" in this project; "
                        "project show lists every runtime recognizer",
                        command.recognizer
                    )
                );
            }

            UF_TRY_VALUE(frame, frameFromPng(command.frame, catalog.fingerprint()));
            UF_TRY_VALUE(
                outcome,
                matchRecognizer(
                    loaded.runtime,
                    *found,
                    frame,
                    annotation::RecognitionPolicy{
                        .maximumPixelComparisons = command.budget,
                    },
                    command.page
                )
            );

            auto const& evidence = outcome.evidence;
            auto pageName        = jsonNull();
            if (outcome.pageId)
            {
                auto const* p_page = catalog.findPage(*outcome.pageId);
                if (p_page != nullptr)
                {
                    pageName = jsonString(p_page->name().value());
                }
            }

            auto const members = std::array{
                JsonMember{
                    .key   = "root",
                    .value = jsonString(command.root.string()),
                },
                JsonMember{
                    .key   = "frame",
                    .value = jsonString(command.frame.string()),
                },
                JsonMember{
                    .key   = "recognizer",
                    .value = runtimeRecognizerJson(*found),
                },
                // Which page the target was located on, and null for the anchor
                // pass, which runs before any page is known.
                JsonMember{.key = "page", .value = std::move(pageName)},
                JsonMember{.key = "hit", .value = jsonBoolean(evidence.hit())},
                // Which appearance produced this evidence, so "why did it match"
                // is answerable from the answer. Null only for an element that
                // declares none and is located by its page.
                JsonMember{
                    .key   = "variant",
                    .value = evidence.variantName()
                        ? jsonString(evidence.variantName()->value())
                        : jsonNull(),
                },
                JsonMember{
                    .key   = "sad_score",
                    .value = evidence.sadScore()
                        ? jsonUnsigned(evidence.sadScore().value_or(0))
                        : jsonNull(),
                },
                JsonMember{
                    .key   = "maximum_sad",
                    .value = jsonUnsigned(evidence.maximumSad()),
                },
                JsonMember{
                    .key   = "matched_rect",
                    .value = evidence.matchedRect()
                        ? rectJson(*evidence.matchedRect())
                        : jsonNull(),
                },
                JsonMember{
                    .key   = "display_confidence",
                    .value = evidence.displayConfidence()
                        ? jsonNumber(evidence.displayConfidence().value_or(0.0F))
                        : jsonNull(),
                },
                JsonMember{
                    .key   = "pixel_comparisons",
                    .value = jsonUnsigned(outcome.pixelComparisons),
                },
            };
            return successJson("match", members);
        }

        // The four enumerations the falsification matrix answers with. Each is
        // one total mapping for the same reason as the three above: a switch
        // over a scoped enum makes the compiler name the case a new enumerator
        // forgot.
        [[nodiscard]]
        auto screenOutcomeName(workbench::ScreenCheckOutcome outcome) -> std::string_view
        {
            switch (outcome)
            {
            case workbench::ScreenCheckOutcome::Correct:
                return "correct";
            case workbench::ScreenCheckOutcome::WrongPage:
                return "wrong_page";
            case workbench::ScreenCheckOutcome::Unknown:
                return "unknown";
            case workbench::ScreenCheckOutcome::Ambiguous:
                return "ambiguous";
            case workbench::ScreenCheckOutcome::Unclaimed:
                return "unclaimed";
            case workbench::ScreenCheckOutcome::Stopped:
                return "stopped";
            }
            UF_UNREACHABLE_MSG("unknown ScreenCheckOutcome value");
        }

        [[nodiscard]]
        auto cellSubjectName(workbench::ModelCellSubject subject) -> std::string_view
        {
            switch (subject)
            {
            case workbench::ModelCellSubject::Element:
                return "element";
            case workbench::ModelCellSubject::Appearance:
                return "appearance";
            }
            UF_UNREACHABLE_MSG("unknown ModelCellSubject value");
        }

        [[nodiscard]]
        auto cellOutcomeName(workbench::ModelCellOutcome outcome) -> std::string_view
        {
            switch (outcome)
            {
            case workbench::ModelCellOutcome::Hit:
                return "hit";
            case workbench::ModelCellOutcome::Miss:
                return "miss";
            case workbench::ModelCellOutcome::Stopped:
                return "stopped";
            case workbench::ModelCellOutcome::NotSearchedHere:
                return "not_searched_here";
            }
            UF_UNREACHABLE_MSG("unknown ModelCellOutcome value");
        }

        [[nodiscard]]
        auto cellVerdictName(workbench::ModelCellColor colour) -> std::string_view
        {
            switch (colour)
            {
            case workbench::ModelCellColor::Expected:
                return "expected";
            case workbench::ModelCellColor::Thin:
                return "thin";
            case workbench::ModelCellColor::Misfire:
                return "misfire";
            case workbench::ModelCellColor::NotSearched:
                return "not_searched";
            }
            UF_UNREACHABLE_MSG("unknown ModelCellColor value");
        }

        // A screen by the file an author can open. The id is the truncated
        // content hash and says nothing to a reader; the installed path is what
        // they took the screenshot as.
        [[nodiscard]]
        auto screenNameJson(
            annotation::AuthoringDocument const& document,
            annotation::SourceId id
        ) -> std::string
        {
            auto const* p_source = document.findSource(id);
            return p_source == nullptr
                ? jsonString(id.value().toString())
                : jsonString(p_source->relativePath());
        }

        [[nodiscard]]
        auto pageNameJson(
            annotation::AuthoringDocument const& document,
            std::optional<annotation::PageId> id
        ) -> std::string
        {
            if (!id.has_value())
            {
                return jsonNull();
            }
            auto const* p_page = document.catalog().findPage(*id);
            return p_page == nullptr
                ? jsonString(id->value().toString())
                : jsonString(p_page->name().value());
        }

        [[nodiscard]]
        auto screenCheckJson(
            annotation::AuthoringDocument const& document,
            workbench::ScreenCheck const& screen
        ) -> std::string
        {
            auto const members = std::array{
                JsonMember{
                    .key   = "screen",
                    .value = screenNameJson(document, screen.sourceId),
                },
                JsonMember{
                    .key   = "expected_page",
                    .value = pageNameJson(document, screen.expectedPageId),
                },
                JsonMember{
                    .key   = "resolved_page",
                    .value = pageNameJson(document, screen.resolvedPageId),
                },
                JsonMember{
                    .key   = "outcome",
                    .value = jsonString(screenOutcomeName(screen.outcome)),
                },
            };
            return jsonObject(members);
        }

        // One measured cell. `verdict` is classifyModelCell's answer rather than
        // something the caller recomputes: the rule that a hit off the diagonal
        // is a misfire belongs to one place, and an agent reading this document
        // must not be the second implementation of it.
        [[nodiscard]]
        auto cellJson(
            annotation::AuthoringDocument const& document,
            workbench::ModelCheckCell const& cell
        ) -> std::string
        {
            auto const members = std::array{
                JsonMember{
                    .key   = "element",
                    .value = elementNameJson(document, cell.elementId),
                },
                JsonMember{
                    .key   = "screen",
                    .value = screenNameJson(document, cell.screenId),
                },
                JsonMember{
                    .key   = "subject",
                    .value = jsonString(cellSubjectName(cell.subject)),
                },
                JsonMember{
                    .key   = "appearance",
                    .value = cell.appearance
                        ? jsonString(cell.appearance->value())
                        : jsonNull(),
                },
                JsonMember{
                    .key   = "outcome",
                    .value = jsonString(cellOutcomeName(cell.outcome)),
                },
                JsonMember{
                    .key   = "expected_hit",
                    .value = jsonBoolean(cell.expectedHit),
                },
                JsonMember{
                    .key   = "verdict",
                    .value = jsonString(
                        cellVerdictName(workbench::classifyModelCell(cell))
                    ),
                },
                JsonMember{
                    .key   = "sad_score",
                    .value = cell.sadScore
                        ? jsonUnsigned(cell.sadScore.value_or(0))
                        : jsonNull(),
                },
                JsonMember{
                    .key   = "maximum_sad",
                    .value = jsonUnsigned(cell.maximumSad),
                },
                JsonMember{
                    .key   = "matched_rect",
                    .value = cell.matchedRect
                        ? rectJson(*cell.matchedRect)
                        : jsonNull(),
                },
            };
            return jsonObject(members);
        }

        [[nodiscard]]
        auto findingJson(
            annotation::AuthoringDocument const& document,
            workbench::ModelFinding const& finding
        ) -> std::string
        {
            auto const members = std::array{
                JsonMember{
                    .key   = "kind",
                    .value = jsonString(workbench::modelFindingKindName(finding.kind)),
                },
                JsonMember{
                    .key   = "element",
                    .value = elementNameJson(document, finding.elementId),
                },
                JsonMember{
                    .key   = "screen",
                    .value = screenNameJson(document, finding.screenId),
                },
                JsonMember{
                    .key   = "appearance",
                    .value = finding.appearance
                        ? jsonString(finding.appearance->value())
                        : jsonNull(),
                },
                JsonMember{
                    .key   = "rival",
                    .value = finding.rival
                        ? jsonString(finding.rival->value())
                        : jsonNull(),
                },
            };
            return jsonObject(members);
        }

        [[nodiscard]]
        auto runCheckModel(CheckModel const& command) -> Result<std::string>
        {
            UF_TRY_VALUE(loaded, workbench::loadAuthoringProject(command.root));
            UF_TRY_VALUE(
                check,
                workbench::runModelCheck(
                    loaded.document,
                    loaded.sources,
                    // No live frame: a capture from the running target is not a
                    // screen the model is authored on and contributes no column.
                    {},
                    annotation::RecognitionPolicy{
                        .maximumPixelComparisons = command.budget,
                    }
                )
            );
            auto const findings = workbench::judgeModelCheck(check);

            auto screens = std::vector<std::string>{};
            screens.reserve(check.screens.size());
            for (auto const& screen : check.screens)
            {
                screens.emplace_back(screenCheckJson(loaded.document, screen));
            }

            auto cells = std::vector<std::string>{};
            cells.reserve(check.cells.size());
            for (auto const& cell : check.cells)
            {
                cells.emplace_back(cellJson(loaded.document, cell));
            }

            auto reported = std::vector<std::string>{};
            reported.reserve(findings.size());
            for (auto const& finding : findings)
            {
                reported.emplace_back(findingJson(loaded.document, finding));
            }

            auto const members = std::array{
                JsonMember{
                    .key   = "root",
                    .value = jsonString(command.root.string()),
                },
                // The verdict first, because it is the field a caller branches
                // on. A model with findings still answers ok: the search ran and
                // measured everything it was asked to, and the findings are its
                // result rather than a failure to produce one.
                JsonMember{
                    .key   = "accepted",
                    .value = jsonBoolean(findings.empty()),
                },
                JsonMember{
                    .key   = "separation_factor",
                    .value = jsonUnsigned(workbench::k_appearanceSeparationFactor),
                },
                JsonMember{.key = "screens", .value = jsonArray(screens)},
                JsonMember{.key = "cells", .value = jsonArray(cells)},
                JsonMember{.key = "findings", .value = jsonArray(reported)},
            };
            return successJson("check", members);
        }

        // One frame decoded into the BGRA8 plane the analysis primitives read.
        // The plane is owned here because BgraImage is a view: every view below
        // borrows the plane of the DecodedFrame it was built from, so the
        // decoded set outlives the views taken over it or nothing does.
        struct DecodedFrame final
        {
            std::vector<std::byte> plane{};

            uint32 width{};
            uint32 height{};
        };

        // Frames need not share an extent -- only contain the analysed rect --
        // so nothing here compares one decode against the next. A frame too
        // small for the rect is refused by the analysis, which names the rect
        // and the extent it did not fit.
        [[nodiscard]]
        auto decodeFrames(
            std::span<std::filesystem::path const> paths
        ) -> Result<std::vector<DecodedFrame>>
        {
            auto decoded = std::vector<DecodedFrame>{};
            decoded.reserve(paths.size());
            for (auto const& path : paths)
            {
                UF_TRY_VALUE_CONTEXT(
                    loaded,
                    image::loadPng(path),
                    std::format("frame \"{}\"", path.string())
                );
                UF_TRY_VALUE(bgra, image::rgba8ToBgra8(std::move(loaded.pixels)));
                decoded.emplace_back(
                    DecodedFrame{
                        .plane  = std::move(bgra),
                        .width  = loaded.width,
                        .height = loaded.height,
                    }
                );
            }
            return decoded;
        }

        // Views over planes the caller owns. The whole set is taken at once
        // rather than one frame at a time precisely because the views cannot
        // outlive it, and a per-frame helper would hand back a dangling one.
        [[nodiscard]]
        auto viewFrames(
            std::span<DecodedFrame const> decoded UF_LIFETIME_BOUND
        ) -> Result<std::vector<BgraImage>>
        {
            auto views = std::vector<BgraImage>{};
            views.reserve(decoded.size());
            for (auto const& frame : decoded)
            {
                auto const width = checkedCast<std::size_t>(frame.width);
                UF_CHECK(width.has_value());
                auto const stride = checkedMultiply(
                    width.value_or(std::size_t{0}),
                    bytesPerPixel(PixelFormat::Bgra8)
                );
                UF_CHECK(stride.has_value());

                UF_TRY_VALUE(
                    view,
                    BgraImage::create(
                        frame.plane,
                        frame.width,
                        frame.height,
                        stride.value_or(std::size_t{0})
                    )
                );
                views.emplace_back(view);
            }
            return views;
        }

        [[nodiscard]]
        auto analysedRect(
            std::optional<PixelRect> const& requested,
            BgraImage const& first
        ) -> Result<PixelRect>
        {
            if (requested)
            {
                return *requested;
            }
            // Without --rect the question is where the UI is, which is a
            // question about the whole screen rather than about a rectangle the
            // author does not have yet. The first frame sets the extent.
            return PixelRect::create(0, 0, first.width(), first.height());
        }

        // A count against the total it was measured over. PixelRect::create
        // refuses an empty rectangle, so the guard is for a zero that cannot
        // arrive rather than one that does.
        [[nodiscard]]
        auto fractionOf(uint64 part, uint64 whole) noexcept -> float
        {
            if (whole == 0)
            {
                return 0.0F;
            }
            return static_cast<float>(
                static_cast<double>(part) / static_cast<double>(whole)
            );
        }

        [[nodiscard]]
        auto framePathsJson(
            std::span<std::filesystem::path const> paths
        ) -> std::string
        {
            auto encoded = std::vector<std::string>{};
            encoded.reserve(paths.size());
            for (auto const& path : paths)
            {
                encoded.emplace_back(jsonString(path.string()));
            }
            return jsonArray(encoded);
        }

        [[nodiscard]]
        auto runAnalyseFrameStability(
            AnalyseFrameStability const& command
        ) -> Result<std::string>
        {
            UF_TRY_VALUE(decoded, decodeFrames(command.frames));
            UF_TRY_VALUE(views, viewFrames(decoded));
            UF_TRY_VALUE(
                rect,
                analysedRect(command.rect, checkedAt(views, std::size_t{0}))
            );
            UF_TRY_VALUE(
                report,
                analyseStability(
                    views,
                    StabilitySpec{
                        .rect          = rect,
                        .grayTolerance = command.grayTolerance,
                        .minimumGap    = command.minimumGap,
                    }
                )
            );

            // The stable mask and the two projections are deliberately not
            // emitted: they are one number per pixel, and per row and column, of
            // the analysed rect, and the regions are what they were cut into.
            // Printing them would bury the answer inside the question.
            auto regions = std::vector<std::string>{};
            regions.reserve(report.regions.size());
            for (auto const& region : report.regions)
            {
                auto const entry = std::array{
                    JsonMember{
                        .key   = "bounds",
                        .value = rectJson(region.bounds),
                    },
                    JsonMember{
                        .key   = "stable_pixels",
                        .value = jsonUnsigned(region.stablePixels),
                    },
                };
                regions.emplace_back(jsonObject(entry));
            }

            auto const members = std::array{
                JsonMember{
                    .key   = "frames",
                    .value = framePathsJson(command.frames),
                },
                JsonMember{.key = "rect", .value = rectJson(rect)},
                JsonMember{
                    .key   = "gray_tolerance",
                    .value = jsonUnsigned(command.grayTolerance),
                },
                JsonMember{
                    .key   = "gap",
                    .value = jsonUnsigned(command.minimumGap),
                },
                JsonMember{
                    .key   = "rect_pixels",
                    .value = jsonUnsigned(report.rectPixels),
                },
                JsonMember{
                    .key   = "stable_pixels",
                    .value = jsonUnsigned(report.stablePixels),
                },
                JsonMember{
                    .key   = "stable_fraction",
                    .value = jsonNumber(
                        fractionOf(report.stablePixels, report.rectPixels)
                    ),
                },
                JsonMember{
                    .key   = "mean_gray_spread",
                    .value = jsonNumber(
                        static_cast<float>(report.meanGraySpread)
                    ),
                },
                // Bounds are in frame coordinates, so a region reads straight
                // back into --rect.
                JsonMember{.key = "regions", .value = jsonArray(regions)},
            };
            return successJson("frames stability", members);
        }

        [[nodiscard]]
        auto runProbeFrameColour(
            ProbeFrameColour const& command
        ) -> Result<std::string>
        {
            UF_TRY_VALUE(decoded, decodeFrames(command.frames));
            UF_TRY_VALUE(views, viewFrames(decoded));
            UF_TRY_VALUE(
                report,
                probeColour(
                    views,
                    ColourProbeSpec{
                        .rect      = command.rect,
                        .keyRed    = command.key.red(),
                        .keyGreen  = command.key.green(),
                        .keyBlue   = command.key.blue(),
                        .tolerance = command.key.tolerance(),
                    }
                )
            );

            auto const members = std::array{
                JsonMember{
                    .key   = "frames",
                    .value = framePathsJson(command.frames),
                },
                JsonMember{.key = "rect", .value = rectJson(command.rect)},
                JsonMember{.key = "key", .value = colourKeyJson(command.key)},
                JsonMember{
                    .key   = "rect_pixels",
                    .value = jsonUnsigned(report.rectPixels),
                },
                JsonMember{
                    .key   = "fully_selected_pixels",
                    .value = jsonUnsigned(report.fullySelectedPixels),
                },
                JsonMember{
                    .key   = "ramp_selected_pixels",
                    .value = jsonUnsigned(report.rampSelectedPixels),
                },
                JsonMember{
                    .key   = "selected_weight",
                    .value = jsonUnsigned(report.selectedWeight),
                },
                // The two means are adjacent because the gap between them is the
                // answer: a masked mean far below the rect mean is a rect whose
                // keyed pixels survive the background changing under them.
                JsonMember{
                    .key   = "masked_mean_gray_spread",
                    .value = jsonNumber(
                        static_cast<float>(report.maskedMeanGraySpread)
                    ),
                },
                JsonMember{
                    .key   = "rect_mean_gray_spread",
                    .value = jsonNumber(
                        static_cast<float>(report.rectMeanGraySpread)
                    ),
                },
            };
            return successJson("frames probe", members);
        }

        [[nodiscard]]
        auto runCensusFrameColours(
            CensusFrameColours const& command
        ) -> Result<std::string>
        {
            auto const paths = std::array{command.frame};
            UF_TRY_VALUE(decoded, decodeFrames(paths));
            UF_TRY_VALUE(views, viewFrames(decoded));
            UF_TRY_VALUE(
                report,
                censusColours(
                    checkedAt(views, std::size_t{0}),
                    ColourCensusSpec{
                        .rect           = command.rect,
                        .maximumEntries = command.maximumEntries,
                    }
                )
            );

            // Channels in the order --key takes them, so a dominant colour is
            // copied straight into the flag that keys on it.
            auto dominant = std::vector<std::string>{};
            dominant.reserve(report.dominant.size());
            for (auto const& colour : report.dominant)
            {
                auto const entry = std::array{
                    JsonMember{.key = "red", .value = jsonUnsigned(colour.red)},
                    JsonMember{
                        .key   = "green",
                        .value = jsonUnsigned(colour.green),
                    },
                    JsonMember{
                        .key   = "blue",
                        .value = jsonUnsigned(colour.blue),
                    },
                    JsonMember{
                        .key   = "count",
                        .value = jsonUnsigned(colour.count),
                    },
                    JsonMember{
                        .key   = "fraction",
                        .value = jsonNumber(
                            fractionOf(colour.count, report.rectPixels)
                        ),
                    },
                };
                dominant.emplace_back(jsonObject(entry));
            }

            auto const members = std::array{
                JsonMember{
                    .key   = "frame",
                    .value = jsonString(command.frame.string()),
                },
                JsonMember{.key = "rect", .value = rectJson(command.rect)},
                JsonMember{
                    .key   = "rect_pixels",
                    .value = jsonUnsigned(report.rectPixels),
                },
                JsonMember{
                    .key   = "distinct_colours",
                    .value = jsonUnsigned(report.distinctColours),
                },
                JsonMember{.key = "dominant", .value = jsonArray(dominant)},
            };
            return successJson("frames census", members);
        }
    }

    auto runAuthoringCommand(
        AuthoringCommand const& command
    ) -> Result<std::string>
    {
        return std::visit(
            [](auto const& specific) -> Result<std::string>
            {
                using Specific = std::remove_cvref_t<decltype(specific)>;
                if constexpr (std::same_as<Specific, InitProject>)
                {
                    return runInitProject(specific);
                }
                else if constexpr (std::same_as<Specific, ShowProject>)
                {
                    return runShowProject(specific);
                }
                else if constexpr (std::same_as<Specific, SaveProject>)
                {
                    return runSaveProject(specific);
                }
                else if constexpr (std::same_as<Specific, CreatePage>)
                {
                    return runCreatePage(specific);
                }
                else if constexpr (std::same_as<Specific, AddElement>)
                {
                    return runAddElement(specific);
                }
                else if constexpr (std::same_as<Specific, ReferenceElement>)
                {
                    return runReferenceElement(specific);
                }
                else if constexpr (std::same_as<Specific, MatchRecognizer>)
                {
                    return runMatchRecognizer(specific);
                }
                else if constexpr (std::same_as<Specific, CheckModel>)
                {
                    return runCheckModel(specific);
                }
                else if constexpr (std::same_as<Specific, AnalyseFrameStability>)
                {
                    return runAnalyseFrameStability(specific);
                }
                else if constexpr (std::same_as<Specific, ProbeFrameColour>)
                {
                    return runProbeFrameColour(specific);
                }
                else
                {
                    static_assert(std::same_as<Specific, CensusFrameColours>);
                    return runCensusFrameColours(specific);
                }
            },
            command
        );
    }

    auto authoringErrorJson(Error const& error) -> std::string
    {
        // The wire name, not the enumerator spelling. Every other JSON surface in
        // the tree already answers with it -- the trace's errorKind, the Luau
        // uf.errors table, the drive protocol's results -- so an agent reading two
        // of them had to carry two spellings of one kind.
        auto kindName = std::optional<std::string_view>{};
        if (auto const kind = automationErrorKind(error); kind)
        {
            kindName = automationErrorWireName(*kind);
        }

        auto context = std::vector<std::string>{};
        for (auto const& frame : error.context())
        {
            context.emplace_back(jsonString(frame));
        }

        // `kind` names what went wrong and `response` names how far it has to
        // unwind, which is the axis a caller actually branches on. An agent
        // driving `match` needs both: a completed search that matched nothing
        // answers ok with "hit":false, while a search that never finished
        // answers this document with a Retry response, and acting on the second
        // as though it were the first is acting on a screen never inspected.
        auto const detail = std::array{
            JsonMember{
                .key   = "kind",
                .value = jsonString(
                    kindName.value_or("UnknownAutomationErrorKind")
                ),
            },
            JsonMember{
                .key   = "response",
                .value = jsonString(failureResponseWireName(failureResponse(error))),
            },
            JsonMember{.key = "message", .value = jsonString(error.message())},
            JsonMember{.key = "context", .value = jsonArray(context)},
        };
        auto const members = std::array{
            JsonMember{.key = "ok", .value = jsonBoolean(false)},
            JsonMember{.key = "error", .value = jsonObject(detail)},
        };
        return jsonObject(members);
    }
}
