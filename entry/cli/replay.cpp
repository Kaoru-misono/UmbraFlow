#include "replay.hpp"

#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/key.hpp>
#include <domain/space.hpp>

#include <engine/ports.hpp>

#include <task/task-host.hpp>

#include <trace/event.hpp>
#include <trace/replay-source.hpp>

#include <format>
#include <memory>
#include <string>
#include <utility>

namespace uf::cli
{
    namespace
    {
        // The routine `replay` runs. The HOST'S source for `check`'s reason: a
        // project that could supply the thing judging it could pass any model.
        //
        // Nothing is formatted in. The one input that varies is the recorded run,
        // and it arrives as DATA through `ctx:replay_steps` rather than as text
        // spliced into this program -- a page name is a project's own string, and
        // a project's string that becomes part of a program is a project that can
        // rewrite what checks it.

        // Where a replay writes its own trace. A fixed name in the working
        // directory, as `check` uses: the input trace is somewhere the caller
        // chose and writing beside it would put two runs' evidence in one place.
        constexpr auto k_replayTracePath = "umbra-flow-replay-trace.jsonl";

        constexpr auto k_replayRoutine = R"lua(
local built = project.load_project(ctx)
local verdict = replay.check(built, ctx:replay_steps())
-- One print per row, as the matrix does, so a line is garbage before the next
-- exists (docs/pitfalls/embedded-vm-memory-ceiling.md). A replay's report is far
-- smaller than a matrix's, and the shape is shared so a reader of either does
-- not have to know which.
for _, group in replay.groups(verdict) do
    for _, row in group.rows do
        print(group.line(row))
    end
end
return #verdict.findings
)lua";

        // The frame source a replay binds: one that refuses.
        //
        // A replay judges a run that already happened, so any frame it could open
        // now is a frame of a screen the run never saw. Refusing is the only
        // honest answer, and it makes the boundary structural -- a routine that
        // tried to observe fails at the attempt rather than quietly measuring
        // today's desktop against yesterday's walk.
        class RefusingFrameSource final : public engine::IFrameSource
        {
        public:
            [[nodiscard]]
            auto capture(CaptureBudget const& /*budget*/) -> Result<Frame> override
            {
                return fail(
                    AutomationErrorKind::UnsupportedCapability,
                    "a replay reads a run that already happened and opens no "
                    "frame; anything captured now is a screen that run never saw"
                );
            }

            // Nothing is bound, so there is no target instance to have gone
            // stale. It answers rather than refusing because a session validates
            // before it captures, and a refusal here would name the wrong thing.
            [[nodiscard]]
            auto validateTargetInstance() -> Status override
            {
                return ok();
            }
        };

        // The action sink a replay binds, on the same reasoning as the frame
        // source: an input delivered while judging a recorded run would act on a
        // target the run is not about.
        class RefusingActionSink final : public engine::IActionSink
        {
            [[nodiscard]]
            static auto refuse(std::string_view verb) -> Status
            {
                return fail(
                    AutomationErrorKind::UnsupportedCapability,
                    std::format(
                        "a replay reads a recorded run and {} nothing",
                        verb
                    )
                );
            }

        public:
            [[nodiscard]]
            auto click(
                Point<ClientSpace> /*point*/,
                ObservationLease const& /*lease*/
            ) -> Status override
            {
                return refuse("clicks");
            }

            [[nodiscard]]
            auto pressKey(
                KeyName /*key*/,
                TargetGeneration /*actionGeneration*/
            ) -> Status override
            {
                return refuse("presses");
            }

            [[nodiscard]]
            auto scroll(
                int32 /*notches*/,
                ObservationLease const& /*lease*/
            ) -> Status override
            {
                return refuse("scrolls");
            }

            [[nodiscard]]
            auto longPress(
                Point<ClientSpace> /*point*/,
                MonotonicInstant::Duration /*hold*/,
                ObservationLease const& /*lease*/
            ) -> Status override
            {
                return refuse("holds");
            }

            [[nodiscard]]
            auto drag(
                Point<ClientSpace> /*start*/,
                Point<ClientSpace> /*end*/,
                MonotonicInstant::Duration /*travel*/,
                ObservationLease const& /*lease*/
            ) -> Status override
            {
                return refuse("drags");
            }

            [[nodiscard]]
            auto movePointer(
                Point<ClientSpace> /*point*/,
                ObservationLease const& /*lease*/
            ) -> Status override
            {
                return refuse("moves");
            }
        };
    }

    auto replayProduct(ReplayArgs const& args) -> Result<ReplayReport>
    {
        UF_TRY_VALUE(recorded, trace::readReplayedRun(args.trace));

        // Before the project is even opened: a stream that delivered no input is
        // not a walk, and reading one as a walk would report a run that stood on
        // every page its sweep tried.
        if (recorded.frontEnd != trace::FrontEnd::Task)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "'{}' was recorded by the '{}' front end, which delivers no "
                    "input; its page resolutions are one frame tried against "
                    "many pages rather than a run walking between them, so "
                    "replaying it would report moves nobody made",
                    args.trace.string(),
                    trace::frontEndWireName(recorded.frontEnd)
                )
            );
        }

        auto host = task::TaskHost{};
        UF_TRY_VALUE(generation, host.loadProject(args.project));
        UF_TRY_VALUE(fingerprint, host.projectFingerprint(generation));
        UF_TRY_VALUE(modelHash, host.projectModelHash(generation));

        if (recorded.modelHash != modelHash)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "'{}' was recorded against page model {} and this project's "
                    "is {}; every move a replay could report is about edges that "
                    "may never have been in the file that run read",
                    args.trace.string(),
                    recorded.modelHash,
                    modelHash
                )
            );
        }

        UF_TRY_VALUE(
            outcome,
            host.runFrameworkRoutine(
                generation,
                task::FrameworkRoutine{
                    .name   = "trace-replay",
                    .source = k_replayRoutine,
                },
                task::TaskRunConfig{
                    .frameSource = std::make_unique<RefusingFrameSource>(),
                    .actionSink  = std::make_unique<RefusingActionSink>(),
                    // The project's own geometry. Nothing is captured, so nothing
                    // is compared against it; it is stated because a run states
                    // the geometry it believes in.
                    .liveFingerprint = fingerprint,
                    .replaySteps     = std::move(recorded.steps),
                    // Its OWN stream, beside the one it reads and never over it.
                    // A replay is a run like any other and writes one; naming it
                    // after the input would let a second replay overwrite the
                    // evidence the first was about.
                    .tracePath = k_replayTracePath,
                }
            )
        );

        auto findings = uint64{0};
        if (!outcome.run.failure)
        {
            auto const converted = checkedIntegralCast<uint64>(outcome.answer);
            if (!converted)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "the replay routine answered with something that is not a "
                    "count of findings"
                );
            }
            findings = *converted;
        }

        return ReplayReport{
            .run      = std::move(outcome.run),
            .findings = findings,
        };
    }

    auto exitCodeForReplay(ReplayReport const& report) noexcept -> ExitCode
    {
        if (report.run.failure)
        {
            return exitCodeForError(*report.run.failure, false);
        }
        return report.findings == 0U ? ExitCode::Success : ExitCode::Failure;
    }
}
