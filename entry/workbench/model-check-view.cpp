#include "model-check-view.hpp"

#include "authoring-actions.hpp"
#include "model-check-job.hpp"
#include "preview.hpp"
#include "app/workbench-app.hpp"

#include <annotation/authoring-document.hpp>
#include <annotation/catalog.hpp>
#include <annotation/recognition-runtime.hpp>

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        // A model check searches every recognizer against every screen, so the
        // work is the product of the two. An allowance scaled by screen count
        // alone starves the check exactly as the model grows large enough to
        // need it. runModelCheck divides this total evenly across the screens,
        // so no screen can spend another's share. The comparison budget does not
        // scale: the policy is copied per screen and never decremented, so every
        // search starts from a full budget while the deadline is the only limit
        // the run really shares.
        //
        // A debug build searches roughly thirty times slower than a release one,
        // so a large model checked in debug will exhaust the ceiling and report
        // the screens it could not reach. Check models on a release build.
        constexpr auto k_modelCheckDeadlinePerSearch = std::chrono::milliseconds{1500};

        // Past this point the number that decides the budget is how long an
        // author will sit and wait, not how much work there is to do.
        constexpr auto k_modelCheckDeadlineCeiling = std::chrono::milliseconds{120'000};

    }

    // The screen a page was authored from. The regression case recording that
    // this source is expected to resolve to this page is the document's only
    // explicit statement of it -- an anchor names the page it identifies, but
    // the image it happens to be drawn on is a different claim -- so that is
    // read first.
    //
    // A page authored before pages carried such a case has none, and would
    // otherwise be stuck with every "add to this page" button disabled. For
    // those, fall back to the screen its first anchor was drawn on: not the
    // same claim, but the same image in every project a page-per-screen
    // workflow produces.
    [[nodiscard]]
    auto pageSampleSource(
        AppState const& state,
        annotation::PageSignature const& page
    ) -> std::optional<annotation::SourceId>
    {
        if (auto const claimed = claimedScreen(state, page.id()))
        {
            return claimed;
        }
        for (auto const& anchorId : page.required())
        {
            if (auto const source = sourceOfRecognizer(state, anchorId))
            {
                return source;
            }
        }
        return std::nullopt;
    }

    // A score as a percentage of the recognizer's own budget: under 100 is a
    // hit and the remainder is the room it has, over 100 is a miss and the
    // excess is how far clear it stayed.
    [[nodiscard]]
    auto budgetPercentText(
        std::optional<uint64> sadScore,
        uint64 maximumSad
    ) -> std::string
    {
        if (!sadScore.has_value() || maximumSad == 0U)
        {
            return "-";
        }
        return std::format("{}%", *sadScore * 100U / maximumSad);
    }

    // What a check found for one screen, phrased as the problem rather than
    // the classification: the author has to know what to change.
    [[nodiscard]]
    auto screenCheckText(
        AppState const& state,
        ScreenCheck const& screen
    ) -> std::string
    {
        switch (screen.outcome)
        {
        case ScreenCheckOutcome::Correct:
            return "resolves correctly";
        case ScreenCheckOutcome::WrongPage:
            return std::format(
                "resolves to \"{}\" instead",
                screen.resolvedPageId.has_value()
                    ? pageName(state, *screen.resolvedPageId)
                    : std::string{"?"}
            );
        case ScreenCheckOutcome::Unknown:
            return "no page resolves: some required mark does not match here";
        case ScreenCheckOutcome::Ambiguous:
            return "two pages both match: they need something to tell them apart";
        case ScreenCheckOutcome::Unclaimed:
            return "no page is recorded for this screen";
        case ScreenCheckOutcome::Stopped:
            return "the search hit its budget before finishing";
        }
        return "?";
    }

    [[nodiscard]]
    auto findScreenCheck(
        AppState const& state,
        annotation::SourceId sourceId
    ) -> ScreenCheck const*
    {
        auto const& check = state.lastModelCheck();
        if (!check.has_value())
        {
            return nullptr;
        }
        auto const found = std::ranges::find(
            check->screens,
            sourceId,
            &ScreenCheck::sourceId
        );
        return found == check->screens.end() ? nullptr : &*found;
    }

    [[nodiscard]]
    auto findMargin(
        AppState const& state,
        annotation::RecognizerId recognizerId
    ) -> RecognizerMargin const*
    {
        auto const& check = state.lastModelCheck();
        if (!check.has_value())
        {
            return nullptr;
        }
        auto const found = std::ranges::find(
            check->margins,
            recognizerId,
            &RecognizerMargin::recognizerId
        );
        return found == check->margins.end() ? nullptr : &*found;
    }

    // Hands the whole model to a worker, with a frame from the running target
    // when one was captured and an empty span when there was none. The
    // deadline is what stops a check running forever; see the constants for
    // how it is sized.
    auto startModelCheck(
        AppState& state,
        PanelUiState& ui,
        std::span<std::byte const> liveFrameBytes
    ) -> void
    {
        auto const assets = state.compilerSourceAssets();
        if (!assets)
        {
            ui.statusLine = std::format(
                "check failed: {}",
                toString(assets.error())
            );
            return;
        }

        // The live frame is one more screen to search, so it is counted here
        // as well: the deadline has to cover the work actually queued.
        auto const screens = std::max<std::size_t>(
            assets->size() + (liveFrameBytes.empty() ? 0U : 1U),
            1U
        );
        auto const marks = std::max<std::size_t>(
            state.document().catalog().recognizers().size(),
            1U
        );
        // Clamped before the multiply, so the product cannot run past what
        // the ceiling would allow anyway.
        auto const budgetedSearches = static_cast<std::size_t>(
            k_modelCheckDeadlineCeiling / k_modelCheckDeadlinePerSearch
        );
        auto const searches  = std::min(screens * marks, budgetedSearches);
        auto const allowance = k_modelCheckDeadlinePerSearch
            * static_cast<std::chrono::milliseconds::rep>(searches);

        // No deadline means an unbounded search, which is the whole thing
        // this budget exists to prevent, so a missing one is never an
        // acceptable fallback. The steady clock would have to overflow to get
        // here.
        auto const deadline = MonotonicInstant::now().checkedAdd(allowance);
        UF_CHECK(deadline.has_value());

        ui.modelCheck.start(
            state.document(),
            *assets,
            liveFrameBytes,
            annotation::RecognitionPolicy{
                .maximumPixelComparisons = k_recognitionComparisonBudget,
                .deadline                = deadline,
            }
        );
        ui.statusLine = liveFrameBytes.empty()
            ? "checking every mark against every screen..."
            : "checking every mark against every screen and the live one...";
    }

    // Takes a finished worker's answer, if there is one, and states it. An
    // edit made while the check ran discarded it, so nothing arriving here
    // can describe a document other than the current one.
    auto collectModelCheck(AppState& state, PanelUiState& ui) -> void
    {
        auto finished = ui.modelCheck.takeResult();
        if (!finished.has_value())
        {
            return;
        }
        if (!*finished)
        {
            ui.statusLine = std::format(
                "check failed: {}",
                toString(finished->error())
            );
            return;
        }

        // Stopped and Unclaimed are not verdicts: the first means the clock
        // ran out before this screen was reached, the second that no
        // regression case says which page it should be. Counting either as
        // a failure tells the author their model is broken when the check
        // simply has nothing to say, so each is reported as what it is.
        auto const countOf = [&screens = (*finished)->screens](
            ScreenCheckOutcome outcome
        ) -> std::ptrdiff_t
        {
            return std::ranges::count(screens, outcome, &ScreenCheck::outcome);
        };
        auto const total     = (*finished)->screens.size();
        auto const stopped   = countOf(ScreenCheckOutcome::Stopped);
        auto const unclaimed = countOf(ScreenCheckOutcome::Unclaimed);
        auto const judged    = static_cast<std::ptrdiff_t>(total)
            - stopped
            - unclaimed;
        auto const wrong = judged - countOf(ScreenCheckOutcome::Correct);

        // The live screen has no recorded expectation to be right or wrong
        // about -- the author is looking at it -- so it is reported as what
        // it resolved to and left out of the counts above. Read before the
        // move, which empties the result.
        auto live = std::string{};
        if (auto const& liveCheck = (*finished)->live)
        {
            live = "; live screen ";
            if (liveCheck->stop.has_value())
            {
                live += "not reached before the deadline";
            }
            else if (liveCheck->resolvedPageId.has_value())
            {
                live += std::format(
                    "is \"{}\"",
                    pageName(state, *liveCheck->resolvedPageId)
                );
            }
            else if (liveCheck->pageKind.has_value())
            {
                live += previewPageKindName(*liveCheck->pageKind);
            }
        }

        state.setLastModelCheck(*std::move(*finished));

        auto summary = wrong == 0
            ? std::format("all {} judged screens resolve correctly", judged)
            : std::format(
                "{} of {} judged screens do not resolve as recorded; see Pages",
                wrong,
                judged
            );
        if (stopped > 0)
        {
            summary += std::format(
                "; {} not reached before the deadline (check on a release build)",
                stopped
            );
        }
        if (unclaimed > 0)
        {
            summary += std::format("; {} have no recorded page", unclaimed);
        }
        summary += live;
        ui.statusLine = std::move(summary);
    }
}
