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
        auto appearanceJson(annotation::Appearance const& appearance) -> std::string
        {
            auto const members = std::array{
                JsonMember{
                    .key   = "name",
                    .value = jsonString(appearance.name().value()),
                },
                JsonMember{
                    .key   = "source_id",
                    .value = jsonString(appearance.sourceId().value().toString()),
                },
                JsonMember{
                    .key   = "template_rect",
                    .value = rectJson(appearance.templateRect()),
                },
                JsonMember{
                    .key   = "min_similarity_bp",
                    .value = jsonUnsigned(appearance.threshold().basisPoints()),
                },
                JsonMember{
                    .key   = "colour_key",
                    .value = colourKeyJson(appearance.colourKey()),
                },
            };
            return jsonObject(members);
        }

        // An empty list is a legal and meaningful answer: it says this rectangle
        // is located by the page being recognised rather than by pixels of its
        // own, which is why nothing here invents a placeholder appearance.
        [[nodiscard]]
        auto appearancesJson(
            std::span<annotation::Appearance const> appearances
        ) -> std::string
        {
            auto encoded = std::vector<std::string>{};
            encoded.reserve(appearances.size());
            for (auto const& appearance : appearances)
            {
                encoded.emplace_back(appearanceJson(appearance));
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
                    .key   = "appearances",
                    .value = appearancesJson(element.appearances()),
                },
            };
            return jsonObject(members);
        }

        // The runtime's view of one element: the same identity and capabilities,
        // and appearances stripped of the two facts that stop at authoring --
        // the screen a template was cut from, and the colour key that produced
        // its mask, which the runtime reads off the template's alpha channel.
        [[nodiscard]]
        auto runtimeElementJson(
            annotation::CompiledElement const& element
        ) -> std::string
        {
            auto appearances = std::vector<std::string>{};
            appearances.reserve(element.appearances().size());
            for (auto const& appearance : element.appearances())
            {
                auto const entry = std::array{
                    JsonMember{
                        .key   = "name",
                        .value = jsonString(appearance.name.value()),
                    },
                    JsonMember{
                        .key   = "template_rect",
                        .value = rectJson(appearance.templateRect),
                    },
                    JsonMember{
                        .key   = "min_similarity_bp",
                        .value = jsonUnsigned(appearance.threshold.basisPoints()),
                    },
                };
                appearances.emplace_back(jsonObject(entry));
            }

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
                JsonMember{.key = "appearances", .value = jsonArray(appearances)},
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

        // One page's use of one element. `search_roi` and `appearance` are null
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
                    .key   = "appearance",
                    .value = reference.appearance
                        ? jsonString(reference.appearance->value())
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
        auto renameElement(
            workbench::AuthoringDraft draft,
            annotation::ElementId id,
            std::string name
        ) -> Result<workbench::AuthoringDraft>
        {
            auto const found = std::ranges::find(
                draft.elements,
                id,
                &workbench::EditableElement::id
            );
            if (found == draft.elements.end())
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
            StatedCapabilities const& drawn
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
            StatedCapabilities const& drawn
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

        // The two ends of one question about a colour key an author just drew:
        // does the mask it produces select a FIGURE out of the rectangle? Below
        // the floor there is not enough of one to locate anything. At or above
        // the share limit the key took the rectangle's ground instead, and
        // whatever the rectangle was drawn around is in the holes, where a
        // masked comparison never looks.
        //
        // Neither number is a gate and neither may become one. `check` is the
        // gate: it searches the element against the screens it must NOT match
        // and answers from measurement, where these two answer only from shape.
        //
        // Under the floor, a match is the lowest score anywhere in the search
        // region with an exact-match early exit (`vision/sad.cpp`), so a handful
        // of saturated pixels never has to survive where the glyph was -- it
        // only has to find SOME offset where all of them land on that colour,
        // and a busy screen always offers one. Measured on this project: masks
        // of 27, 30 and 35 pixels each scored zero on frames whose content had
        // visibly changed.
        //
        // At or above half the rectangle, every selected pixel is within
        // tolerance of one colour by construction, so a mask that large is a
        // solid patch of that colour and any patch of it the same size matches.
        // Measured on this project: an orange button fill at 68%, a white info
        // disc at 63% and a disabled grey button at 75% all measure beautifully
        // and distinguish nothing, while every element that survived cross-page
        // falsification selects between 6.6% and 25.8% of its rectangle.
        constexpr auto k_minimumMaskPixels  = uint64{50};
        constexpr auto k_maximumMaskPercent = uint64{50};

        // The mask the compiler is about to bake, measured on the frame it was
        // drawn from. It goes through probeColour -- the function `frames probe`
        // answers with -- rather than counting here, so the number this warning
        // fires on and the number an author checks it against cannot become two
        // different numbers.
        //
        // probeColour requires two frames because its two spread figures are
        // across-frame measurements, and a drawing verb has exactly one frame.
        // The same view is handed to it twice: selection reads frames[0], which
        // ColourProbeReport documents, so the counts are exactly what the probe
        // would report for this rectangle and key, and both spreads come back
        // zero. That is the truth about one frame, and it is why the warning
        // below is built on counts alone.
        [[nodiscard]]
        auto measureDrawnMask(
            std::span<annotation::AuthoringSourceAsset const> assets,
            annotation::SourceId sourceId,
            PixelRect const& templateRect,
            annotation::ColourKey const& key
        ) -> Result<ColourProbeReport>
        {
            auto const found = std::ranges::find(
                assets,
                sourceId,
                &annotation::AuthoringSourceAsset::id
            );
            if (found == assets.end())
            {
                return invalid(
                    "the screen this rectangle was drawn on is not loaded"
                );
            }

            UF_TRY_VALUE(
                decoded,
                image::decodePng(found->pngBytes, "authoring source")
            );
            UF_TRY_VALUE(bgra, image::rgba8ToBgra8(std::move(decoded.pixels)));

            auto const width = checkedCast<std::size_t>(decoded.width);
            UF_CHECK(width.has_value());
            auto const stride = checkedMultiply(
                width.value_or(std::size_t{0}),
                bytesPerPixel(PixelFormat::Bgra8)
            );
            UF_CHECK(stride.has_value());

            UF_TRY_VALUE(
                view,
                BgraImage::create(
                    bgra,
                    decoded.width,
                    decoded.height,
                    stride.value_or(std::size_t{0})
                )
            );

            auto const frames = std::array{view, view};
            return probeColour(
                frames,
                ColourProbeSpec{
                    .rect      = templateRect,
                    .keyRed    = key.red(),
                    .keyGreen  = key.green(),
                    .keyBlue   = key.blue(),
                    .tolerance = key.tolerance(),
                }
            );
        }

        // Absent when the mask has a figure in it, which is the ordinary case
        // and must stay the ordinary case: a warning an author sees on good work
        // is one they stop reading. The floor is answered first because it is
        // the more specific diagnosis -- a small rectangle can be under both.
        [[nodiscard]]
        auto maskWarning(
            ColourProbeReport const& report
        ) -> std::optional<std::string>
        {
            if (report.fullySelectedPixels < k_minimumMaskPixels)
            {
                return std::format(
                    "this key selects {} of the rectangle's {} pixels, under "
                    "the {} a mask needs to measure anything. A match is the "
                    "lowest score anywhere in the search region, so a mask this "
                    "small never has to survive where the glyph is -- it only "
                    "has to find some offset where every selected pixel lands "
                    "on that colour, and a busy screen always offers one. Widen "
                    "the rectangle, loosen --tolerance, or key a different "
                    "feature. This is a hint; `check` is what decides.",
                    report.fullySelectedPixels,
                    report.rectPixels,
                    k_minimumMaskPixels
                );
            }
            // Cross-multiplied rather than divided, so the comparison is exact
            // at every rectangle size. Neither side can overflow: a rectangle is
            // bounded by the largest decodable image.
            auto const selectedHundredths = report.fullySelectedPixels * 100U;
            auto const limitHundredths    = report.rectPixels * k_maximumMaskPercent;
            if (selectedHundredths >= limitHundredths)
            {
                return std::format(
                    "this key selects {} of the rectangle's {} pixels, {}% of "
                    "it: at half the rectangle or more the key has taken the "
                    "fill rather than the figure drawn on it. Every selected "
                    "pixel is within tolerance of one colour, so a mask this "
                    "large is a solid patch of that colour and any patch of it "
                    "the same size matches, while the glyph-shaped holes carry "
                    "no weight. Key the glyph instead. This is a hint; `check` "
                    "is what decides.",
                    report.fullySelectedPixels,
                    report.rectPixels,
                    report.fullySelectedPixels * 100U / report.rectPixels
                );
            }
            return {};
        }

        // What the drawn key keeps, said at the moment it is drawn rather than
        // at a match that fails much later or never fails at all. Null when the
        // draw carries no key: an unkeyed template compares every pixel of the
        // rectangle, so there is no mask, and "the key selected the wrong thing"
        // is not a question that can be asked of it.
        [[nodiscard]]
        auto maskJson(
            std::span<annotation::AuthoringSourceAsset const> assets,
            annotation::SourceId sourceId,
            ElementDraw const& draw
        ) -> Result<std::string>
        {
            if (!draw.colourKey)
            {
                return jsonNull();
            }

            UF_TRY_VALUE(
                report,
                measureDrawnMask(
                    assets,
                    sourceId,
                    draw.templateRect,
                    *draw.colourKey
                )
            );
            auto const warning = maskWarning(report);
            auto warningValue  = jsonNull();
            if (warning)
            {
                warningValue = jsonString(*warning);
            }

            // The two counts carry the names `frames probe` gives them, because
            // they are the same two numbers from the same function and an author
            // comparing the two documents must not have to translate.
            auto const members = std::array{
                JsonMember{
                    .key   = "rect_pixels",
                    .value = jsonUnsigned(report.rectPixels),
                },
                JsonMember{
                    .key   = "fully_selected_pixels",
                    .value = jsonUnsigned(report.fullySelectedPixels),
                },
                JsonMember{
                    .key   = "selected_fraction",
                    .value = jsonNumber(
                        fractionOf(report.fullySelectedPixels, report.rectPixels)
                    ),
                },
                JsonMember{
                    .key   = "warning",
                    .value = std::move(warningValue),
                },
            };
            return jsonObject(members);
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

        // `mask` is an already-encoded value from maskJson, not a second thing
        // to measure here: the measurement needs the source bytes, which the
        // session owns and the saved document does not.
        [[nodiscard]]
        auto drawJson(
            annotation::AuthoringDocument const& document,
            annotation::ElementId id,
            bool ingested,
            std::string mask
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
                JsonMember{.key = "mask", .value = std::move(mask)},
            };
            return jsonObject(members);
        }

        // The frame a match runs against, built from a PNG exactly as the live
        // path builds one from a captured window: BGRA8, one packed row stride,
        // and an identity transform, so the element compares the same pixels
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
                    stop.elementId.value().toString(),
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

        // Which of the runtime's two entry points measures this element, or why
        // neither of them measures anything.
        enum class MatchPath : uint8
        {
            PageSignature,
            ActionTarget,
            NoPixels,
            Unreachable,
        };

        // An element with no appearance is answered first, before either entry
        // point: it declares no pixels, so nothing about it can be measured on a
        // frame. Otherwise identify wins when an element declares both. The
        // anchor pass is global and page-independent, so it is the measurement
        // that needs no page argument at all, and a reference exercising
        // identify may not refine the search region -- so an element that also
        // interacts is searched over the same rectangle of the same region
        // either way.
        [[nodiscard]]
        auto matchPathOf(
            annotation::CompiledElement const& element
        ) noexcept -> MatchPath
        {
            if (element.appearances().empty())
            {
                return MatchPath::NoPixels;
            }
            if (element.capabilities().hasIdentify())
            {
                return MatchPath::PageSignature;
            }
            if (element.capabilities().hasInteract())
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
            annotation::CompiledElement const& element,
            std::optional<std::string> const& requested
        ) -> Result<annotation::PageId>
        {
            auto pages = std::vector<annotation::PageId>{};
            for (auto const& reference : catalog.references())
            {
                if (
                    reference.elementId == element.id()
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
                            element.name().value(),
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
                        element.name().value()
                    )
                );
            }
            if (pages.size() > 1U)
            {
                return invalid(
                    std::format(
                        "\"{}\" is clicked on more than one page, so --page has "
                        "to name which: {}",
                        element.name().value(),
                        pageNameList(catalog, pages)
                    )
                );
            }
            return pages.front();
        }

        [[nodiscard]]
        auto matchActionTarget(
            annotation::RecognitionRuntime const& runtime,
            annotation::CompiledElement const& element,
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
                    element.id(),
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
            annotation::CompiledElement const& element,
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
                element.id(),
                &annotation::AnchorEvidence::elementId
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
                        element.name().value()
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
        // already resolved the page -- so it is refused with the reason. So is
        // one that declares no appearance, whichever capabilities it has.
        [[nodiscard]]
        auto matchElement(
            annotation::RecognitionRuntime const& runtime,
            annotation::CompiledElement const& element,
            Frame const& frame,
            annotation::RecognitionPolicy const& policy,
            std::optional<std::string> const& requestedPage
        ) -> Result<MatchOutcome>
        {
            auto const& catalog = runtime.manifest().catalog();
            switch (matchPathOf(element))
            {
            case MatchPath::NoPixels:
                // The runtime would answer, and its answer would be a hit at
                // the annotated rectangle -- on this frame and on every other,
                // because nothing was compared. Reporting that as a match is
                // the one reading of this verb an author must not be given: it
                // is the only tool that tells a rectangle that is really there
                // from one that merely used to be.
                return invalid(
                    std::format(
                        "\"{}\" declares no appearance, so there is nothing to "
                        "match: it is located by the page being recognised, and "
                        "resolving that page is the whole of its evidence. Give "
                        "it one with `element appearance` if these pixels are "
                        "meant to be stable, or check the page instead",
                        element.name().value()
                    )
                );
            case MatchPath::PageSignature:
                if (requestedPage)
                {
                    return invalid(
                        std::format(
                            "--page does not apply to \"{}\": it identifies a "
                            "page, and the anchor pass runs before any page is "
                            "known",
                            element.name().value()
                        )
                    );
                }
                return matchPageAnchor(runtime, element, frame, policy);
            case MatchPath::ActionTarget:
            {
                UF_TRY_VALUE(
                    pageId,
                    interactPageOf(catalog, element, requestedPage)
                );
                return matchActionTarget(
                    runtime,
                    element,
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
                        element.name().value()
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
            // runtime element now, so the two lists share their names; what
            // this still answers is whether the published manifest loads at all,
            // and what the compiler made of each appearance.
            UF_TRY_VALUE(runtime, engine::loadRuntimeProject(command.root));
            auto runtimeElements = std::vector<std::string>{};
            for (auto const& element : runtime.runtime.manifest().catalog().elements())
            {
                runtimeElements.emplace_back(runtimeElementJson(element));
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
                    .key   = "runtime_elements",
                    .value = jsonArray(runtimeElements),
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
                renameElement(
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
                mask,
                maskJson(session.assets, resolved.id, command.anchor)
            );
            UF_TRY_VALUE(
                drawn,
                drawJson(
                    document,
                    annotation::ElementId{anchorId},
                    resolved.ingested,
                    std::move(mask)
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
            auto const appearance = workbench::EditableAppearance{
                .name                  = std::string{workbench::k_defaultAppearanceName},
                .sourceId              = resolved.id,
                .templateRect          = command.draw.templateRect,
                .similarityBasisPoints = command.draw.threshold.basisPoints(),
                .colourKey             = command.draw.colourKey,
            };
            session.draft.elements.emplace_back(
                workbench::EditableElement{
                    .id           = elementId,
                    .name         = command.draw.name,
                    .capabilities = declaredCapabilitiesOf(command.capabilities),
                    .searchRoi    = searchRoi,
                    .appearances  = {appearance},
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
            UF_TRY_VALUE(
                mask,
                maskJson(session.assets, resolved.id, command.draw)
            );
            UF_TRY_VALUE(
                drawn,
                drawJson(
                    document,
                    elementId,
                    resolved.ingested,
                    std::move(mask)
                )
            );

            auto const members = std::array{
                JsonMember{.key = "page", .value = jsonString(command.page)},
                JsonMember{.key = "authored", .value = drawn},
            };
            return successJson("page add", members);
        }

        [[nodiscard]]
        auto runAddRegion(AddRegion const& command) -> Result<std::string>
        {
            UF_TRY_VALUE(session, openSession(command.root));
            UF_TRY_VALUE(pageId, findPageByName(session.draft, command.page));
            UF_TRY_VALUE(
                elementResourceId,
                derivedResourceId(
                    "element",
                    session.draft.projectId.value(),
                    command.name
                )
            );

            // No source is opened and no template is cut, which is the whole of
            // the difference from runAddElement above. The rectangle becomes the
            // ELEMENT's search region rather than a refinement on this page's
            // reference, and the choice is forced twice over. A reference's
            // region is optional and means "this page narrows the element's",
            // so it needs an element region to narrow; leaving that at the whole
            // screen would say this rectangle may be anywhere, and every page
            // referencing it without its own refinement would locate it at the
            // screen's centre. And there is no second page yet: `page add`
            // draws an element AND its owning page's use of it in one edit, so
            // what the author is describing here is the element.
            auto const elementId = annotation::ElementId{elementResourceId};
            session.draft.elements.emplace_back(
                workbench::EditableElement{
                    .id           = elementId,
                    .name         = command.name,
                    .capabilities = declaredCapabilitiesOf(command.capabilities),
                    .searchRoi    = command.region,
                    .appearances  = {},
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
            // The same two keys the drawing half answers with, because it is the
            // same verb: a caller reads `authored.element.appearances` and finds it
            // empty rather than reading a different document shape. Nothing was
            // ingested and there is no mask, and both say so rather than being
            // left out.
            UF_TRY_VALUE(
                drawn,
                drawJson(document, elementId, false, jsonNull())
            );

            auto const members = std::array{
                JsonMember{.key = "page", .value = jsonString(command.page)},
                JsonMember{.key = "authored", .value = drawn},
            };
            return successJson("page add", members);
        }

        // The element named, and what it declares it can do, kept apart from the
        // draft it was read out of: the draft is moved into the edit below, and
        // a borrow into it would outlive the owner it names.
        struct NamedElement final
        {
            annotation::ElementId           id;
            workbench::EditableCapabilities capabilities{};
        };

        [[nodiscard]]
        auto findElementByName(
            workbench::AuthoringDraft const& draft,
            std::string const& name
        ) -> Result<NamedElement>
        {
            auto const found = std::ranges::find(
                draft.elements,
                name,
                &workbench::EditableElement::name
            );
            if (found == draft.elements.end())
            {
                return invalid(
                    std::format(
                        "no element named \"{}\" is part of this project",
                        name
                    )
                );
            }
            return NamedElement{
                .id           = found->id,
                .capabilities = found->capabilities,
            };
        }

        // A second look at pixels the project already holds, appended to the
        // element's ordered appearance list.
        //
        // Written here rather than as an edit-layer verb for the reason
        // runAddElement gives: that layer draws ONE rectangle per element and
        // every verb in it is built on that, while this is the verb that makes
        // the number two. The rules it has to keep are the model's own --
        // distinct names, and a template that fits the region every appearance
        // of this element shares -- and both are enforced by
        // validateElementShape when the draft is rebuilt below. The name is
        // checked here as well, only so the refusal can name the element the
        // author typed instead of reporting that some element has a duplicate.
        [[nodiscard]]
        auto runAddAppearance(AddAppearance const& command) -> Result<std::string>
        {
            UF_TRY_VALUE(opened, openSession(command.root));
            UF_TRY_VALUE(
                resolved,
                resolveSource(std::move(opened), command.draw.source)
            );
            auto session = std::move(resolved.session);

            auto const found = std::ranges::find(
                session.draft.elements,
                command.element,
                &workbench::EditableElement::name
            );
            if (found == session.draft.elements.end())
            {
                return invalid(
                    std::format(
                        "no element named \"{}\" is part of this project",
                        command.element
                    )
                );
            }
            if (
                std::ranges::contains(
                    found->appearances,
                    command.draw.name,
                    &workbench::EditableAppearance::name
                )
            )
            {
                return invalid(
                    std::format(
                        "\"{}\" already has an appearance named \"{}\"; an "
                        "appearance name is how a page pins one and how a "
                        "script reads which matched, so two cannot share it",
                        command.element,
                        command.draw.name
                    )
                );
            }

            auto const elementId = found->id;
            found->appearances.emplace_back(
                workbench::EditableAppearance{
                    .name                  = command.draw.name,
                    .sourceId              = resolved.id,
                    .templateRect          = command.draw.templateRect,
                    .similarityBasisPoints = command.draw.threshold.basisPoints(),
                    .colourKey             = command.draw.colourKey,
                }
            );

            UF_TRY_VALUE(document, commitSession(command.root, session));
            UF_TRY_VALUE(
                mask,
                maskJson(session.assets, resolved.id, command.draw)
            );
            UF_TRY_VALUE(
                drawn,
                drawJson(
                    document,
                    elementId,
                    resolved.ingested,
                    std::move(mask)
                )
            );

            auto const members = std::array{
                JsonMember{
                    .key   = "appearance",
                    .value = jsonString(command.draw.name),
                },
                JsonMember{.key = "authored", .value = drawn},
            };
            return successJson("element appearance", members);
        }

        // One capability, as the element declares it and as a page asks for it.
        // Three rows rather than three branches, so the set a refusal prints and
        // the set it checks are the same list read twice.
        struct ExercisedUse final
        {
            std::string_view name{};

            bool requested{};
            bool declared{};
        };

        [[nodiscard]]
        auto exercisedUsesOf(
            workbench::EditableCapabilities const& declared,
            StatedCapabilities const& exercised
        ) -> std::array<ExercisedUse, 3>
        {
            return std::array{
                ExercisedUse{
                    .name      = "identify",
                    .requested = exercised.identify.has_value(),
                    .declared  = declared.identify.has_value(),
                },
                ExercisedUse{
                    .name      = "interact",
                    .requested = exercised.interact,
                    .declared  = declared.interact.has_value(),
                },
                ExercisedUse{
                    .name      = "read",
                    .requested = exercised.read,
                    .declared  = declared.read.has_value(),
                },
            };
        }

        [[nodiscard]]
        auto declaredCapabilityList(
            std::span<ExercisedUse const> uses
        ) -> std::string
        {
            auto listed = std::string{};
            for (auto const& use : uses)
            {
                if (!use.declared)
                {
                    continue;
                }
                if (!listed.empty())
                {
                    listed += ", ";
                }
                listed += use.name;
            }
            return listed;
        }

        // Two levels, one direction: the element declares what it can do, and a
        // page's reference declares what that page does with it. The edit layer
        // refuses the same row, but from a draft it can only name the element.
        // Naming the page, the use asked for, and what the element actually
        // declares is the difference between an author correcting the command
        // and an author guessing which half of it was wrong.
        [[nodiscard]]
        auto requireDeclaredCapabilities(
            std::string const& page,
            std::string const& element,
            workbench::EditableCapabilities const& declared,
            StatedCapabilities const& exercised
        ) -> Status
        {
            auto const uses = exercisedUsesOf(declared, exercised);
            for (auto const& use : uses)
            {
                if (!use.requested || use.declared)
                {
                    continue;
                }
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "page \"{}\" would exercise {} on \"{}\", which declares "
                        "{}; a page exercises only what the element declares",
                        page,
                        use.name,
                        element,
                        declaredCapabilityList(uses)
                    )
                );
            }
            return ok();
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

            // Absent means the page takes every use a placement carries on its
            // own, which is what the edit layer reads a missing set as. Stating
            // one is what makes identify reachable at all -- and the only place
            // its role can be typed.
            auto exercised = std::optional<workbench::EditableExercised>{};
            if (command.exercised)
            {
                UF_TRY(
                    requireDeclaredCapabilities(
                        command.page,
                        command.element,
                        element.capabilities,
                        *command.exercised
                    )
                );
                exercised = exercisedCapabilitiesOf(*command.exercised);
            }

            // --search-roi travels as the optional it was typed as. Seeding it
            // from the element would pin a copy of a rectangle a later
            // correction moves, and it would leave every reference refining a
            // region -- which is the one thing a reference that exercises
            // identify may not do. --appearance travels the same way, and for the
            // same reason: absent means "search every appearance", which is a
            // different instruction from "search the first one".
            UF_TRY_VALUE(
                referenced,
                workbench::referenceElementOnPage(
                    std::move(session.draft),
                    workbench::ReferenceElementSpec{
                        .elementId  = element.id,
                        .pageId     = pageId,
                        .exercised  = exercised,
                        .searchRoi  = command.searchRoi,
                        .appearance = command.appearance,
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
        auto runMatchElement(
            MatchElement const& command
        ) -> Result<std::string>
        {
            UF_TRY_VALUE(loaded, engine::loadRuntimeProject(command.root));
            auto const& catalog    = loaded.runtime.manifest().catalog();
            auto const elements = catalog.elements();

            auto const found = std::ranges::find_if(
                elements,
                [&command](annotation::CompiledElement const& element)
                {
                    return element.name().value() == command.element;
                }
            );
            if (found == elements.end())
            {
                return invalid(
                    std::format(
                        "no element named \"{}\" in this project; "
                        "project show lists every runtime element",
                        command.element
                    )
                );
            }

            UF_TRY_VALUE(frame, frameFromPng(command.frame, catalog.fingerprint()));
            UF_TRY_VALUE(
                outcome,
                matchElement(
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
                    .key   = "element",
                    .value = runtimeElementJson(*found),
                },
                // Which page the target was located on, and null for the anchor
                // pass, which runs before any page is known.
                JsonMember{.key = "page", .value = std::move(pageName)},
                JsonMember{.key = "hit", .value = jsonBoolean(evidence.hit())},
                // Which appearance produced this evidence, so "why did it match"
                // is answerable from the answer. Null only for an element that
                // declares none and is located by its page.
                JsonMember{
                    .key   = "appearance",
                    .value = evidence.appearanceName()
                        ? jsonString(evidence.appearanceName()->value())
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

        // The five enumerations the falsification matrix answers with. Each is
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

        // What the model states about a row's subject on a screen, named rather
        // than reduced to "is a hit expected".
        //
        // The three are not two. A bool collapses Absent and Unclaimed into one
        // false, and they are the opposite instruction to whoever reads the
        // matrix: under Absent a hit is a defect to repair, under Unclaimed a
        // hit is the same element genuinely being on an overlay screen its page
        // does not name, and there is nothing to do. The distinction is not
        // rare -- an element located by its page takes part in no signature, so
        // every screen its own pages do not claim answers Unclaimed for it, and
        // every one of those rows would otherwise read as a hit that was not
        // expected.
        [[nodiscard]]
        auto cellExpectationName(
            workbench::ModelCellExpectation expectation
        ) -> std::string_view
        {
            switch (expectation)
            {
            case workbench::ModelCellExpectation::Match:
                return "match";
            case workbench::ModelCellExpectation::Absent:
                return "absent";
            case workbench::ModelCellExpectation::Unclaimed:
                return "unclaimed";
            }
            UF_UNREACHABLE_MSG("unknown ModelCellExpectation value");
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
                    .key   = "expectation",
                    .value = jsonString(cellExpectationName(cell.expectation)),
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
                else if constexpr (std::same_as<Specific, AddRegion>)
                {
                    return runAddRegion(specific);
                }
                else if constexpr (std::same_as<Specific, AddAppearance>)
                {
                    return runAddAppearance(specific);
                }
                else if constexpr (std::same_as<Specific, ReferenceElement>)
                {
                    return runReferenceElement(specific);
                }
                else if constexpr (std::same_as<Specific, MatchElement>)
                {
                    return runMatchElement(specific);
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
