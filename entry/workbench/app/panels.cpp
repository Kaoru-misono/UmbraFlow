#include "panels.hpp"

#include "canvas-math.hpp"
#include "workbench-app.hpp"

#include "preview.hpp"
#include "project-persistence.hpp"
#include "source-ingestion.hpp"

#include <annotation/authoring-document.hpp>
#include <annotation/catalog.hpp>
#include <annotation/recognition-runtime.hpp>

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        constexpr auto k_templateColor = IM_COL32(96, 220, 120, 255);
        constexpr auto k_searchRoiColor = IM_COL32(96, 168, 255, 255);
        constexpr auto k_gripColor      = IM_COL32(240, 240, 240, 255);
        constexpr auto k_gripRadius     = 5.0F;
        constexpr auto k_zoomWheelBase  = 1.1F;
        constexpr auto k_thresholdMax   = 10'000;

        // The Preview action runs on the GUI thread, so it is bounded twice
        // over. The comparison budget matches the 256 Mi-pixel order of
        // magnitude the authoring compiler bounds its own work with
        // (k_maximumCompilationPixelWork in authoring-compiler.cpp), and the
        // deadline caps the wall-clock time a large source may spend before the
        // next frame. Hitting either limit surfaces as a PreviewStop row.
        constexpr auto k_previewComparisonBudget = uint64{256} * 1024U * 1024U;
        constexpr auto k_previewDeadline         = std::chrono::seconds{2};

        constexpr auto k_annotationTypeItems = std::array<char const*, 3>{
            "PageAnchor",
            "ActionTarget",
            "InfoRegion",
        };
        constexpr auto k_classificationItems = std::array<char const*, 3>{
            "Positive",
            "Negative",
            "Confusable",
        };
        constexpr auto k_pageRoleItems = std::array<char const*, 3>{
            "None",
            "Required",
            "Forbidden",
        };

        [[nodiscard]]
        auto shortId(annotation::ResourceId const& id) -> std::string
        {
            return id.toString().substr(0, 8);
        }

        auto seedBuffer(
            std::array<char, 256>& buffer,
            std::string const& text
        ) -> void
        {
            auto const count = std::min(text.size(), buffer.size() - 1U);
            std::copy_n(text.begin(), count, buffer.begin());
            buffer.at(count) = '\0';
        }

        [[nodiscard]]
        auto findEditableRecognizer(
            AuthoringDraft& draft,
            annotation::RecognizerId id
        ) -> EditableRecognizer*
        {
            auto const found = std::ranges::find(
                draft.m_recognizers,
                id,
                &EditableRecognizer::m_id
            );
            if (found == draft.m_recognizers.end())
            {
                return nullptr;
            }
            return &*found;
        }

        [[nodiscard]]
        auto pageName(AppState const& state, annotation::PageId id) -> std::string
        {
            auto const* page = state.document().catalog().findPage(id);
            if (page != nullptr)
            {
                return page->name().value();
            }
            return shortId(id.value());
        }

        // Names the type change and everything the conversion had to repair with
        // it, so an authorization the author did not ask for and a field the
        // conversion could not keep are both stated rather than discovered later.
        [[nodiscard]]
        auto retypeSummary(
            AppState const& state,
            RetypedRecognizer const& retyped,
            char const* typeName
        ) -> std::string
        {
            auto summary = std::format("type set to {}", typeName);
            if (auto const page = retyped.m_authorizedPage)
            {
                summary += std::format(
                    "; authorized page \"{}\"",
                    pageName(state, *page)
                );
            }
            if (retyped.m_withdrawnRoles > 0U)
            {
                summary += std::format(
                    "; withdrew from {} page {}",
                    retyped.m_withdrawnRoles,
                    retyped.m_withdrawnRoles == 1U ? "signature" : "signatures"
                );
            }
            if (retyped.m_clearedAuthorizations > 0U)
            {
                summary += std::format(
                    "; cleared {} page {}",
                    retyped.m_clearedAuthorizations,
                    retyped.m_clearedAuthorizations == 1U
                        ? "authorization"
                        : "authorizations"
                );
            }
            if (retyped.m_clearedClick)
            {
                summary += "; cleared the default click";
            }
            return summary;
        }

        // Queues an edited draft for the end of the frame instead of committing it
        // where the widget was handled; see PendingEdit for why a mid-draw commit
        // is unsafe. Every request describes its edit, because the description
        // becomes the status line and the status line is what the operation log
        // records; an edit that left no trace there is one nobody can reconstruct
        // afterwards. A frame carries one request: a second would have been built
        // against the same document as the first and would silently drop it, and
        // only a widget deactivating in the same frame as another's click can
        // produce one, so the first request wins and the second click is retried
        // by the user on the next frame.
        auto requestEdit(
            PanelUiState& ui,
            AuthoringDraft draft,
            std::string description
        ) -> void
        {
            if (ui.m_pendingEdit.has_value())
            {
                return;
            }
            ui.m_pendingEdit = PendingEdit{
                .m_draft       = std::move(draft),
                .m_description = std::move(description),
            };
        }

        // Commits the frame's queued edit, if any, and states the outcome on the
        // status line: the requester's description when the document changed, the
        // build's rejection when it was refused. An edit that changes nothing
        // leaves the line alone.
        auto applyPendingEdit(AppState& state, PanelUiState& ui) -> void
        {
            if (!ui.m_pendingEdit.has_value())
            {
                return;
            }
            auto const request = *std::exchange(ui.m_pendingEdit, std::nullopt);

            auto const applied = state.applyEdit(request.m_draft);
            if (!applied)
            {
                ui.m_statusLine = std::format(
                    "edit rejected: {}",
                    toString(applied.error())
                );
                return;
            }
            if (*applied)
            {
                ui.m_statusLine = request.m_description;
            }
        }

        // Every recognizer and page name in a draft. The catalog requires names to
        // be unique across both kinds, so a fresh name has to be checked against
        // all of them, not just its own kind.
        [[nodiscard]]
        auto takenNames(AuthoringDraft const& draft) -> std::vector<std::string>
        {
            auto names = std::vector<std::string>{};
            names.reserve(draft.m_recognizers.size() + draft.m_pages.size());
            for (auto const& recognizer : draft.m_recognizers)
            {
                names.emplace_back(recognizer.m_name);
            }
            for (auto const& page : draft.m_pages)
            {
                names.emplace_back(page.m_name);
            }
            return names;
        }

        // A fresh entity takes the first free "<stem>_N" instead of a fixed name
        // that the catalog refuses the second time the button is pressed. One of
        // the first size() + 1 candidates is always free, so the search ends.
        [[nodiscard]]
        auto freshName(
            std::vector<std::string> const& taken,
            std::string_view stem
        ) -> std::string
        {
            auto candidate = std::string{};
            for (auto index = std::size_t{1}; index <= taken.size() + 1U; ++index)
            {
                candidate = std::format("{}_{}", stem, index);
                if (!std::ranges::contains(taken, candidate))
                {
                    break;
                }
            }
            return candidate;
        }

        // Names a deletion and the neighbours it had to edit, so a withdrawal or
        // a discarded regression case is stated rather than discovered later.
        [[nodiscard]]
        auto deletionSummary(
            std::string_view what,
            DeletedEntity const& deleted
        ) -> std::string
        {
            auto summary = std::format("deleted {}", what);
            if (deleted.m_withdrawnRoles > 0U)
            {
                summary += std::format(
                    "; withdrew it from {} page {}",
                    deleted.m_withdrawnRoles,
                    deleted.m_withdrawnRoles == 1U ? "signature" : "signatures"
                );
            }
            if (deleted.m_clearedAuthorizations > 0U)
            {
                summary += std::format(
                    "; cleared it from {} recognizer {}",
                    deleted.m_clearedAuthorizations,
                    deleted.m_clearedAuthorizations == 1U
                        ? "authorization"
                        : "authorizations"
                );
            }
            if (deleted.m_removedRegressions > 0U)
            {
                summary += std::format(
                    "; removed {} regression {}",
                    deleted.m_removedRegressions,
                    deleted.m_removedRegressions == 1U ? "case" : "cases"
                );
            }
            return summary;
        }

        // Queues a deletion, reporting a refusal immediately. Deletions are
        // refused rather than cascaded when the cascade would reach something
        // only the author can decide about.
        auto requestDeletion(
            PanelUiState& ui,
            Result<DeletedEntity> deleted,
            std::string_view what
        ) -> void
        {
            if (!deleted)
            {
                ui.m_statusLine = std::format(
                    "delete rejected: {}",
                    toString(deleted.error())
                );
                return;
            }
            auto description = deletionSummary(what, *deleted);
            requestEdit(ui, std::move(deleted->m_draft), std::move(description));
        }

        // The source a recognizer was authored against, so a selection can follow
        // the recognizer to the image its rectangles are meaningful on.
        [[nodiscard]]
        auto sourceOfRecognizer(
            AppState const& state,
            annotation::RecognizerId id
        ) -> std::optional<annotation::SourceId>
        {
            for (auto const& relationship : state.document().recognizerSources())
            {
                if (relationship.m_recognizerId == id)
                {
                    return relationship.m_sourceId;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]]
        auto addDefaultRecognizer(
            AppState& state
        ) -> Result<annotation::RecognizerId>
        {
            auto const source = state.selectedSourceId();
            if (!source.has_value())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "adding a recognizer requires a selected source"
                );
            }

            auto edited        = state.draft();
            auto const width   = edited.m_fingerprint.width();
            auto const height  = edited.m_fingerprint.height();
            UF_TRY_VALUE(
                templateRect,
                PixelRect::create(
                    0U,
                    0U,
                    std::min<uint32>(16U, width),
                    std::min<uint32>(16U, height)
                )
            );
            UF_TRY_VALUE(searchRoi, PixelRect::create(0U, 0U, width, height));

            auto const id = annotation::RecognizerId{mintResourceId()};
            edited.m_recognizers.emplace_back(
                EditableRecognizer{
                    .m_id             = id,
                    .m_name           = freshName(takenNames(edited), "recognizer"),
                    .m_annotationType = annotation::AnnotationType::PageAnchor,
                    .m_sourceId       = *source,
                    .m_templateRect   = templateRect,
                    .m_searchRoi      = searchRoi,
                    .m_similarityBasisPoints = 9'000U,
                    .m_defaultClick   = {},
                    .m_allowedPageIds = {},
                }
            );
            UF_TRY_VALUE(changed, state.applyEdit(edited));
            static_cast<void>(changed);
            return id;
        }

        [[nodiscard]]
        auto addDefaultPage(AppState& state) -> Result<annotation::PageId>
        {
            // A page is rejected unless it names at least one required or
            // forbidden recognizer, and only a page anchor may fill either role,
            // so a new page is seeded with the selected page anchor as its
            // required recognizer. Further membership is then adjusted from the
            // properties panel. An empty page can never be committed.
            auto const selected = state.selectedRecognizerId();
            if (!selected.has_value())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "adding a page requires a selected page anchor recognizer"
                );
            }

            auto edited        = state.draft();
            auto const* anchor = findEditableRecognizer(edited, *selected);
            if (
                anchor == nullptr
                || anchor->m_annotationType != annotation::AnnotationType::PageAnchor
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "the selected recognizer is not a page anchor; "
                    "a page must be anchored by one"
                );
            }

            auto const id = annotation::PageId{mintResourceId()};
            edited.m_pages.emplace_back(
                EditablePage{
                    .m_id        = id,
                    .m_name      = freshName(takenNames(edited), "page"),
                    .m_required  = {*selected},
                    .m_forbidden = {},
                }
            );
            UF_TRY_VALUE(changed, state.applyEdit(edited));
            static_cast<void>(changed);
            return id;
        }

        [[nodiscard]]
        auto rectScreenOrigin(
            CanvasView view,
            CanvasPoint canvasOrigin,
            PixelRect const& rect
        ) -> CanvasPoint
        {
            return sourceToScreen(
                view,
                canvasOrigin,
                static_cast<float>(rect.x()),
                static_cast<float>(rect.y())
            );
        }

        auto drawRectWithGrips(
            ImDrawList& drawList,
            CanvasView view,
            CanvasPoint canvasOrigin,
            PixelRect const& rect,
            ImU32 color
        ) -> void
        {
            auto const origin = rectScreenOrigin(view, canvasOrigin, rect);
            auto const width  = static_cast<float>(rect.width()) * view.m_zoom;
            auto const height = static_cast<float>(rect.height()) * view.m_zoom;

            drawList.AddRect(
                ImVec2{origin.m_x, origin.m_y},
                ImVec2{origin.m_x + width, origin.m_y + height},
                color,
                0.0F,
                0,
                2.0F
            );

            auto const centers = std::array<CanvasPoint, 8>{
                CanvasPoint{origin.m_x, origin.m_y},
                CanvasPoint{origin.m_x + width / 2.0F, origin.m_y},
                CanvasPoint{origin.m_x + width, origin.m_y},
                CanvasPoint{origin.m_x + width, origin.m_y + height / 2.0F},
                CanvasPoint{origin.m_x + width, origin.m_y + height},
                CanvasPoint{origin.m_x + width / 2.0F, origin.m_y + height},
                CanvasPoint{origin.m_x, origin.m_y + height},
                CanvasPoint{origin.m_x, origin.m_y + height / 2.0F},
            };
            for (auto const& center : centers)
            {
                drawList.AddRectFilled(
                    ImVec2{center.m_x - k_gripRadius, center.m_y - k_gripRadius},
                    ImVec2{center.m_x + k_gripRadius, center.m_y + k_gripRadius},
                    k_gripColor
                );
            }
        }

        [[nodiscard]]
        auto previewPageKindName(PreviewPageKind kind) -> char const*
        {
            switch (kind)
            {
            case PreviewPageKind::Resolved:
                return "Resolved";
            case PreviewPageKind::Unknown:
                return "Unknown";
            case PreviewPageKind::Ambiguous:
                return "Ambiguous";
            }
            return "?";
        }

        auto drawSourcesPanel(
            AppState& state,
            WorkbenchServices const& services,
            PanelUiState& ui
        ) -> void
        {
            if (!ImGui::Begin("Sources"))
            {
                ImGui::End();
                return;
            }

            for (auto const& source : state.document().sources())
            {
                auto const id       = source.id();
                auto const selected = state.selectedSourceId() == id;

                // Scope the row by the stable SourceId. The relative path is
                // derived from the content hash, so two sources with identical
                // bytes would collide on it.
                auto const idText = id.value().toString();
                ImGui::PushID(idText.c_str());
                auto const provenance = std::holds_alternative<
                    annotation::WgcSourceProvenance
                >(source.provenance()) ? "wgc" : "imported";
                auto const row = std::format(
                    "{}  {}  {}",
                    shortId(id.value()),
                    source.contentHash().hex().substr(0, 8),
                    provenance
                );
                if (ImGui::Selectable(row.c_str(), selected))
                {
                    state.setSelectedSourceId(id);
                }
                ImGui::PopID();
            }

            ImGui::Separator();

            ImGui::InputText(
                "Target title",
                ui.m_targetTitle.data(),
                ui.m_targetTitle.size()
            );
            auto const hasTarget = ui.m_targetTitle.at(0) != '\0';

            ImGui::BeginDisabled(!hasTarget);
            if (ImGui::Button("Capture"))
            {
                auto const id = annotation::SourceId{mintResourceId()};
                auto const title = std::string{ui.m_targetTitle.data()};
                auto ingested = services.m_captureFromTarget(id, title);
                if (!ingested)
                {
                    ui.m_statusLine = std::format(
                        "capture failed: {}",
                        toString(ingested.error())
                    );
                }
                else
                {
                    auto const added = state.addIngestedSource(
                        std::move(*ingested)
                    );
                    ui.m_statusLine = added.has_value()
                        ? std::string{"captured source"}
                        : std::format("capture add failed: {}", toString(added.error()));
                }
            }
            ImGui::EndDisabled();
            if (
                !hasTarget
                && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)
            )
            {
                ImGui::SetTooltip(
                    "Enter a target window title substring to enable capture"
                );
            }

            auto const selectedSource = state.selectedSourceId();
            ImGui::SameLine();
            ImGui::BeginDisabled(!selectedSource.has_value());
            if (ImGui::Button("Delete Source") && selectedSource.has_value())
            {
                requestDeletion(
                    ui,
                    deleteSource(state.draft(), *selectedSource),
                    "source"
                );
            }
            ImGui::EndDisabled();

            if (ImGui::Button("Import PNG..."))
            {
                auto picked = services.m_pickPngToImport();
                if (!picked)
                {
                    ui.m_statusLine = std::format(
                        "import dialog failed: {}",
                        toString(picked.error())
                    );
                }
                else if (picked->has_value())
                {
                    auto const id = annotation::SourceId{mintResourceId()};
                    auto ingested = importSourcePng(id, **picked);
                    if (!ingested)
                    {
                        ui.m_statusLine = std::format(
                            "import failed: {}",
                            toString(ingested.error())
                        );
                    }
                    else
                    {
                        auto const added = state.addIngestedSource(
                            std::move(*ingested)
                        );
                        ui.m_statusLine = added.has_value()
                            ? std::string{"imported source"}
                            : std::format(
                                "import add failed: {}",
                                toString(added.error())
                            );
                    }
                }
            }

            ImGui::End();
        }

        // The catalog's recognizers, so any of them can be selected again. Without
        // this list a recognizer is reachable only in the moment it is created:
        // the properties panel edits whatever is selected, and nothing else moves
        // the selection.
        auto drawRecognizersPanel(AppState& state, PanelUiState& ui) -> void
        {
            if (!ImGui::Begin("Recognizers"))
            {
                ImGui::End();
                return;
            }

            for (auto const& recognizer : state.document().catalog().recognizers())
            {
                auto const id       = recognizer.id();
                auto const selected = state.selectedRecognizerId() == id;

                // Scope the row by the stable id: names are unique today, but the
                // row must keep its identity while a rename is in flight.
                auto const idText = id.value().toString();
                ImGui::PushID(idText.c_str());
                auto const row = std::format(
                    "{}  [{}]",
                    recognizer.name().value(),
                    k_annotationTypeItems.at(
                        static_cast<std::size_t>(
                            std::to_underlying(recognizer.annotationType())
                        )
                    )
                );
                if (ImGui::Selectable(row.c_str(), selected))
                {
                    state.setSelectedRecognizerId(id);
                    // The canvas draws the selected recognizer's rectangles over
                    // the selected source, so follow the recognizer to the source
                    // it was authored against; otherwise its rectangles would be
                    // drawn over an unrelated image.
                    if (auto const source = sourceOfRecognizer(state, id))
                    {
                        state.setSelectedSourceId(*source);
                    }
                }
                ImGui::PopID();
            }

            if (state.document().catalog().recognizers().empty())
            {
                ImGui::TextUnformatted(
                    "No recognizers yet. Capture a source, then New Recognizer."
                );
            }

            auto const selected = state.selectedRecognizerId();
            ImGui::BeginDisabled(!selected.has_value());
            if (ImGui::Button("Delete Recognizer") && selected.has_value())
            {
                requestDeletion(
                    ui,
                    deleteRecognizer(state.draft(), *selected),
                    "recognizer"
                );
            }
            ImGui::EndDisabled();

            ImGui::End();
        }

        // The catalog's pages. Page membership is edited from the properties
        // panel, but a page is otherwise invisible unless some recognizer happens
        // to be selected, and there was no way to remove one at all.
        auto drawPagesPanel(AppState& state, PanelUiState& ui) -> void
        {
            if (!ImGui::Begin("Pages"))
            {
                ImGui::End();
                return;
            }

            for (auto const& page : state.document().catalog().pages())
            {
                auto const idText = page.id().value().toString();
                ImGui::PushID(idText.c_str());
                ImGui::Text(
                    "%s  %zu required  %zu forbidden",
                    page.name().value().c_str(),
                    page.required().size(),
                    page.forbidden().size()
                );
                ImGui::SameLine();
                if (ImGui::SmallButton("Delete"))
                {
                    requestDeletion(
                        ui,
                        deletePage(state.draft(), page.id()),
                        std::format("page \"{}\"", page.name().value())
                    );
                }
                ImGui::PopID();
            }

            if (state.document().catalog().pages().empty())
            {
                ImGui::TextUnformatted(
                    "No pages yet. Select a page anchor, then New Page."
                );
            }

            ImGui::End();
        }

        auto editSelectedRectOnRelease(
            AppState& state,
            PanelUiState& ui,
            annotation::RecognizerId recognizerId,
            PixelRect const& editedRect
        ) -> void
        {
            // Committing only on release keeps a whole drag gesture to a single
            // undo entry instead of one per moved pixel.
            auto draft       = state.draft();
            auto* recognizer = findEditableRecognizer(draft, recognizerId);
            if (recognizer != nullptr)
            {
                auto const isTemplate = (
                    ui.m_dragTarget == CanvasDragTarget::TemplateRect
                );
                if (isTemplate)
                {
                    recognizer->m_templateRect = editedRect;
                }
                else
                {
                    recognizer->m_searchRoi = editedRect;
                }
                requestEdit(
                    ui,
                    std::move(draft),
                    std::format(
                        "{} set to {},{} {}x{}",
                        isTemplate ? "template rect" : "search roi",
                        editedRect.x(),
                        editedRect.y(),
                        editedRect.width(),
                        editedRect.height()
                    )
                );
            }
            ui.m_dragTarget = CanvasDragTarget::None;
            ui.m_dragGrip.reset();
            ui.m_dragStartRect.reset();
        }

        auto handleRectEditing(
            AppState& state,
            PanelUiState& ui,
            ImDrawList& drawList,
            CanvasView view,
            CanvasPoint canvasOrigin,
            annotation::RecognizerDefinition const& definition,
            uint32 sourceWidth,
            uint32 sourceHeight,
            bool hovered
        ) -> void
        {
            auto const recognizerId = definition.id();
            auto const templateRect = definition.templateRect();
            auto const searchRoi    = definition.searchRoi();
            auto const mouse        = ImGui::GetIO().MousePos;

            if (
                ui.m_dragTarget == CanvasDragTarget::None
                && hovered
                && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            )
            {
                auto const templateOrigin = rectScreenOrigin(
                    view,
                    canvasOrigin,
                    templateRect
                );
                auto const templateGrip = hitTestGrip(
                    templateOrigin,
                    static_cast<float>(templateRect.width()) * view.m_zoom,
                    static_cast<float>(templateRect.height()) * view.m_zoom,
                    CanvasPoint{mouse.x, mouse.y},
                    k_gripRadius
                );
                if (templateGrip.has_value())
                {
                    ui.m_dragTarget    = CanvasDragTarget::TemplateRect;
                    ui.m_dragGrip      = templateGrip;
                    ui.m_dragStartRect = templateRect;
                }
                else
                {
                    auto const roiOrigin = rectScreenOrigin(
                        view,
                        canvasOrigin,
                        searchRoi
                    );
                    auto const roiGrip = hitTestGrip(
                        roiOrigin,
                        static_cast<float>(searchRoi.width()) * view.m_zoom,
                        static_cast<float>(searchRoi.height()) * view.m_zoom,
                        CanvasPoint{mouse.x, mouse.y},
                        k_gripRadius
                    );
                    if (roiGrip.has_value())
                    {
                        ui.m_dragTarget    = CanvasDragTarget::SearchRoi;
                        ui.m_dragGrip      = roiGrip;
                        ui.m_dragStartRect = searchRoi;
                    }
                }
            }

            auto previewTemplate = templateRect;
            auto previewRoi      = searchRoi;
            if (
                ui.m_dragTarget != CanvasDragTarget::None
                && ui.m_dragGrip.has_value()
                && ui.m_dragStartRect.has_value()
            )
            {
                auto const delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                auto const deltaX = static_cast<int32>(
                    std::lround(delta.x / view.m_zoom)
                );
                auto const deltaY = static_cast<int32>(
                    std::lround(delta.y / view.m_zoom)
                );
                auto const edited = resizeRectByGrip(
                    *ui.m_dragStartRect,
                    *ui.m_dragGrip,
                    deltaX,
                    deltaY,
                    sourceWidth,
                    sourceHeight
                );
                if (edited.has_value())
                {
                    if (ui.m_dragTarget == CanvasDragTarget::TemplateRect)
                    {
                        previewTemplate = *edited;
                    }
                    else
                    {
                        previewRoi = *edited;
                    }

                    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                    {
                        editSelectedRectOnRelease(
                            state,
                            ui,
                            recognizerId,
                            *edited
                        );
                    }
                }
                else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                {
                    ui.m_dragTarget = CanvasDragTarget::None;
                    ui.m_dragGrip.reset();
                    ui.m_dragStartRect.reset();
                }
            }

            drawRectWithGrips(
                drawList,
                view,
                canvasOrigin,
                previewRoi,
                k_searchRoiColor
            );
            drawRectWithGrips(
                drawList,
                view,
                canvasOrigin,
                previewTemplate,
                k_templateColor
            );
        }

        auto drawCanvasPanel(
            AppState& state,
            WorkbenchServices const& services,
            PanelUiState& ui
        ) -> void
        {
            if (!ImGui::Begin("Canvas"))
            {
                ImGui::End();
                return;
            }

            auto const selectedSource = state.selectedSourceId();
            if (!selectedSource.has_value())
            {
                ImGui::TextUnformatted("Select a source to view it.");
                ImGui::End();
                return;
            }

            auto const assets = state.sources();
            auto const asset  = std::ranges::find(
                assets,
                *selectedSource,
                &annotation::AuthoringSourceAsset::m_id
            );
            if (asset == assets.end())
            {
                ImGui::TextUnformatted("Selected source has no image bytes.");
                ImGui::End();
                return;
            }

            auto const texture = services.m_textureFor(*asset);
            if (!texture)
            {
                ImGui::TextUnformatted("Failed to decode the source image.");
                ImGui::End();
                return;
            }

            auto const cursor = ImGui::GetCursorScreenPos();
            auto const region = ImGui::GetContentRegionAvail();
            auto const size   = ImVec2{
                std::max(region.x, 64.0F),
                std::max(region.y, 64.0F),
            };
            ImGui::InvisibleButton(
                "canvas-surface",
                size,
                ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle
            );
            auto const hovered      = ImGui::IsItemHovered();
            auto const canvasOrigin = CanvasPoint{cursor.x, cursor.y};

            auto view = state.canvasView();

            auto const wheel = ImGui::GetIO().MouseWheel;
            if (hovered && wheel != 0.0F)
            {
                auto const mouse = ImGui::GetIO().MousePos;
                auto const under = screenToSource(
                    view,
                    canvasOrigin,
                    mouse.x,
                    mouse.y
                );
                view = zoomCanvasAroundSourcePoint(
                    view,
                    under.m_x,
                    under.m_y,
                    view.m_zoom * std::pow(k_zoomWheelBase, wheel)
                );
                state.setCanvasView(view);
            }

            if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
            {
                auto const delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
                view = panCanvas(view, delta.x, delta.y);
                state.setCanvasView(view);
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
            }

            auto* drawList = ImGui::GetWindowDrawList();
            drawList->PushClipRect(
                ImVec2{canvasOrigin.m_x, canvasOrigin.m_y},
                ImVec2{canvasOrigin.m_x + size.x, canvasOrigin.m_y + size.y},
                true
            );

            auto const topLeft = sourceToScreen(view, canvasOrigin, 0.0F, 0.0F);
            auto const bottomRight = sourceToScreen(
                view,
                canvasOrigin,
                static_cast<float>(texture->m_width),
                static_cast<float>(texture->m_height)
            );
            drawList->AddImage(
                static_cast<ImTextureID>(texture->m_textureHandle),
                ImVec2{topLeft.m_x, topLeft.m_y},
                ImVec2{bottomRight.m_x, bottomRight.m_y}
            );

            if (auto const recognizerId = state.selectedRecognizerId())
            {
                auto const* definition = state.document().catalog().findRecognizer(
                    *recognizerId
                );
                if (definition != nullptr)
                {
                    handleRectEditing(
                        state,
                        ui,
                        *drawList,
                        view,
                        canvasOrigin,
                        *definition,
                        texture->m_width,
                        texture->m_height,
                        hovered
                    );
                }
            }

            drawList->PopClipRect();
            ImGui::End();
        }

        auto drawPageMembership(
            AppState& state,
            PanelUiState& ui,
            annotation::RecognizerDefinition const& definition
        ) -> void
        {
            auto const recognizerId = definition.id();
            // The two columns are mutually exclusive by the catalog's rules: a
            // page anchor belongs to a page by holding a role in its signature
            // and may authorize none, while every other type authorizes pages
            // directly and may hold no role. Offering both for either type only
            // produces a rejected edit, so each is disabled where it cannot
            // apply.
            auto const isAnchor = (
                definition.annotationType() == annotation::AnnotationType::PageAnchor
            );
            ImGui::SeparatorText("Pages");
            for (auto const& page : state.document().catalog().pages())
            {
                auto const pageId = page.id();
                ImGui::PushID(page.name().value().c_str());

                auto member = std::ranges::contains(
                    definition.allowedPageIds(),
                    pageId
                );
                ImGui::BeginDisabled(isAnchor);
                if (ImGui::Checkbox(page.name().value().c_str(), &member))
                {
                    auto draft       = state.draft();
                    auto* recognizer = findEditableRecognizer(draft, recognizerId);
                    if (recognizer != nullptr)
                    {
                        std::erase(recognizer->m_allowedPageIds, pageId);
                        if (member)
                        {
                            recognizer->m_allowedPageIds.emplace_back(pageId);
                        }
                        requestEdit(
                            ui,
                            std::move(draft),
                            std::format(
                                "{} page \"{}\"",
                                member ? "authorized" : "withdrew authorization on",
                                page.name().value()
                            )
                        );
                    }
                }
                ImGui::EndDisabled();
                if (
                    isAnchor
                    && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)
                )
                {
                    ImGui::SetTooltip(
                        "A page anchor joins a page through the role beside it, "
                        "not through an authorization"
                    );
                }

                auto role = 0;
                if (std::ranges::contains(page.required(), recognizerId))
                {
                    role = 1;
                }
                else if (std::ranges::contains(page.forbidden(), recognizerId))
                {
                    role = 2;
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120.0F);
                ImGui::BeginDisabled(!isAnchor);
                auto const roleChanged = ImGui::Combo(
                    "role",
                    &role,
                    k_pageRoleItems.data(),
                    static_cast<int>(k_pageRoleItems.size())
                );
                ImGui::EndDisabled();
                if (
                    !isAnchor
                    && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)
                )
                {
                    ImGui::SetTooltip(
                        "Only a page anchor may be a page's required or "
                        "forbidden recognizer"
                    );
                }
                if (roleChanged)
                {
                    auto draft      = state.draft();
                    auto const spot = std::ranges::find(
                        draft.m_pages,
                        pageId,
                        &EditablePage::m_id
                    );
                    if (spot != draft.m_pages.end())
                    {
                        std::erase(spot->m_required, recognizerId);
                        std::erase(spot->m_forbidden, recognizerId);
                        if (role == 1)
                        {
                            spot->m_required.emplace_back(recognizerId);
                        }
                        else if (role == 2)
                        {
                            spot->m_forbidden.emplace_back(recognizerId);
                        }
                        requestEdit(
                            ui,
                            std::move(draft),
                            std::format(
                                "page \"{}\" role {}",
                                page.name().value(),
                                k_pageRoleItems.at(static_cast<std::size_t>(role))
                            )
                        );
                    }
                }

                ImGui::PopID();
            }
        }

        auto drawRegressionClassification(
            AppState& state,
            PanelUiState& ui
        ) -> void
        {
            ImGui::SeparatorText("Regression");
            auto const selectedSource = state.selectedSourceId();
            auto const cases          = state.document().regressions();
            auto const existing       = selectedSource.has_value()
                ? std::ranges::find(
                    cases,
                    *selectedSource,
                    &annotation::RegressionCase::sourceId
                )
                : cases.end();

            if (existing == cases.end())
            {
                ImGui::TextUnformatted(
                    "No regression case for the selected source."
                );
                return;
            }

            auto classification = static_cast<int>(
                std::to_underlying(existing->classification())
            );
            if (
                ImGui::Combo(
                    "Classification",
                    &classification,
                    k_classificationItems.data(),
                    static_cast<int>(k_classificationItems.size())
                )
            )
            {
                auto draft      = state.draft();
                auto const spot = std::ranges::find(
                    draft.m_regressions,
                    existing->id(),
                    &EditableRegression::m_id
                );
                if (spot != draft.m_regressions.end())
                {
                    spot->m_classification =
                        static_cast<annotation::RegressionClassification>(
                            classification
                        );
                    requestEdit(
                        ui,
                        std::move(draft),
                        std::format(
                            "classification {}",
                            k_classificationItems.at(
                                static_cast<std::size_t>(classification)
                            )
                        )
                    );
                }
            }
        }

        auto drawPropertiesPanel(AppState& state, PanelUiState& ui) -> void
        {
            if (!ImGui::Begin("Properties"))
            {
                ImGui::End();
                return;
            }

            auto const recognizerId = state.selectedRecognizerId();
            if (!recognizerId.has_value())
            {
                ImGui::TextUnformatted("Select a recognizer to edit it.");
                ImGui::End();
                return;
            }
            auto const* definition = state.document().catalog().findRecognizer(
                *recognizerId
            );
            if (definition == nullptr)
            {
                ImGui::TextUnformatted("The selected recognizer is gone.");
                ImGui::End();
                return;
            }

            // Reseed the name field from the document when the selection moves
            // to another recognizer, or when the document's name for it diverges
            // from what the buffer was seeded with (an undo, redo, or external
            // rename) and the user is not mid-edit. m_nameInputActive records
            // whether the field held focus on the previous frame: IsItemActive
            // read here would report the item drawn before the field, not the
            // field itself, which is only submitted below.
            auto const currentName = definition->name().value();
            if (
                ui.m_nameBufferFor != *recognizerId
                || (
                    ui.m_nameSeededValue != currentName
                    && !ui.m_nameInputActive
                )
            )
            {
                seedBuffer(ui.m_nameBuffer, currentName);
                ui.m_nameBufferFor   = *recognizerId;
                ui.m_nameSeededValue = currentName;
            }
            ImGui::InputText(
                "Name",
                ui.m_nameBuffer.data(),
                ui.m_nameBuffer.size()
            );
            ui.m_nameInputActive = ImGui::IsItemActive();
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                auto draft       = state.draft();
                auto* recognizer = findEditableRecognizer(draft, *recognizerId);
                if (recognizer != nullptr)
                {
                    recognizer->m_name = std::string{ui.m_nameBuffer.data()};
                    auto description = std::format(
                        "renamed \"{}\" to \"{}\"",
                        currentName,
                        recognizer->m_name
                    );
                    requestEdit(ui, std::move(draft), std::move(description));
                }
            }

            auto typeIndex = static_cast<int>(
                std::to_underlying(definition->annotationType())
            );
            if (
                ImGui::Combo(
                    "Type",
                    &typeIndex,
                    k_annotationTypeItems.data(),
                    static_cast<int>(k_annotationTypeItems.size())
                )
            )
            {
                // The type and the fields the catalog ties to it have to move
                // together, so this goes through retypeRecognizer rather than
                // writing the field and committing.
                auto retyped = retypeRecognizer(
                    state.draft(),
                    *recognizerId,
                    static_cast<annotation::AnnotationType>(typeIndex)
                );
                if (!retyped)
                {
                    ui.m_statusLine = std::format(
                        "type change rejected: {}",
                        toString(retyped.error())
                    );
                }
                else
                {
                    // Summarized against the pre-commit document so a page named
                    // in the summary is resolved before the edit lands.
                    auto const summary = retypeSummary(
                        state,
                        *retyped,
                        k_annotationTypeItems.at(
                            static_cast<std::size_t>(typeIndex)
                        )
                    );
                    requestEdit(ui, std::move(retyped->m_draft), summary);
                }
            }

            auto threshold = static_cast<int>(definition->threshold().basisPoints());
            ImGui::InputInt("Threshold (bp)", &threshold);
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                threshold  = std::clamp(threshold, 0, k_thresholdMax);
                auto draft = state.draft();
                auto* recognizer = findEditableRecognizer(draft, *recognizerId);
                if (recognizer != nullptr)
                {
                    recognizer->m_similarityBasisPoints =
                        static_cast<uint32>(threshold);
                    requestEdit(
                        ui,
                        std::move(draft),
                        std::format("threshold set to {} bp", threshold)
                    );
                }
            }

            // Only an action target may define a default click, so the toggle is
            // unavailable for the other types instead of committing a rejected
            // edit.
            auto const isActionTarget = (
                definition->annotationType() == annotation::AnnotationType::ActionTarget
            );
            auto hasClickOffset = definition->defaultClick().has_value();
            ImGui::BeginDisabled(!isActionTarget);
            auto const clickToggled = ImGui::Checkbox(
                "Click offset",
                &hasClickOffset
            );
            ImGui::EndDisabled();
            if (
                !isActionTarget
                && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)
            )
            {
                ImGui::SetTooltip(
                    "Only an action target may define a default click"
                );
            }
            if (clickToggled)
            {
                auto draft       = state.draft();
                auto* recognizer = findEditableRecognizer(draft, *recognizerId);
                if (recognizer != nullptr)
                {
                    recognizer->m_defaultClick = hasClickOffset
                        ? std::optional<EditableTemplateOffset>{
                            EditableTemplateOffset{.m_x = 0U, .m_y = 0U}
                        }
                        : std::nullopt;
                    requestEdit(
                        ui,
                        std::move(draft),
                        hasClickOffset
                            ? "click offset enabled at 0,0"
                            : "click offset removed"
                    );
                }
            }
            if (auto const offset = definition->defaultClick())
            {
                auto const templateRect = definition->templateRect();
                auto offsetX = static_cast<int>(offset->x());
                auto offsetY = static_cast<int>(offset->y());
                ImGui::InputInt("Offset X", &offsetX);
                auto const editedX = ImGui::IsItemDeactivatedAfterEdit();
                ImGui::InputInt("Offset Y", &offsetY);
                auto const editedY = ImGui::IsItemDeactivatedAfterEdit();
                if (editedX || editedY)
                {
                    offsetX = std::clamp(
                        offsetX,
                        0,
                        static_cast<int>(templateRect.width()) - 1
                    );
                    offsetY = std::clamp(
                        offsetY,
                        0,
                        static_cast<int>(templateRect.height()) - 1
                    );
                    auto draft       = state.draft();
                    auto* recognizer = findEditableRecognizer(draft, *recognizerId);
                    if (recognizer != nullptr)
                    {
                        recognizer->m_defaultClick = EditableTemplateOffset{
                            .m_x = static_cast<uint32>(offsetX),
                            .m_y = static_cast<uint32>(offsetY),
                        };
                        requestEdit(
                            ui,
                            std::move(draft),
                            std::format(
                                "click offset set to {},{}",
                                offsetX,
                                offsetY
                            )
                        );
                    }
                }
            }

            drawPageMembership(state, ui, *definition);
            drawRegressionClassification(state, ui);

            ImGui::End();
        }

        auto drawPreviewResult(PreviewResult const& preview) -> void
        {
            ImGui::SeparatorText("Preview");
            if (preview.m_pageStop.has_value())
            {
                ImGui::Text(
                    "page stop: recognizer %s reason %d",
                    shortId(preview.m_pageStop->m_recognizerId.value()).c_str(),
                    static_cast<int>(
                        std::to_underlying(preview.m_pageStop->m_reason)
                    )
                );
            }
            else if (preview.m_pageKind.has_value())
            {
                if (preview.m_resolvedPageId.has_value())
                {
                    ImGui::Text(
                        "page: %s (%s)",
                        previewPageKindName(*preview.m_pageKind),
                        shortId(preview.m_resolvedPageId->value()).c_str()
                    );
                }
                else
                {
                    ImGui::Text("page: %s", previewPageKindName(*preview.m_pageKind));
                }
            }

            for (auto const& row : preview.m_anchorRows)
            {
                auto const sad = row.m_sadScore.has_value()
                    ? std::format("{}", *row.m_sadScore)
                    : std::string{"-"};
                auto const rect = row.m_matchedRect.has_value()
                    ? std::format(
                        "{},{} {}x{}",
                        row.m_matchedRect->x(),
                        row.m_matchedRect->y(),
                        row.m_matchedRect->width(),
                        row.m_matchedRect->height()
                    )
                    : std::string{"-"};
                ImGui::Text(
                    "%s  hit=%d  sad=%s/%llu  rect=%s",
                    shortId(row.m_recognizerId.value()).c_str(),
                    row.m_hit ? 1 : 0,
                    sad.c_str(),
                    static_cast<unsigned long long>(row.m_maximumSad),
                    rect.c_str()
                );
            }

            if (preview.m_actionStop.has_value())
            {
                ImGui::Text(
                    "action stop: reason %d",
                    static_cast<int>(
                        std::to_underlying(preview.m_actionStop->m_reason)
                    )
                );
            }
            else if (preview.m_actionEvidence.has_value())
            {
                ImGui::Text(
                    "action %s  hit=%d",
                    shortId(preview.m_actionEvidence->m_recognizerId.value()).c_str(),
                    preview.m_actionEvidence->m_hit ? 1 : 0
                );
            }
        }

        auto drawActionsPanel(AppState& state, PanelUiState& ui) -> void
        {
            if (!ImGui::Begin("Actions"))
            {
                ImGui::End();
                return;
            }

            if (ImGui::Button("Save + Generate"))
            {
                auto const assets = state.compilerSourceAssets();
                if (!assets)
                {
                    ui.m_statusLine = std::format(
                        "save failed: {}",
                        toString(assets.error())
                    );
                }
                else
                {
                    auto const status = saveAndGenerateAuthoringProject(
                        state.projectRoot(),
                        state.document(),
                        *assets
                    );
                    if (!status)
                    {
                        ui.m_statusLine = std::format(
                            "save failed: {}",
                            toString(status.error())
                        );
                    }
                    else
                    {
                        state.markSaved();
                        ui.m_statusLine = "saved and generated";
                    }
                }
            }

            if (ImGui::Button("Preview"))
            {
                auto const selectedSource = state.selectedSourceId();
                if (!selectedSource.has_value())
                {
                    ui.m_statusLine = "preview requires a selected source";
                }
                else if (auto const assets = state.compilerSourceAssets(); !assets)
                {
                    ui.m_statusLine = std::format(
                        "preview failed: {}",
                        toString(assets.error())
                    );
                }
                else
                {
                    auto const policy = annotation::RecognitionPolicy{
                        .m_maximumPixelComparisons = k_previewComparisonBudget,
                        .m_deadline = MonotonicInstant::now().checkedAdd(
                            k_previewDeadline
                        ),
                    };
                    auto preview = runPreview(
                        state.document(),
                        *assets,
                        *selectedSource,
                        state.selectedRecognizerId(),
                        policy
                    );
                    if (!preview)
                    {
                        ui.m_statusLine = std::format(
                            "preview failed: {}",
                            toString(preview.error())
                        );
                    }
                    else
                    {
                        state.setLastPreview(std::move(*preview));
                        ui.m_statusLine = "preview complete";
                    }
                }
            }

            ImGui::BeginDisabled(!state.canUndo());
            if (ImGui::Button("Undo"))
            {
                if (state.undo())
                {
                    ui.m_statusLine = std::format(
                        "undo: {} recognizers, {} pages",
                        state.document().catalog().recognizers().size(),
                        state.document().catalog().pages().size()
                    );
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!state.canRedo());
            if (ImGui::Button("Redo"))
            {
                if (state.redo())
                {
                    ui.m_statusLine = std::format(
                        "redo: {} recognizers, {} pages",
                        state.document().catalog().recognizers().size(),
                        state.document().catalog().pages().size()
                    );
                }
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(!state.selectedSourceId().has_value());
            if (ImGui::Button("New Recognizer"))
            {
                auto const created = addDefaultRecognizer(state);
                if (!created)
                {
                    ui.m_statusLine = std::format(
                        "new recognizer failed: {}",
                        toString(created.error())
                    );
                }
                else
                {
                    state.setSelectedRecognizerId(*created);
                    ui.m_statusLine = std::format(
                        "added recognizer ({} total)",
                        state.document().catalog().recognizers().size()
                    );
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("New Page"))
            {
                auto const created = addDefaultPage(state);
                ui.m_statusLine = created.has_value()
                    ? std::format(
                        "added page ({} total)",
                        state.document().catalog().pages().size()
                    )
                    : std::format("new page failed: {}", toString(created.error()));
            }

            if (!ui.m_statusLine.empty())
            {
                ImGui::TextWrapped("%s", ui.m_statusLine.c_str());
            }

            if (auto const& preview = state.lastPreview())
            {
                drawPreviewResult(*preview);
            }

            ImGui::End();
        }
    }

    auto drawWorkbench(
        AppState& state,
        WorkbenchServices const& services,
        PanelUiState& ui
    ) -> void
    {
        // Host a full-viewport dock space so the four panels can be docked and
        // resized against each other. Enabled by ImGuiConfigFlags_DockingEnable
        // in the GUI shell; with no ini file the layout is not persisted between
        // launches, so panels start floating.
        static_cast<void>(ImGui::DockSpaceOverViewport());

        drawSourcesPanel(state, services, ui);
        drawRecognizersPanel(state, ui);
        drawPagesPanel(state, ui);
        drawCanvasPanel(state, services, ui);
        drawPropertiesPanel(state, ui);

        // Every panel above borrows into the document while it draws, so the
        // frame's edit lands here, once they are all done with it. The actions
        // panel follows rather than precedes it: it mutates the document itself
        // and re-reads what it touches, so it must see the frame's edit already
        // applied -- undoing right after a rename has to undo that rename, not
        // race it.
        applyPendingEdit(state, ui);
        drawActionsPanel(state, ui);

        // Mirror each new status-line outcome to the operation log so a session's
        // actions and errors are not lost when the next action overwrites the
        // transient line. Consecutive identical outcomes collapse to one entry.
        if (
            services.m_appendLog
            && !ui.m_statusLine.empty()
            && ui.m_statusLine != ui.m_lastLoggedStatus
        )
        {
            services.m_appendLog(ui.m_statusLine);
            ui.m_lastLoggedStatus = ui.m_statusLine;
        }
    }
}
