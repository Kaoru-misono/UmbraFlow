#include "command-runner.hpp"

#include "json-writer.hpp"

#include <authoring-edit.hpp>
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
        // the name, truncated to the sixteen bytes a ResourceId carries -- the
        // construction annotation::derivedRuntimeRecognizerId already uses.
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

        [[nodiscard]]
        auto annotationTypeName(
            annotation::AnnotationType type
        ) -> std::string_view
        {
            switch (type)
            {
            case annotation::AnnotationType::PageAnchor:
                return "page_anchor";
            case annotation::AnnotationType::ActionTarget:
                return "action_target";
            case annotation::AnnotationType::InfoRegion:
                return "info_region";
            }
            UF_UNREACHABLE_MSG("unknown AnnotationType value");
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
                    .key   = "type",
                    .value = jsonString(annotationTypeName(element.annotationType())),
                },
                JsonMember{
                    .key   = "source_id",
                    .value = jsonString(element.sourceId().value().toString()),
                },
                JsonMember{
                    .key   = "template_rect",
                    .value = rectJson(element.templateRect()),
                },
                JsonMember{
                    .key   = "search_roi",
                    .value = rectJson(element.searchRoi()),
                },
                JsonMember{
                    .key   = "min_similarity_bp",
                    .value = jsonUnsigned(element.threshold().basisPoints()),
                },
                JsonMember{
                    .key   = "colour_key",
                    .value = colourKeyJson(element.colourKey()),
                },
                JsonMember{.key = "shared", .value = jsonBoolean(element.shared())},
            };
            return jsonObject(members);
        }

        [[nodiscard]]
        auto elementNameJson(
            annotation::AuthoringDocument const& document,
            annotation::RecognizerId id
        ) -> std::string
        {
            auto const* p_element = document.findElement(id);
            return p_element == nullptr
                ? jsonString(id.value().toString())
                : jsonString(p_element->name().value());
        }

        [[nodiscard]]
        auto pageJson(
            annotation::AuthoringDocument const& document,
            annotation::PageSignature const& page
        ) -> std::string
        {
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

            auto placements = std::vector<std::string>{};
            for (auto const& placement : document.placements())
            {
                if (placement.pageId != page.id())
                {
                    continue;
                }
                auto const entry = std::array{
                    JsonMember{
                        .key   = "element",
                        .value = elementNameJson(document, placement.elementId),
                    },
                    JsonMember{
                        .key   = "search_roi",
                        .value = rectJson(placement.searchRoi),
                    },
                };
                placements.emplace_back(jsonObject(entry));
            }

            auto const members = std::array{
                JsonMember{
                    .key   = "id",
                    .value = jsonString(page.id().value().toString()),
                },
                JsonMember{.key = "name", .value = jsonString(page.name().value())},
                JsonMember{.key = "required", .value = jsonArray(required)},
                JsonMember{.key = "forbidden", .value = jsonArray(forbidden)},
                JsonMember{.key = "placements", .value = jsonArray(placements)},
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

        // The CLI names an element; addPageMember and createPageFromSource mint
        // their own placeholder names because their GUI caller has no name to
        // give yet. Renaming in the draft rather than teaching them a name keeps
        // the linkage they perform -- an anchor into a signature, a target into a
        // placement -- as the one implementation of it. An unusable name is
        // refused by ResourceName::create when the draft is built.
        [[nodiscard]]
        auto renameRecognizer(
            workbench::AuthoringDraft draft,
            annotation::RecognizerId id,
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
            annotation::RecognizerId id,
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
            uint64                     pixelComparisons{};
        };

        // A page anchor is only ever searched as part of resolving a page, so
        // that is how it is measured here; an action target has its own single
        // evaluation. An info region has neither -- evaluateActionTarget refuses
        // anything that is not an action target, and page resolution does not
        // look at info regions at all -- so it is refused with the reason.
        [[nodiscard]]
        auto matchRecognizer(
            annotation::RecognitionRuntime const& runtime,
            annotation::RecognizerDefinition const& recognizer,
            Frame const& frame,
            annotation::RecognitionPolicy const& policy
        ) -> Result<MatchOutcome>
        {
            auto const fingerprint = runtime.manifest().catalog().fingerprint();
            if (recognizer.annotationType() == annotation::AnnotationType::InfoRegion)
            {
                return invalid(
                    std::format(
                        "\"{}\" is an info_region, which the runtime evaluates "
                        "only as part of a task reading a page",
                        recognizer.name().value()
                    )
                );
            }

            if (recognizer.annotationType() == annotation::AnnotationType::ActionTarget)
            {
                UF_TRY_VALUE(
                    attempt,
                    runtime.evaluateActionTarget(
                        frame,
                        fingerprint,
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
                    .pixelComparisons = attempt.completedPixelComparisons,
                };
            }

            UF_TRY_VALUE(
                attempt,
                runtime.evaluatePage(frame, fingerprint, policy)
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

            // The names `match` accepts. An element placed on several pages
            // compiles into one runtime recognizer per page under a derived
            // name, so the authored element list above is not the list of things
            // that can be matched; this is.
            UF_TRY_VALUE(runtime, engine::loadRuntimeProject(command.root));
            auto recognizers = std::vector<std::string>{};
            for (auto const& recognizer : runtime.runtime.manifest().catalog().recognizers())
            {
                auto const entry = std::array{
                    JsonMember{
                        .key   = "id",
                        .value = jsonString(recognizer.id().value().toString()),
                    },
                    JsonMember{
                        .key   = "name",
                        .value = jsonString(recognizer.name().value()),
                    },
                    JsonMember{
                        .key   = "type",
                        .value = jsonString(
                            annotationTypeName(recognizer.annotationType())
                        ),
                    },
                    JsonMember{
                        .key   = "template_rect",
                        .value = rectJson(recognizer.templateRect()),
                    },
                    JsonMember{
                        .key   = "search_roi",
                        .value = rectJson(recognizer.searchRoi()),
                    },
                    JsonMember{
                        .key   = "min_similarity_bp",
                        .value = jsonUnsigned(recognizer.threshold().basisPoints()),
                    },
                };
                recognizers.emplace_back(jsonObject(entry));
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
                        .anchorId              = annotation::RecognizerId{anchorId},
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
                    annotation::RecognizerId{anchorId},
                    command.anchor.name
                )
            );
            UF_TRY_VALUE(
                keyed,
                workbench::setElementColourKey(
                    std::move(namedAnchor),
                    annotation::RecognizerId{anchorId},
                    command.anchor.colourKey
                )
            );

            session.draft = std::move(keyed);
            UF_TRY_VALUE(document, commitSession(command.root, session));
            UF_TRY_VALUE(
                drawn,
                drawJson(
                    document,
                    annotation::RecognizerId{anchorId},
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
                elementId,
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

            auto const recognizerId = annotation::RecognizerId{elementId};
            UF_TRY_VALUE(
                added,
                workbench::addPageMember(
                    std::move(session.draft),
                    workbench::PageMemberSpec{
                        .recognizerId          = recognizerId,
                        .pageId                = pageId,
                        .sourceId              = resolved.id,
                        .templateRect          = command.draw.templateRect,
                        .searchRoi             = searchRoi,
                        .similarityBasisPoints = command.draw.threshold.basisPoints(),
                        .kind                  = command.role == ElementRole::Anchor
                            ? workbench::PageMemberKind::Anchor
                            : workbench::PageMemberKind::ActionTarget,
                    }
                )
            );
            UF_TRY_VALUE(
                named,
                renameRecognizer(
                    std::move(added.draft),
                    recognizerId,
                    command.draw.name
                )
            );
            UF_TRY_VALUE(
                keyed,
                workbench::setElementColourKey(
                    std::move(named),
                    recognizerId,
                    command.draw.colourKey
                )
            );

            session.draft = std::move(keyed);
            UF_TRY_VALUE(document, commitSession(command.root, session));
            UF_TRY_VALUE(drawn, drawJson(document, recognizerId, resolved.ingested));

            auto const members = std::array{
                JsonMember{.key = "page", .value = jsonString(command.page)},
                JsonMember{.key = "authored", .value = drawn},
            };
            return successJson(
                command.role == ElementRole::Anchor
                    ? "page add-anchor"
                    : "page add-target",
                members
            );
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
                    }
                )
            );

            auto const& evidence = outcome.evidence;
            auto const recognizerMembers = std::array{
                JsonMember{
                    .key   = "id",
                    .value = jsonString(found->id().value().toString()),
                },
                JsonMember{
                    .key   = "name",
                    .value = jsonString(found->name().value()),
                },
                JsonMember{
                    .key   = "type",
                    .value = jsonString(annotationTypeName(found->annotationType())),
                },
                JsonMember{
                    .key   = "template_rect",
                    .value = rectJson(found->templateRect()),
                },
                JsonMember{
                    .key   = "search_roi",
                    .value = rectJson(found->searchRoi()),
                },
                JsonMember{
                    .key   = "min_similarity_bp",
                    .value = jsonUnsigned(found->threshold().basisPoints()),
                },
            };

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
                    .value = jsonObject(recognizerMembers),
                },
                JsonMember{.key = "hit", .value = jsonBoolean(evidence.hit())},
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
                else if constexpr (std::same_as<Specific, MatchRecognizer>)
                {
                    return runMatchRecognizer(specific);
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
