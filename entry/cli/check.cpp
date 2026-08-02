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
#include <format>
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
        // WHETHER AN OCR ENGINE IS BOUND IS FORMATTED IN FOR THE SAME REASON,
        // and it is the only way the routine can name the flag that is missing.
        // A cell claiming what a region READS is measured by reading it, so a
        // project that holds one and a check started with no engine can produce
        // no verdict about half its model. The routine refuses that outright
        // before the first screen, because the alternative is a report whose
        // green cells and whose unmeasured cells look alike. It asks TWO
        // questions to decide it: what the claims need, and what the screens'
        // own page declarations need -- a page identified by what its title box
        // reads is read on every screen that declares it, whether or not that
        // screen claims a text of its own.
        //
        // AND THE PER-CYCLE READ BUDGET IS FORMATTED IN AS THE THIRD OF THE SAME
        // KIND. The host fixes that budget before the VM exists and the routine
        // is what knows how many cells one screen claims, so only the routine can
        // compare them -- and a budget that is short otherwise surfaces as a
        // RecognitionIncomplete part-way through screen three, which says truly
        // what stopped and nothing about why. It is checked BEFORE the engine
        // clause, because a binary that cannot budget the file it was handed
        // should say so before telling an operator to pass a flag that would not
        // have helped.
        //
        // It reports through `print` rather than by returning the text, because
        // a routine is run for its effect on the evidence stream and its stdout,
        // and the one thing the host needs back is the number below.
        constexpr auto k_checkRoutinePrefix = R"lua(
local screens_on_disk = )lua";

        constexpr auto k_checkRoutineInfix = R"lua(
local ocr_models_given = )lua";

        constexpr auto k_checkRoutineReadBudget = R"lua(
local read_budget = )lua";

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

local widest = oracle.Claims.most_reads_on_one_screen(built.claims)
if widest > read_budget then
    error(
        "one screen of this project claims "
            .. tostring(widest)
            .. " text cells and this check was started with a read budget of "
            .. tostring(read_budget)
            .. " per observation; the matrix opens one observation per screen, "
            .. "so it would run out part-way through and report the rest as "
            .. "cells nobody measured",
        0
    )
end

local reads_claims = oracle.Claims.reads_text(built.claims)
local reads_pages  = recognition.needs_engine(built)
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

local verdict = regress.check(ctx, built)
print(regress.render(verdict))
return #verdict.findings
)lua";

        [[nodiscard]]
        auto checkRoutineSource(
            std::size_t screensOnDisk,
            bool ocrModelsGiven,
            uint32 readBudget
        ) -> std::string
        {
            auto source = std::string{k_checkRoutinePrefix};
            source += std::to_string(screensOnDisk);
            source += k_checkRoutineInfix;
            source += ocrModelsGiven ? "true" : "false";
            source += k_checkRoutineReadBudget;
            source += std::to_string(readBudget);
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

        // The per-cycle text-read budget one check runs under, taken from the
        // file it is about.
        //
        // WHY THE DEFAULT IS THE WRONG NUMBER HERE. k_defaultMaximumReadsPerCycle
        // guards a LOOP and a BLOCK READ: a wait that reads once per poll has no
        // bound of its own, and a block read costs one for locating plus one per
        // line it found, so that constant is where the host stops paying for
        // inference nobody decided to spend. The matrix does neither. It opens
        // one observation per screen and spends at most one single-line read per
        // element on it, so what one of its cycles needs is a property of the
        // project file and is known before the run -- a number the constant can
        // be above or below without either being wrong.
        //
        // SO THE CEILING IS THAT PROPERTY AND NOT A BIGGER CONSTANT. It is the
        // number of elements the project declares, TWICE, and each factor is one
        // walk of that same list. Once for the cells: regress walks each element
        // once per screen and reads only the ones with no templates, and only
        // where a claim asked. Once more for the page a screen declares itself to
        // be, whose identify rows are elements of this same file and can be no
        // more numerous, and which is resolved on the screen's own observation
        // because that is the frame the claim is about.
        //
        // THE FIRST FACTOR IS A HEURISTIC AND NO LONGER A BOUND, since an element
        // may draw no rectangle of its own and then be claimed several times on
        // one screen, each claim naming its own region -- a confirm button drawn
        // once and read on nine of them. The exact count is a fact about the file
        // rather than about the element list, so the routine below asks the model
        // for it (`oracle.Claims.most_reads_on_one_screen`) and refuses in its own
        // words when this number is short of it. What is left uncovered is a
        // screen whose cell reads FIT while its cell reads plus its declared
        // page's reads do not; that runs out mid-walk, which is the loud
        // RecognitionIncomplete refusal it has always been rather than a cell
        // quietly reported as a miss.
        //
        // A project declaring no elements gets a budget of zero, which is the
        // honest answer: there is no element for a claim to be about, so there is
        // no read to spend.
        constexpr auto k_readsPerElementPerScreen = uint64{2};

        [[nodiscard]]
        auto readBudgetForCheck(std::size_t declaredElements) -> Result<uint32>
        {
            auto const budget = checkedCast<uint32>(
                uint64{declaredElements} * k_readsPerElementPerScreen
            );
            if (!budget)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "this project declares {} elements, which is more than a "
                        "run can hold a per-cycle read budget for",
                        declaredElements
                    )
                );
            }
            return *budget;
        }
    }

    auto checkProduct(CheckArgs const& args) -> Result<CheckReport>
    {
        auto host = task::TaskHost{};
        UF_TRY_VALUE(generation, host.loadProject(args.project));
        UF_TRY_VALUE(fingerprint, host.projectFingerprint(generation));
        UF_TRY_VALUE(declaredElements, host.projectElementCount(generation));
        UF_TRY_VALUE(readBudget, readBudgetForCheck(declaredElements));

        // The same binding `run`, `drive` and `explore` use, so a cell the
        // matrix reads is read by the engine a run would have used. Built before
        // the frame source for the reason a run builds it before the target: a
        // model directory that will not produce an engine must fail before any
        // screen is opened.
        UF_TRY_VALUE(ocrEngine, platform::bindOcrEngine(args.ocrModels));

        UF_TRY_VALUE(
            frameSource,
            FileFrameSource::create(args.project / k_screensDirectory, fingerprint)
        );
        auto const screensOnDisk = frameSource->fileCount();
        // One local feeds both the routine's clause and the run's config, so the
        // number the routine compares against cannot drift from the number the
        // host actually enforces.
        auto const source = checkRoutineSource(
            screensOnDisk,
            args.ocrModels.has_value(),
            readBudget
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
                    // The project's own geometry, because the screens being
                    // measured are the project's own; see
                    // TaskHost::projectFingerprint.
                    .liveFingerprint         = fingerprint,
                    .maximumPixelComparisons = args.budget,
                    .recognitionTimeout      = args.recognitionTimeout,
                    // See readBudgetForCheck: the matrix's read count is a fact
                    // about this file, so the ceiling is read off it rather than
                    // left at the default that bounds a wait loop.
                    .maximumReadsPerCycle = readBudget,
                    .tracePath            = args.trace,
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
