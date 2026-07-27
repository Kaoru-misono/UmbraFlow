#pragma once

#include "panel-state.hpp"
#include "preview.hpp"
#include "app/workbench-app.hpp"

#include <annotation/catalog.hpp>

#include <core/types/integer.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace uf::workbench
{
    // Driving a whole-model check from the UI and describing what came back.
    // The drawing lives in the panels; everything here is values and text, so
    // test-workbench can hold the descriptions to their meaning without a GUI.
    //
    // The distinction the text has to preserve is that Stopped and Unclaimed are
    // not verdicts. A screen the clock never reached, and one no regression case
    // claims, both mean the check has nothing to say -- reporting either as a
    // failure tells the author their model is broken when it is not.

    // The screen a page's regression case records, which is the screen a margin
    // is measured against.
    [[nodiscard]]
    auto pageSampleSource(
        AppState const& state,
        annotation::PageSignature const& page
    ) -> std::optional<annotation::SourceId>;

    // A score as a share of the budget it had to beat. Reported against the
    // shared budget rather than as a ratio, because maximumSad depends only on
    // the template and the threshold -- never on the screen -- so two screens'
    // numbers stay directly comparable.
    [[nodiscard]]
    auto budgetPercentText(
        std::optional<uint64> sadScore,
        uint64 maximumSad
    ) -> std::string;

    // One screen's line in the Pages panel: what it resolved to, what it was
    // recorded as, and which of those two the outcome actually means.
    [[nodiscard]]
    auto screenCheckText(AppState const& state, ScreenCheck const& screen) -> std::string;

    // The last check's entry for a screen, or nullptr when the last check did
    // not cover it. Valid until the next check replaces the result.
    [[nodiscard]]
    auto findScreenCheck(
        AppState const& state UF_LIFETIME_BOUND,
        annotation::SourceId sourceId
    ) -> ScreenCheck const*;

    // The last check's margin for a recognizer, under the same lifetime rule as
    // findScreenCheck.
    [[nodiscard]]
    auto findMargin(
        AppState const& state UF_LIFETIME_BOUND,
        annotation::RecognizerId recognizerId
    ) -> RecognizerMargin const*;

    // The last check's grid cell for one element on one screen, or nullptr when
    // the last check did not cover the pair. Keyed by the element id, never a
    // derived per-page id, so it matches the same element the margins do. Valid
    // until the next check replaces the result.
    [[nodiscard]]
    auto findModelCell(
        AppState const& state UF_LIFETIME_BOUND,
        annotation::RecognizerId elementId,
        annotation::SourceId screenId
    ) -> ModelCheckCell const*;

    // Starts a check on the job's worker thread, sized so the deadline scales
    // with the work rather than with the screen count alone. liveFrameBytes is
    // empty when no frame was captured; capture stays with the caller because it
    // belongs to the thread that owns the graphics device.
    auto startModelCheck(
        AppState& state,
        PanelUiState& ui,
        std::span<std::byte const> liveFrameBytes
    ) -> void;

    // Takes a finished check, if one is waiting, and states it. Called before
    // anything draws, so a verdict appears in the same frame it was delivered.
    auto collectModelCheck(AppState& state, PanelUiState& ui) -> void;
}
