#pragma once

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace uf::cli
{
    inline constexpr auto k_defaultRunRecognitionTimeout = (
        std::chrono::duration_cast<MonotonicInstant::Duration>(
            std::chrono::milliseconds{2000}
        )
    );
    inline constexpr auto k_defaultRunMaxFrameAge = (
        std::chrono::duration_cast<MonotonicInstant::Duration>(
            std::chrono::milliseconds{750}
        )
    );

    // A per-recognition ceiling on template pixel comparisons, shared by
    // `umbra-flow run` and `umbra-authoring` so one number governs both.
    //
    // What a search costs is arithmetic rather than a guess. One candidate
    // position costs templateWidth * templateHeight comparisons, and the search
    // walks (roiWidth - templateWidth + 1) * (roiHeight - templateHeight + 1)
    // positions, so its worst case is the product; pruning only ever lowers it.
    // The figure to cover is not one search, because evaluatePage shares one
    // budget across every page anchor in the catalog -- it is the sum over them.
    //
    // The envelope: a page evaluation whose anchors are each as costly as the
    // widest ordinary authored one, a 200x50 template in a 480x90 search region.
    // That is 281 * 41 = 11,521 positions x 10,000 pixels = 115,210,000
    // comparisons, and eight pages identified by two anchors each make sixteen
    // of them: 1,843,360,000. 2^31 = 2,147,483,648 covers that with headroom.
    //
    // It deliberately does not cover a full-frame search for a small template.
    // A 66x46 template over 1600x900 walks 1535 * 855 = 1,312,425 positions x
    // 3,036 pixels = 3,984,522,300 comparisons, about 1.9x this ceiling, so such
    // a project has to author a search region or raise --budget on purpose.
    // Sizing the default for that case instead would leave nothing failing
    // closed on a project whose geometry is genuinely unbounded.
    inline constexpr auto k_defaultPixelComparisonBudget = uint64{1} << 31;

    inline constexpr auto k_defaultTracePath = std::string_view{
        "umbra-flow-trace.jsonl"
    };

    // `check` writes to its own default so a falsification run does not
    // overwrite the evidence of the last real one. An operator checking a model
    // between two runs is the ordinary case, not the exception.
    inline constexpr auto k_defaultCheckTracePath = std::string_view{
        "umbra-flow-check-trace.jsonl"
    };

    // Parsed inputs for the `run` subcommand. Every field carries a resolved
    // value once parsing succeeds; the optional flags fall back to the defaults
    // above so the composition never observes an unset field. A run is always a
    // project-owned task addressed as (project, task): the single-step
    // --page/--action smoke path is gone, so there is one mode and nothing to
    // choose between.
    //
    // How long a task waits for a page, and how often it re-observes while
    // waiting, are deliberately absent. The wait loop is the framework's Luau,
    // so those two numbers are written in the task that waits -- a CLI flag for
    // them would be an operator overriding a decision the script owns, and
    // after the loop moved there was nothing left for such a flag to reach.
    struct RunArgs final
    {
        std::filesystem::path project{};
        std::string           selector{};
        std::string           task{};

        uint64                     budget{k_defaultPixelComparisonBudget};
        MonotonicInstant::Duration recognitionTimeout{k_defaultRunRecognitionTimeout};
        MonotonicInstant::Duration maxFrameAge{k_defaultRunMaxFrameAge};

        std::filesystem::path trace{k_defaultTracePath};

        // The "models" directory cycle_read's OCR engine loads from, or absent
        // for a run that never reads text. Absent by default, because the
        // weights are tens of megabytes and a run that reads nothing must not
        // pay for them; cycle_read refuses on its own terms instead. When
        // present, the layout is the one modules/ocr/external commits and the
        // build stages beside this binary: `ocrModels / "ppocr-v6-small-rec" /
        // {inference.onnx, inference.yml}`.
        std::optional<std::filesystem::path> ocrModels{};

        auto operator==(RunArgs const&) const -> bool = default;
    };

    [[nodiscard]]
    auto parseRunArguments(std::span<std::string const> raw) -> Result<RunArgs>;

    [[nodiscard]] auto runUsageText() noexcept -> std::string_view;

    // How long `drive` waits with an empty command queue before it ends the session
    // on its own.
    //
    // CALIBRATION: two minutes matches the m0-demo input agent's own idle timeout,
    // which is the protocol this one follows. It exists so an operator that walks
    // away, or a driving process that died, does not leave a session holding a
    // capture device and a bound target indefinitely.
    inline constexpr auto k_defaultDriveIdleTimeout = (
        std::chrono::duration_cast<MonotonicInstant::Duration>(
            std::chrono::seconds{120}
        )
    );

    // Parsed inputs for the `drive` subcommand: the operator front-end.
    //
    // It binds a target and a project exactly as RunArgs does, and every recognition
    // and delivery bound below is the same field with the same default, because the
    // two front-ends must not be able to run under different guarantees. What
    // replaces --task is the pair of IPC files: commands arrive as JSON lines
    // appended to `queue`, and one JSON result line per command is appended to
    // `results`.
    //
    // There is deliberately no --task here and no --queue on RunArgs. The two modes
    // are exclusive, and the argument shapes say so before anything else does: there
    // is no way to spell a session that has both a task and a command queue. The
    // structural refusal is TaskHost's front-end claim; this is the same fact stated
    // where an operator meets it first.
    //
    // There are likewise no timeout, poll-interval or retry defaults here. Those are
    // policy, every convenience command requires them as fields, and a CLI flag for
    // them would be a second place task-side policy could live.
    struct DriveArgs final
    {
        std::filesystem::path project{};
        std::string           selector{};

        std::filesystem::path queue{};
        std::filesystem::path results{};

        uint64                     budget{k_defaultPixelComparisonBudget};
        MonotonicInstant::Duration recognitionTimeout{k_defaultRunRecognitionTimeout};
        MonotonicInstant::Duration maxFrameAge{k_defaultRunMaxFrameAge};
        MonotonicInstant::Duration idleTimeout{k_defaultDriveIdleTimeout};

        std::filesystem::path trace{k_defaultTracePath};

        // Same field, same default, same reason as RunArgs::ocrModels: the two
        // front-ends must not be able to run cycle_read under different
        // guarantees.
        std::optional<std::filesystem::path> ocrModels{};

        auto operator==(DriveArgs const&) const -> bool = default;
    };

    [[nodiscard]]
    auto parseDriveArguments(std::span<std::string const> raw) -> Result<DriveArgs>;

    [[nodiscard]] auto driveUsageText() noexcept -> std::string_view;

    // Parsed inputs for the `explore` subcommand: the agent front-end.
    //
    // IT IS `drive` WITH ONE THING CHANGED, and the change is what the queue
    // holds. A drive queue holds commands -- one primitive each, arguments as
    // scalars -- and an explore queue holds Luau CHUNKS, which is the whole
    // difference between an operator and an agent: an agent composes verbs, and
    // the composition is the thing being tested (docs/plans/2026-08-01-three-
    // layers-and-agent-operator.md 3). Every recognition and delivery bound below
    // is the same field with the same default `run` and `drive` pass, because the
    // three front-ends must not be able to run under different guarantees.
    //
    // --ocr-models is here for the same reason it is on the other two, and it
    // matters more: reading a region is how an agent checks the text a teaching
    // document claims is on screen.
    //
    // There is no --task and no --chunk. A session is a queue or it is nothing:
    // a one-shot chunk flag would be a second way in with no cursor behind it,
    // and a restart would replay it.
    struct ExploreArgs final
    {
        std::filesystem::path project{};
        std::string           selector{};

        std::filesystem::path queue{};
        std::filesystem::path results{};

        uint64                     budget{k_defaultPixelComparisonBudget};
        MonotonicInstant::Duration recognitionTimeout{k_defaultRunRecognitionTimeout};
        MonotonicInstant::Duration maxFrameAge{k_defaultRunMaxFrameAge};
        MonotonicInstant::Duration idleTimeout{k_defaultDriveIdleTimeout};

        std::filesystem::path trace{k_defaultTracePath};

        std::optional<std::filesystem::path> ocrModels{};

        auto operator==(ExploreArgs const&) const -> bool = default;
    };

    [[nodiscard]]
    auto parseExploreArguments(std::span<std::string const> raw) -> Result<ExploreArgs>;

    [[nodiscard]] auto exploreUsageText() noexcept -> std::string_view;

    // Parsed inputs for the `check` subcommand: the falsification matrix over a
    // whole project.
    //
    // There is no --selector and there never will be. A capture from a running
    // target contributes no column to this matrix: a frame taken to measure
    // against is not a screen the model is authored on, and the matrix is a
    // statement about the authored ones. The pixels come from
    // <project>/assets/screens, which is why this subcommand binds no target,
    // declares no DPI awareness, and runs on every host.
    //
    // --ocr-models IS here, and until 2026-08-01 this comment said the opposite:
    // the matrix measured template distances, and reading text was held to be
    // the one capability with no score to falsify against. CONFIDENCE IS THAT
    // SCORE. An element with no templates identifies by the text its rectangle
    // reads, so the project file can claim what a region reads on one screen,
    // and such a cell is measured exactly as every other one is -- against a
    // number, at or above the read floor the element declares. That made the
    // half of the model the matrix had been silent about falsifiable, and a
    // matrix that stayed silent about it would be reporting green over cells
    // nobody measured.
    //
    // A project whose claims include text and a check started without this flag
    // is refused by name at the top of the routine, before any screen is
    // measured. Degrading those cells to "unclaimed" instead would be the exact
    // failure the previous sentence describes.
    struct CheckArgs final
    {
        std::filesystem::path project{};

        uint64                     budget{k_defaultPixelComparisonBudget};
        MonotonicInstant::Duration recognitionTimeout{k_defaultRunRecognitionTimeout};

        std::filesystem::path trace{k_defaultCheckTracePath};

        // Same field, same default, same reason as RunArgs::ocrModels: the
        // front-ends must not be able to run cycle_read under different
        // guarantees, and a check that read text a different way would be
        // measuring something a run does not do.
        std::optional<std::filesystem::path> ocrModels{};

        auto operator==(CheckArgs const&) const -> bool = default;
    };

    [[nodiscard]]
    auto parseCheckArguments(std::span<std::string const> raw) -> Result<CheckArgs>;

    [[nodiscard]] auto checkUsageText() noexcept -> std::string_view;

    // Every usage, for the bare invocation and for an unknown subcommand: the
    // modes are equal citizens, so none is the one a reader is shown by default.
    [[nodiscard]] auto usageText() -> std::string;
}
