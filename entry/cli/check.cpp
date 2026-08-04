#include "check.hpp"

#include "file-frame-source.hpp"
#include "platform/ocr-engine-binding.hpp"

#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/key.hpp>
#include <domain/space.hpp>

#include <engine/ports.hpp>

#include <task/task-host.hpp>

#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace uf::cli
{
    namespace
    {
        // The screens directory, relative to the project root. The trusted
        // framework spells the same path on its own side as
        // `oracle.screen_directory`; nothing derives one from the other, and what
        // holds them equal is that every case in tests/cli/test-check.cpp fails
        // outright if they disagree.
        constexpr auto k_screensDirectory = "assets/screens";

        // The routine `check` runs. It is the HOST'S source and never the
        // project's -- a project that could supply the thing judging it could
        // pass any model -- so this text is compiled into the binary and reaches
        // the VM through runFrameworkRoutine, which never touches <project>/tasks.
        //
        // Three host-side facts are formatted in. Two because only the routine can
        // compare them against what the project file declares:
        //   - the screen count, which holds both sides to one set of screens. A
        //     declared screen with no file already fails on its own project_read;
        //     a file no screen declares would silently shift the content-hash
        //     pairing from that file onward, and only the count catches it.
        //   - whether an OCR engine is bound, so the routine can name the missing
        //     flag. A project measuring what a region READS and a check started
        //     with no engine produce no verdict about half the model, and a report
        //     whose green cells and unmeasured cells look alike is worse than a
        //     refusal. Two questions decide it: what the claims need, and what the
        //     screens' own page declarations need.
        // And one because it is the operator's, not the file's: whether to sweep
        // every declared page over every screen (see CheckArgs::sweepPages). It is
        // stated on every run rather than left to the framework's own default, so
        // reading this source says which measurement the CLI asked for.
        //
        // It reports through `print` rather than by returning the text; the one
        // thing the host needs back is the number below.
        constexpr auto k_checkRoutinePrefix = R"lua(
local screens_on_disk = )lua";

        constexpr auto k_checkRoutineInfix = R"lua(
local ocr_models_given = )lua";

        constexpr auto k_checkRoutineSweep = R"lua(
local sweep_pages = )lua";

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

local reads_claims = oracle.Claims.reads_text(built.claims)
local reads_pages  = recognition.needs_engine(built, sweep_pages)
if (reads_claims or reads_pages) and not ocr_models_given then
    error(
        "this project measures what a region READS -- a claim about what an "
            .. "element reads on a screen, or a screen declaring a page whose "
            .. "signature is one -- and this check was started with no OCR "
            .. "engine, so those cells cannot be measured at all; pass "
            .. "--ocr-models DIR, because a matrix that reported them as "
            .. "unclaimed would be green over the half nobody measured",
        0
    )
end

local verdict = regress.check(ctx, built, { sweep_pages = sweep_pages })
-- One print per row rather than one print of the whole report, so each line is
-- garbage before the next exists. A matrix over a real corpus is tens of
-- thousands of rows, and holding them all exceeds the VM ceiling on top of the
-- rows they were rendered from (docs/pitfalls/embedded-vm-memory-ceiling.md).
for _, group in regress.groups(verdict) do
    for _, row in group.rows do
        print(group.line(row))
    end
end
return #verdict.findings
)lua";

        [[nodiscard]]
        auto checkRoutineSource(
            std::size_t screensOnDisk,
            bool ocrModelsGiven,
            bool sweepPages
        ) -> std::string
        {
            auto source = std::string{k_checkRoutinePrefix};
            source += std::to_string(screensOnDisk);
            source += k_checkRoutineInfix;
            source += ocrModelsGiven ? "true" : "false";
            source += k_checkRoutineSweep;
            source += sweepPages ? "true" : "false";
            source += k_checkRoutineBody;
            return source;
        }

        // The action sink a check binds. A falsification run measures and never
        // acts, so every verb refuses rather than counting or discarding: an
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

            [[nodiscard]]
            auto longPress(
                Point<ClientSpace> /*point*/,
                MonotonicInstant::Duration /*hold*/,
                ObservationLease const& /*lease*/
            ) -> Status override
            {
                return fail(
                    AutomationErrorKind::UnsupportedCapability,
                    "a falsification check measures screens and presses nothing"
                );
            }

            [[nodiscard]]
            auto movePointer(
                Point<ClientSpace> /*point*/,
                ObservationLease const& /*lease*/
            ) -> Status override
            {
                return fail(
                    AutomationErrorKind::UnsupportedCapability,
                    "a falsification check measures screens and moves no pointer"
                );
            }
        };

        // The per-cycle text-read budget one check runs under: none.
        //
        // k_defaultMaximumReadsPerCycle bounds a WAIT LOOP's cycle against the
        // observation lease it holds -- one poll of a live target, where reading
        // more than a fraction of a second's worth means the frame has gone stale
        // underneath the answer. A check's cycle is the whole of one screen's
        // measurement and is meant to run to completion: every read it refused
        // would be a cell the report then has to call unmeasured, and the frames
        // arrive one file per capture, so the walk cannot re-open a cycle to buy
        // more. Nothing about an offline run over files makes the count of reads
        // the right ceiling; wall-clock is, and TaskRunConfig::maxScriptRuntime
        // already is that ceiling.
        //
        // Spelled as the widest value the field holds rather than as zero: zero is
        // a budget of no reads at all and refuses the first one
        // (task::TaskContext::cycleRead).
        //
        // IT HAS TO BE UNREACHABLE AND NOT MERELY GENEROUS. A sweep resolves the
        // pages in the model's own order and the page a screen DECLARES may be
        // last in it, so a budget exhausted part-way through raises out of
        // `observe.resolve_page` -- turning what would have been an ordinary
        // unresolved_page finding into a failed run. Nothing sized from the file
        // can be argued safe here, and this is: a cycle charges once per DISTINCT
        // region of one screen for a single-line read, and once plus one per line
        // located for a block read, so reaching 2^32 needs a page model whose
        // rectangles and the lines inside them sum to four billion on one screen.
        constexpr auto k_uncappedReadsPerCycle = std::numeric_limits<uint32>::max();

        // What one matrix cell is allowed to cost the VM, and the floor under a
        // project too small for the product to matter.
        //
        // The matrix is the one routine whose memory need is a property of the
        // FILE: it holds a row per measured cell and renders every one, so a
        // corpus that doubles doubles the heap. The script layer's default
        // ceiling is sized for a business task and a real corpus walks into it --
        // the reference project's 85 screens and 331 elements produced 28,985
        // rows, which died mid-report under 64 MiB.
        //
        // Elements times screens is a LOWER BOUND and not the row count: the walk
        // emits one row per element per search rectangle, and one more per
        // appearance for the elements declaring several. On that project the
        // bound was met exactly for the first term (28,135) and four
        // multi-appearance elements added 850. Sizing from the bound therefore
        // under-budgets by whatever the surplus is, which the hysteresis below
        // absorbs -- it is not a reason to believe the product IS the count.
        //
        // Four kibibytes per cell is the row's live table (a few hundred bytes)
        // plus the two dozen short-lived strings rendering it mints, times the
        // same hysteresis factor `cycle_crop` uses: Luau throws the instant the
        // allocator refuses and never collects and retries, so a ceiling set at
        // what is live leaves the incremental collector no room to stay ahead of
        // the garbage (docs/pitfalls/embedded-vm-memory-ceiling.md).
        constexpr auto k_memoryPerMatrixCellBytes = uint64{4} * 1024;
        constexpr auto k_baseCheckMemoryBytes     = uint64{64} * 1024 * 1024;

        [[nodiscard]]
        auto memoryQuotaForCheck(
            std::size_t declaredElements,
            std::size_t screensOnDisk
        ) noexcept -> uint64
        {
            return k_baseCheckMemoryBytes
                + uint64{declaredElements} * uint64{screensOnDisk}
                * k_memoryPerMatrixCellBytes;
        }
    }

    auto checkProduct(CheckArgs const& args) -> Result<CheckReport>
    {
        auto host = task::TaskHost{};
        UF_TRY_VALUE(generation, host.loadProject(args.project));
        UF_TRY_VALUE(fingerprint, host.projectFingerprint(generation));
        UF_TRY_VALUE(declaredElements, host.projectElementCount(generation));

        // The same binding `run`, `drive` and `explore` use, so a cell the matrix
        // reads is read by the engine a run would have used. Built before the
        // frame source so a model directory that will not produce an engine fails
        // before any screen is opened.
        UF_TRY_VALUE(ocrEngine, platform::bindOcrEngine(args.ocrModels));

        UF_TRY_VALUE(
            frameSource,
            FileFrameSource::create(args.project / k_screensDirectory, fingerprint)
        );
        auto const screensOnDisk = frameSource->fileCount();
        auto const source        = checkRoutineSource(
            screensOnDisk,
            args.ocrModels.has_value(),
            args.sweepPages
        );

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
                    .ocrEngine   = std::move(ocrEngine),
                    // The project's own geometry: the screens being measured are
                    // the project's own.
                    .liveFingerprint         = fingerprint,
                    .maximumPixelComparisons = args.budget,
                    .recognitionTimeout      = args.recognitionTimeout,
                    // See k_uncappedReadsPerCycle.
                    .maximumReadsPerCycle = k_uncappedReadsPerCycle,
                    // See memoryQuotaForCheck.
                    .memoryQuotaBytes = memoryQuotaForCheck(
                        declaredElements,
                        screensOnDisk
                    ),
                    .tracePath = args.trace,
                }
            )
        );

        auto findings = uint64{0};
        if (!outcome.run.failure)
        {
            // A negative, fractional or unrepresentable answer is reported as an
            // internal invariant rather than rounded into a verdict: a check whose
            // own answer cannot be read has not accepted anything.
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
