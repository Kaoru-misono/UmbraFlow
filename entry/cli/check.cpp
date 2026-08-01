#include "check.hpp"

#include "file-frame-source.hpp"

#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/key.hpp>
#include <domain/space.hpp>

#include <engine/ports.hpp>

#include <task/task-host.hpp>

#include <memory>
#include <string>
#include <utility>

namespace uf::cli
{
    namespace
    {
        // The screens directory, relative to the project root. It mirrors
        // assets/templates, which is where the runtime loader already reads a
        // project's content-addressed pixels from.
        //
        // The trusted framework spells the same path on its own side, as
        // `oracle.screen_directory`, and one of the two has to name it first: the
        // host lists the directory before any VM exists, and the routine derives
        // each screen's file from its content hash. Nothing derives one from the
        // other, and what holds them equal is that every case in
        // tests/cli/test-check.cpp fails outright if they disagree -- the host
        // would find no screens where the routine reads them, or the reverse.
        constexpr auto k_screensDirectory = "assets/screens";

        // The routine `check` runs.
        //
        // IT IS THE HOST'S SOURCE AND NEVER THE PROJECT'S. A project that could
        // supply the thing judging it could pass any model; this text is
        // compiled into the binary and reaches the VM through
        // runFrameworkRoutine, which never touches <project>/tasks.
        //
        // THE SCREEN COUNT IS FORMATTED IN, and it is the whole of how the two
        // sides are held to one set of screens. The host serves this run's
        // frames from a directory it listed; the routine walks the screens the
        // project file declares, in content-hash order, opening one observation
        // for each -- so a declared screen with no file fails on its own
        // project_read, and a file no screen declares would silently shift the
        // pairing from that file onward. Stating the count the host found closes
        // the second case where it can still be attributed.
        //
        // It reports through `print` rather than by returning the text, because
        // a routine is run for its effect on the evidence stream and its stdout,
        // and the one thing the host needs back is the number below.
        constexpr auto k_checkRoutinePrefix = R"lua(
local screens_on_disk = )lua";

        constexpr auto k_checkRoutineBody = R"lua(

local built = project.load_project(ctx)
local declared = #built.claims.screens
if declared ~= screens_on_disk then
    error(
        "this project declares "
            .. tostring(declared)
            .. " screens and its assets/screens directory holds "
            .. tostring(screens_on_disk)
            .. "; the matrix pairs the Nth capture with the Nth screen in "
            .. "content-hash order, so the two have to be one set",
        0
    )
end

local verdict = regress.check(ctx, built)
print(regress.render(verdict))
return #verdict.findings
)lua";

        [[nodiscard]]
        auto checkRoutineSource(std::size_t screensOnDisk) -> std::string
        {
            auto source = std::string{k_checkRoutinePrefix};
            source += std::to_string(screensOnDisk);
            source += k_checkRoutineBody;
            return source;
        }

        // The action sink a check binds. A falsification run measures and never
        // acts, so both verbs refuse rather than counting or discarding: an
        // input this run delivered would be a defect in the routine, and the
        // only useful behaviour is to say so.
        class RefusingActionSink final : public engine::IActionSink
        {
        public:
            [[nodiscard]]
            auto click(
                Point<ClientSpace> /*point*/,
                ObservationLease const& /*lease*/
            ) -> Status override
            {
                return fail(
                    AutomationErrorKind::UnsupportedCapability,
                    "a falsification check measures screens and delivers no input"
                );
            }

            [[nodiscard]]
            auto pressKey(
                KeyName /*key*/,
                TargetGeneration /*actionGeneration*/
            ) -> Status override
            {
                return fail(
                    AutomationErrorKind::UnsupportedCapability,
                    "a falsification check measures screens and presses no key"
                );
            }

            [[nodiscard]]
            auto scroll(
                int32 /*notches*/,
                ObservationLease const& /*lease*/
            ) -> Status override
            {
                return fail(
                    AutomationErrorKind::UnsupportedCapability,
                    "a falsification check measures screens and scrolls nothing"
                );
            }
        };
    }

    auto checkProduct(CheckArgs const& args) -> Result<CheckReport>
    {
        auto host = task::TaskHost{};
        UF_TRY_VALUE(generation, host.loadProject(args.project));
        UF_TRY_VALUE(fingerprint, host.projectFingerprint(generation));

        UF_TRY_VALUE(
            frameSource,
            FileFrameSource::create(args.project / k_screensDirectory, fingerprint)
        );
        auto const screensOnDisk = frameSource->fileCount();
        auto const source        = checkRoutineSource(screensOnDisk);

        UF_TRY_VALUE(
            outcome,
            host.runFrameworkRoutine(
                generation,
                task::FrameworkRoutine{
                    .name   = "falsification-matrix",
                    .source = source,
                },
                task::TaskRunConfig{
                    .frameSource = std::move(frameSource),
                    .actionSink  = std::make_unique<RefusingActionSink>(),
                    // The project's own geometry, because the screens being
                    // measured are the project's own; see
                    // TaskHost::projectFingerprint.
                    .liveFingerprint         = fingerprint,
                    .maximumPixelComparisons = args.budget,
                    .recognitionTimeout      = args.recognitionTimeout,
                    .tracePath               = args.trace,
                }
            )
        );

        auto findings = uint64{0};
        if (!outcome.run.failure)
        {
            // The routine answers with a count of findings. A negative, a
            // fractional or an unrepresentable answer is a routine that stopped
            // meaning what it says, and it is reported as an internal invariant
            // rather than rounded into a verdict: a check whose own answer
            // cannot be read has not accepted anything.
            auto const converted = checkedIntegralCast<uint64>(outcome.answer);
            if (!converted)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "the falsification routine answered with something that is "
                    "not a count of findings"
                );
            }
            findings = *converted;
        }

        return CheckReport{
            .run      = std::move(outcome.run),
            .findings = findings,
        };
    }

    auto exitCodeForCheck(CheckReport const& report) noexcept -> ExitCode
    {
        if (report.run.failure)
        {
            return exitCodeForError(*report.run.failure, false);
        }
        return report.findings == 0U ? ExitCode::Success : ExitCode::Failure;
    }
}
