#pragma once

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <task/task-host.hpp>

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

    // Taken from the host's own ceiling rather than restated, so the value an
    // operator is told is the default and the one a run is actually held to
    // cannot drift apart.
    inline constexpr auto k_defaultRunMaxRuntime = (
        std::chrono::duration_cast<MonotonicInstant::Duration>(
            task::k_defaultMaxScriptRuntime
        )
    );

    // A per-recognition ceiling on template pixel comparisons, shared by
    // `umbra-flow run` and `umbra-authoring` so one number governs both.
    //
    // A search's worst case is (roiW - tplW + 1) * (roiH - tplH + 1) positions
    // times tplW * tplH pixels, and evaluatePage shares one budget across every
    // page anchor, so the figure to cover is the sum over them. The envelope:
    // sixteen anchors each as costly as the widest ordinary authored one, a
    // 200x50 template in a 480x90 region -- 281 * 41 * 10,000 = 115,210,000 each,
    // 1,843,360,000 together. 2^31 = 2,147,483,648 covers that with headroom.
    //
    // It deliberately does not cover a full-frame search for a small template: a
    // 66x46 template over 1600x900 costs 1535 * 855 * 3,036 = 3,984,522,300, about
    // 1.9x this. Such a project authors a search region or raises --budget on
    // purpose, because sizing the default for that case would leave nothing
    // failing closed on genuinely unbounded geometry.
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
    // value once parsing succeeds, so the composition never observes an unset
    // field. A run is always a project-owned task addressed as (project, task).
    //
    // How long a task waits for a page, and how often it re-observes while
    // waiting, are deliberately absent: the wait loop is the framework's Luau, so
    // a CLI flag for them would be an operator overriding a decision the script
    // owns.
    struct RunArgs final
    {
        std::filesystem::path project{};
        std::string           selector{};
        std::string           task{};

        uint64                     budget{k_defaultPixelComparisonBudget};
        MonotonicInstant::Duration recognitionTimeout{k_defaultRunRecognitionTimeout};
        MonotonicInstant::Duration maxFrameAge{k_defaultRunMaxFrameAge};

        // The whole task is one unit of script, so this bounds the RUN, not a
        // step of it.
        MonotonicInstant::Duration maxRuntime{k_defaultRunMaxRuntime};

        std::filesystem::path trace{k_defaultTracePath};

        // The "models" directory cycle_read's OCR engine loads from. Absent by
        // default because the weights are tens of megabytes and a run that reads
        // nothing must not pay for them; cycle_read refuses on its own terms. The
        // layout is the one modules/ocr/external commits and the build stages
        // beside this binary: `ocrModels / "ppocr-v6-small-rec" /
        // {inference.onnx, inference.yml}`.
        std::optional<std::filesystem::path> ocrModels{};

        auto operator==(RunArgs const&) const -> bool = default;
    };

    [[nodiscard]]
    auto parseRunArguments(std::span<std::string const> raw) -> Result<RunArgs>;

    [[nodiscard]] auto runUsageText() noexcept -> std::string_view;

    // How long `explore` waits with an empty queue before it ends the session on
    // its own, so an agent process that died does not leave a session holding a
    // capture device and a bound target indefinitely. An agent that knows it is
    // done says so on its last line instead of spending this
    // (`ExploreChunk::endsSession`); the timeout is the fallback for the case
    // where nobody is left to say it.
    inline constexpr auto k_defaultExploreIdleTimeout = (
        std::chrono::duration_cast<MonotonicInstant::Duration>(
            std::chrono::seconds{120}
        )
    );

    // Parsed inputs for the `explore` subcommand: the agent front-end. What
    // replaces --task is the pair of IPC files -- Luau CHUNKS arrive as JSON
    // lines appended to `queue`, one JSON result line per chunk is appended to
    // `results` -- because an agent's smallest useful act is already a
    // composition, and the composition is the thing being tested
    // (docs/plans/2026-08-01-three-layers-and-agent-operator.md 3).
    //
    // There is deliberately no --task here and no --queue on RunArgs: the modes
    // are exclusive, and the argument shapes say so where a caller meets them
    // first. TaskHost's front-end claim is the structural refusal. Every
    // recognition and delivery bound below is the same field with the same
    // default RunArgs carries, because the front-ends must not be able to run
    // under different guarantees.
    //
    // There is no --task and no --chunk. A session is a queue or it is nothing: a
    // one-shot chunk flag would be a second way in with no cursor behind it, and
    // a restart would replay it.
    struct ExploreArgs final
    {
        std::filesystem::path project{};
        std::string           selector{};

        std::filesystem::path queue{};
        std::filesystem::path results{};

        uint64                     budget{k_defaultPixelComparisonBudget};
        MonotonicInstant::Duration recognitionTimeout{k_defaultRunRecognitionTimeout};
        MonotonicInstant::Duration maxFrameAge{k_defaultRunMaxFrameAge};
        MonotonicInstant::Duration idleTimeout{k_defaultExploreIdleTimeout};

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
    // There is no --selector and there never will be. A frame taken from a
    // running target is not a screen the model is authored on, and the matrix is
    // a statement about the authored ones. The pixels come from
    // <project>/assets/screens, which is why this subcommand binds no target,
    // declares no DPI awareness, and runs on every host.
    //
    // --ocr-models IS here because a project whose claims include text and a
    // check started without this flag is refused by name at the top of the
    // routine, before any screen is measured.
    struct CheckArgs final
    {
        std::filesystem::path project{};

        uint64                     budget{k_defaultPixelComparisonBudget};
        MonotonicInstant::Duration recognitionTimeout{k_defaultRunRecognitionTimeout};

        std::filesystem::path trace{k_defaultCheckTracePath};

        // Same field, same default, same reason as RunArgs::ocrModels: a check
        // that read text a different way would measure something a run does not
        // do.
        std::optional<std::filesystem::path> ocrModels{};

        // Whether to offer every declared page to every screen. OFF by default,
        // and the only default in this struct chosen by a stopwatch: the sweep
        // resolves pages times screens and measured as most of a check's wall
        // clock over the reference corpus (87 pages by 85 screens, about 100 s of
        // 110 s). Nothing it produces is a finding and the exit code never moves
        // on it, so the default answers the question an author asks after every
        // edit and the flag answers the one asked of a corpus.
        bool sweepPages{false};

        auto operator==(CheckArgs const&) const -> bool = default;
    };

    [[nodiscard]]
    auto parseCheckArguments(std::span<std::string const> raw) -> Result<CheckArgs>;

    [[nodiscard]] auto checkUsageText() noexcept -> std::string_view;

    // Every usage, for the bare invocation and for an unknown subcommand: the
    // modes are equal citizens, so none is the one a reader is shown by default.
    [[nodiscard]] auto usageText() -> std::string;
}
