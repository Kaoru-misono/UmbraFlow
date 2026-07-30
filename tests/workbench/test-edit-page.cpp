#include "../annotation/test-helpers.hpp"
#include "authoring-fixture.hpp"

#include <authoring-actions.hpp>
#include <authoring-edit.hpp>
#include <edit-page.hpp>
#include <page-view.hpp>
#include <panel-state.hpp>
#include <workbench-app.hpp>

#include <annotation/authoring-document.hpp>
#include <annotation/resource.hpp>
#include <annotation/content-hash.hpp>

#include <core/error/result.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        constexpr auto k_sourceId  = "00000000-0000-0000-0000-000000000801";
        constexpr auto k_anchorId  = "00000000-0000-0000-0000-000000000811";
        constexpr auto k_awayId    = "00000000-0000-0000-0000-000000000812";
        constexpr auto k_regionId  = "00000000-0000-0000-0000-000000000821";
        constexpr auto k_homePage  = "00000000-0000-0000-0000-000000000831";
        constexpr auto k_awayPage  = "00000000-0000-0000-0000-000000000832";
        constexpr auto k_regId     = "00000000-0000-0000-0000-000000000841";

        // A two-page project: "home" identified by home_marker with an
        // interactive region on it, and "away" identified by away_marker. The
        // source is recorded as an example of the home page. Enough shape to
        // reach every EditPage operation the phase-1 plan names.
        [[nodiscard]]
        auto document() -> annotation::AuthoringDocument
        {
            auto const fingerprint = annotation::test::fingerprint(8, 8, 96, 96);
            auto const sourceId    = annotation::test::sourceId(k_sourceId);
            auto const anchorId    = annotation::test::elementId(k_anchorId);
            auto const awayId      = annotation::test::elementId(k_awayId);
            auto const regionId    = annotation::test::elementId(k_regionId);
            auto const homePage    = annotation::test::pageId(k_homePage);
            auto const awayPage    = annotation::test::pageId(k_awayPage);
            auto const sourceHash  = annotation::sha256(
                std::span<std::byte const>{}
            );
            REQUIRE(sourceHash.has_value());

            auto source = annotation::AuthoringSource::create(
                annotation::AuthoringSourceSpec{
                    .id          = sourceId,
                    .contentHash = *sourceHash,
                    .fingerprint = fingerprint,
                    .provenance  = annotation::ImportedSourceProvenance{},
                }
            );
            REQUIRE(source.has_value());

            auto created = annotation::AuthoringDocument::create(
                annotation::test::projectId(),
                fingerprint,
                {*source},
                {
                    test::markElement(
                        fingerprint,
                        anchorId,
                        "home_marker",
                        sourceId,
                        annotation::test::pixelRect(0, 0, 2, 2),
                        annotation::test::pixelRect(0, 0, 4, 4)
                    ),
                    test::markElement(
                        fingerprint,
                        awayId,
                        "away_marker",
                        sourceId,
                        annotation::test::pixelRect(0, 0, 2, 2),
                        annotation::test::pixelRect(0, 0, 4, 4)
                    ),
                    test::clickableElement(
                        fingerprint,
                        regionId,
                        "daily_button",
                        sourceId,
                        annotation::test::pixelRect(4, 4, 2, 2),
                        annotation::test::pixelRect(3, 3, 4, 4)
                    ),
                },
                {
                    annotation::test::page(homePage, "home"),
                    annotation::test::page(awayPage, "away"),
                },
                {
                    annotation::test::reference(
                        homePage,
                        anchorId,
                        annotation::test::identifiesAs()
                    ),
                    annotation::test::reference(
                        awayPage,
                        awayId,
                        annotation::test::identifiesAs()
                    ),
                    annotation::test::reference(
                        homePage,
                        regionId,
                        annotation::test::interacts(),
                        annotation::Holding::Owned,
                        annotation::test::pixelRect(3, 3, 4, 4)
                    ),
                },
                {
                    annotation::RegressionCase{
                        annotation::RegressionSpec{
                            .id       = annotation::test::regressionId(k_regId),
                            .sourceId = sourceId,
                            .classification =
                                annotation::RegressionClassification::Positive,
                            .expectation    = annotation::ResolvedRegression{
                                .pageId = homePage,
                            },
                        }
                    },
                }
            );
            REQUIRE(created.has_value());
            return *std::move(created);
        }

        [[nodiscard]]
        auto appState() -> AppState
        {
            return AppState{
                std::filesystem::path{"personal.workbench"},
                document(),
                {},
            };
        }

        [[nodiscard]]
        auto recognizerName(
            AppState const& state,
            annotation::ElementId id
        ) -> std::string
        {
            for (auto const& recognizer : state.document().catalog().recognizers())
            {
                if (recognizer.id() == id)
                {
                    return recognizer.name().value();
                }
            }
            return {};
        }
    }

    TEST_CASE("an opened page edits and commits through the queue")
    {
        auto state = appState();
        auto ui    = PanelUiState{};
        auto const anchorId = annotation::test::elementId(k_anchorId);

        auto page = EditPage::open(state, annotation::test::pageId(k_homePage));
        REQUIRE(page.has_value());

        auto anchor = page->anchor(anchorId);
        REQUIRE(anchor.has_value());
        CHECK(anchor->name() == "home_marker");
        CHECK(anchor->rename("renamed_marker").has_value());

        // The live document is untouched until the commit is applied: EditPage
        // routes through the same one-per-frame queue the free functions use.
        std::move(*page).commit(ui, "renamed the marker");
        CHECK(recognizerName(state, anchorId) == "home_marker");

        applyPendingEdit(state, ui);
        CHECK(recognizerName(state, anchorId) == "renamed_marker");
        CHECK(ui.statusLine == "renamed the marker");
        CHECK(state.canUndo());
    }

    TEST_CASE("a commit over a stale base is refused after an undo")
    {
        auto state = appState();
        auto ui    = PanelUiState{};
        auto const anchorId = annotation::test::elementId(k_anchorId);

        // A first edit lands so there is something to undo, and to advance the
        // revision the second EditPage will be opened against.
        {
            auto first = EditPage::open(state, annotation::test::pageId(k_homePage));
            REQUIRE(first.has_value());
            REQUIRE(first->anchor(anchorId).has_value());
            REQUIRE(first->anchor(anchorId)->rename("first_name").has_value());
            std::move(*first).commit(ui, "first edit");
            applyPendingEdit(state, ui);
        }
        REQUIRE(recognizerName(state, anchorId) == "first_name");

        auto page = EditPage::open(state, annotation::test::pageId(k_homePage));
        REQUIRE(page.has_value());
        REQUIRE(page->anchor(anchorId)->rename("second_name").has_value());

        // The author undoes between opening the page and committing it, so the
        // version this draft was built against is no longer current.
        REQUIRE(state.undo());
        REQUIRE(recognizerName(state, anchorId) == "home_marker");

        std::move(*page).commit(ui, "second edit");
        applyPendingEdit(state, ui);

        // The stale commit must not resurrect the undone version.
        CHECK(ui.statusLine.find("rejected") != std::string::npos);
        CHECK(recognizerName(state, anchorId) == "home_marker");
    }

    TEST_CASE("a rejected commit moves nothing")
    {
        auto state = appState();
        auto ui    = PanelUiState{};
        auto const anchorId = annotation::test::elementId(k_anchorId);

        auto page = EditPage::open(state, annotation::test::pageId(k_homePage));
        REQUIRE(page.has_value());
        // An empty name cannot survive the rebuild, so the commit is refused.
        REQUIRE(page->anchor(anchorId)->rename("").has_value());
        std::move(*page).commit(ui, "renamed the marker");
        applyPendingEdit(state, ui);

        CHECK(ui.statusLine.find("edit rejected") != std::string::npos);
        CHECK(recognizerName(state, anchorId) == "home_marker");
        CHECK_FALSE(state.canUndo());
    }

    TEST_CASE("a second commit in one frame does not displace the first")
    {
        auto state = appState();
        auto ui    = PanelUiState{};
        auto const anchorId = annotation::test::elementId(k_anchorId);

        auto first  = EditPage::open(state, annotation::test::pageId(k_homePage));
        auto second = EditPage::open(state, annotation::test::pageId(k_homePage));
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        REQUIRE(first->anchor(anchorId)->rename("first").has_value());
        REQUIRE(second->anchor(anchorId)->rename("second").has_value());

        std::move(*first).commit(ui, "first edit");
        std::move(*second).commit(ui, "second edit");
        applyPendingEdit(state, ui);

        CHECK(recognizerName(state, anchorId) == "first");
        CHECK(ui.statusLine == "first edit");
    }

    TEST_CASE("a created member is selected only once its commit lands")
    {
        auto state = appState();
        auto ui    = PanelUiState{};

        auto page = EditPage::open(state, annotation::test::pageId(k_homePage));
        REQUIRE(page.has_value());
        auto added = page->placeAnchor(
            EditPage::NewAnchorSpec{.sourceId = annotation::test::sourceId(k_sourceId)}
        );
        REQUIRE(added.has_value());
        auto const addedId = added->id;

        std::move(*page).commitSelecting(
            ui,
            "placed an identifying mark",
            addedId,
            std::optional<annotation::SourceId>{annotation::test::sourceId(k_sourceId)}
        );

        // Selecting before the commit lands would point the selection at an id a
        // rejected edit never added.
        CHECK_FALSE(state.selectedRecognizerId().has_value());

        applyPendingEdit(state, ui);
        REQUIRE(state.selectedRecognizerId().has_value());
        CHECK(*state.selectedRecognizerId() == addedId);
        CHECK(recognizerName(state, addedId) == "anchor_1");
    }

    TEST_CASE("placeDrawn creates a member from a drawn rect and selects it")
    {
        auto state = appState();
        auto ui    = PanelUiState{};
        auto const sourceId = annotation::test::sourceId(k_sourceId);
        auto const homePage = annotation::test::pageId(k_homePage);
        auto const drawn    = annotation::test::pixelRect(2, 2, 3, 3);

        auto page = EditPage::open(state, homePage);
        REQUIRE(page.has_value());
        auto added = page->placeDrawn(
            EditPage::NewDrawnMemberSpec{
                .sourceId     = sourceId,
                .kind         = PageMemberKind::ActionTarget,
                .templateRect = drawn,
            }
        );
        REQUIRE(added.has_value());
        CHECK(added->kind == PageMemberKind::ActionTarget);
        auto const newId = added->id;

        std::move(*page).commitSelecting(
            ui,
            "drew an interactive region",
            newId,
            std::optional<annotation::SourceId>{sourceId}
        );
        // Selecting waits for the commit to land, exactly as the button paths do.
        CHECK_FALSE(state.selectedRecognizerId().has_value());

        applyPendingEdit(state, ui);
        REQUIRE(state.selectedRecognizerId().has_value());
        CHECK(*state.selectedRecognizerId() == newId);
        // One transaction, one undo entry.
        CHECK(state.canUndo());

        auto const* recognizer =
            state.document().catalog().findRecognizer(newId);
        REQUIRE(recognizer != nullptr);
        REQUIRE(recognizer->variants().size() == 1U);
        CHECK(recognizer->variants().front().templateRect == drawn);
        CHECK(recognizer->capabilities().hasInteract());

        // The element's search region is the one seeded from the drawn template
        // -- grown by its extent and clamped to the 8x8 frame -- not the whole
        // frame by luck. It always encloses the template, and the page's
        // reference inherits it rather than pinning a copy.
        CHECK(recognizer->searchRoi() == annotation::test::pixelRect(0, 0, 8, 8));
        auto const* reference = state.document().catalog().findReference(
            homePage,
            newId
        );
        REQUIRE(reference != nullptr);
        CHECK_FALSE(reference->searchRoi.has_value());
    }

    TEST_CASE("placeDrawn refuses a template that is too small")
    {
        auto state = appState();
        auto const sourceId = annotation::test::sourceId(k_sourceId);

        auto page = EditPage::open(state, annotation::test::pageId(k_homePage));
        REQUIRE(page.has_value());
        auto const added = page->placeDrawn(
            EditPage::NewDrawnMemberSpec{
                .sourceId     = sourceId,
                .kind         = PageMemberKind::Anchor,
                .templateRect = annotation::test::pixelRect(0, 0, 1, 1),
            }
        );
        REQUIRE_FALSE(added.has_value());
    }

    TEST_CASE("shownPageForScreen resolves the claiming page and honours a selection")
    {
        auto const state    = appState();
        auto const sourceId = annotation::test::sourceId(k_sourceId);
        auto const homePage = annotation::test::pageId(k_homePage);
        auto const awayPage = annotation::test::pageId(k_awayPage);

        // The source is recorded as the home page's example, so a screen with no
        // selection page draws home's members.
        CHECK(shownPageForScreen(state, sourceId) == homePage);

        // A selection page wins outright, so an element selected under away draws
        // away's members over the same screen.
        CHECK(shownPageForScreen(state, sourceId, awayPage) == awayPage);
    }

    TEST_CASE("placeExisting authorizes an existing region on this page")
    {
        auto state = appState();
        auto ui    = PanelUiState{};
        auto const regionId = annotation::test::elementId(k_regionId);
        auto const awayPage = annotation::test::pageId(k_awayPage);

        auto page = EditPage::open(state, awayPage);
        REQUIRE(page.has_value());
        CHECK(page->placeExisting(regionId).has_value());
        std::move(*page).commit(ui, "placed daily_button on away");
        applyPendingEdit(state, ui);

        // The reference IS the authorisation, and it is Referenced rather than
        // Owned: these pixels are borrowed from the page that owns them.
        auto const* reference = state.document().catalog().findReference(
            awayPage,
            regionId
        );
        REQUIRE(reference != nullptr);
        CHECK(reference->exercised.hasInteract());
        CHECK(reference->holding == annotation::Holding::Referenced);
    }

    TEST_CASE("classifyScreen sets an existing case's classification")
    {
        auto state = appState();
        auto ui    = PanelUiState{};
        auto const sourceId = annotation::test::sourceId(k_sourceId);

        auto page = EditPage::open(state, annotation::test::pageId(k_homePage));
        REQUIRE(page.has_value());
        CHECK(
            page->classifyScreen(
                sourceId,
                annotation::RegressionClassification::Negative
            ).has_value()
        );
        std::move(*page).commit(ui, "classified the screen");
        applyPendingEdit(state, ui);

        auto classification =
            std::optional<annotation::RegressionClassification>{};
        for (auto const& regression : state.document().regressions())
        {
            if (regression.sourceId() == sourceId)
            {
                classification = regression.classification();
            }
        }
        REQUIRE(classification.has_value());
        CHECK(*classification == annotation::RegressionClassification::Negative);
    }

    TEST_CASE("recording a pageless expectation commits and undo restores it")
    {
        // The screen starts recorded as the home page. Recording it as none of
        // the pages must land through the one-per-frame queue, and an undo must
        // put the resolved case back rather than leave the screen uncased.
        auto state = appState();
        auto ui    = PanelUiState{};
        auto const sourceId = annotation::test::sourceId(k_sourceId);
        auto const homePage = annotation::test::pageId(k_homePage);

        auto const caseFor =
            [&](AppState const& s)
            -> std::optional<annotation::RegressionExpectation>
        {
            for (auto const& regression : s.document().regressions())
            {
                if (regression.sourceId() == sourceId)
                {
                    return regression.expectation();
                }
            }
            return std::nullopt;
        };

        REQUIRE(
            caseFor(state)
            == annotation::RegressionExpectation{
                annotation::ResolvedRegression{.pageId = homePage},
            }
        );

        requestScreenExpectation(
            state,
            ui,
            sourceId,
            PagelessExpectation::Unknown
        );
        // Nothing moves until the frame's edit is applied.
        REQUIRE(
            caseFor(state)
            == annotation::RegressionExpectation{
                annotation::ResolvedRegression{.pageId = homePage},
            }
        );

        applyPendingEdit(state, ui);
        CHECK(
            caseFor(state)
            == annotation::RegressionExpectation{annotation::UnknownRegression{}}
        );
        REQUIRE(state.canUndo());

        REQUIRE(state.undo());
        CHECK(
            caseFor(state)
            == annotation::RegressionExpectation{
                annotation::ResolvedRegression{.pageId = homePage},
            }
        );
    }

    TEST_CASE("setTemplateRect moves the one element on every page it is placed")
    {
        auto state = appState();
        auto ui    = PanelUiState{};
        auto const regionId = annotation::test::elementId(k_regionId);
        auto const homePage = annotation::test::pageId(k_homePage);
        auto const awayPage = annotation::test::pageId(k_awayPage);
        auto const newRect  = annotation::test::pixelRect(3, 3, 3, 3);

        auto page = EditPage::open(state, homePage);
        REQUIRE(page.has_value());

        // Put the region on a second page, so a template correction has more
        // than one reference to be seen by.
        REQUIRE(page->region(regionId)->referenceOnPage(awayPage).has_value());
        REQUIRE(page->region(regionId)->pagesReferencing().size() == 2U);

        REQUIRE(page->region(regionId)->setTemplateRect(newRect).has_value());
        std::move(*page).commit(ui, "moved the template");
        applyPendingEdit(state, ui);

        // One element, one appearance, corrected once -- yet referenced by both
        // pages, so both see the correction and neither holds a copy.
        auto const* recognizer =
            state.document().catalog().findRecognizer(regionId);
        REQUIRE(recognizer != nullptr);
        REQUIRE(recognizer->variants().size() == 1U);
        CHECK(recognizer->variants().front().templateRect == newRect);
        CHECK(
            state.document().catalog().findReference(homePage, regionId)
            != nullptr
        );
        CHECK(
            state.document().catalog().findReference(awayPage, regionId)
            != nullptr
        );
    }

    TEST_CASE("a page view assembles the authored data and ids")
    {
        auto const state = appState();
        auto const anchorId = annotation::test::elementId(k_anchorId);
        auto const regionId = annotation::test::elementId(k_regionId);

        auto page = EditPage::open(state, annotation::test::pageId(k_homePage));
        REQUIRE(page.has_value());
        auto const view = page->view();

        CHECK(view.id == annotation::test::pageId(k_homePage));
        CHECK(view.name == "home");
        REQUIRE(view.claimedScreen.has_value());
        CHECK(*view.claimedScreen == annotation::test::sourceId(k_sourceId));

        REQUIRE(view.identifiedBy.size() == 1U);
        CHECK(view.identifiedBy.front().id == anchorId);
        CHECK(view.identifiedBy.front().name == "home_marker");

        REQUIRE(view.regions.size() == 1U);
        CHECK(view.regions.front().id == regionId);
        CHECK(view.regions.front().name == "daily_button");

        // Two pages, so PageView::all yields both, in draft order.
        auto const views = PageView::all(state.draft());
        REQUIRE(views.size() == 2U);
        CHECK(views.front().name == "home");
        CHECK(views.back().name == "away");
    }

    TEST_CASE("every capability a page exercises puts the element in a group")
    {
        // The bug this pins: a page member that no group rendered was reachable
        // only through undo. Reachability is a property of the VIEW, so the
        // assertion is on the view. Under a capability set one element sits in
        // several groups at once, which is the case the model change exists for.
        auto const state    = appState();
        auto const regionId = annotation::test::elementId(k_regionId);
        auto const homePage = annotation::test::pageId(k_homePage);

        auto draft           = state.draft();
        auto const reference = std::ranges::find_if(
            draft.references,
            [&](EditableReference const& candidate)
            {
                return candidate.pageId == homePage
                    && candidate.elementId == regionId;
            }
        );
        REQUIRE(reference != draft.references.end());

        auto const target = std::ranges::find(
            draft.recognizers,
            regionId,
            &EditableRecognizer::id
        );
        REQUIRE(target != draft.recognizers.end());
        target->capabilities.read = annotation::Read{};
        reference->exercised.read = annotation::ExercisedRead{};

        auto const view = PageView::of(draft, homePage);
        REQUIRE(view.has_value());

        auto const has = [&](std::vector<PageView::MemberRow> const& rows)
        {
            return std::ranges::any_of(
                rows,
                [&](PageView::MemberRow const& row) { return row.id == regionId; }
            );
        };
        CHECK(has(view->regions));
        CHECK(has(view->infos));
        CHECK_FALSE(has(view->identifiedBy));

        // And it is a document the model accepts, so the two groups describe a
        // state an author can actually reach.
        CHECK(buildAuthoringDocument(draft).has_value());
    }

    TEST_CASE("createFrom builds a page around a screen and its first anchor")
    {
        auto state = appState();
        auto ui    = PanelUiState{};
        auto const sourceId = annotation::test::sourceId(k_sourceId);

        auto page = EditPage::createFrom(state, sourceId);
        REQUIRE(page.has_value());

        // The page comes with exactly one identifying mark, which is what the
        // pages panel selects once the commit lands.
        auto const view = page->view();
        REQUIRE(view.identifiedBy.size() == 1U);
        auto const anchorId = view.identifiedBy.front().id;

        std::move(*page).commitSelecting(
            ui,
            "added a page",
            anchorId,
            std::optional<annotation::SourceId>{sourceId}
        );
        CHECK_FALSE(state.selectedRecognizerId().has_value());

        applyPendingEdit(state, ui);
        CHECK(state.document().catalog().pages().size() == 3U);
        REQUIRE(state.selectedRecognizerId().has_value());
        CHECK(*state.selectedRecognizerId() == anchorId);
    }

    TEST_CASE("placeRegion authorizes a fresh interactive region on this page")
    {
        auto state = appState();
        auto ui    = PanelUiState{};
        auto const awayPage = annotation::test::pageId(k_awayPage);

        auto page = EditPage::open(state, awayPage);
        REQUIRE(page.has_value());
        auto added = page->placeRegion(
            EditPage::NewRegionSpec{.sourceId = annotation::test::sourceId(k_sourceId)}
        );
        REQUIRE(added.has_value());
        auto const newId = added->id;
        std::move(*page).commit(ui, "placed a region");
        applyPendingEdit(state, ui);

        auto const* recognizer =
            state.document().catalog().findRecognizer(newId);
        REQUIRE(recognizer != nullptr);
        CHECK(recognizer->capabilities().hasInteract());
        auto const* reference = state.document().catalog().findReference(
            awayPage,
            newId
        );
        REQUIRE(reference != nullptr);
        CHECK(reference->exercised.hasInteract());
    }

    TEST_CASE("claimScreen records the screen as this page's example")
    {
        auto state = appState();
        auto ui    = PanelUiState{};
        auto const awayPage = annotation::test::pageId(k_awayPage);
        auto const sourceId = annotation::test::sourceId(k_sourceId);

        auto page = EditPage::open(state, awayPage);
        REQUIRE(page.has_value());
        CHECK(page->claimScreen(sourceId).has_value());
        std::move(*page).commit(ui, "recorded the screen");
        applyPendingEdit(state, ui);

        // A screen resolves to one page, so the claim re-points the existing
        // case; the away page's view now names the source as its example.
        auto reopened = EditPage::open(state, awayPage);
        REQUIRE(reopened.has_value());
        auto const view = reopened->view();
        REQUIRE(view.claimedScreen.has_value());
        CHECK(*view.claimedScreen == sourceId);
    }

    namespace
    {
        // The base project with the interactive region placed on the away page as
        // well as home, at a different search rectangle on each, so an edit to one
        // page's range can be checked against the other's.
        [[nodiscard]]
        auto twoPlacementState() -> AppState
        {
            auto const fingerprint = annotation::test::fingerprint(8, 8, 96, 96);
            auto const sourceId    = annotation::test::sourceId(k_sourceId);
            auto const anchorId    = annotation::test::elementId(k_anchorId);
            auto const awayId      = annotation::test::elementId(k_awayId);
            auto const regionId    = annotation::test::elementId(k_regionId);
            auto const homePage    = annotation::test::pageId(k_homePage);
            auto const awayPage    = annotation::test::pageId(k_awayPage);
            auto const sourceHash  = annotation::sha256(
                std::span<std::byte const>{}
            );
            REQUIRE(sourceHash.has_value());

            auto source = annotation::AuthoringSource::create(
                annotation::AuthoringSourceSpec{
                    .id          = sourceId,
                    .contentHash = *sourceHash,
                    .fingerprint = fingerprint,
                    .provenance  = annotation::ImportedSourceProvenance{},
                }
            );
            REQUIRE(source.has_value());

            auto created = annotation::AuthoringDocument::create(
                annotation::test::projectId(),
                fingerprint,
                {*source},
                {
                    test::markElement(
                        fingerprint,
                        anchorId,
                        "home_marker",
                        sourceId,
                        annotation::test::pixelRect(0, 0, 2, 2),
                        annotation::test::pixelRect(0, 0, 4, 4)
                    ),
                    test::markElement(
                        fingerprint,
                        awayId,
                        "away_marker",
                        sourceId,
                        annotation::test::pixelRect(0, 0, 2, 2),
                        annotation::test::pixelRect(0, 0, 4, 4)
                    ),
                    test::clickableElement(
                        fingerprint,
                        regionId,
                        "daily_button",
                        sourceId,
                        annotation::test::pixelRect(4, 4, 2, 2),
                        annotation::test::pixelRect(3, 3, 4, 4)
                    ),
                },
                {
                    annotation::test::page(homePage, "home"),
                    annotation::test::page(awayPage, "away"),
                },
                {
                    annotation::test::reference(
                        homePage,
                        anchorId,
                        annotation::test::identifiesAs()
                    ),
                    annotation::test::reference(
                        awayPage,
                        awayId,
                        annotation::test::identifiesAs()
                    ),
                    annotation::test::reference(
                        homePage,
                        regionId,
                        annotation::test::interacts(),
                        annotation::Holding::Owned,
                        annotation::test::pixelRect(3, 3, 4, 4)
                    ),
                    annotation::test::reference(
                        awayPage,
                        regionId,
                        annotation::test::interacts(),
                        annotation::Holding::Referenced,
                        annotation::test::pixelRect(4, 4, 4, 4)
                    ),
                },
                {
                    annotation::RegressionCase{
                        annotation::RegressionSpec{
                            .id       = annotation::test::regressionId(k_regId),
                            .sourceId = sourceId,
                            .classification =
                                annotation::RegressionClassification::Positive,
                            .expectation    = annotation::ResolvedRegression{
                                .pageId = homePage,
                            },
                        }
                    },
                }
            );
            REQUIRE(created.has_value());
            return AppState{
                std::filesystem::path{"personal.workbench"},
                *std::move(created),
                {},
            };
        }

        // The region searched for one element on one page: the reference's
        // refinement when it made one, the element's own otherwise.
        [[nodiscard]]
        auto referenceRoi(
            AppState const& state,
            annotation::PageId page,
            annotation::ElementId element
        ) -> std::optional<PixelRect>
        {
            auto const* p_reference = state.document().catalog().findReference(
                page,
                element
            );
            auto const* p_element = state.document().findElement(element);
            if (p_reference == nullptr || p_element == nullptr)
            {
                return std::nullopt;
            }
            return p_reference->searchRoi.value_or(p_element->searchRoi());
        }
    }

    TEST_CASE("referenceContext resolves the page that claims the shown screen")
    {
        auto const state    = twoPlacementState();
        auto const sourceId = annotation::test::sourceId(k_sourceId);
        auto const regionId = annotation::test::elementId(k_regionId);
        auto const anchorId = annotation::test::elementId(k_anchorId);
        auto const homePage = annotation::test::pageId(k_homePage);

        // The source is recorded as the home page's example, and the region is
        // placed on home, so its home placement is the editing context.
        auto const context = referenceContext(state, regionId, sourceId);
        REQUIRE(context.has_value());
        CHECK(context->page == homePage);
        CHECK(context->searchRoi == annotation::test::pixelRect(3, 3, 4, 4));

        // A reference exercising identify reads the element's own region and
        // may not refine it, so it never yields an editable context.
        CHECK_FALSE(referenceContext(state, anchorId, sourceId).has_value());
    }

    TEST_CASE("referenceContext prefers the selection's page over the shown screen's claim")
    {
        auto const state    = twoPlacementState();
        auto const sourceId = annotation::test::sourceId(k_sourceId);
        auto const regionId = annotation::test::elementId(k_regionId);
        auto const homePage = annotation::test::pageId(k_homePage);
        auto const awayPage = annotation::test::pageId(k_awayPage);

        // With no selection page, the shown screen's claim (home) resolves, as
        // the fallback that predates the typed selection.
        auto const fallback = referenceContext(state, regionId, sourceId);
        REQUIRE(fallback.has_value());
        CHECK(fallback->page == homePage);
        CHECK(fallback->searchRoi == annotation::test::pixelRect(3, 3, 4, 4));

        // A selection page wins outright: an element selected under the away page
        // edits the away placement even though the shown screen is claimed by
        // home. This is the one behaviour U1a intentionally changes.
        auto const chosen = referenceContext(
            state,
            regionId,
            sourceId,
            awayPage
        );
        REQUIRE(chosen.has_value());
        CHECK(chosen->page == awayPage);
        CHECK(chosen->searchRoi == annotation::test::pixelRect(4, 4, 4, 4));
    }

    TEST_CASE("a page-context ROI edit moves only that page's reference")
    {
        auto state = twoPlacementState();
        auto ui    = PanelUiState{};
        auto const regionId = annotation::test::elementId(k_regionId);
        auto const homePage = annotation::test::pageId(k_homePage);
        auto const awayPage = annotation::test::pageId(k_awayPage);
        auto const edited   = annotation::test::pixelRect(1, 1, 6, 6);

        editSelectedRectOnRelease(
            state,
            ui,
            regionId,
            std::optional<annotation::PageId>{homePage},
            edited
        );
        applyPendingEdit(state, ui);

        // The home reference moved; the away reference and the element's own
        // default range are both untouched -- the defect this covers moved all of
        // them at once.
        CHECK(referenceRoi(state, homePage, regionId) == edited);
        CHECK(
            referenceRoi(state, awayPage, regionId)
            == annotation::test::pixelRect(4, 4, 4, 4)
        );
        auto const* element = state.document().findElement(regionId);
        REQUIRE(element != nullptr);
        CHECK(element->searchRoi() == annotation::test::pixelRect(3, 3, 4, 4));
        CHECK(ui.statusLine.find("on page \"home\"") != std::string::npos);
    }

    TEST_CASE("a context-free ROI edit moves the element default, not a reference")
    {
        auto state = twoPlacementState();
        auto ui    = PanelUiState{};
        auto const regionId = annotation::test::elementId(k_regionId);
        auto const homePage = annotation::test::pageId(k_homePage);
        auto const awayPage = annotation::test::pageId(k_awayPage);
        auto const edited   = annotation::test::pixelRect(1, 1, 6, 6);

        editSelectedRectOnRelease(
            state,
            ui,
            regionId,
            std::optional<annotation::PageId>{},
            edited
        );
        applyPendingEdit(state, ui);

        auto const* element = state.document().findElement(regionId);
        REQUIRE(element != nullptr);
        CHECK(element->searchRoi() == edited);
        CHECK(
            referenceRoi(state, homePage, regionId)
            == annotation::test::pixelRect(3, 3, 4, 4)
        );
        CHECK(
            referenceRoi(state, awayPage, regionId)
            == annotation::test::pixelRect(4, 4, 4, 4)
        );
    }
}
