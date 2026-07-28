#include "panels.hpp"

#include "../canvas-math.hpp"
#include "../workbench-app.hpp"

#include "authoring-actions.hpp"
#include "edit-page.hpp"
#include "model-check-view.hpp"
#include "page-view.hpp"
#include "panel-state.hpp"
#include "preview.hpp"
#include "project-persistence.hpp"
#include "project-tree.hpp"
#include "source-ingestion.hpp"

#include <annotation/authoring-document.hpp>
#include <annotation/catalog.hpp>
#include <annotation/recognition-runtime.hpp>

#include <core/error/contracts.hpp>
#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <vision/sad.hpp>

#include <imgui.h>
#include <imgui_internal.h>

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
#include <variant>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        constexpr auto k_templateColor = IM_COL32(96, 220, 120, 255);
        constexpr auto k_searchRoiColor = IM_COL32(96, 168, 255, 255);
        constexpr auto k_gripColor      = IM_COL32(240, 240, 240, 255);

        // A template box belonging to another screen: dimmed, so it reads as
        // context rather than as something to drag.
        constexpr auto k_foreignTemplateColor = IM_COL32(96, 220, 120, 110);

        // Where a preview or model-check found a template. Deliberately unlike the
        // authored template (green) and search ROI (blue): amber when the match
        // passed its threshold, red when the closest match still missed, so an
        // author reads recognition evidence apart from what they drew.
        constexpr auto k_matchHitColor  = IM_COL32(255, 196, 0, 255);
        constexpr auto k_matchMissColor = IM_COL32(255, 96, 96, 255);

        auto const k_passColor = ImVec4{0.38F, 0.86F, 0.47F, 1.0F};
        auto const k_failColor = ImVec4{0.96F, 0.55F, 0.38F, 1.0F};

        constexpr auto k_gripRadius     = 5.0F;
        constexpr auto k_zoomWheelBase  = 1.1F;

        // How far, in screen pixels, a left press must move before it is a
        // rubber-band rather than a click that selects. A few pixels of slack lets
        // a click that jitters still select.
        constexpr auto k_dragThreshold = 4.0F;

        // A page member that is not the selection: bright L-shaped corner
        // brackets in the template hue locate it crisply, a faint body outline
        // keeps its extent readable, and the search region stays a faint hint --
        // so the selected element still reads as the one being edited without
        // the rest sinking into the screenshot.
        constexpr auto k_memberCornerColor       = IM_COL32(96, 220, 120, 255);
        constexpr auto k_memberOutlineColor      = IM_COL32(96, 220, 120, 70);
        constexpr auto k_foreignCornerColor      = IM_COL32(200, 220, 200, 200);
        constexpr auto k_mutedSearchRoiColor     = IM_COL32(96, 168, 255, 110);

        // The rectangle a rubber-band is dragging out, before its kind is chosen.
        constexpr auto k_rubberBandColor = IM_COL32(240, 240, 240, 220);

        // Gates which drop targets accept a shared region being dragged.
        constexpr auto k_sharedRegionPayload = "uf.shared-region";

        // The Preview action runs on the GUI thread, so it is bounded twice over:
        // by k_recognitionComparisonBudget in preview.hpp, and by this deadline
        // capping the wall-clock time a large source may spend before the next
        // frame. Hitting either limit surfaces as a PreviewStop row.
        constexpr auto k_previewDeadline = std::chrono::seconds{2};

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


        auto seedBuffer(
            std::array<char, 256>& buffer,
            std::string const& text
        ) -> void
        {
            auto const count = std::min(text.size(), buffer.size() - 1U);
            std::copy_n(text.begin(), count, buffer.begin());
            buffer.at(count) = '\0';
        }

        // Commits the in-progress inline tree rename through the same draft-edit
        // path the Inspector's element rename uses: a recognizer edits its own
        // name, a page edits its own. A duplicate or empty name is refused by the
        // build and surfaced by applyPendingEdit exactly as any rejected edit;
        // an unchanged name is a no-op. Clears the rename either way.
        auto commitInlineRename(AppState& state, PanelUiState& ui) -> void
        {
            if (!ui.inlineRename.has_value())
            {
                return;
            }
            auto const rename  = *ui.inlineRename;
            auto const newName = std::string{rename.buffer.data()};
            ui.inlineRename.reset();

            if (rename.kind == PanelUiState::RenameKind::Recognizer)
            {
                auto const id    = annotation::RecognizerId{rename.id};
                auto draft       = state.draft();
                auto* recognizer = findEditableRecognizer(draft, id);
                if (recognizer == nullptr || recognizer->name == newName)
                {
                    return;
                }
                auto description = std::format(
                    "renamed \"{}\" to \"{}\"",
                    recognizer->name,
                    newName
                );
                recognizer->name = newName;
                requestEdit(ui, std::move(draft), std::move(description));
                return;
            }

            auto const id   = annotation::PageId{rename.id};
            auto draft      = state.draft();
            auto const page = std::ranges::find(
                draft.pages,
                id,
                &EditablePage::id
            );
            if (page == draft.pages.end() || page->name == newName)
            {
                return;
            }
            auto description = std::format(
                "renamed page \"{}\" to \"{}\"",
                page->name,
                newName
            );
            page->name = newName;
            requestEdit(ui, std::move(draft), std::move(description));
        }

        // Opens an inline rename over one tree row, replacing any rename already
        // in progress so only one is ever open. The buffer seeds from the current
        // name; the field takes keyboard focus on its first frame.
        auto beginInlineRename(
            PanelUiState& ui,
            PanelUiState::RenameKind kind,
            annotation::ResourceId id,
            std::string const& currentName
        ) -> void
        {
            auto rename = PanelUiState::InlineRename{
                .kind = kind,
                .id   = id,
            };
            seedBuffer(rename.buffer, currentName);
            ui.inlineRename = std::move(rename);
        }

        // Draws the inline rename field for the current row and drives its
        // lifecycle: focus on the first frame, commit on Enter, cancel on Esc or
        // a click away (both surface as the field deactivating without an Enter).
        auto drawInlineRenameField(AppState& state, PanelUiState& ui) -> void
        {
            if (!ui.inlineRename.has_value())
            {
                return;
            }
            if (ui.inlineRename->justOpened)
            {
                ImGui::SetKeyboardFocusHere();
                ui.inlineRename->justOpened = false;
            }
            ImGui::SetNextItemWidth(180.0F);
            auto const committed = ImGui::InputText(
                "##inline-rename",
                ui.inlineRename->buffer.data(),
                ui.inlineRename->buffer.size(),
                ImGuiInputTextFlags_EnterReturnsTrue
                    | ImGuiInputTextFlags_AutoSelectAll
            );
            if (committed)
            {
                commitInlineRename(state, ui);
                return;
            }
            if (ImGui::IsItemDeactivated())
            {
                ui.inlineRename.reset();
            }
        }

        // The shared entry for every delete-everywhere surface: the tree Remove
        // button, the canvas element menu, the Inspector button, and the Delete
        // key. A deletion the delete path itself refuses (an anchor a page
        // depends on) surfaces its message now rather than behind a confirmation
        // that could not have committed; otherwise it selects the element, so the
        // confirmation cannot outlive it, and opens the confirmation naming what
        // the deletion withdraws.
        auto requestDeleteEverywhere(
            AppState& state,
            PanelUiState& ui,
            annotation::RecognizerId id
        ) -> void
        {
            auto const* definition = state.document().catalog().findRecognizer(id);
            if (definition == nullptr)
            {
                return;
            }
            auto const name = definition->name().value();

            auto probe = deleteRecognizer(state.draft(), id);
            if (!probe)
            {
                requestDeletion(ui, std::move(probe), std::format("\"{}\"", name));
                return;
            }

            if (state.selectedRecognizerId() != id)
            {
                selectRecognizer(state, id, state.selectedSourceId());
            }

            auto const placements = pagesPlacedOn(state.draft(), id).size();
            auto detail           = std::string{};
            if (placements > 0U)
            {
                detail = std::format(
                    "Removes it from {} page{}.",
                    placements,
                    placements == 1U ? "" : "s"
                );
            }
            else if (probe->withdrawnRoles > 0U)
            {
                detail = std::format(
                    "Withdraws it from {} page signature{}.",
                    probe->withdrawnRoles,
                    probe->withdrawnRoles == 1U ? "" : "s"
                );
            }
            else
            {
                detail = "It is not placed on any page.";
            }

            ui.pendingDelete = PanelUiState::PendingDelete{
                .id     = id,
                .name   = name,
                .detail = std::move(detail),
            };
            ui.pendingDeleteJustRequested = true;
        }

        // The delete-everywhere confirmation modal, opened the frame an entry
        // point requests it. It names the element and what the deletion
        // withdraws; Delete routes through the existing deletion path (one edit,
        // committed by applyPendingEdit this frame), Cancel does nothing. A
        // selection that has moved off the element abandons the confirmation, so
        // it can never delete something other than what it was raised for.
        auto drawDeleteConfirmPopup(AppState& state, PanelUiState& ui) -> void
        {
            if (
                ui.pendingDelete.has_value()
                && state.selectedRecognizerId() != ui.pendingDelete->id
            )
            {
                ui.pendingDelete.reset();
                ui.pendingDeleteJustRequested = false;
            }
            if (std::exchange(ui.pendingDeleteJustRequested, false))
            {
                ImGui::OpenPopup("confirm-delete-everywhere");
            }

            auto const center = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowPos(
                center,
                ImGuiCond_Appearing,
                ImVec2{0.5F, 0.5F}
            );
            if (
                !ImGui::BeginPopupModal(
                    "confirm-delete-everywhere",
                    nullptr,
                    ImGuiWindowFlags_AlwaysAutoResize
                )
            )
            {
                return;
            }
            if (!ui.pendingDelete.has_value())
            {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                return;
            }
            auto const pending = *ui.pendingDelete;
            ImGui::Text("Delete \"%s\" everywhere?", pending.name.c_str());
            ImGui::TextDisabled("%s", pending.detail.c_str());
            ImGui::TextDisabled("This can be undone.");
            ImGui::Separator();
            if (ImGui::Button("Delete"))
            {
                requestDeletion(
                    ui,
                    deleteRecognizer(state.draft(), pending.id),
                    std::format("\"{}\"", pending.name)
                );
                ui.pendingDelete.reset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                ui.pendingDelete.reset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }










        // The rectangles a freshly drawn recognizer starts from: a small template
        // at the origin and a search region covering the whole frame. The author
        // drags both to where they belong on the canvas, so the only requirement
        // here is that they are legal for any project resolution.




        // A rectangle with no grips, for a box the author cannot edit from the
        // screen currently on display.
        auto drawRectOutline(
            ImDrawList& drawList,
            CanvasView view,
            CanvasPoint canvasOrigin,
            PixelRect const& rect,
            ImU32 color
        ) -> void
        {
            auto const origin = rectScreenOrigin(view, canvasOrigin, rect);
            auto const width  = static_cast<float>(rect.width()) * view.zoom;
            auto const height = static_cast<float>(rect.height()) * view.zoom;
            drawList.AddRect(
                ImVec2{origin.x, origin.y},
                ImVec2{origin.x + width, origin.y + height},
                color,
                0.0F,
                0,
                1.0F
            );
        }

        // A non-selected member's template drawn as four bright L-shaped corner
        // brackets plus a faint full outline: the corners locate it crisply over
        // any screenshot while the body stays out of the way, where a uniformly
        // dimmed rectangle sank into busy pixels. Arm length adapts to the box so
        // small marks stay readable and large regions are not shouted over.
        auto drawCornerBrackets(
            ImDrawList& drawList,
            CanvasView view,
            CanvasPoint canvasOrigin,
            PixelRect const& rect,
            ImU32 brightColor,
            ImU32 faintColor
        ) -> void
        {
            auto const origin = rectScreenOrigin(view, canvasOrigin, rect);
            auto const width  = static_cast<float>(rect.width()) * view.zoom;
            auto const height = static_cast<float>(rect.height()) * view.zoom;
            drawList.AddRect(
                ImVec2{origin.x, origin.y},
                ImVec2{origin.x + width, origin.y + height},
                faintColor,
                0.0F,
                0,
                1.0F
            );
            auto const arm = std::min(
                12.0F,
                0.25F * std::min(width, height)
            );
            if (arm < 2.0F)
            {
                return;
            }
            auto const right     = origin.x + width;
            auto const bottom    = origin.y + height;
            auto const thickness = 3.0F;
            auto const corner = [&](float x, float y, float dx, float dy)
            {
                drawList.AddLine(
                    ImVec2{x, y},
                    ImVec2{x + (arm * dx), y},
                    brightColor,
                    thickness
                );
                drawList.AddLine(
                    ImVec2{x, y},
                    ImVec2{x, y + (arm * dy)},
                    brightColor,
                    thickness
                );
            };
            corner(origin.x, origin.y, 1.0F, 1.0F);
            corner(right, origin.y, -1.0F, 1.0F);
            corner(origin.x, bottom, 1.0F, -1.0F);
            corner(right, bottom, -1.0F, -1.0F);
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
            auto const width  = static_cast<float>(rect.width()) * view.zoom;
            auto const height = static_cast<float>(rect.height()) * view.zoom;

            drawList.AddRect(
                ImVec2{origin.x, origin.y},
                ImVec2{origin.x + width, origin.y + height},
                color,
                0.0F,
                0,
                2.0F
            );

            auto const centers = std::array<CanvasPoint, 8>{
                CanvasPoint{origin.x, origin.y},
                CanvasPoint{origin.x + width / 2.0F, origin.y},
                CanvasPoint{origin.x + width, origin.y},
                CanvasPoint{origin.x + width, origin.y + height / 2.0F},
                CanvasPoint{origin.x + width, origin.y + height},
                CanvasPoint{origin.x + width / 2.0F, origin.y + height},
                CanvasPoint{origin.x, origin.y + height},
                CanvasPoint{origin.x, origin.y + height / 2.0F},
            };
            for (auto const& center : centers)
            {
                drawList.AddRectFilled(
                    ImVec2{center.x - k_gripRadius, center.y - k_gripRadius},
                    ImVec2{center.x + k_gripRadius, center.y + k_gripRadius},
                    k_gripColor
                );
            }
        }

        // Defined with the Log tab below; the toolbar and inspector need it above
        // their own definitions to colour the persistent status summary.
        [[nodiscard]]
        auto logSeverityColor(LogSeverity severity) -> std::optional<ImVec4>;

        // Adds a captured source to the project. Split out of the toolbar and run
        // by the shell after the frame's parked edit commits: ingestion makes its
        // own history commit, and doing it before applyPendingEdit would silently
        // drop a draft the panels built against the pre-capture document.
        auto performCapture(
            AppState& state,
            WorkbenchServices const& services,
            PanelUiState& ui
        ) -> void
        {
            auto const id    = annotation::SourceId{mintResourceId()};
            auto const title = std::string{ui.targetTitle.data()};
            auto ingested    = services.captureFromTarget(id, title);
            if (!ingested)
            {
                ui.report(
                    LogSeverity::Error,
                    std::format("capture failed: {}", toString(ingested.error()))
                );
                return;
            }
            auto const added = state.addIngestedSource(std::move(*ingested));
            if (added.has_value())
            {
                ui.report(LogSeverity::Info, "captured source");
            }
            else
            {
                ui.report(
                    LogSeverity::Error,
                    std::format("capture add failed: {}", toString(added.error()))
                );
            }
        }

        // Opens the OS picker and imports the chosen PNG as a source, under the
        // same post-commit ordering as performCapture. A cancelled dialog is
        // silent.
        auto performImport(
            AppState& state,
            WorkbenchServices const& services,
            PanelUiState& ui
        ) -> void
        {
            auto picked = services.pickPngToImport();
            if (!picked)
            {
                ui.report(
                    LogSeverity::Error,
                    std::format(
                        "import dialog failed: {}",
                        toString(picked.error())
                    )
                );
                return;
            }
            if (!picked->has_value())
            {
                return;
            }
            auto const id = annotation::SourceId{mintResourceId()};
            auto ingested = importSourcePng(id, **picked);
            if (!ingested)
            {
                ui.report(
                    LogSeverity::Error,
                    std::format("import failed: {}", toString(ingested.error()))
                );
                return;
            }
            auto const added = state.addIngestedSource(std::move(*ingested));
            if (added.has_value())
            {
                ui.report(LogSeverity::Info, "imported source");
            }
            else
            {
                ui.report(
                    LogSeverity::Error,
                    std::format("import add failed: {}", toString(added.error()))
                );
            }
        }

        // The slim top toolbar: the project-level actions and the target picker,
        // plus the one persistent status line the redesign keeps as its plain
        // spine. Nothing here mutates the document inline. Save, undo, and redo
        // are queued through requestToolbarCommand and run after this frame's
        // parked edit lands, so an undo reverses the same-frame widget edit rather
        // than racing it; capture and import set flags the shell drains for the
        // same reason. The dirty dot reads the edit-history save position, so an
        // undo back to the saved state clears it and a redo past it sets it again.
        auto drawToolbar(AppState& state, PanelUiState& ui) -> void
        {
            if (!ImGui::Begin("Toolbar"))
            {
                ImGui::End();
                return;
            }

            if (ImGui::Button("Save + Generate"))
            {
                requestToolbarCommand(ui, ToolbarCommand::SaveAndGenerate);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!state.canUndo());
            if (ImGui::Button("Undo"))
            {
                requestToolbarCommand(ui, ToolbarCommand::Undo);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!state.canRedo());
            if (ImGui::Button("Redo"))
            {
                requestToolbarCommand(ui, ToolbarCommand::Redo);
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();

            ImGui::SetNextItemWidth(180.0F);
            ImGui::InputText(
                "##target",
                ui.targetTitle.data(),
                ui.targetTitle.size()
            );
            auto const hasTarget = ui.targetTitle.at(0) != '\0';
            ImGui::SameLine();
            ImGui::BeginDisabled(!hasTarget);
            if (ImGui::Button("Capture"))
            {
                ui.captureRequested = true;
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
            ImGui::SameLine();
            if (ImGui::Button("Import PNG..."))
            {
                ui.importRequested = true;
            }

            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            if (state.dirty())
            {
                ImGui::TextColored(
                    ImVec4{0.95F, 0.65F, 0.15F, 1.0F},
                    "%s",
                    "* unsaved"
                );
            }
            else
            {
                ImGui::TextDisabled("saved");
            }

            // The persistent one-line summary: the transient status the actions
            // used to print in their own panel, kept visible frame to frame so the
            // last outcome is always on screen even when its panel is collapsed.
            if (!ui.statusLine.empty())
            {
                ImGui::SameLine();
                ImGui::TextDisabled("|");
                ImGui::SameLine();
                auto const color = logSeverityColor(ui.statusSeverity);
                if (color.has_value())
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, *color);
                }
                ImGui::TextUnformatted(ui.statusLine.c_str());
                if (color.has_value())
                {
                    ImGui::PopStyleColor();
                }
            }

            ImGui::End();
        }









        // One selectable recognizer row inside a page's group. pageScreen is the
        // screen that page stands for, and pageContext the page itself, or both
        // nothing for the rows that belong to no page. The page context makes the
        // row select Element(id, pageContext=this page) so the canvas edits this
        // page's placement even when the shown screen's claim is ambiguous.
        auto drawPageMemberRow(
            AppState& state,
            AuthoringDraft const& draft,
            PanelUiState& ui,
            annotation::RecognizerDefinition const& recognizer,
            std::optional<annotation::SourceId> pageScreen,
            std::optional<annotation::PageId> pageContext
        ) -> void
        {
            auto const id     = recognizer.id();
            auto const idText = id.value().toString();
            ImGui::PushID(idText.c_str());

            // Only an interactive region can be reused, so only those carry the
            // box. Ticking it puts the element in Shared regions, which is where
            // another page can take it from; the bullet keeps the other rows
            // lined up with these.
            auto const isRegion = (
                recognizer.annotationType() == annotation::AnnotationType::ActionTarget
            );
            if (isRegion)
            {
                auto reusable = isRegionShared(state, id);
                if (ImGui::Checkbox("##shared", &reusable))
                {
                    requestRegionShared(state, ui, id, reusable);
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::SetTooltip(
                        "Reusable on other pages. Tick it, then drag the element "
                        "from Shared regions onto the page it also appears on."
                    );
                }
                ImGui::SameLine();
            }
            else
            {
                ImGui::Bullet();
            }

            // A rename in progress over this row replaces its label with the
            // inline field; the Selectable and buttons return until it closes.
            if (
                ui.inlineRename.has_value()
                && ui.inlineRename->kind == PanelUiState::RenameKind::Recognizer
                && ui.inlineRename->id == id.value()
            )
            {
                drawInlineRenameField(state, ui);
                ImGui::PopID();
                return;
            }

            auto const memberCount = pagesPlacedOn(draft, id).size();
            auto const label       = memberCount > 1U
                ? std::format(
                    "{}  (on {} pages)",
                    recognizer.name().value(),
                    memberCount
                )
                : recognizer.name().value();
            // Sized to its own text rather than left to span the row. A
            // default-sized Selectable claims the whole remaining width, so the
            // SameLine after it starts at the right edge of the panel and
            // everything that follows -- Remove, and the margin -- is pushed out
            // of the clip rect and cannot be clicked.
            if (
                ImGui::Selectable(
                    label.c_str(),
                    state.selectedRecognizerId() == id,
                    ImGuiSelectableFlags_AllowOverlap,
                    ImVec2{ImGui::CalcTextSize(label.c_str()).x, 0.0F}
                )
            )
            {
                selectRecognizer(state, id, pageScreen, pageContext);
            }
            // Double-click renames in place. The first click of the pair selects
            // through the Selectable above, which is harmless; the second opens
            // the field.
            if (
                ImGui::IsItemHovered()
                && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
            )
            {
                beginInlineRename(
                    ui,
                    PanelUiState::RenameKind::Recognizer,
                    id.value(),
                    recognizer.name().value()
                );
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove"))
            {
                requestDeleteEverywhere(state, ui, id);
            }

            // The margin, once a check has produced one. "here" is the score on
            // the screen this mark was drawn on and has to stay under 100 %;
            // "elsewhere" is the closest it comes on any other screen and has to
            // stay above it. A mark whose two numbers sit close together is one
            // frame of drift away from resolving the wrong page.
            if (auto const* p_margin = findMargin(state, id))
            {
                ImGui::SameLine();
                auto summary = std::format(
                    "here {}  elsewhere {}",
                    budgetPercentText(
                        p_margin->ownSadScore,
                        p_margin->maximumSad
                    ),
                    budgetPercentText(
                        p_margin->nearestOtherSadScore,
                        p_margin->maximumSad
                    )
                );
                // Only shown once a live check has produced one. This is the
                // number that moves: the stills never change, the running game
                // does.
                if (p_margin->liveSadScore.has_value())
                {
                    summary += std::format(
                        "  live {}",
                        budgetPercentText(
                            p_margin->liveSadScore,
                            p_margin->maximumSad
                        )
                    );
                }
                ImGui::TextDisabled("%s", summary.c_str());
            }
            ImGui::PopID();
        }

        // Opens a fresh page from the selected screen through EditPage. The
        // created page's anchor is selected once the commit lands, and every
        // refusal reads exactly as the free-function flow it replaces did.
        auto createPageFromSelectedScreen(
            AppState& state,
            PanelUiState& ui
        ) -> void
        {
            auto const source = state.selectedSourceId();
            if (!source.has_value())
            {
                ui.report(LogSeverity::Error, "select a screen first");
                return;
            }

            auto page = EditPage::createFrom(state, *source);
            if (!page)
            {
                ui.report(
                    LogSeverity::Error,
                    std::format(
                        "new page failed: {}",
                        toString(page.error())
                    )
                );
                return;
            }

            // createFrom mints exactly one anchor for the page, so the view's
            // first signature row names it; that is what the description and the
            // selection both point at.
            auto const view = page->view();
            UF_CHECK(!view.identifiedBy.empty());
            auto const anchorId = view.identifiedBy.front().id;
            auto description     = std::format(
                "added page \"{}\" with \"{}\" identifying it; "
                "drag its box over a mark unique to this screen",
                view.name,
                view.identifiedBy.front().name
            );
            std::move(*page).commitSelecting(
                ui,
                std::move(description),
                anchorId,
                source
            );
        }

        // The status line a new page member leaves, worded by the button that
        // created it, so an anchor and a region each say what to drag its box
        // over.
        [[nodiscard]]
        auto newMemberDescription(
            std::string_view name,
            PageMemberKind kind
        ) -> std::string
        {
            auto const [role, what] = [kind]() -> std::pair<char const*, char const*>
            {
                switch (kind)
                {
                case PageMemberKind::Anchor:
                    return {"a mark identifying this page", "mark"};
                case PageMemberKind::ActionTarget:
                    return {"an interactive region on this page", "region"};
                case PageMemberKind::InfoRegion:
                    return {"an info region on this page", "info region"};
                }
                UF_UNREACHABLE_MSG("unknown PageMemberKind value");
            }();
            return std::format(
                "added \"{}\" as {}; drag its box over the {}",
                name,
                role,
                what
            );
        }

        // Adds one member to a page through EditPage, typed by the group whose
        // button was pressed, and selects it once the commit lands.
        auto placeNewMember(
            AppState& state,
            PanelUiState& ui,
            annotation::PageId pageId,
            annotation::SourceId sourceId,
            PageMemberKind kind
        ) -> void
        {
            auto page = EditPage::open(state, pageId);
            if (!page)
            {
                ui.report(
                    LogSeverity::Error,
                    std::format(
                        "add failed: {}",
                        toString(page.error())
                    )
                );
                return;
            }

            if (kind == PageMemberKind::Anchor)
            {
                auto added = page->placeAnchor(
                    EditPage::NewAnchorSpec{.sourceId = sourceId}
                );
                if (!added)
                {
                    ui.report(
                        LogSeverity::Error,
                        std::format(
                            "add failed: {}",
                            toString(added.error())
                        )
                    );
                    return;
                }
                auto const newId = added->id;
                std::move(*page).commitSelecting(
                    ui,
                    newMemberDescription(added->name, kind),
                    newId,
                    sourceId
                );
            }
            else
            {
                // Interactive and info elements share the placement path; the
                // group whose button was pressed picks the kind, and info carries
                // no click offset.
                auto added = kind == PageMemberKind::ActionTarget
                    ? page->placeRegion(EditPage::NewRegionSpec{.sourceId = sourceId})
                    : page->placeInfo(EditPage::NewRegionSpec{.sourceId = sourceId});
                if (!added)
                {
                    ui.report(
                        LogSeverity::Error,
                        std::format(
                            "add failed: {}",
                            toString(added.error())
                        )
                    );
                    return;
                }
                auto const newId = added->id;
                std::move(*page).commitSelecting(
                    ui,
                    newMemberDescription(added->name, kind),
                    newId,
                    sourceId
                );
            }
        }

        // Records that one screen is an example of a page through EditPage,
        // leaving the same status line the free-function claim did.
        auto recordScreenForPage(
            AppState& state,
            PanelUiState& ui,
            annotation::PageId pageId,
            annotation::SourceId sourceId
        ) -> void
        {
            auto page = EditPage::open(state, pageId);
            if (!page)
            {
                ui.report(
                    LogSeverity::Error,
                    std::format(
                        "recording the screen failed: {}",
                        toString(page.error())
                    )
                );
                return;
            }
            if (auto const status = page->claimScreen(sourceId); !status)
            {
                ui.report(
                    LogSeverity::Error,
                    std::format(
                        "recording the screen failed: {}",
                        toString(status.error())
                    )
                );
                return;
            }
            std::move(*page).commit(
                ui,
                std::format(
                    "screen {} recorded as page \"{}\"",
                    shortId(sourceId.value()),
                    pageName(state, pageId)
                )
            );
        }

        // The authoring source behind a screen id, for a row that has only the id
        // in hand (a page's regression list, a bucket). Null when the id names no
        // source in the current document.
        [[nodiscard]]
        auto findSource(
            AppState const& state UF_LIFETIME_BOUND,
            annotation::SourceId id
        ) -> annotation::AuthoringSource const*
        {
            for (auto const& source : state.document().sources())
            {
                if (source.id() == id)
                {
                    return &source;
                }
            }
            return nullptr;
        }

        // One captured-screen row: short id, content-hash prefix, and provenance,
        // selected as Screen(id). Highlighted only when a Screen is the selection
        // -- not when an element merely shows over it -- so a page's member and
        // its sample screen do not light up together. suffix tags the row (a
        // page's primary sample, say).
        auto drawScreenRow(
            AppState& state,
            annotation::SourceId id,
            std::string_view suffix
        ) -> void
        {
            auto const* source = findSource(state, id);
            if (source == nullptr)
            {
                return;
            }
            auto const screen   = state.selection().asScreen();
            auto const selected = screen.has_value() && screen->sourceId == id;
            auto const idText   = id.value().toString();
            ImGui::PushID(idText.c_str());
            auto const provenance = std::holds_alternative<
                annotation::WgcSourceProvenance
            >(source->provenance()) ? "wgc" : "imported";
            auto const label = std::format(
                "{}  {}  {}{}",
                shortId(id.value()),
                source->contentHash().hex().substr(0, 8),
                provenance,
                suffix
            );
            if (ImGui::Selectable(label.c_str(), selected))
            {
                state.select(AppState::Selection::Screen{id});
            }
            ImGui::PopID();
        }

        // One top-level screen bucket. "Needs classification" is the true to-do,
        // shown with a count and open by default even when empty; the two
        // expected-outcome buckets are finished work, badged with a count and
        // hidden entirely when they hold nothing so they never read as a nag.
        auto drawScreenBucket(
            AppState& state,
            char const* label,
            ScreenBucket bucket,
            bool isTodo
        ) -> void
        {
            auto const screens = screensInBucket(state.document(), bucket);
            if (screens.empty() && !isTodo)
            {
                return;
            }
            auto const header = std::format("{} ({})", label, screens.size());
            auto const flags  = isTodo
                ? ImGuiTreeNodeFlags_DefaultOpen
                : ImGuiTreeNodeFlags_None;
            if (ImGui::TreeNodeEx(header.c_str(), flags))
            {
                if (screens.empty())
                {
                    ImGui::TextDisabled("none");
                }
                for (auto const& id : screens)
                {
                    drawScreenRow(state, id, "");
                }
                ImGui::TreePop();
            }
        }

        // The workbench's navigator, page-centric and unified. The screen buckets
        // sit above the pages; a page nests the regression screens that resolve to
        // it plus the marks and elements authored on it. Everything an element,
        // page, or screen needs is reachable here, and every creation, deletion,
        // and share the former Screens and Pages panels offered is preserved.
        auto drawProjectTree(AppState& state, PanelUiState& ui) -> void
        {
            if (!ImGui::Begin("Project"))
            {
                ImGui::End();
                return;
            }

            auto const& catalog = state.document().catalog();

            if (state.document().sources().empty() && catalog.pages().empty())
            {
                ImGui::TextWrapped(
                    "Empty project. Capture a running target or import a PNG from "
                    "the toolbar to add the first screen, then classify it into a "
                    "page here."
                );
                ImGui::End();
                return;
            }

            // Screens outside any page, split three ways: the to-do, and the two
            // deliberately pageless outcomes.
            ImGui::SeparatorText("Screens");
            drawScreenBucket(
                state,
                "Needs classification",
                ScreenBucket::NeedsClassification,
                true
            );
            drawScreenBucket(
                state,
                "Expected unknown",
                ScreenBucket::ExpectedUnknown,
                false
            );
            drawScreenBucket(
                state,
                "Expected ambiguous",
                ScreenBucket::ExpectedAmbiguous,
                false
            );

            ImGui::SeparatorText("Pages");
            ImGui::BeginDisabled(!state.selectedSourceId().has_value());
            if (ImGui::Button("+ Page from selected screen"))
            {
                createPageFromSelectedScreen(state, ui);
            }
            ImGui::EndDisabled();
            if (
                !state.selectedSourceId().has_value()
                && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)
            )
            {
                ImGui::SetTooltip(
                    "Select a screen above first, then press this to author a page "
                    "from it"
                );
            }

            // Which page a shared region was dropped on this frame. The element
            // being dragged lives on PanelUiState instead, because the frame that
            // accepts the drop is the frame the drag source stops submitting
            // itself.
            auto droppedOnPage = std::optional<annotation::PageId>{};

            if (catalog.pages().empty())
            {
                ImGui::TextWrapped(
                    "No pages yet. Select a screen above, then press the button to "
                    "author a page from it."
                );
            }

            // The pages are drawn reflectively from their views: each row comes
            // from the page's own signature and placements rather than from a
            // per-page scan of every recognizer. The draft is copied once and the
            // views built from it; margins, live scores, and screen verdicts stay
            // id-keyed and are merged at draw time through the existing lookups.
            auto const draft = state.draft();

            for (auto const& page : catalog.pages())
            {
                auto const pageId = page.id();
                auto const idText = pageId.value().toString();
                ImGui::PushID(idText.c_str());

                auto const view = PageView::of(draft, pageId);

                // A rename in progress over this page draws the inline field in
                // place of the header; the disclosure and children return until
                // it closes.
                if (
                    ui.inlineRename.has_value()
                    && ui.inlineRename->kind == PanelUiState::RenameKind::Page
                    && ui.inlineRename->id == pageId.value()
                )
                {
                    drawInlineRenameField(state, ui);
                    ImGui::PopID();
                    continue;
                }

                // The header selects Page(pageId): clicking the label selects the
                // page, clicking the arrow only toggles it open, and a
                // double-click renames it. Highlighted while that page is the
                // selection, so the inspector's page summary and this row agree on
                // what is chosen.
                auto const selectedPage = state.selection().asPage();
                auto flags = ImGuiTreeNodeFlags_DefaultOpen
                    | ImGuiTreeNodeFlags_OpenOnArrow;
                if (selectedPage.has_value() && selectedPage->pageId == pageId)
                {
                    flags |= ImGuiTreeNodeFlags_Selected;
                }
                auto const open = ImGui::TreeNodeEx(
                    page.name().value().c_str(),
                    flags
                );
                if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
                {
                    state.select(AppState::Selection::Page{pageId});
                }
                if (
                    ImGui::IsItemHovered()
                    && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
                )
                {
                    beginInlineRename(
                        ui,
                        PanelUiState::RenameKind::Page,
                        pageId.value(),
                        page.name().value()
                    );
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Delete Page"))
                {
                    requestDeletion(
                        ui,
                        deletePage(state.draft(), pageId),
                        std::format("page \"{}\"", page.name().value())
                    );
                }
                if (!open)
                {
                    ImGui::PopID();
                    continue;
                }

                // A page relates to screens through per-source regression cases,
                // and several may resolve to it, so the screens are a list, not a
                // single claim. The primary authoring sample is marked where it
                // differs; its verdict, once a check has run, sits beside it.
                auto const sample            = pageSampleSource(state, page);
                auto const regressionScreens = regressionScreensForPage(
                    state.document(),
                    pageId
                );
                ImGui::SeparatorText("Regression screens");
                if (regressionScreens.empty() && !sample.has_value())
                {
                    ImGui::TextDisabled("none recorded");
                }
                for (auto const& screenId : regressionScreens)
                {
                    auto const isSample = sample.has_value() && *sample == screenId;
                    drawScreenRow(state, screenId, isSample ? "  (sample)" : "");
                    if (auto const* p_screen = findScreenCheck(state, screenId))
                    {
                        ImGui::SameLine();
                        auto const correct = (
                            p_screen->outcome == ScreenCheckOutcome::Correct
                        );
                        ImGui::TextColored(
                            correct ? k_passColor : k_failColor,
                            "%s",
                            screenCheckText(state, *p_screen).c_str()
                        );
                    }
                }

                // A page authored before its screen was recorded has an inferred
                // sample not in the list above -- and, until recorded, no verdict
                // to measure against. One press states it, which is what turns the
                // page's verdict on.
                if (
                    sample.has_value()
                    && !std::ranges::contains(regressionScreens, *sample)
                )
                {
                    drawScreenRow(state, *sample, "  (authoring sample)");
                    if (!claimedScreen(state, pageId).has_value())
                    {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Record this screen"))
                        {
                            recordScreenForPage(state, ui, pageId, *sample);
                        }
                    }
                }

                ImGui::SeparatorText("Identified by");
                if (view.has_value())
                {
                    for (auto const& row : view->identifiedBy)
                    {
                        if (
                            auto const* p_recognizer =
                                catalog.findRecognizer(row.id)
                        )
                        {
                            drawPageMemberRow(
                                state,
                                draft,
                                ui,
                                *p_recognizer,
                                sample,
                                pageId
                            );
                        }
                    }
                }
                ImGui::BeginDisabled(!sample.has_value());
                if (ImGui::SmallButton("+ Identifying mark") && sample.has_value())
                {
                    placeNewMember(
                        state,
                        ui,
                        pageId,
                        *sample,
                        PageMemberKind::Anchor
                    );
                }
                ImGui::EndDisabled();

                ImGui::SeparatorText("Interactive regions");
                if (view.has_value())
                {
                    for (auto const& row : view->regions)
                    {
                        if (
                            auto const* p_recognizer =
                                catalog.findRecognizer(row.id)
                        )
                        {
                            drawPageMemberRow(
                                state,
                                draft,
                                ui,
                                *p_recognizer,
                                sample,
                                pageId
                            );
                        }
                    }
                }
                ImGui::BeginDisabled(!sample.has_value());
                if (ImGui::SmallButton("+ Interactive region") && sample.has_value())
                {
                    placeNewMember(
                        state,
                        ui,
                        pageId,
                        *sample,
                        PageMemberKind::ActionTarget
                    );
                }
                ImGui::EndDisabled();

                // Info regions are placed through the same path with the info
                // kind; without this button they were reachable only by drawing an
                // interactive region and retyping it.
                ImGui::SameLine();
                ImGui::BeginDisabled(!sample.has_value());
                if (ImGui::SmallButton("+ Info region") && sample.has_value())
                {
                    placeNewMember(
                        state,
                        ui,
                        pageId,
                        *sample,
                        PageMemberKind::InfoRegion
                    );
                }
                ImGui::EndDisabled();

                // Where a shared element is dropped to say "this page has it
                // too". A visible strip rather than the list above it: a drop
                // target the author cannot see is one they have to find by
                // waving the mouse around.
                // Dimmed by colour rather than by the disabled flag: a disabled
                // item is not reliably a drop target, and a target the author
                // cannot drop onto is worse than one that looks clickable.
                // Its click does nothing, which is what the wording says.
                ImGui::SameLine();
                ImGui::PushStyleColor(
                    ImGuiCol_Text,
                    ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)
                );
                static_cast<void>(
                    ImGui::Selectable(
                        "  or drop a shared region here  ",
                        false,
                        ImGuiSelectableFlags_None,
                        ImVec2{240.0F, 0.0F}
                    )
                );
                ImGui::PopStyleColor();
                if (ImGui::BeginDragDropTarget())
                {
                    auto const* p_payload = ImGui::AcceptDragDropPayload(
                        k_sharedRegionPayload
                    );
                    if (p_payload != nullptr)
                    {
                        droppedOnPage = pageId;
                    }
                    ImGui::EndDragDropTarget();
                }

                // Info elements placed on this page. Drawn like interactive
                // regions so nothing placeable is invisible -- a retyped Info
                // region used to vanish from the tree and be reachable only by
                // undo.
                if (view.has_value() && !view->infos.empty())
                {
                    ImGui::SeparatorText("Info regions");
                    for (auto const& row : view->infos)
                    {
                        if (
                            auto const* p_recognizer =
                                catalog.findRecognizer(row.id)
                        )
                        {
                            drawPageMemberRow(
                                state,
                                draft,
                                ui,
                                *p_recognizer,
                                sample,
                                pageId
                            );
                        }
                    }
                }

                // A forbidden anchor is an exclusivity rule between two pages
                // rather than something authored on this screen, so it is only
                // shown once one exists.
                if (view.has_value() && !view->mustNotShow.empty())
                {
                    ImGui::SeparatorText("Must not show");
                    for (auto const& row : view->mustNotShow)
                    {
                        if (
                            auto const* p_recognizer =
                                catalog.findRecognizer(row.id)
                        )
                        {
                            drawPageMemberRow(
                                state,
                                draft,
                                ui,
                                *p_recognizer,
                                sample,
                                pageId
                            );
                        }
                    }
                }

                ImGui::TreePop();
                ImGui::PopID();
            }

            // The shared-region palette: a live drag source, kept a separate
            // group from the orphan bucket below (a tool next to a junk drawer is
            // the merge decision 4 refuses). Drop targets are the per-page strips
            // drawn above; the drag identity is remembered on ui.draggedRegion
            // because the frame the drop lands on is the frame the source stops
            // submitting itself.
            auto shared = std::vector<annotation::RecognizerId>{};
            for (auto const& recognizer : catalog.recognizers())
            {
                if (isRegionShared(state, recognizer.id()))
                {
                    shared.emplace_back(recognizer.id());
                }
            }
            if (!shared.empty())
            {
                ImGui::SeparatorText("Shared regions");
                ImGui::TextWrapped(
                    "Drag one onto a page that also has it. It only works where "
                    "the element looks the same; the drop says so straight away."
                );
                for (auto const& recognizer : catalog.recognizers())
                {
                    if (!std::ranges::contains(shared, recognizer.id()))
                    {
                        continue;
                    }
                    auto const pages = pagesPlacedOn(
                        draft,
                        recognizer.id()
                    ).size();
                    ImGui::Bullet();
                    ImGui::Selectable(
                        std::format(
                            "{}  (on {} {})",
                            recognizer.name().value(),
                            pages,
                            pages == 1U ? "page" : "pages"
                        ).c_str(),
                        false
                    );
                    if (ImGui::BeginDragDropSource())
                    {
                        // A one-byte payload: the type string is what gates the
                        // target, and the identity is remembered on the ui state
                        // rather than marshalled through a byte copy of a domain
                        // id.
                        auto const marker = uint8{1};
                        ImGui::SetDragDropPayload(
                            k_sharedRegionPayload,
                            &marker,
                            sizeof(marker)
                        );
                        ui.draggedRegion = recognizer.id();
                        ImGui::Text(
                            "put \"%s\" on a page",
                            recognizer.name().value().c_str()
                        );
                        ImGui::EndDragDropSource();
                    }
                }
            }

            // Anything the tree above cannot reach: an info region, or a
            // recognizer left behind by a retype or a page deletion. Without this
            // group it would be permanently unselectable.
            auto unassigned = std::vector<annotation::RecognizerId>{};
            for (auto const& recognizer : catalog.recognizers())
            {
                auto const listed = std::ranges::any_of(
                    catalog.pages(),
                    [&recognizer](annotation::PageSignature const& page)
                    {
                        return std::ranges::contains(
                                   page.required(),
                                   recognizer.id()
                               )
                            || std::ranges::contains(
                                   page.forbidden(),
                                   recognizer.id()
                               );
                    }
                );
                if (!listed && recognizer.allowedPageIds().empty())
                {
                    unassigned.emplace_back(recognizer.id());
                }
            }
            if (!unassigned.empty())
            {
                ImGui::SeparatorText("Not on any page");
                for (auto const& recognizer : catalog.recognizers())
                {
                    if (std::ranges::contains(unassigned, recognizer.id()))
                    {
                        // No page context, so these follow to the screen their
                        // template was cut from.
                        drawPageMemberRow(
                            state,
                            draft,
                            ui,
                            recognizer,
                            std::nullopt,
                            std::nullopt
                        );
                    }
                }
            }

            // Acted on after every panel row has been drawn, so the edit is
            // parked rather than committed while the tree is still reading the
            // document it would replace.
            if (droppedOnPage.has_value() && ui.draggedRegion.has_value())
            {
                requestSharedRegionOnPage(
                    state,
                    ui,
                    *ui.draggedRegion,
                    *droppedOnPage
                );
                ui.draggedRegion.reset();
            }

            ImGui::End();
        }


        auto handleRectEditing(
            AppState& state,
            PanelUiState& ui,
            ImDrawList& drawList,
            CanvasView view,
            CanvasPoint canvasOrigin,
            annotation::RecognizerDefinition const& definition,
            PixelRect const& searchRoi,
            std::optional<annotation::PageId> pageContext,
            uint32 sourceWidth,
            uint32 sourceHeight,
            bool hovered,
            bool templateEditable
        ) -> void
        {
            auto const recognizerId = definition.id();
            auto const templateRect = definition.templateRect();
            auto const mouse        = ImGui::GetIO().MousePos;

            if (
                ui.dragTarget == PanelUiState::CanvasDragTarget::None
                && hovered
                && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            )
            {
                // A template box drawn over a screen it was not cut from is a
                // shared element being edited from another page. Dragging it here
                // would recut the template from its own screen at coordinates
                // picked on this one, which is silently wrong, so only the search
                // region responds.
                auto const templateGrip = templateEditable
                    ? hitTestGrip(
                        rectScreenOrigin(view, canvasOrigin, templateRect),
                        static_cast<float>(templateRect.width()) * view.zoom,
                        static_cast<float>(templateRect.height()) * view.zoom,
                        CanvasPoint{mouse.x, mouse.y},
                        k_gripRadius
                    )
                    : std::nullopt;
                if (templateGrip.has_value())
                {
                    ui.dragTarget    = PanelUiState::CanvasDragTarget::TemplateRect;
                    ui.dragGrip      = templateGrip;
                    ui.dragStartRect = templateRect;
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
                        static_cast<float>(searchRoi.width()) * view.zoom,
                        static_cast<float>(searchRoi.height()) * view.zoom,
                        CanvasPoint{mouse.x, mouse.y},
                        k_gripRadius
                    );
                    if (roiGrip.has_value())
                    {
                        ui.dragTarget    = PanelUiState::CanvasDragTarget::SearchRoi;
                        ui.dragGrip      = roiGrip;
                        ui.dragStartRect = searchRoi;
                    }
                }
            }

            auto previewTemplate = templateRect;
            auto previewRoi      = searchRoi;
            if (
                ui.dragTarget != PanelUiState::CanvasDragTarget::None
                && ui.dragGrip.has_value()
                && ui.dragStartRect.has_value()
            )
            {
                auto const delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                auto const deltaX = static_cast<int32>(
                    std::lround(delta.x / view.zoom)
                );
                auto const deltaY = static_cast<int32>(
                    std::lround(delta.y / view.zoom)
                );
                auto const edited = resizeRectByGrip(
                    *ui.dragStartRect,
                    *ui.dragGrip,
                    deltaX,
                    deltaY,
                    sourceWidth,
                    sourceHeight
                );
                if (edited.has_value())
                {
                    if (ui.dragTarget == PanelUiState::CanvasDragTarget::TemplateRect)
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
                            pageContext,
                            *edited
                        );
                    }
                }
                else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                {
                    ui.dragTarget = PanelUiState::CanvasDragTarget::None;
                    ui.dragGrip.reset();
                    ui.dragStartRect.reset();
                }
            }

            drawRectWithGrips(
                drawList,
                view,
                canvasOrigin,
                previewRoi,
                k_searchRoiColor
            );
            if (templateEditable)
            {
                drawRectWithGrips(
                    drawList,
                    view,
                    canvasOrigin,
                    previewTemplate,
                    k_templateColor
                );
            }
            else
            {
                // Shown without grips and in outline only: it says where the
                // template sits on the screen it was cut from, which is a
                // different image from the one underneath it.
                drawRectOutline(
                    drawList,
                    view,
                    canvasOrigin,
                    previewTemplate,
                    k_foreignTemplateColor
                );
            }
        }

        // Draws one recognizer's matched rectangle as recognition evidence: the
        // place a template was found on the shown screen, snapped to whole screen
        // pixels and labelled with the recognizer and whether it passed. Rows with
        // no matched rectangle -- a search the budget stopped -- draw nothing.
        auto drawMatchOverlay(
            ImDrawList& drawList,
            CanvasView view,
            CanvasPoint canvasOrigin,
            PreviewAnchorRow const& row
        ) -> void
        {
            if (!row.matchedRect.has_value())
            {
                return;
            }
            auto const bounds = snappedScreenBounds(
                view,
                canvasOrigin,
                *row.matchedRect
            );
            auto const color = row.hit ? k_matchHitColor : k_matchMissColor;
            drawList.AddRect(
                ImVec2{bounds.left, bounds.top},
                ImVec2{bounds.right, bounds.bottom},
                color,
                0.0F,
                0,
                2.0F
            );
            auto const label = std::format(
                "{} {}",
                shortId(row.recognizerId.value()),
                row.hit ? "hit" : "miss"
            );
            drawList.AddText(
                ImVec2{bounds.left, bounds.top - ImGui::GetTextLineHeight()},
                color,
                label.c_str()
            );
        }

        // Draws every matched rectangle the last preview found on the shown
        // screen. The preview owns the evidence -- it is read through the same
        // stored result the Actions panel shows as text -- and is drawn only when
        // it was evaluated against the screen on display, so a stale preview from
        // another screen never paints boxes over unrelated pixels. Model-check
        // margins are scores rather than rectangles, so the canvas evidence comes
        // from the preview alone.
        auto drawEvidenceOverlay(
            AppState const& state,
            ImDrawList& drawList,
            CanvasView view,
            CanvasPoint canvasOrigin,
            annotation::SourceId shownScreen
        ) -> void
        {
            auto const& preview = state.lastPreview();
            if (!preview.has_value() || preview->sourceId != shownScreen)
            {
                return;
            }
            for (auto const& row : preview->anchorRows)
            {
                drawMatchOverlay(drawList, view, canvasOrigin, row);
            }
            if (preview->actionEvidence.has_value())
            {
                drawMatchOverlay(
                    drawList,
                    view,
                    canvasOrigin,
                    *preview->actionEvidence
                );
            }
        }

        // One member of the shown page as the canvas draws and hit-tests it: the
        // element's template rectangle, this page's own search region, and whether
        // the template may be edited over the shown screen (only over the screen it
        // was cut from). Assembled per frame from the page view.
        struct DrawnMember final
        {
            annotation::RecognizerId   id;
            PixelRect                  templateRect;
            PixelRect                  searchRoi;
            annotation::AnnotationType type{};
            bool                       templateEditable{};
        };

        [[nodiscard]]
        auto collectDrawnMembers(
            AppState const& state,
            annotation::PageId pageId,
            annotation::SourceId shownScreen
        ) -> std::vector<DrawnMember>
        {
            auto members = std::vector<DrawnMember>{};
            auto const view = PageView::of(state.draft(), pageId);
            if (!view.has_value())
            {
                return members;
            }
            auto const editable = [&](annotation::RecognizerId id) -> bool
            {
                auto const authored = sourceOfRecognizer(state, id);
                return authored.has_value() && *authored == shownScreen;
            };
            for (auto const& row : view->identifiedBy)
            {
                members.emplace_back(
                    DrawnMember{
                        .id               = row.id,
                        .templateRect     = row.templateRect,
                        .searchRoi        = row.searchRoi,
                        .type             = annotation::AnnotationType::PageAnchor,
                        .templateEditable = editable(row.id),
                    }
                );
            }
            for (auto const& row : view->regions)
            {
                members.emplace_back(
                    DrawnMember{
                        .id               = row.id,
                        .templateRect     = row.templateRect,
                        .searchRoi        = row.searchRoiOnThisPage,
                        .type             = annotation::AnnotationType::ActionTarget,
                        .templateEditable = editable(row.id),
                    }
                );
            }
            for (auto const& row : view->infos)
            {
                members.emplace_back(
                    DrawnMember{
                        .id               = row.id,
                        .templateRect     = row.templateRect,
                        .searchRoi        = row.searchRoiOnThisPage,
                        .type             = annotation::AnnotationType::InfoRegion,
                        .templateEditable = editable(row.id),
                    }
                );
            }
            return members;
        }

        // Changes a recognizer's type as one transaction through the free-function
        // retype path, reporting the same summary the inspector's type combo does.
        // Shared by that combo and the canvas context menu's Retype submenu.
        auto requestRetype(
            AppState& state,
            PanelUiState& ui,
            annotation::RecognizerId id,
            annotation::AnnotationType type,
            char const* typeName
        ) -> void
        {
            auto retyped = retypeRecognizer(state.draft(), id, type);
            if (!retyped)
            {
                ui.report(
                    LogSeverity::Error,
                    std::format(
                        "type change rejected: {}",
                        toString(retyped.error())
                    )
                );
                return;
            }
            // Summarized against the pre-commit document so a page named in the
            // summary is resolved before the edit lands.
            auto summary = retypeSummary(state, *retyped, typeName);
            requestEdit(ui, std::move(retyped->draft), std::move(summary));
        }

        // Creates a member from a rubber-banded rectangle on the shown page and
        // selects it once the commit lands. One transaction: the drawn template and
        // its seeded search region ride in together, since the one-commit-per-frame
        // queue rejects a create-then-retemplate pair.
        auto createDrawnMember(
            AppState& state,
            PanelUiState& ui,
            annotation::PageId pageId,
            annotation::SourceId sourceId,
            PageMemberKind kind,
            PixelRect templateRect
        ) -> void
        {
            auto page = EditPage::open(state, pageId);
            if (!page)
            {
                ui.report(
                    LogSeverity::Error,
                    std::format("draw failed: {}", toString(page.error()))
                );
                return;
            }
            auto added = page->placeDrawn(
                EditPage::NewDrawnMemberSpec{
                    .sourceId     = sourceId,
                    .kind         = kind,
                    .templateRect = templateRect,
                }
            );
            if (!added)
            {
                ui.report(
                    LogSeverity::Error,
                    std::format("draw failed: {}", toString(added.error()))
                );
                return;
            }
            auto const newId = added->id;
            std::move(*page).commitSelecting(
                ui,
                newMemberDescription(added->name, kind),
                newId,
                sourceId
            );
        }

        // Withdraws a member's placement through the action-layer closure rule and
        // parks the resulting draft for the frame's single commit.
        auto removeMemberFromPage(
            AppState& state,
            PanelUiState& ui,
            annotation::RecognizerId id,
            annotation::PageId pageId,
            std::string_view name
        ) -> void
        {
            auto removed = removePlacementFromPage(state.draft(), id, pageId);
            if (!removed)
            {
                ui.report(
                    LogSeverity::Error,
                    std::format(
                        "remove rejected: {}",
                        toString(removed.error())
                    )
                );
                return;
            }
            requestEdit(
                ui,
                std::move(*removed),
                std::format(
                    "removed \"{}\" from page \"{}\"",
                    name,
                    pageName(state, pageId)
                )
            );
        }

        // The element context menu: Select is implicit (the right-click already
        // selected it), then Duplicate, a Retype submenu, and the two removals.
        // Regression recording is a screen concern and is deliberately absent here.
        auto drawCanvasElementMenu(AppState& state, PanelUiState& ui) -> void
        {
            if (!ImGui::BeginPopup("canvas-element-menu"))
            {
                return;
            }
            if (!ui.contextMenuTarget.has_value())
            {
                ImGui::EndPopup();
                return;
            }
            auto const id          = *ui.contextMenuTarget;
            auto const* definition = state.document().catalog().findRecognizer(id);
            if (definition == nullptr)
            {
                ImGui::TextDisabled("(element is gone)");
                ImGui::EndPopup();
                return;
            }
            auto const name = definition->name().value();
            auto const type = definition->annotationType();
            ImGui::TextDisabled("%s", name.c_str());
            ImGui::Separator();

            if (ImGui::Selectable("Duplicate"))
            {
                requestDuplicateElement(state, ui, id);
            }
            if (ImGui::BeginMenu("Retype"))
            {
                for (
                    auto index = std::size_t{0};
                    index < k_annotationTypeItems.size();
                    ++index
                )
                {
                    auto const target =
                        static_cast<annotation::AnnotationType>(index);
                    auto const isCurrent = target == type;
                    if (
                        ImGui::MenuItem(
                            k_annotationTypeItems.at(index),
                            nullptr,
                            isCurrent,
                            !isCurrent
                        )
                    )
                    {
                        requestRetype(
                            state,
                            ui,
                            id,
                            target,
                            k_annotationTypeItems.at(index)
                        );
                    }
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();
            auto const canRemove = ui.contextMenuPage.has_value()
                && type != annotation::AnnotationType::PageAnchor;
            ImGui::BeginDisabled(!canRemove);
            if (ImGui::Selectable("Remove from this page") && canRemove)
            {
                removeMemberFromPage(
                    state,
                    ui,
                    id,
                    *ui.contextMenuPage,
                    name
                );
            }
            ImGui::EndDisabled();
            if (ImGui::Selectable("Delete everywhere"))
            {
                requestDeleteEverywhere(state, ui, id);
            }
            ImGui::EndPopup();
        }

        // The background context menu: the screen-scoped regression recordings, the
        // same requestScreenExpectation actions the inspector offers under a screen
        // selection.
        auto drawCanvasBackgroundMenu(
            AppState& state,
            PanelUiState& ui,
            annotation::SourceId shownScreen
        ) -> void
        {
            if (!ImGui::BeginPopup("canvas-background-menu"))
            {
                return;
            }
            ImGui::TextDisabled("Record this screen as");
            ImGui::Separator();
            if (ImGui::Selectable("None of the pages"))
            {
                requestScreenExpectation(
                    state,
                    ui,
                    shownScreen,
                    PagelessExpectation::Unknown
                );
            }
            if (ImGui::Selectable("Ambiguous"))
            {
                requestScreenExpectation(
                    state,
                    ui,
                    shownScreen,
                    PagelessExpectation::Ambiguous
                );
            }
            ImGui::EndPopup();
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
                ImGui::TextUnformatted("Select a screen or element to view it.");
                ImGui::End();
                return;
            }

            auto const assets = state.sources();
            auto const asset  = std::ranges::find(
                assets,
                *selectedSource,
                &annotation::AuthoringSourceAsset::id
            );
            if (asset == assets.end())
            {
                ImGui::TextUnformatted("Selected source has no image bytes.");
                ImGui::End();
                return;
            }

            auto const texture = services.textureFor(*asset);
            if (!texture)
            {
                ImGui::TextUnformatted("Failed to decode the source image.");
                ImGui::End();
                return;
            }

            auto const cursor = ImGui::GetCursorScreenPos();
            auto const region = ImGui::GetContentRegionAvail();
            // Reserve one line at the bottom for the navigation strip, so its
            // buttons never overlap the canvas surface -- an overlapping button and
            // canvas gesture would both fire on the same press.
            auto const footer = ImGui::GetFrameHeightWithSpacing();
            auto const size   = ImVec2{
                std::max(region.x, 64.0F),
                std::max(region.y - footer, 64.0F),
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
                    under.x,
                    under.y,
                    view.zoom * std::pow(k_zoomWheelBase, wheel)
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
                ImVec2{canvasOrigin.x, canvasOrigin.y},
                ImVec2{canvasOrigin.x + size.x, canvasOrigin.y + size.y},
                true
            );

            auto const topLeft = sourceToScreen(view, canvasOrigin, 0.0F, 0.0F);
            auto const bottomRight = sourceToScreen(
                view,
                canvasOrigin,
                static_cast<float>(texture->width),
                static_cast<float>(texture->height)
            );
            drawList->AddImage(
                static_cast<ImTextureID>(texture->textureHandle),
                ImVec2{topLeft.x, topLeft.y},
                ImVec2{bottomRight.x, bottomRight.y}
            );

            // The page whose members the canvas shows: the one an element is
            // selected under, else the one that claims this screen. Without it
            // there is nothing to draw beyond the selected element and no page to
            // draw-to-create onto.
            auto const shownPage = shownPageForScreen(
                state,
                *selectedSource,
                state.selection().pageContext()
            );
            auto const members = shownPage.has_value()
                ? collectDrawnMembers(state, *shownPage, *selectedSource)
                : std::vector<DrawnMember>{};
            auto const selectedId = state.selectedRecognizerId();

            // The template rectangles are the click targets; the search regions are
            // drawn but not hit-tested, since a whole-frame range would swallow
            // every empty-space press and leave nothing to rubber-band on.
            auto templateRects = std::vector<PixelRect>{};
            templateRects.reserve(members.size());
            for (auto const& member : members)
            {
                templateRects.emplace_back(member.templateRect);
            }

            // Every member of the shown page in a muted style; the selected one is
            // drawn strong, with grips, by handleRectEditing just below.
            for (auto const& member : members)
            {
                if (selectedId.has_value() && *selectedId == member.id)
                {
                    continue;
                }
                drawRectOutline(
                    *drawList,
                    view,
                    canvasOrigin,
                    member.searchRoi,
                    k_mutedSearchRoiColor
                );
                drawCornerBrackets(
                    *drawList,
                    view,
                    canvasOrigin,
                    member.templateRect,
                    member.templateEditable
                        ? k_memberCornerColor
                        : k_foreignCornerColor,
                    k_memberOutlineColor
                );
            }

            if (selectedId.has_value())
            {
                auto const* definition =
                    state.document().catalog().findRecognizer(*selectedId);
                if (definition != nullptr)
                {
                    // The template is only editable over the screen it was cut
                    // from. For a shared element seen from another page they are
                    // different images.
                    auto const authoredOn = sourceOfRecognizer(state, *selectedId);
                    // The page that places this region: the one the element was
                    // selected under when it carries a context, otherwise the one
                    // that claims this screen. The canvas then draws and edits
                    // that page's own search range, so editing it on one page does
                    // not move it on the others; with no page the element's
                    // default range is shown.
                    auto const context = placementContext(
                        state,
                        *selectedId,
                        *selectedSource,
                        state.selection().pageContext()
                    );
                    auto const searchRoi = context.has_value()
                        ? context->searchRoi
                        : definition->searchRoi();
                    auto const pageContext = context.has_value()
                        ? std::optional<annotation::PageId>{context->page}
                        : std::optional<annotation::PageId>{};
                    handleRectEditing(
                        state,
                        ui,
                        *drawList,
                        view,
                        canvasOrigin,
                        *definition,
                        searchRoi,
                        pageContext,
                        texture->width,
                        texture->height,
                        hovered,
                        authoredOn == selectedSource
                    );
                }
            }

            // The gesture machine follows a grip drag handleRectEditing owns: while
            // a grip is held it is GripEditing, and it falls back to Idle the frame
            // the grip is let go.
            if (ui.dragTarget != PanelUiState::CanvasDragTarget::None)
            {
                ui.canvasGesture = PanelUiState::CanvasGesture::GripEditing;
            }
            else if (ui.canvasGesture == PanelUiState::CanvasGesture::GripEditing)
            {
                ui.canvasGesture = PanelUiState::CanvasGesture::Idle;
            }

            auto const mouse       = ImGui::GetIO().MousePos;
            auto const mouseSource = screenToSource(
                view,
                canvasOrigin,
                mouse.x,
                mouse.y
            );

            // Left press arbitration, only when a grip did not already claim it: a
            // hit on a drawn template selects (cycling through an overlap), and an
            // empty press on a page begins a rubber-band; on an unclassified screen
            // it names the fix instead.
            if (
                hovered
                && ui.canvasGesture == PanelUiState::CanvasGesture::Idle
                && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            )
            {
                auto const hits = rectsUnderPoint(
                    templateRects,
                    mouseSource.x,
                    mouseSource.y
                );
                if (!hits.empty())
                {
                    auto current = std::optional<std::size_t>{};
                    for (
                        auto index = std::size_t{0};
                        index < members.size();
                        ++index
                    )
                    {
                        if (
                            selectedId.has_value()
                            && members[index].id == *selectedId
                        )
                        {
                            current = index;
                        }
                    }
                    if (auto const next = nextRectInCycle(hits, current))
                    {
                        selectRecognizer(
                            state,
                            members[*next].id,
                            *selectedSource,
                            shownPage
                        );
                    }
                }
                else if (shownPage.has_value())
                {
                    ui.canvasGesture         = PanelUiState::CanvasGesture::PressPending;
                    ui.rubberBandStartSource = mouseSource;
                }
                else
                {
                    ui.report(
                        LogSeverity::Info,
                        "classify this screen into a page first, then draw "
                        "elements on it"
                    );
                }
            }

            // A press below the drag threshold stays undecided; past it becomes a
            // rubber-band, and a release before then is a click that selected
            // nothing.
            if (ui.canvasGesture == PanelUiState::CanvasGesture::PressPending)
            {
                if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                {
                    ui.canvasGesture = PanelUiState::CanvasGesture::Idle;
                    ui.rubberBandStartSource.reset();
                }
                else
                {
                    auto const delta =
                        ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                    if (exceedsDragThreshold(delta.x, delta.y, k_dragThreshold))
                    {
                        ui.canvasGesture =
                            PanelUiState::CanvasGesture::RubberBanding;
                    }
                }
            }

            // The rubber-band is rebuilt from its anchor to the cursor every frame
            // and, on release, hands the drawn rectangle to the type picker.
            if (
                ui.canvasGesture == PanelUiState::CanvasGesture::RubberBanding
                && ui.rubberBandStartSource.has_value()
            )
            {
                auto const rect = rubberBandRect(
                    ui.rubberBandStartSource->x,
                    ui.rubberBandStartSource->y,
                    mouseSource.x,
                    mouseSource.y,
                    texture->width,
                    texture->height
                );
                if (rect.has_value())
                {
                    drawRectOutline(
                        *drawList,
                        view,
                        canvasOrigin,
                        *rect,
                        k_rubberBandColor
                    );
                }
                if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                {
                    if (rect.has_value())
                    {
                        ui.pendingCreateRect = rect;
                        ImGui::OpenPopup("canvas-create-kind");
                    }
                    ui.canvasGesture = PanelUiState::CanvasGesture::Idle;
                    ui.rubberBandStartSource.reset();
                }
            }

            // Escape abandons any gesture in progress -- a rubber-band, or a grip
            // drag not yet released and committed.
            if (
                ui.canvasGesture != PanelUiState::CanvasGesture::Idle
                && ImGui::IsKeyPressed(ImGuiKey_Escape)
            )
            {
                ui.canvasGesture = PanelUiState::CanvasGesture::Idle;
                ui.rubberBandStartSource.reset();
                ui.dragTarget = PanelUiState::CanvasDragTarget::None;
                ui.dragGrip.reset();
                ui.dragStartRect.reset();
            }

            // Right press opens a context menu: the element's when it lands on a
            // drawn template (selecting it first), the screen's otherwise.
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                auto const hits = rectsUnderPoint(
                    templateRects,
                    mouseSource.x,
                    mouseSource.y
                );
                if (!hits.empty())
                {
                    auto const index = hits.front();
                    selectRecognizer(
                        state,
                        members[index].id,
                        *selectedSource,
                        shownPage
                    );
                    ui.contextMenuTarget = members[index].id;
                    ui.contextMenuPage   = shownPage;
                    ImGui::OpenPopup("canvas-element-menu");
                }
                else
                {
                    ui.contextMenuTarget.reset();
                    ui.contextMenuPage.reset();
                    ImGui::OpenPopup("canvas-background-menu");
                }
            }

            // Recognition evidence on top of the authored boxes: where the last
            // preview actually found each template on this screen.
            drawEvidenceOverlay(
                state,
                *drawList,
                view,
                canvasOrigin,
                *selectedSource
            );

            drawList->PopClipRect();

            // The type picker for a just-drawn rectangle. Dismissing it by clicking
            // away drops the held rectangle so no stale draw survives to next time.
            if (ImGui::BeginPopup("canvas-create-kind"))
            {
                auto const create = [&](PageMemberKind kind)
                {
                    if (ui.pendingCreateRect.has_value() && shownPage.has_value())
                    {
                        createDrawnMember(
                            state,
                            ui,
                            *shownPage,
                            *selectedSource,
                            kind,
                            *ui.pendingCreateRect
                        );
                    }
                    ui.pendingCreateRect.reset();
                };
                ImGui::TextDisabled("Create here");
                ImGui::Separator();
                if (ImGui::Selectable("Identifying mark"))
                {
                    create(PageMemberKind::Anchor);
                }
                if (ImGui::Selectable("Interactive region"))
                {
                    create(PageMemberKind::ActionTarget);
                }
                if (ImGui::Selectable("Info region"))
                {
                    create(PageMemberKind::InfoRegion);
                }
                ImGui::Separator();
                if (ImGui::Selectable("Cancel"))
                {
                    ui.pendingCreateRect.reset();
                }
                ImGui::EndPopup();
            }
            else if (ui.pendingCreateRect.has_value())
            {
                ui.pendingCreateRect.reset();
            }

            drawCanvasElementMenu(state, ui);
            drawCanvasBackgroundMenu(state, ui, *selectedSource);

            // The navigation strip on the reserved footer line: the zoom read-out
            // and the Fit / 100% buttons, so wheel and middle-drag are no longer
            // the only way to reach a fit or a reset.
            if (ImGui::Button("Fit"))
            {
                state.setCanvasView(
                    fitCanvasView(
                        texture->width,
                        texture->height,
                        size.x,
                        size.y
                    )
                );
            }
            ImGui::SameLine();
            if (ImGui::Button("100%"))
            {
                state.setCanvasView(
                    centeredCanvasView(
                        1.0F,
                        texture->width,
                        texture->height,
                        size.x,
                        size.y
                    )
                );
            }
            ImGui::SameLine();
            ImGui::Text(
                "%d%%",
                static_cast<int>(std::lround(view.zoom * 100.0F))
            );

            ImGui::End();
        }

        // The selected element's fields, copied once at the top of the properties
        // panel. The panel and its helpers read this instead of a
        // RecognizerDefinition const* borrowed from the live document: any edit the
        // panel parks is applied after it finishes drawing, and applying rebuilds
        // the document, so a pointer held across the panel's widgets would be a
        // use-after-free of the kind docs/pitfalls/workbench-authoring-ui.md
        // records. A value snapshot cannot dangle.
        struct SelectedElement final
        {
            annotation::RecognizerId              id;
            std::string                           name{};
            annotation::AnnotationType            type{};
            uint32                                thresholdBasisPoints{};
            PixelRect                             templateRect;
            std::optional<EditableTemplateOffset> defaultClick{};
            std::vector<annotation::PageId>       allowedPages{};
        };

        // What a shared element's copy needs said about it. Putting it on another
        // page is a drag from Shared regions in the Pages panel, not a control
        // here: the author is choosing a page, and the pages are over there.
        auto drawSharing(
            AppState& state,
            SelectedElement const& element
        ) -> void
        {
            if (element.type != annotation::AnnotationType::ActionTarget)
            {
                return;
            }

            auto const pages = pagesPlacedOn(state.draft(), element.id);
            if (pages.size() <= 1U)
            {
                return;
            }

            ImGui::SeparatorText("Shared across pages");
            ImGui::TextWrapped(
                "Drawn once, used on %zu pages. Moving the template box moves it "
                "on all of them; each page keeps its own search range.",
                pages.size()
            );

            // The canvas dims a template box it will not let the author drag.
            // Saying why beats leaving them to work it out from the colour.
            auto const authoredOn = sourceOfRecognizer(state, element.id);
            if (authoredOn.has_value() && authoredOn != state.selectedSourceId())
            {
                ImGui::TextWrapped(
                    "The template was cut from screen %s, so only the search "
                    "range is editable here. Select this element under that "
                    "screen's page to move the template.",
                    shortId(authoredOn->value()).c_str()
                );
            }
        }

        // The recognizer's type, changed as one transaction. Nothing on the main
        // path sets it -- a member is typed by the group it was created in -- so
        // this exists to repair a mistake or to move an element between the kinds;
        // it sits with the other identity fields.
        auto drawTypeCombo(
            AppState& state,
            PanelUiState& ui,
            SelectedElement const& element
        ) -> void
        {
            auto typeIndex = static_cast<int>(
                std::to_underlying(element.type)
            );
            if (
                !ImGui::Combo(
                    "Type",
                    &typeIndex,
                    k_annotationTypeItems.data(),
                    static_cast<int>(k_annotationTypeItems.size())
                )
            )
            {
                return;
            }

            // The type and the fields the catalog ties to it have to move
            // together, so this goes through the shared retype path rather than
            // writing the field and committing.
            requestRetype(
                state,
                ui,
                element.id,
                static_cast<annotation::AnnotationType>(typeIndex),
                k_annotationTypeItems.at(static_cast<std::size_t>(typeIndex))
            );
        }

        auto drawPageMembership(
            AppState& state,
            PanelUiState& ui,
            SelectedElement const& element
        ) -> void
        {
            auto const recognizerId = element.id;
            // Two unrelated relationships, kept apart by the catalog's rules: the
            // checkbox is permission to click this element on that page, and the
            // combo is the part this mark plays in identifying it. A page anchor
            // may only do the second and every other type only the first, so each
            // is disabled where it cannot apply. Both are consequences of the
            // page group a member was created in; editing them by hand is for
            // sharing one element across pages and for exclusivity rules.
            auto const isAnchor = (
                element.type == annotation::AnnotationType::PageAnchor
            );
            for (auto const& page : state.document().catalog().pages())
            {
                auto const pageId = page.id();
                ImGui::PushID(page.name().value().c_str());

                auto member = std::ranges::contains(
                    element.allowedPages,
                    pageId
                );
                ImGui::BeginDisabled(isAnchor);
                if (ImGui::Checkbox(page.name().value().c_str(), &member))
                {
                    if (member)
                    {
                        // Authorize through EditPage::placeExisting, which seeds
                        // the placement's per-page search region from the
                        // element's own.
                        auto opened = EditPage::open(state, pageId);
                        if (!opened)
                        {
                            ui.report(
                                LogSeverity::Error,
                                std::format(
                                    "place rejected: {}",
                                    toString(opened.error())
                                )
                            );
                        }
                        else if (
                            auto const status = opened->placeExisting(recognizerId);
                            !status
                        )
                        {
                            ui.report(
                                LogSeverity::Error,
                                std::format(
                                    "place rejected: {}",
                                    toString(status.error())
                                )
                            );
                        }
                        else
                        {
                            std::move(*opened).commit(
                                ui,
                                std::format(
                                    "placed \"{}\" on page \"{}\"",
                                    element.name,
                                    page.name().value()
                                )
                            );
                        }
                    }
                    else
                    {
                        auto removed = removePlacementFromPage(
                            state.draft(),
                            recognizerId,
                            pageId
                        );
                        if (!removed)
                        {
                            ui.report(
                                LogSeverity::Error,
                                std::format(
                                    "remove rejected: {}",
                                    toString(removed.error())
                                )
                            );
                        }
                        else
                        {
                            requestEdit(
                                ui,
                                std::move(*removed),
                                std::format(
                                    "removed \"{}\" from page \"{}\"",
                                    element.name,
                                    page.name().value()
                                )
                            );
                        }
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
                        draft.pages,
                        pageId,
                        &EditablePage::id
                    );
                    if (spot != draft.pages.end())
                    {
                        std::erase(spot->required, recognizerId);
                        std::erase(spot->forbidden, recognizerId);
                        if (role == 1)
                        {
                            spot->required.emplace_back(recognizerId);
                        }
                        else if (role == 2)
                        {
                            spot->forbidden.emplace_back(recognizerId);
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
            if (!selectedSource.has_value())
            {
                ImGui::TextUnformatted(
                    "Select a screen to record a regression case."
                );
                return;
            }

            auto const cases    = state.document().regressions();
            auto const existing = std::ranges::find(
                cases,
                *selectedSource,
                &annotation::RegressionCase::sourceId
            );

            // What this screen is recorded as, if anything, so the author sees
            // the state before changing it. Resolving to a page is authored from
            // the Pages panel; the buttons here record the two pageless cases,
            // which is what could not be recorded from the GUI before.
            if (existing == cases.end())
            {
                ImGui::TextUnformatted("No regression case for this screen yet.");
            }
            else
            {
                auto const& expectation = existing->expectation();
                if (
                    auto const* p_resolved =
                        std::get_if<annotation::ResolvedRegression>(&expectation)
                )
                {
                    ImGui::Text(
                        "Recorded: resolves to page %s.",
                        pageName(state, p_resolved->pageId).c_str()
                    );
                }
                else if (
                    std::holds_alternative<annotation::UnknownRegression>(
                        expectation
                    )
                )
                {
                    ImGui::TextUnformatted("Recorded: none of the pages.");
                }
                else
                {
                    ImGui::TextUnformatted("Recorded: ambiguous.");
                }
            }

            if (ImGui::Button("Record: none of the pages"))
            {
                requestScreenExpectation(
                    state,
                    ui,
                    *selectedSource,
                    PagelessExpectation::Unknown
                );
            }
            ImGui::SameLine();
            if (ImGui::Button("Record: ambiguous"))
            {
                requestScreenExpectation(
                    state,
                    ui,
                    *selectedSource,
                    PagelessExpectation::Ambiguous
                );
            }

            if (existing == cases.end())
            {
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
                    draft.regressions,
                    existing->id(),
                    &EditableRegression::id
                );
                if (spot != draft.regressions.end())
                {
                    spot->classification =
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

        // The element half of the Inspector: the selected recognizer's fields,
        // unchanged from the former Properties panel. Draws no window chrome and
        // no regression section -- the dispatcher owns the window, and regression
        // is a screen concern shown under a screen selection.
        auto drawElementInspector(
            AppState& state,
            PanelUiState& ui,
            annotation::RecognizerId recognizerId
        ) -> void
        {
            auto const* definition = state.document().catalog().findRecognizer(
                recognizerId
            );
            if (definition == nullptr)
            {
                ImGui::TextUnformatted("The selected element is gone.");
                return;
            }

            // Copy every field the panel reads off the document now, before any
            // widget below parks an edit. applyPendingEdit rebuilds the document
            // after this panel returns, so a RecognizerDefinition const* kept
            // across the widgets would dangle the instant one committed
            // (docs/pitfalls/workbench-authoring-ui.md). A value snapshot cannot,
            // and every helper below reads it rather than the live pointer.
            auto const element = SelectedElement{
                .id                   = recognizerId,
                .name                 = definition->name().value(),
                .type                 = definition->annotationType(),
                .thresholdBasisPoints = definition->threshold().basisPoints(),
                .templateRect         = definition->templateRect(),
                .defaultClick         = definition->defaultClick().has_value()
                    ? std::optional<EditableTemplateOffset>{
                        EditableTemplateOffset{
                            .x = definition->defaultClick()->x(),
                            .y = definition->defaultClick()->y(),
                        }
                    }
                    : std::nullopt,
                .allowedPages = {
                    definition->allowedPageIds().begin(),
                    definition->allowedPageIds().end(),
                },
            };

            // Reseed the name field from the document when the selection moves
            // to another recognizer, or when the document's name for it diverges
            // from what the buffer was seeded with (an undo, redo, or external
            // rename) and the user is not mid-edit. m_nameInputActive records
            // whether the field held focus on the previous frame: IsItemActive
            // read here would report the item drawn before the field, not the
            // field itself, which is only submitted below.
            auto const& currentName = element.name;
            if (
                ui.nameBufferFor != recognizerId
                || (
                    ui.nameSeededValue != currentName
                    && !ui.nameInputActive
                )
            )
            {
                seedBuffer(ui.nameBuffer, currentName);
                ui.nameBufferFor   = recognizerId;
                ui.nameSeededValue = currentName;
            }
            ImGui::InputText(
                "Name",
                ui.nameBuffer.data(),
                ui.nameBuffer.size()
            );
            ui.nameInputActive = ImGui::IsItemActive();
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                auto draft       = state.draft();
                auto* recognizer = findEditableRecognizer(draft, recognizerId);
                if (recognizer != nullptr)
                {
                    recognizer->name = std::string{ui.nameBuffer.data()};
                    auto description = std::format(
                        "renamed \"{}\" to \"{}\"",
                        currentName,
                        recognizer->name
                    );
                    requestEdit(ui, std::move(draft), std::move(description));
                }
            }

            // A copy of the selected element, minted as an independent second
            // element on the same screen. One undo entry removes it.
            if (ImGui::Button("Duplicate"))
            {
                requestDuplicateElement(state, ui, element.id);
            }
            // Delete everywhere, behind the same confirmation the tree, canvas
            // menu, and Delete key raise. A refusal (an anchor a page depends on)
            // surfaces without a confirmation that could not have committed.
            ImGui::SameLine();
            if (ImGui::Button("Delete"))
            {
                requestDeleteEverywhere(state, ui, element.id);
            }

            // The type sits here with the other identity fields rather than under
            // an Advanced header a real author could not find.
            drawTypeCombo(state, ui, element);

            // Page membership is promoted out of Advanced to near the top, for the
            // same reason: it is where the author says which pages an element
            // belongs to, and it was going unfound.
            ImGui::SeparatorText("On pages");
            drawPageMembership(state, ui, element);

            ImGui::SeparatorText("Detection");

            // Edited as a percentage to two decimals; persisted as integer basis
            // points. The document never sees the float -- the commit rounds it to
            // the nearest basis point (design lock OQ-1 / §1.4).
            auto thresholdPercent = thresholdPercentFromBasisPoints(
                element.thresholdBasisPoints
            );
            ImGui::InputFloat("Threshold (%)", &thresholdPercent, 0.0F, 0.0F, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                auto const basisPoints = thresholdBasisPointsFromPercent(
                    thresholdPercent
                );
                auto draft       = state.draft();
                auto* recognizer = findEditableRecognizer(draft, element.id);
                if (recognizer != nullptr)
                {
                    recognizer->similarityBasisPoints = basisPoints;
                    requestEdit(
                        ui,
                        std::move(draft),
                        std::format(
                            "threshold set to {:.2f}%",
                            static_cast<double>(
                                thresholdPercentFromBasisPoints(basisPoints)
                            )
                        )
                    );
                }
            }

            // Only an action target may define a default click, so the toggle is
            // unavailable for the other types instead of committing a rejected
            // edit.
            auto const isActionTarget = (
                element.type == annotation::AnnotationType::ActionTarget
            );
            auto hasClickOffset = element.defaultClick.has_value();
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
                auto* recognizer = findEditableRecognizer(draft, element.id);
                if (recognizer != nullptr)
                {
                    recognizer->defaultClick = hasClickOffset
                        ? std::optional<EditableTemplateOffset>{
                            EditableTemplateOffset{.x = 0U, .y = 0U}
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
            if (auto const offset = element.defaultClick)
            {
                auto const templateRect = element.templateRect;
                auto offsetX = static_cast<int>(offset->x);
                auto offsetY = static_cast<int>(offset->y);
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
                    auto* recognizer = findEditableRecognizer(draft, element.id);
                    if (recognizer != nullptr)
                    {
                        recognizer->defaultClick = EditableTemplateOffset{
                            .x = static_cast<uint32>(offsetX),
                            .y = static_cast<uint32>(offsetY),
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

            drawSharing(state, element);
        }

        // The screen half of the Inspector: what the screen is recorded as and
        // the controls to change it. The regression section already keys off the
        // selected source, which under a Screen selection is this screen, so it is
        // reused as-is; deletion lives here too, where the former Screens panel
        // put it.
        auto drawScreenInspector(
            AppState& state,
            PanelUiState& ui,
            annotation::SourceId sourceId
        ) -> void
        {
            if (auto const* source = findSource(state, sourceId))
            {
                auto const provenance = std::holds_alternative<
                    annotation::WgcSourceProvenance
                >(source->provenance()) ? "wgc" : "imported";
                ImGui::Text("Screen %s", shortId(sourceId.value()).c_str());
                ImGui::TextDisabled(
                    "%s  %s",
                    source->contentHash().hex().substr(0, 8).c_str(),
                    provenance
                );
            }

            if (ImGui::Button("Delete Screen"))
            {
                requestDeletion(
                    ui,
                    deleteSource(state.draft(), sourceId),
                    "screen"
                );
            }

            drawRegressionClassification(state, ui);
        }

        // The page half of the Inspector: a summary of what the page holds and a
        // jump to its sample screen. Read-only; the page's members are authored
        // from the tree.
        auto drawPageInspector(AppState& state, annotation::PageId pageId) -> void
        {
            auto const* page = state.document().catalog().findPage(pageId);
            if (page == nullptr)
            {
                ImGui::TextUnformatted("The selected page is gone.");
                return;
            }
            ImGui::Text("Page \"%s\"", page->name().value().c_str());
            ImGui::Separator();
            if (auto const view = PageView::of(state.draft(), pageId))
            {
                ImGui::Text(
                    "Identifying marks: %zu",
                    view->identifiedBy.size()
                );
                ImGui::Text("Interactive regions: %zu", view->regions.size());
                ImGui::Text("Info regions: %zu", view->infos.size());
                ImGui::Text("Must not show: %zu", view->mustNotShow.size());
            }
            ImGui::Text(
                "Regression screens: %zu",
                regressionScreensForPage(state.document(), pageId).size()
            );

            auto const sample = pageSampleSource(state, *page);
            if (sample.has_value())
            {
                if (
                    ImGui::Button(
                        std::format(
                            "Show sample screen %s",
                            shortId(sample->value())
                        ).c_str()
                    )
                )
                {
                    state.select(AppState::Selection::Screen{*sample});
                }
            }
            else
            {
                ImGui::TextDisabled("No sample screen recorded yet.");
            }
        }

        // The Inspector: one window whose body follows the typed selection, so an
        // element, a screen, and a page each show the controls that act on them
        // and nothing shows a control for something not selected.
        auto drawInspector(AppState& state, PanelUiState& ui) -> void
        {
            if (!ImGui::Begin("Inspector"))
            {
                ImGui::End();
                return;
            }

            auto const& selection = state.selection();
            if (auto const element = selection.asElement())
            {
                drawElementInspector(state, ui, element->recognizerId);
            }
            else if (auto const screen = selection.asScreen())
            {
                drawScreenInspector(state, ui, screen->sourceId);
            }
            else if (auto const page = selection.asPage())
            {
                drawPageInspector(state, page->pageId);
            }
            else
            {
                ImGui::TextWrapped(
                    "Select a screen, page, or element in the project tree to "
                    "inspect and edit it here."
                );
            }

            ImGui::End();
        }

        // A recognizer's authored name, or a short form of its id when the
        // document no longer holds it. Never empty: it goes straight into a
        // status line beside the reason a search stopped.
        [[nodiscard]]
        auto recognizerName(
            AppState const& state,
            annotation::RecognizerId id
        ) -> std::string
        {
            auto const* definition = state.document().catalog().findRecognizer(id);
            if (definition == nullptr)
            {
                return shortId(id.value());
            }
            return definition->name().value();
        }

        // The one word that names why a search stopped, for a status line. The
        // author reads "budget" or "deadline" and knows whether to shrink the
        // search or wait longer -- the two have opposite fixes.
        [[nodiscard]]
        auto stopReasonWord(SadSearchStopReason reason) noexcept -> char const*
        {
            switch (reason)
            {
            case SadSearchStopReason::ComparisonBudgetExhausted:
                return "budget";
            case SadSearchStopReason::TimedOut:
                return "deadline";
            case SadSearchStopReason::Cancelled:
                return "cancelled";
            }
            return "stopped";
        }

        // One concrete line stating what a finished preview produced: the page it
        // resolved to and how many evidence boxes the canvas drew, plus any note
        // the preview carried -- a skipped action search, or a box missing
        // because a search stopped. Lowercase, in the style of the other action
        // status lines.
        [[nodiscard]]
        auto previewStatusLine(
            AppState const& state,
            PreviewResult const& preview
        ) -> std::string
        {
            auto message = std::string{"preview: "};
            if (preview.pageStop.has_value())
            {
                message += std::format(
                    "page search stopped at \"{}\" ({})",
                    recognizerName(state, preview.pageStop->recognizerId),
                    stopReasonWord(preview.pageStop->reason)
                );
            }
            else if (preview.resolvedPageId.has_value())
            {
                message += std::format(
                    "resolves to \"{}\"",
                    pageName(state, *preview.resolvedPageId)
                );
            }
            else if (
                preview.pageKind == PreviewPageKind::Ambiguous
            )
            {
                message += "two pages both match";
            }
            else
            {
                message += "no page resolves";
            }

            auto hits   = std::size_t{0};
            auto misses = std::size_t{0};
            for (auto const& row : preview.anchorRows)
            {
                (row.hit ? hits : misses) += 1U;
            }
            if (preview.actionEvidence.has_value())
            {
                (preview.actionEvidence->hit ? hits : misses) += 1U;
            }
            message += std::format("; {} hits, {} misses drawn", hits, misses);

            if (preview.actionSkipNote.has_value())
            {
                message += std::format("; {}", *preview.actionSkipNote);
            }
            if (preview.actionStop.has_value())
            {
                message += std::format(
                    "; action search stopped at \"{}\" ({})",
                    recognizerName(state, preview.actionStop->recognizerId),
                    stopReasonWord(preview.actionStop->reason)
                );
            }
            return message;
        }

        auto drawPreviewResult(PreviewResult const& preview) -> void
        {
            ImGui::SeparatorText("Preview");
            if (preview.pageStop.has_value())
            {
                ImGui::Text(
                    "page stop: recognizer %s reason %d",
                    shortId(preview.pageStop->recognizerId.value()).c_str(),
                    static_cast<int>(
                        std::to_underlying(preview.pageStop->reason)
                    )
                );
            }
            else if (preview.pageKind.has_value())
            {
                if (preview.resolvedPageId.has_value())
                {
                    ImGui::Text(
                        "page: %s (%s)",
                        previewPageKindName(*preview.pageKind),
                        shortId(preview.resolvedPageId->value()).c_str()
                    );
                }
                else
                {
                    ImGui::Text("page: %s", previewPageKindName(*preview.pageKind));
                }
            }

            for (auto const& row : preview.anchorRows)
            {
                auto const sad = row.sadScore.has_value()
                    ? std::format("{}", *row.sadScore)
                    : std::string{"-"};
                auto const rect = row.matchedRect.has_value()
                    ? std::format(
                        "{},{} {}x{}",
                        row.matchedRect->x(),
                        row.matchedRect->y(),
                        row.matchedRect->width(),
                        row.matchedRect->height()
                    )
                    : std::string{"-"};
                ImGui::Text(
                    "%s  hit=%d  sad=%s/%llu  rect=%s",
                    shortId(row.recognizerId.value()).c_str(),
                    row.hit ? 1 : 0,
                    sad.c_str(),
                    static_cast<unsigned long long>(row.maximumSad),
                    rect.c_str()
                );
            }

            if (preview.actionStop.has_value())
            {
                ImGui::Text(
                    "action stop: reason %d",
                    static_cast<int>(
                        std::to_underlying(preview.actionStop->reason)
                    )
                );
            }
            else if (preview.actionEvidence.has_value())
            {
                ImGui::Text(
                    "action %s  hit=%d",
                    shortId(preview.actionEvidence->recognizerId.value()).c_str(),
                    preview.actionEvidence->hit ? 1 : 0
                );
            }

            if (preview.actionSkipNote.has_value())
            {
                ImGui::TextWrapped("%s", preview.actionSkipNote->c_str());
            }
        }



        // Runs a preview against the selected screen and stores it, reporting the
        // verdict or a refusal. Shared by the Evidence tab's Preview button and
        // the F5 shortcut so both behave identically; the shortcut drains it after
        // the frame's edit commits, matching the tab which is drawn post-commit,
        // so a preview always scores the document on screen.
        auto performPreview(AppState& state, PanelUiState& ui) -> void
        {
            auto const selectedSource = state.selectedSourceId();
            if (!selectedSource.has_value())
            {
                ui.report(
                    LogSeverity::Error,
                    "preview requires a selected screen"
                );
                return;
            }
            auto const assets = state.compilerSourceAssets();
            if (!assets)
            {
                ui.report(
                    LogSeverity::Error,
                    std::format("preview failed: {}", toString(assets.error()))
                );
                return;
            }
            auto const policy = annotation::RecognitionPolicy{
                .maximumPixelComparisons = k_recognitionComparisonBudget,
                .deadline                = MonotonicInstant::now().checkedAdd(k_previewDeadline),
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
                ui.report(
                    LogSeverity::Error,
                    std::format("preview failed: {}", toString(preview.error()))
                );
                return;
            }
            ui.report(LogSeverity::Info, previewStatusLine(state, *preview));
            state.setLastPreview(std::move(*preview));
        }

        // The Evidence tab: run a preview and read its verdict. Three explicit
        // states, never a silent blank -- results when a preview is stored, a
        // "re-run" prompt when one was invalidated by an edit or a screen change,
        // and a first-run hint when none has been run at all.
        auto drawEvidenceTab(AppState& state, PanelUiState& ui) -> void
        {
            if (ImGui::Button("Preview"))
            {
                performPreview(state, ui);
            }

            ImGui::Separator();
            if (auto const& preview = state.lastPreview())
            {
                drawPreviewResult(*preview);
            }
            else if (state.previewInvalidated())
            {
                ImGui::TextWrapped(
                    "The project changed since the last preview. Press Preview to "
                    "run it again."
                );
            }
            else
            {
                ImGui::TextWrapped(
                    "Not run yet. Select a screen, then press Preview to see how "
                    "the model resolves it."
                );
            }
        }

        // The translucent fill a grid cell reads in, blended over the table row
        // background so the score text stays legible. Green expected, amber thin
        // or stopped, red misfire, and a faint grey for a cell not searched here.
        [[nodiscard]]
        auto modelCellBg(ModelCellColor color) -> ImVec4
        {
            switch (color)
            {
            case ModelCellColor::Expected:
                return ImVec4{0.30F, 0.72F, 0.40F, 0.35F};
            case ModelCellColor::Thin:
                return ImVec4{0.95F, 0.65F, 0.15F, 0.40F};
            case ModelCellColor::Misfire:
                return ImVec4{0.95F, 0.35F, 0.35F, 0.45F};
            case ModelCellColor::NotSearched:
                return ImVec4{0.50F, 0.50F, 0.50F, 0.12F};
            }
            UF_UNREACHABLE_MSG("unknown ModelCellColor value");
        }

        // The compact text drawn inside a grid cell: the score as a share of the
        // budget for a measured cell, a stop marker, or a dash where the element
        // was not searched. The colour and the tooltip carry the rest.
        [[nodiscard]]
        auto modelCellText(ModelCheckCell const& cell) -> std::string
        {
            switch (cell.outcome)
            {
            case ModelCellOutcome::Hit:
            case ModelCellOutcome::Miss:
                return budgetPercentText(cell.sadScore, cell.maximumSad);
            case ModelCellOutcome::Stopped:
                return "stop";
            case ModelCellOutcome::NotSearchedHere:
                return "-";
            }
            UF_UNREACHABLE_MSG("unknown ModelCellOutcome value");
        }

        // A cell's hover text: the element, the screen, the outcome word, and the
        // score against the threshold when one was measured.
        [[nodiscard]]
        auto modelCellTooltip(
            AppState const& state,
            ModelCheckCell const& cell
        ) -> std::string
        {
            auto const name   = recognizerName(state, cell.elementId);
            auto const screen = shortId(cell.screenId.value());
            switch (cell.outcome)
            {
            case ModelCellOutcome::Hit:
                return std::format(
                    "{} on {}: hit -- {} of budget",
                    name,
                    screen,
                    budgetPercentText(cell.sadScore, cell.maximumSad)
                );
            case ModelCellOutcome::Miss:
                return std::format(
                    "{} on {}: miss -- {} of budget",
                    name,
                    screen,
                    budgetPercentText(cell.sadScore, cell.maximumSad)
                );
            case ModelCellOutcome::Stopped:
                return std::format(
                    "{} on {}: stopped ({})",
                    name,
                    screen,
                    cell.stopReason.has_value()
                        ? stopReasonWord(*cell.stopReason)
                        : "unknown"
                );
            case ModelCellOutcome::NotSearchedHere:
                return std::format(
                    "{} on {}: not searched -- this screen's page does not "
                    "place it",
                    name,
                    screen
                );
            }
            UF_UNREACHABLE_MSG("unknown ModelCellOutcome value");
        }

        // The marks-x-screens grid: one row per mark, one column per captured
        // screen, in a bounded scrolling region with the mark column and the
        // screen header frozen. A cell's colour reads its outcome against whether
        // the mark is authored to match there; clicking one selects the mark and
        // follows to that screen, with the screen's page as context when it places
        // the mark. Drawn from the same margins the tables above use for rows, so
        // an info region -- absent from the margins -- is absent here too.
        auto drawModelMatrix(AppState& state, ModelCheck const& check) -> void
        {
            if (check.screens.empty() || check.margins.empty())
            {
                return;
            }

            ImGui::SeparatorText("Mark x screen matrix");
            auto const columns = 1 + static_cast<int>(check.screens.size());
            auto const matrixFlags = ImGuiTableFlags_Borders
                | ImGuiTableFlags_RowBg
                | ImGuiTableFlags_ScrollX
                | ImGuiTableFlags_ScrollY
                | ImGuiTableFlags_SizingFixedFit;
            // A bounded height so a large model scrolls inside the drawer rather
            // than pushing the tables above off the top on a 1080p screen.
            auto const matrixSize = ImVec2{0.0F, 260.0F};
            if (!ImGui::BeginTable("mark-screen-matrix", columns, matrixFlags, matrixSize))
            {
                return;
            }

            ImGui::TableSetupScrollFreeze(1, 1);
            ImGui::TableSetupColumn("Mark", ImGuiTableColumnFlags_WidthFixed);
            for (auto const& screen : check.screens)
            {
                ImGui::TableSetupColumn(
                    shortId(screen.sourceId.value()).c_str(),
                    ImGuiTableColumnFlags_WidthFixed
                );
            }
            ImGui::TableHeadersRow();

            auto rowIndex = 0;
            for (auto const& margin : check.margins)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    recognizerName(state, margin.recognizerId).c_str()
                );

                auto columnIndex = 0;
                for (auto const& screen : check.screens)
                {
                    ImGui::TableNextColumn();
                    auto const* p_cell = findModelCell(
                        state,
                        margin.recognizerId,
                        screen.sourceId
                    );
                    if (p_cell == nullptr)
                    {
                        ++columnIndex;
                        continue;
                    }

                    ImGui::TableSetBgColor(
                        ImGuiTableBgTarget_CellBg,
                        ImGui::GetColorU32(modelCellBg(classifyModelCell(*p_cell)))
                    );
                    auto const label = std::format(
                        "{}##cell-{}-{}",
                        modelCellText(*p_cell),
                        rowIndex,
                        columnIndex
                    );
                    if (
                        ImGui::Selectable(
                            label.c_str(),
                            false,
                            ImGuiSelectableFlags_None
                        )
                    )
                    {
                        // Follow with page context only when the screen's page
                        // actually places the mark, so the canvas edits that
                        // placement rather than falling back to a foreign claim.
                        auto const pageContext = p_cell->expectedHit
                            ? screen.expectedPageId
                            : std::optional<annotation::PageId>{};
                        selectRecognizer(
                            state,
                            margin.recognizerId,
                            screen.sourceId,
                            pageContext
                        );
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip(
                            "%s",
                            modelCellTooltip(state, *p_cell).c_str()
                        );
                    }
                    ++columnIndex;
                }
                ++rowIndex;
            }
            ImGui::EndTable();
        }

        // The Model tab: run a whole-model check and read the tables the data
        // supports -- one screen-verdict row per captured screen, one margin row
        // per mark, and the marks-x-screens grid (drawModelMatrix) beneath them.
        // Same three explicit states as Evidence.
        auto drawModelTab(
            AppState& state,
            WorkbenchServices const& services,
            PanelUiState& ui
        ) -> void
        {
            auto const checking = ui.modelCheck.running();
            ImGui::BeginDisabled(checking);
            if (ImGui::Button(checking ? "Checking..." : "Check Model"))
            {
                startModelCheck(state, ui, {});
            }
            ImGui::EndDisabled();

            // The captured screens are stills and never change; only the running
            // target drifts, so measuring against it is the one check that says
            // whether a mark still holds on the game as it is right now. Capture
            // stays on the GUI thread that owns the graphics device; only the
            // searching moves off it.
            auto const hasTarget = ui.targetTitle.at(0) != '\0';
            ImGui::SameLine();
            ImGui::BeginDisabled(checking || !hasTarget);
            if (ImGui::Button("Check Against Live Screen"))
            {
                auto captured = services.captureFromTarget(
                    annotation::SourceId{mintResourceId()},
                    std::string{ui.targetTitle.data()}
                );
                if (!captured)
                {
                    ui.report(
                        LogSeverity::Error,
                        std::format(
                            "live capture failed: {}",
                            toString(captured.error())
                        )
                    );
                }
                else
                {
                    startModelCheck(state, ui, captured->asset.pngBytes);
                }
            }
            ImGui::EndDisabled();
            if (
                !hasTarget
                && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)
            )
            {
                ImGui::SetTooltip(
                    "Enter the target window title in the toolbar to enable this"
                );
            }

            ImGui::Separator();
            auto const& check = state.lastModelCheck();
            if (!check.has_value())
            {
                if (state.modelCheckInvalidated())
                {
                    ImGui::TextWrapped(
                        "The project changed since the last check. Press Check "
                        "Model to run it again."
                    );
                }
                else
                {
                    ImGui::TextWrapped(
                        "Not run yet. Press Check Model to score every mark "
                        "against every screen."
                    );
                }
                return;
            }

            auto const tableFlags = ImGuiTableFlags_Borders
                | ImGuiTableFlags_RowBg
                | ImGuiTableFlags_SizingStretchProp;

            ImGui::SeparatorText("Screen verdicts");
            if (
                ImGui::BeginTable("screen-verdicts", 2, tableFlags)
            )
            {
                ImGui::TableSetupColumn("Screen");
                ImGui::TableSetupColumn("Verdict");
                ImGui::TableHeadersRow();
                for (auto const& screen : check->screens)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(
                        shortId(screen.sourceId.value()).c_str()
                    );
                    ImGui::TableNextColumn();
                    auto const correct = (
                        screen.outcome == ScreenCheckOutcome::Correct
                    );
                    ImGui::TextColored(
                        correct ? k_passColor : k_failColor,
                        "%s",
                        screenCheckText(state, screen).c_str()
                    );
                }
                ImGui::EndTable();
            }

            // Each mark's own score ("here", which stays under 100% to match) and
            // its closest score elsewhere (which must stay above); a live column
            // when the check was given a running frame. The gap is the number
            // worth watching -- a mark whose two are close is one frame of drift
            // from resolving the wrong page.
            ImGui::SeparatorText("Mark margins");
            if (
                ImGui::BeginTable("mark-margins", 4, tableFlags)
            )
            {
                ImGui::TableSetupColumn("Mark");
                ImGui::TableSetupColumn("Here");
                ImGui::TableSetupColumn("Elsewhere");
                ImGui::TableSetupColumn("Live");
                ImGui::TableHeadersRow();
                for (auto const& margin : check->margins)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(
                        recognizerName(state, margin.recognizerId).c_str()
                    );
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(
                        budgetPercentText(
                            margin.ownSadScore,
                            margin.maximumSad
                        ).c_str()
                    );
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(
                        budgetPercentText(
                            margin.nearestOtherSadScore,
                            margin.maximumSad
                        ).c_str()
                    );
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(
                        budgetPercentText(
                            margin.liveSadScore,
                            margin.maximumSad
                        ).c_str()
                    );
                }
                ImGui::EndTable();
            }

            drawModelMatrix(state, *check);
        }

        // The colour a severity is drawn in, or nothing for info, which keeps the
        // theme's default text colour. Errors read red and warnings amber so a
        // failure is legible at a glance in a scrolling list.
        [[nodiscard]]
        auto logSeverityColor(
            LogSeverity severity
        ) -> std::optional<ImVec4>
        {
            switch (severity)
            {
            case LogSeverity::Info:
                return std::nullopt;
            case LogSeverity::Warning:
                return ImVec4{0.95F, 0.65F, 0.15F, 1.0F};
            case LogSeverity::Error:
                return ImVec4{0.95F, 0.35F, 0.35F, 1.0F};
            }
            UF_UNREACHABLE_MSG("unknown LogSeverity value");
        }

        // The Log tab: the bounded operation-log history newest at the bottom,
        // each line timestamped and coloured by severity, with a button that
        // empties the in-memory buffer. The body of the former standalone Log
        // window, now one tab of the verify drawer.
        auto drawLogTab(PanelUiState& ui) -> void
        {
            if (ImGui::Button("Clear"))
            {
                ui.clearLog();
            }
            ImGui::SameLine();
            ImGui::Text("%zu events", ui.logEvents.size());
            ImGui::Separator();

            if (
                ImGui::BeginChild(
                    "log-scroll",
                    ImVec2{0.0F, 0.0F},
                    ImGuiChildFlags_None,
                    ImGuiWindowFlags_HorizontalScrollbar
                )
            )
            {
                for (auto const& event : ui.logEvents)
                {
                    ImGui::TextUnformatted(event.timestamp.c_str());
                    ImGui::SameLine();

                    auto const color = logSeverityColor(event.severity);
                    if (color.has_value())
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, *color);
                    }
                    ImGui::TextWrapped("%s", event.message.c_str());
                    if (color.has_value())
                    {
                        ImGui::PopStyleColor();
                    }
                }

                // Follow the tail only while the view is already pinned to the
                // bottom, so a user who scrolled up to read is left where they
                // are.
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                {
                    ImGui::SetScrollHereY(1.0F);
                }
            }
            ImGui::EndChild();
        }

        // The bottom verify drawer: one docked window with Evidence, Model, and
        // Log tabs. Drawn after applyPendingEdit, so its Preview and Check buttons
        // act on the document the frame's edit produced -- the same post-commit
        // ordering the former Actions panel relied on. When the drawer is
        // collapsed or unfocused it is simply a docked window; the persistent
        // one-line summary lives on the toolbar's status line, so no custom
        // collapse is needed here.
        auto drawVerifyDrawer(
            AppState& state,
            WorkbenchServices const& services,
            PanelUiState& ui
        ) -> void
        {
            if (!ImGui::Begin("Verify"))
            {
                ImGui::End();
                return;
            }

            if (ImGui::BeginTabBar("verify-tabs"))
            {
                if (ImGui::BeginTabItem("Evidence"))
                {
                    drawEvidenceTab(state, ui);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Model"))
                {
                    drawModelTab(state, services, ui);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Log"))
                {
                    drawLogTab(ui);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }

            ImGui::End();
        }

        // The programmatic default dock layout: toolbar across the top, project
        // tree on the left, inspector on the right, verify drawer along the
        // bottom, and the canvas filling the centre. Built only on a first run
        // that restored no layout from the ini (the shell persists one to
        // LOCALAPPDATA), so a user's saved arrangement always wins -- this never
        // fights persistence, it seeds the first-ever session and the smoke run,
        // which has ini persistence turned off.
        auto buildDefaultLayout(
            ImGuiID dockspaceId,
            ImGuiViewport const& viewport
        ) -> void
        {
            ImGui::DockBuilderRemoveNode(dockspaceId);
            ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspaceId, viewport.WorkSize);

            auto center      = ImGuiID{};
            auto const top   = ImGui::DockBuilderSplitNode(
                dockspaceId,
                ImGuiDir_Up,
                0.06F,
                nullptr,
                &center
            );
            auto centerAfterLeft = ImGuiID{};
            auto const left      = ImGui::DockBuilderSplitNode(
                center,
                ImGuiDir_Left,
                0.22F,
                nullptr,
                &centerAfterLeft
            );
            auto centerAfterRight = ImGuiID{};
            auto const right      = ImGui::DockBuilderSplitNode(
                centerAfterLeft,
                ImGuiDir_Right,
                0.28F,
                nullptr,
                &centerAfterRight
            );
            auto canvas       = ImGuiID{};
            auto const bottom = ImGui::DockBuilderSplitNode(
                centerAfterRight,
                ImGuiDir_Down,
                0.30F,
                nullptr,
                &canvas
            );

            ImGui::DockBuilderDockWindow("Toolbar", top);
            ImGui::DockBuilderDockWindow("Project", left);
            ImGui::DockBuilderDockWindow("Inspector", right);
            ImGui::DockBuilderDockWindow("Verify", bottom);
            ImGui::DockBuilderDockWindow("Canvas", canvas);
            ImGui::DockBuilderFinish(dockspaceId);
        }

        // The global keyboard shortcuts, routed through the same deferred paths
        // the toolbar and Evidence tab use so frame ordering is preserved: save,
        // undo, and redo queue a toolbar command dispatched after the frame's edit
        // commits; F5 flags a preview the shell runs post-commit; Delete opens the
        // delete-everywhere confirmation for the selected element, or hints when
        // nothing deletable is selected. Suppressed while a text field holds
        // keyboard input or any popup or menu is open, so typing a name or
        // answering a dialog never fires an action.
        auto handleShortcuts(AppState& state, PanelUiState& ui) -> void
        {
            auto const& io = ImGui::GetIO();
            if (
                io.WantTextInput
                || ImGui::IsPopupOpen(
                    nullptr,
                    ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel
                )
            )
            {
                return;
            }
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
            {
                requestToolbarCommand(ui, ToolbarCommand::SaveAndGenerate);
            }
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z))
            {
                requestToolbarCommand(ui, ToolbarCommand::Undo);
            }
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y))
            {
                requestToolbarCommand(ui, ToolbarCommand::Redo);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_F5))
            {
                ui.previewRequested = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Delete))
            {
                if (auto const selected = state.selectedRecognizerId())
                {
                    requestDeleteEverywhere(state, ui, *selected);
                }
                else
                {
                    ui.report(
                        LogSeverity::Info,
                        "select an element to delete it"
                    );
                }
            }
        }
    }

    auto drawWorkbench(
        AppState& state,
        WorkbenchServices const& services,
        PanelUiState& ui
    ) -> void
    {
        // Host a full-viewport dock space so the panels can be docked and resized
        // against each other. On the first frame with no layout restored from the
        // ini -- a fresh install or the persistence-free smoke run leaves the
        // dockspace a single empty leaf -- seed the redesign's default
        // arrangement; a user's saved layout has splits or docked windows and is
        // left untouched.
        auto* p_viewport       = ImGui::GetMainViewport();
        auto const dockspaceId = ImGui::DockSpaceOverViewport(0, p_viewport);
        static auto layoutInitialized = false;
        if (!layoutInitialized)
        {
            layoutInitialized = true;
            auto const* p_node = ImGui::DockBuilderGetNode(dockspaceId);
            if (
                p_node == nullptr
                || (p_node->IsLeafNode() && p_node->Windows.Size == 0)
            )
            {
                buildDefaultLayout(dockspaceId, *p_viewport);
            }
        }

        // Collect a finished check before anything draws, so its verdicts appear
        // in the same frame the worker delivered them.
        collectModelCheck(state, ui);

        // Global shortcuts before the panels, so a queued save/undo/redo, a
        // flagged preview, or a delete confirmation is in hand by the time the
        // deferred paths below run -- the same ordering the toolbar buttons rely
        // on.
        handleShortcuts(state, ui);

        drawToolbar(state, ui);
        drawProjectTree(state, ui);
        drawCanvasPanel(state, services, ui);
        drawInspector(state, ui);

        // The delete-everywhere confirmation, drawn after the panels that raise
        // it and before the commit, so a confirmed deletion is parked in time for
        // applyPendingEdit to commit it this frame.
        drawDeleteConfirmPopup(state, ui);

        // Every panel above borrows into the document while it draws, so the
        // frame's edit lands here, once they are all done with it. The deferred
        // toolbar command and the queued ingestion follow rather than precede it:
        // a save or an undo must see the frame's widget-deactivation edit already
        // applied, and ingestion commits its own history entry that would drop the
        // parked edit if it ran first.
        applyPendingEdit(state, ui);
        dispatchToolbarCommand(state, ui);
        if (std::exchange(ui.captureRequested, false))
        {
            performCapture(state, services, ui);
        }
        if (std::exchange(ui.importRequested, false))
        {
            performImport(state, services, ui);
        }
        if (services.pruneTextures)
        {
            services.pruneTextures(state.document().sources());
        }
        // The F5 preview runs here, on the document the frame's edit produced,
        // so its result is the one the Evidence tab renders just below.
        if (std::exchange(ui.previewRequested, false))
        {
            performPreview(state, ui);
        }

        // The verify drawer is drawn after the commit so its Preview and Check
        // buttons act on the document the frame's edit produced -- the same
        // post-commit ordering the former Actions panel relied on.
        drawVerifyDrawer(state, services, ui);

        // Mirror each new status-line outcome to the bounded event history and
        // the on-disk log, so a session's actions and errors are not lost when
        // the next action overwrites the transient line. captureLogEvent owns the
        // consecutive-duplicate collapse and the bounding; the same timestamp
        // stamps both the in-memory entry and the disk line.
        auto const timestamp = formatLogTimestamp(
            std::chrono::system_clock::now()
        );
        if (auto const event = ui.captureLogEvent(timestamp))
        {
            if (services.appendLog)
            {
                services.appendLog(
                    event->severity,
                    event->timestamp,
                    event->message
                );
            }
        }
    }
}
