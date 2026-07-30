#include "../annotation/test-helpers.hpp"

#include <model-check-view.hpp>
#include <panel-state.hpp>
#include <preview.hpp>
#include <workbench-app.hpp>

#include <annotation/authoring-document.hpp>
#include <annotation/resource.hpp>
#include <annotation/content-hash.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        constexpr auto k_sourceId = "00000000-0000-0000-0000-000000000601";
        constexpr auto k_otherId  = "00000000-0000-0000-0000-000000000602";
        constexpr auto k_anchorId = "00000000-0000-0000-0000-000000000611";
        constexpr auto k_pageId   = "00000000-0000-0000-0000-000000000621";

        [[nodiscard]]
        auto document() -> annotation::AuthoringDocument
        {
            auto const fingerprint = annotation::test::fingerprint(8, 8, 96, 96);
            auto const sourceId    = annotation::test::sourceId(k_sourceId);
            auto const anchorId    = annotation::test::elementId(k_anchorId);
            auto const pageId      = annotation::test::pageId(k_pageId);
            auto const sourceHash  = annotation::sha256(std::span<std::byte const>{});
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
                    annotation::test::anchorElement(
                        fingerprint,
                        anchorId,
                        "home_marker",
                        sourceId,
                        annotation::test::pixelRect(0, 0, 2, 2),
                        annotation::test::pixelRect(0, 0, 4, 4)
                    ),
                },
                {annotation::test::page(pageId, "home", {anchorId})},
                {},
                {}
            );
            REQUIRE(created.has_value());
            return *std::move(created);
        }

        [[nodiscard]]
        auto appState() -> AppState
        {
            return AppState{std::filesystem::path{"personal.workbench"}, document(), {}};
        }

        [[nodiscard]]
        auto screen(ScreenCheckOutcome outcome, char const* id = k_sourceId) -> ScreenCheck
        {
            return ScreenCheck{
                .sourceId = annotation::test::sourceId(id),
                .outcome  = outcome,
            };
        }

        // Hands the job a finished result without running a search, so the text
        // collectModelCheck produces can be held to its meaning directly.
        auto deliver(PanelUiState& ui, ModelCheck check) -> void
        {
            ui.modelCheck.startWith(
                [check = std::move(check)](std::stop_token) -> Result<ModelCheck>
                {
                    return check;
                }
            );
            while (ui.modelCheck.running())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }
    }

    TEST_CASE("a score is reported as its share of the budget it had to beat")
    {
        CHECK(budgetPercentText(std::optional<uint64>{0U}, 200U) == "0%");
        CHECK(budgetPercentText(std::optional<uint64>{100U}, 200U) == "50%");
        CHECK(budgetPercentText(std::optional<uint64>{200U}, 200U) == "100%");

        // No score and no budget are both "nothing to report", not "zero".
        CHECK(budgetPercentText(std::nullopt, 200U) == "-");
        CHECK(budgetPercentText(std::optional<uint64>{1U}, 0U) == "-");
    }

    TEST_CASE("a screen the check could not judge does not read as a failure")
    {
        auto const state = appState();

        CHECK(screenCheckText(state, screen(ScreenCheckOutcome::Correct)) == "resolves correctly");

        // The two the author must be able to tell apart from a wrong answer.
        CHECK(
            screenCheckText(state, screen(ScreenCheckOutcome::Unclaimed))
            == "no page is recorded for this screen"
        );
        CHECK(
            screenCheckText(state, screen(ScreenCheckOutcome::Stopped))
            == "the search hit its budget before finishing"
        );

        // And the ones that are genuinely a verdict against the model.
        CHECK(
            screenCheckText(state, screen(ScreenCheckOutcome::Unknown))
            == "no page resolves: some required mark does not match here"
        );
        CHECK(
            screenCheckText(state, screen(ScreenCheckOutcome::Ambiguous))
            == "two pages both match: they need something to tell them apart"
        );
    }

    TEST_CASE("every screen resolving correctly is reported as clean")
    {
        auto state = appState();
        auto ui    = PanelUiState{};

        auto check = ModelCheck{};
        check.screens.emplace_back(screen(ScreenCheckOutcome::Correct, k_sourceId));
        check.screens.emplace_back(screen(ScreenCheckOutcome::Correct, k_otherId));
        deliver(ui, std::move(check));

        collectModelCheck(state, ui);

        CHECK(ui.statusLine.find("all 2 judged screens resolve correctly") != std::string::npos);
        CHECK(state.lastModelCheck().has_value());
    }

    TEST_CASE("an unreached screen is counted apart from a wrong one")
    {
        auto state = appState();
        auto ui    = PanelUiState{};

        auto check = ModelCheck{};
        check.screens.emplace_back(screen(ScreenCheckOutcome::Correct, k_sourceId));
        check.screens.emplace_back(screen(ScreenCheckOutcome::Stopped, k_otherId));
        deliver(ui, std::move(check));

        collectModelCheck(state, ui);

        // One screen was judged and it was right, so the model is not reported
        // as broken; the screen the clock never reached is stated separately.
        CHECK(ui.statusLine.find("all 1 judged screens resolve correctly") != std::string::npos);
        CHECK(ui.statusLine.find("1 not reached before the deadline") != std::string::npos);
    }

    TEST_CASE("a screen with no recorded page is counted apart from a wrong one")
    {
        auto state = appState();
        auto ui    = PanelUiState{};

        auto check = ModelCheck{};
        check.screens.emplace_back(screen(ScreenCheckOutcome::Correct, k_sourceId));
        check.screens.emplace_back(screen(ScreenCheckOutcome::Unclaimed, k_otherId));
        deliver(ui, std::move(check));

        collectModelCheck(state, ui);

        CHECK(ui.statusLine.find("all 1 judged screens resolve correctly") != std::string::npos);
        CHECK(ui.statusLine.find("1 have no recorded page") != std::string::npos);
    }

    TEST_CASE("a screen that resolves to the wrong page is reported as one")
    {
        auto state = appState();
        auto ui    = PanelUiState{};

        auto check = ModelCheck{};
        check.screens.emplace_back(screen(ScreenCheckOutcome::WrongPage, k_sourceId));
        check.screens.emplace_back(screen(ScreenCheckOutcome::Correct, k_otherId));
        deliver(ui, std::move(check));

        collectModelCheck(state, ui);

        CHECK(
            ui.statusLine.find("1 of 2 judged screens do not resolve as recorded")
            != std::string::npos
        );
    }

    TEST_CASE("a failed check reports the failure instead of a verdict")
    {
        auto state = appState();
        auto ui    = PanelUiState{};

        ui.modelCheck.startWith(
            [](std::stop_token) -> Result<ModelCheck>
            {
                return fail(AutomationErrorKind::InvalidResource, "could not build a runtime");
            }
        );
        while (ui.modelCheck.running())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }

        collectModelCheck(state, ui);

        CHECK(ui.statusLine.find("check failed") != std::string::npos);
        CHECK_FALSE(state.lastModelCheck().has_value());
    }

    TEST_CASE("collecting with nothing in flight leaves the status line alone")
    {
        auto state    = appState();
        auto ui       = PanelUiState{};
        ui.statusLine = "untouched";

        collectModelCheck(state, ui);

        CHECK(ui.statusLine == "untouched");
    }

    TEST_CASE("a check result is found by screen and by recognizer")
    {
        auto state = appState();

        auto check = ModelCheck{};
        check.screens.emplace_back(screen(ScreenCheckOutcome::Correct, k_sourceId));
        check.margins.emplace_back(
            RecognizerMargin{
                .recognizerId = annotation::test::elementId(k_anchorId),
                .maximumSad   = 1234U,
            }
        );
        state.setLastModelCheck(std::move(check));

        auto const* p_screen = findScreenCheck(state, annotation::test::sourceId(k_sourceId));
        REQUIRE(p_screen != nullptr);
        CHECK(p_screen->outcome == ScreenCheckOutcome::Correct);

        auto const* p_margin = findMargin(state, annotation::test::elementId(k_anchorId));
        REQUIRE(p_margin != nullptr);
        CHECK(p_margin->maximumSad == 1234U);

        // A screen the last check did not cover has no entry, rather than a
        // default-constructed one that would read as a verdict.
        CHECK(findScreenCheck(state, annotation::test::sourceId(k_otherId)) == nullptr);
    }

    TEST_CASE("a grid cell is found by element and screen")
    {
        auto state = appState();

        auto check = ModelCheck{};
        check.cells.emplace_back(
            ModelCheckCell{
                .elementId   = annotation::test::elementId(k_anchorId),
                .screenId    = annotation::test::sourceId(k_sourceId),
                .outcome     = ModelCellOutcome::Hit,
                .sadScore    = std::optional<uint64>{0U},
                .maximumSad  = 200U,
                .expectedHit = true,
            }
        );
        state.setLastModelCheck(std::move(check));

        auto const* p_cell = findModelCell(
            state,
            annotation::test::elementId(k_anchorId),
            annotation::test::sourceId(k_sourceId)
        );
        REQUIRE(p_cell != nullptr);
        CHECK(p_cell->outcome == ModelCellOutcome::Hit);

        // A pair the last check did not cover has no cell, so the grid reads it
        // as absent rather than as a default-constructed verdict.
        CHECK(
            findModelCell(
                state,
                annotation::test::elementId(k_anchorId),
                annotation::test::sourceId(k_otherId)
            )
            == nullptr
        );
    }
}
