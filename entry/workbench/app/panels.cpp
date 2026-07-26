#include "panels.hpp"

#include "canvas-math.hpp"
#include "workbench-app.hpp"

#include "authoring-actions.hpp"
#include "edit-page.hpp"
#include "model-check-view.hpp"
#include "page-view.hpp"
#include "panel-state.hpp"
#include "preview.hpp"
#include "project-persistence.hpp"
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

        // A template box belonging to another screen: dimmed, so it reads as
        // context rather than as something to drag.
        constexpr auto k_foreignTemplateColor = IM_COL32(96, 220, 120, 110);

        auto const k_passColor = ImVec4{0.38F, 0.86F, 0.47F, 1.0F};
        auto const k_failColor = ImVec4{0.96F, 0.55F, 0.38F, 1.0F};

        constexpr auto k_gripRadius     = 5.0F;
        constexpr auto k_zoomWheelBase  = 1.1F;
        constexpr auto k_thresholdMax   = 10'000;

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

        auto drawSourcesPanel(
            AppState& state,
            WorkbenchServices const& services,
            PanelUiState& ui
        ) -> void
        {
            // Named for what it holds rather than for the domain type: every row
            // is one captured screen, and a page is authored from one of them.
            if (!ImGui::Begin("Screens"))
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
                ui.targetTitle.data(),
                ui.targetTitle.size()
            );
            auto const hasTarget = ui.targetTitle.at(0) != '\0';

            ImGui::BeginDisabled(!hasTarget);
            if (ImGui::Button("Capture"))
            {
                auto const id = annotation::SourceId{mintResourceId()};
                auto const title = std::string{ui.targetTitle.data()};
                auto ingested = services.captureFromTarget(id, title);
                if (!ingested)
                {
                    ui.statusLine = std::format(
                        "capture failed: {}",
                        toString(ingested.error())
                    );
                }
                else
                {
                    auto const added = state.addIngestedSource(
                        std::move(*ingested)
                    );
                    ui.statusLine = added.has_value()
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
            if (ImGui::Button("Delete Screen") && selectedSource.has_value())
            {
                requestDeletion(
                    ui,
                    deleteSource(state.draft(), *selectedSource),
                    "screen"
                );
            }
            ImGui::EndDisabled();

            if (ImGui::Button("Import PNG..."))
            {
                auto picked = services.pickPngToImport();
                if (!picked)
                {
                    ui.statusLine = std::format(
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
                        ui.statusLine = std::format(
                            "import failed: {}",
                            toString(ingested.error())
                        );
                    }
                    else
                    {
                        auto const added = state.addIngestedSource(
                            std::move(*ingested)
                        );
                        ui.statusLine = added.has_value()
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









        // One selectable recognizer row inside a page's group. pageScreen is the
        // screen that page stands for, or nothing for the rows that belong to no
        // page.
        auto drawPageMemberRow(
            AppState& state,
            PanelUiState& ui,
            annotation::RecognizerDefinition const& recognizer,
            std::optional<annotation::SourceId> pageScreen
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

            auto const memberCount = pagesPlacedOn(state.draft(), id).size();
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
                selectRecognizer(state, id, pageScreen);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove"))
            {
                requestDeletion(
                    ui,
                    deleteRecognizer(state.draft(), id),
                    std::format("\"{}\"", recognizer.name().value())
                );
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
                ui.statusLine = "select a screen first";
                return;
            }

            auto page = EditPage::createFrom(state, *source);
            if (!page)
            {
                ui.statusLine = std::format(
                    "new page failed: {}",
                    toString(page.error())
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
            return std::format(
                "added \"{}\" as {}; drag its box over the {}",
                name,
                kind == PageMemberKind::Anchor
                    ? "a mark identifying this page"
                    : "an interactive region on this page",
                kind == PageMemberKind::Anchor ? "mark" : "region"
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
                ui.statusLine = std::format(
                    "add failed: {}",
                    toString(page.error())
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
                    ui.statusLine = std::format(
                        "add failed: {}",
                        toString(added.error())
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
                auto added = page->placeRegion(
                    EditPage::NewRegionSpec{.sourceId = sourceId}
                );
                if (!added)
                {
                    ui.statusLine = std::format(
                        "add failed: {}",
                        toString(added.error())
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
                ui.statusLine = std::format(
                    "recording the screen failed: {}",
                    toString(page.error())
                );
                return;
            }
            if (auto const status = page->claimScreen(sourceId); !status)
            {
                ui.statusLine = std::format(
                    "recording the screen failed: {}",
                    toString(status.error())
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

        // The workbench's navigator. A page is a screen the author captured, and
        // everything authored on that screen hangs beneath it: the marks that
        // identify it and the elements that may be clicked there. Both are
        // created by the buttons in the group they belong to, so the annotation
        // type, the signature role, and the authorization are all consequences of
        // where the author pressed rather than fields to fill in.
        auto drawPagesPanel(AppState& state, PanelUiState& ui) -> void
        {
            if (!ImGui::Begin("Pages"))
            {
                ImGui::End();
                return;
            }

            ImGui::BeginDisabled(!state.selectedSourceId().has_value());
            if (ImGui::Button("New Page From Selected Screen"))
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
                    "Capture or import a screen first, then select it in Screens"
                );
            }
            ImGui::Separator();

            auto const& catalog = state.document().catalog();

            // Which page a shared region was dropped on this frame. The element
            // being dragged lives on PanelUiState instead, because the frame that
            // accepts the drop is the frame the drag source stops submitting
            // itself.
            auto droppedOnPage = std::optional<annotation::PageId>{};

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
                        state.draft(),
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
                ImGui::Separator();
            }

            if (catalog.pages().empty())
            {
                ImGui::TextWrapped(
                    "No pages yet. Capture the screen you want to automate, "
                    "select it, then press the button above."
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

                auto const open = ImGui::TreeNodeEx(
                    page.name().value().c_str(),
                    ImGuiTreeNodeFlags_DefaultOpen
                );
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

                auto const sample = pageSampleSource(state, page);
                if (sample.has_value())
                {
                    if (
                        ImGui::SmallButton(
                            std::format(
                                "screen {}",
                                shortId(sample->value())
                            ).c_str()
                        )
                    )
                    {
                        state.setSelectedSourceId(*sample);
                    }
                    if (auto const* p_screen = findScreenCheck(state, *sample))
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

                    // A page authored before pages recorded their screen has
                    // nothing for a check to measure against. One press states
                    // it, which is also what turns the page's verdict on.
                    if (!claimedScreen(state, pageId).has_value())
                    {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Record this screen"))
                        {
                            recordScreenForPage(state, ui, pageId, *sample);
                        }
                    }
                }
                else
                {
                    ImGui::TextUnformatted("screen -");
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
                            drawPageMemberRow(state, ui, *p_recognizer, sample);
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
                            drawPageMemberRow(state, ui, *p_recognizer, sample);
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
                            drawPageMemberRow(state, ui, *p_recognizer, sample);
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
                            drawPageMemberRow(state, ui, *p_recognizer, sample);
                        }
                    }
                }

                ImGui::TreePop();
                ImGui::PopID();
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
                        drawPageMemberRow(state, ui, recognizer, std::nullopt);
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
            uint32 sourceWidth,
            uint32 sourceHeight,
            bool hovered,
            bool templateEditable
        ) -> void
        {
            auto const recognizerId = definition.id();
            auto const templateRect = definition.templateRect();
            auto const searchRoi    = definition.searchRoi();
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

            if (auto const recognizerId = state.selectedRecognizerId())
            {
                auto const* definition = state.document().catalog().findRecognizer(
                    *recognizerId
                );
                if (definition != nullptr)
                {
                    // The template is only editable over the screen it was cut
                    // from. For a shared element seen from another page they are
                    // different images.
                    auto const authoredOn = sourceOfRecognizer(state, *recognizerId);
                    handleRectEditing(
                        state,
                        ui,
                        *drawList,
                        view,
                        canvasOrigin,
                        *definition,
                        texture->width,
                        texture->height,
                        hovered,
                        authoredOn == selectedSource
                    );
                }
            }

            drawList->PopClipRect();
            ImGui::End();
        }

        // What a shared element's copy needs said about it. Putting it on another
        // page is a drag from Shared regions in the Pages panel, not a control
        // here: the author is choosing a page, and the pages are over there.
        auto drawSharing(
            AppState& state,
            annotation::RecognizerDefinition const& definition
        ) -> void
        {
            if (
                definition.annotationType() != annotation::AnnotationType::ActionTarget
            )
            {
                return;
            }

            auto const pages = pagesPlacedOn(state.draft(), definition.id());
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
            auto const authoredOn = sourceOfRecognizer(state, definition.id());
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
        // this exists to repair a mistake or to reach the info-region type, and
        // it lives under Advanced with the two membership relationships.
        auto drawTypeCombo(
            AppState& state,
            PanelUiState& ui,
            annotation::RecognizerDefinition const& definition
        ) -> void
        {
            auto typeIndex = static_cast<int>(
                std::to_underlying(definition.annotationType())
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
            // together, so this goes through retypeRecognizer rather than
            // writing the field and committing.
            auto retyped = retypeRecognizer(
                state.draft(),
                definition.id(),
                static_cast<annotation::AnnotationType>(typeIndex)
            );
            if (!retyped)
            {
                ui.statusLine = std::format(
                    "type change rejected: {}",
                    toString(retyped.error())
                );
                return;
            }

            // Summarized against the pre-commit document so a page named in the
            // summary is resolved before the edit lands.
            auto const summary = retypeSummary(
                state,
                *retyped,
                k_annotationTypeItems.at(static_cast<std::size_t>(typeIndex))
            );
            requestEdit(ui, std::move(retyped->draft), summary);
        }

        auto drawPageMembership(
            AppState& state,
            PanelUiState& ui,
            annotation::RecognizerDefinition const& definition
        ) -> void
        {
            auto const recognizerId = definition.id();
            // Two unrelated relationships, kept apart by the catalog's rules: the
            // checkbox is permission to click this element on that page, and the
            // combo is the part this mark plays in identifying it. A page anchor
            // may only do the second and every other type only the first, so each
            // is disabled where it cannot apply. Both are consequences of the
            // page group a member was created in; editing them by hand is for
            // sharing one element across pages and for exclusivity rules.
            auto const isAnchor = (
                definition.annotationType() == annotation::AnnotationType::PageAnchor
            );
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
                    if (member)
                    {
                        // Authorize through EditPage::placeExisting, which seeds
                        // the placement's per-page search region from the
                        // element's own.
                        auto opened = EditPage::open(state, pageId);
                        if (!opened)
                        {
                            ui.statusLine = std::format(
                                "place rejected: {}",
                                toString(opened.error())
                            );
                        }
                        else if (
                            auto const status = opened->placeExisting(recognizerId);
                            !status
                        )
                        {
                            ui.statusLine = std::format(
                                "place rejected: {}",
                                toString(status.error())
                            );
                        }
                        else
                        {
                            std::move(*opened).commit(
                                ui,
                                std::format(
                                    "placed \"{}\" on page \"{}\"",
                                    definition.name().value(),
                                    page.name().value()
                                )
                            );
                        }
                    }
                    else
                    {
                        // Withdraw. The interactive-only region handle cannot
                        // serve the info row this checkbox also governs, so the
                        // placement is removed directly -- but with the same
                        // closure guard removeFromThisPage applies: an interactive
                        // element's last placement is refused, naming deletion,
                        // rather than silently deleted as the v1 copy model did.
                        auto draft            = state.draft();
                        auto const interactive = (
                            definition.annotationType()
                            == annotation::AnnotationType::ActionTarget
                        );
                        auto const onOtherPage = std::ranges::any_of(
                            draft.placements,
                            [&](EditablePlacement const& placement)
                            {
                                return placement.elementId == recognizerId
                                    && placement.pageId != pageId;
                            }
                        );
                        if (interactive && !onOtherPage)
                        {
                            ui.statusLine = std::format(
                                "\"{}\" is only on this page; an interactive region "
                                "must stay on at least one, so delete it instead of "
                                "removing it here",
                                definition.name().value()
                            );
                        }
                        else
                        {
                            std::erase_if(
                                draft.placements,
                                [&](EditablePlacement const& placement)
                                {
                                    return placement.elementId == recognizerId
                                        && placement.pageId == pageId;
                                }
                            );
                            requestEdit(
                                ui,
                                std::move(draft),
                                std::format(
                                    "removed \"{}\" from page \"{}\"",
                                    definition.name().value(),
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
                ui.nameBufferFor != *recognizerId
                || (
                    ui.nameSeededValue != currentName
                    && !ui.nameInputActive
                )
            )
            {
                seedBuffer(ui.nameBuffer, currentName);
                ui.nameBufferFor   = *recognizerId;
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
                auto* recognizer = findEditableRecognizer(draft, *recognizerId);
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

            auto threshold = static_cast<int>(definition->threshold().basisPoints());
            ImGui::InputInt("Threshold (bp)", &threshold);
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                threshold  = std::clamp(threshold, 0, k_thresholdMax);
                auto draft = state.draft();
                auto* recognizer = findEditableRecognizer(draft, *recognizerId);
                if (recognizer != nullptr)
                {
                    recognizer->similarityBasisPoints =
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

            drawSharing(state, *definition);

            if (ImGui::CollapsingHeader("Advanced"))
            {
                drawTypeCombo(state, ui, *definition);
                drawPageMembership(state, ui, *definition);
            }
            drawRegressionClassification(state, ui);

            ImGui::End();
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
        }



        auto drawActionsPanel(
            AppState& state,
            WorkbenchServices const& services,
            PanelUiState& ui
        ) -> void
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
                    ui.statusLine = std::format(
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
                        ui.statusLine = std::format(
                            "save failed: {}",
                            toString(status.error())
                        );
                    }
                    else
                    {
                        state.markSaved();
                        ui.statusLine = "saved and generated";
                    }
                }
            }

            if (ImGui::Button("Preview"))
            {
                auto const selectedSource = state.selectedSourceId();
                if (!selectedSource.has_value())
                {
                    ui.statusLine = "preview requires a selected source";
                }
                else if (auto const assets = state.compilerSourceAssets(); !assets)
                {
                    ui.statusLine = std::format(
                        "preview failed: {}",
                        toString(assets.error())
                    );
                }
                else
                {
                    auto const policy = annotation::RecognitionPolicy{
                        .maximumPixelComparisons = k_recognitionComparisonBudget,
                        .deadline = MonotonicInstant::now().checkedAdd(
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
                        ui.statusLine = std::format(
                            "preview failed: {}",
                            toString(preview.error())
                        );
                    }
                    else
                    {
                        state.setLastPreview(std::move(*preview));
                        ui.statusLine = "preview complete";
                    }
                }
            }

            // The check every author needs and none can do by eye: a mark always
            // matches the image it was cut from, so only its score on the other
            // screens says whether it identifies one screen or merely exists.
            // It runs on a worker thread; see ModelCheckJob for why.
            auto const checking = ui.modelCheck.running();
            ImGui::BeginDisabled(checking);
            if (ImGui::Button(checking ? "Checking..." : "Check Model"))
            {
                startModelCheck(state, ui, {});
            }

            // The captured screens are stills and never change; only the running
            // target drifts, with highlights, counters, and animation. Measuring
            // against it is the one check that says whether a mark still holds on
            // the game as it is right now.
            auto const hasTarget = ui.targetTitle.at(0) != '\0';
            ImGui::SameLine();
            ImGui::BeginDisabled(!hasTarget);
            if (ImGui::Button("Check Against Live Screen"))
            {
                // Capture stays here: it belongs to the GUI thread that owns the
                // graphics device. Only the searching moves off it.
                auto captured = services.captureFromTarget(
                    annotation::SourceId{mintResourceId()},
                    std::string{ui.targetTitle.data()}
                );
                if (!captured)
                {
                    ui.statusLine = std::format(
                        "live capture failed: {}",
                        toString(captured.error())
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
                    "Enter the target window title in Screens to enable this"
                );
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(!state.canUndo());
            if (ImGui::Button("Undo"))
            {
                if (state.undo())
                {
                    ui.statusLine = std::format(
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
                    ui.statusLine = std::format(
                        "redo: {} recognizers, {} pages",
                        state.document().catalog().recognizers().size(),
                        state.document().catalog().pages().size()
                    );
                }
            }
            ImGui::EndDisabled();

            if (!ui.statusLine.empty())
            {
                ImGui::TextWrapped("%s", ui.statusLine.c_str());
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

        // Collect a finished check before anything draws, so its verdicts appear
        // in the same frame the worker delivered them.
        collectModelCheck(state, ui);

        drawSourcesPanel(state, services, ui);
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
        drawActionsPanel(state, services, ui);

        // Mirror each new status-line outcome to the operation log so a session's
        // actions and errors are not lost when the next action overwrites the
        // transient line. Consecutive identical outcomes collapse to one entry.
        if (
            services.appendLog
            && !ui.statusLine.empty()
            && ui.statusLine != ui.lastLoggedStatus
        )
        {
            services.appendLog(ui.statusLine);
            ui.lastLoggedStatus = ui.statusLine;
        }
    }
}
