#include "binding-fixture.hpp"

#include <task/script-bindings.hpp>
#include <task/cycle-ledger.hpp>
#include <task/task-context.hpp>

#include <script/engine.hpp>
#include <script/testing/cancel-probe.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <engine/session.hpp>

#include <trace/event.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::task
{
    namespace
    {
        // Requests a stop the first time a frame is captured, then still hands back
        // a valid frame. Wiring its stop token into both the engine session and the
        // task VM reproduces the single cancel source: once the stop is requested,
        // the VM interrupt hard-breaks the task thread, so no script statement after
        // the cancelled call runs.
        class StopOnCaptureFrameSource final : public engine::IFrameSource
        {
            Frame            m_frame;
            std::stop_source m_stop;

        public:
            StopOnCaptureFrameSource(Frame frame, std::stop_source stop) noexcept
                : m_frame{std::move(frame)}
                , m_stop{std::move(stop)}
            {
            }

            [[nodiscard]]
            auto capture(CaptureBudget const& /*budget*/) -> Result<Frame> override
            {
                m_stop.request_stop();
                return m_frame;
            }

            [[nodiscard]] auto validateTargetInstance() -> Status override
            {
                return ok();
            }
        };

        // Fails only when recording a native call, so a test can prove a verb
        // aborts when its task.native_call event cannot be recorded rather than
        // dropping the evidence silently. Letting the engine's own events through
        // keeps the failure exactly where the test aims it.
        class FailOnNativeCallTraceSink final : public trace::ITraceSink
        {
        public:
            [[nodiscard]]
            auto emit(trace::StampedTraceEvent const& event) -> Status override
            {
                if (event.event().kind == trace::TraceEventKind::TaskNativeCall)
                {
                    return fail(
                        AutomationErrorKind::IoFailure,
                        "trace sink deliberately failing on a native call"
                    );
                }
                return ok();
            }
        };

        // Fails only when recording a verb's failure. A sink that failed on every
        // event would abort the first successful verb and no test could ever reach
        // the failure path it wants to observe.
        class FailOnFailedTraceSink final : public trace::ITraceSink
        {
        public:
            [[nodiscard]]
            auto emit(trace::StampedTraceEvent const& event) -> Status override
            {
                auto const& call = event.event().nativeCall;
                if (
                    call.has_value()
                    && call->outcome == trace::NativeCallOutcome::Failed
                )
                {
                    return fail(
                        AutomationErrorKind::IoFailure,
                        "trace sink deliberately failing on a failure record"
                    );
                }
                return ok();
            }
        };

        [[nodiscard]]
        auto kindsOf(std::vector<trace::StampedTraceEvent> const& events)
            -> std::vector<trace::TraceEventKind>
        {
            auto kinds = std::vector<trace::TraceEventKind>{};
            kinds.reserve(events.size());
            for (auto const& event : events)
            {
                kinds.emplace_back(event.event().kind);
            }
            return kinds;
        }

        // Builds a context seeded with `seed` and draws `count` values from its
        // RNG, so two draws can be compared for reproducibility. The frame is never
        // captured; only the seeded RNG is exercised.
        [[nodiscard]]
        auto drawContextDoubles(uint64 seed, std::size_t count) -> std::vector<double>
        {
            auto built = buildBinding(resolvingFrames(FrameId{50}));
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.randomSeed = seed},
            };

            auto values = std::vector<double>{};
            values.reserve(count);
            for (std::size_t index = 0; index < count; ++index)
            {
                values.emplace_back(context.nextRandomUnitDouble());
            }
            return values;
        }

        TEST_CASE("Two contexts with the same seed draw the same random sequence")
        {
            // The seed is what a trace records to replay a run: two contexts given
            // the same seed produce an identical sequence of at least a hundred
            // numbers, and a different seed diverges.
            auto const first  = drawContextDoubles(0x00C0'FFEE, 100);
            auto const second = drawContextDoubles(0x00C0'FFEE, 100);
            CHECK(first == second);

            auto const other = drawContextDoubles(0x0BAD'F00D, 100);
            CHECK(other != first);
        }

        TEST_CASE("The task binding runs one observation cycle into one delivered click")
        {
            auto built = buildBinding(resolvingFrames(FrameId{17}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const source = withTemplate(R"lua(
                local template = ctx:template_load(TEMPLATE)
                local cycle = ctx:cycle_open()
                local hit = ctx:cycle_match(cycle, template, 0, 0, 3, 1)
                if hit == nil then return 0 end
                ctx:cycle_click(cycle, hit)
                return 1
            )lua");

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
            // The click consumed the cycle, so the host holds no frame afterwards.
            CHECK_FALSE(context.hasOpenCycle());
        }

        TEST_CASE("The task binding refuses to open a second cycle while one is open")
        {
            // The one-cycle rule. It is a framework bug rather than a script
            // error -- the framework opens and closes one cycle per poll -- so it
            // is InternalInvariant, not a Tier B failure a retry policy would
            // treat as recoverable. The refusal also lands BEFORE the capture, so
            // the second open costs no frame, and the first cycle is untouched.
            auto frameSource = std::make_unique<FakeFrameSource>(
                resolvingFrames(FrameId{18})
            );
            auto* const p_frames = frameSource.get();
            auto built = buildBindingWith(
                std::move(frameSource),
                std::stop_token{},
                std::make_unique<DiscardingTraceSink>()
            );
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const source = withTemplate(R"lua(
                local template = ctx:template_load(TEMPLATE)
                local first = ctx:cycle_open()

                local ok, err = pcall(function() return ctx:cycle_open() end)
                if ok then return 0 end
                if err.kind ~= 'internal_invariant' then return 0 end
                if err.retryable ~= false then return 0 end
                if getmetatable(err) ~= 'uf.error' then return 0 end

                -- The refused open left the first cycle whole: it still matches
                -- and clicks.
                local hit = ctx:cycle_match(first, template, 0, 0, 3, 1)
                if hit == nil then return 0 end
                ctx:cycle_click(first, hit)
                return 1
            )lua");

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
            CHECK(p_frames->captureCount() == 1U);
        }

        TEST_CASE("The task binding fails every operation on a consumed or closed cycle")
        {
            auto built = buildBinding(resolvingFrames(FrameId{20}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // After the click consumes the cycle, cycle_match, cycle_read and a
            // second cycle_click all fail with a frozen, protected
            // stale_observation error a script cannot mutate. A closed cycle's
            // ticket is just as dead, and so is a match it produced.
            auto const source = withTemplate(R"lua(
                local template = ctx:template_load(TEMPLATE)
                local cycle = ctx:cycle_open()
                local hit = ctx:cycle_match(cycle, template, 0, 0, 3, 1)
                ctx:cycle_click(cycle, hit)

                local okMatch, errMatch = pcall(function()
                    return ctx:cycle_match(cycle, template, 0, 0, 3, 1)
                end)
                if okMatch or errMatch.kind ~= 'stale_observation' then return 0 end
                -- The kind a raised error carries is a key of uf.errors whose
                -- value is that same string: both come from the one domain
                -- mapping, so a script's comparison can never silently miss.
                if uf.errors[errMatch.kind] ~= errMatch.kind then return 0 end
                if errMatch.retryable ~= true then return 0 end
                if getmetatable(errMatch) ~= 'uf.error' then return 0 end
                if pcall(function() errMatch.kind = 'tampered' end) then return 0 end

                local okRead, errRead = pcall(function()
                    return ctx:cycle_read(cycle, 0, 0, 1, 1)
                end)
                if okRead or errRead.kind ~= 'stale_observation' then return 0 end

                local okClick, errClick = pcall(function()
                    return ctx:cycle_click(cycle, hit)
                end)
                if okClick or errClick.kind ~= 'stale_observation' then return 0 end

                -- A closed cycle is equally dead, and its match is refused even
                -- against a cycle that IS open.
                local closed = ctx:cycle_open()
                local staleHit = ctx:cycle_match(closed, template, 0, 0, 3, 1)
                if staleHit == nil then return 0 end
                ctx:cycle_close(closed)
                local okClosed, errClosed = pcall(function()
                    return ctx:cycle_match(closed, template, 0, 0, 3, 1)
                end)
                if okClosed or errClosed.kind ~= 'stale_observation' then return 0 end

                local reopened = ctx:cycle_open()
                local okStale, errStale = pcall(function()
                    return ctx:cycle_click(reopened, staleHit)
                end)
                if okStale or errStale.kind ~= 'stale_observation' then return 0 end
                ctx:cycle_close(reopened)
                return 1
            )lua");

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
            CHECK_FALSE(context.hasOpenCycle());
        }

        TEST_CASE("ctx:cycle_close is idempotent and closing a consumed cycle is a no-op")
        {
            auto built = buildBinding(resolvingFrames(FrameId{21}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // Every close below is unguarded: a raise anywhere would abort the
            // script and never reach the return, which is the assertion. This is
            // what lets a framework cleanup path close unconditionally.
            auto const source = withTemplate(R"lua(
                local template = ctx:template_load(TEMPLATE)
                local first = ctx:cycle_open()
                ctx:cycle_close(first)
                ctx:cycle_close(first)

                local second = ctx:cycle_open()
                local hit = ctx:cycle_match(second, template, 0, 0, 3, 1)
                ctx:cycle_click(second, hit)
                ctx:cycle_close(second)

                -- The first ticket is still dead after all of that, and closing it
                -- once more still does nothing.
                ctx:cycle_close(first)
                return 1
            )lua");

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
            CHECK_FALSE(context.hasOpenCycle());
        }

        TEST_CASE("Closing a cycle is what releases the frame, and only closing does")
        {
            // The whole point of the protocol: the frame's lifetime is the
            // ledger's, not the collector's. Both halves run with the VM already
            // destroyed, so every Lua handle has been finalised either way -- the
            // only difference is whether the script closed the cycle.
            SUBCASE("a cycle left open still holds its frame after the VM is gone")
            {
                auto built = buildBinding(resolvingFrames(FrameId{22}));
                REQUIRE(built.session.has_value());
                TaskContext context{*std::move(built.session), *built.recorder};

                constexpr std::string_view source = R"lua(
                    local cycle = ctx:cycle_open()
                    return 1
                )lua";

                CHECK(runBound(context, built, source) == doctest::Approx(1.0));
                CHECK(context.hasOpenCycle());
            }

            SUBCASE("a closed cycle holds nothing")
            {
                auto built = buildBinding(resolvingFrames(FrameId{23}));
                REQUIRE(built.session.has_value());
                TaskContext context{*std::move(built.session), *built.recorder};

                constexpr std::string_view source = R"lua(
                    local cycle = ctx:cycle_open()
                    ctx:cycle_close(cycle)
                    return 1
                )lua";

                CHECK(runBound(context, built, source) == doctest::Approx(1.0));
                CHECK_FALSE(context.hasOpenCycle());
            }
        }

        TEST_CASE("A ticket minted by one generation is rejected by another")
        {
            // Two contexts, so two ledgers and two generation stamps. Both open
            // their first cycle, so both hold ordinal 1 and only the generation
            // stamp tells the two tickets apart.
            auto firstBuilt = buildBinding(resolvingFrames(FrameId{24}));
            REQUIRE(firstBuilt.session.has_value());
            TaskContext first{*std::move(firstBuilt.session), *firstBuilt.recorder};

            auto secondBuilt = buildBinding(resolvingFrames(FrameId{25}));
            REQUIRE(secondBuilt.session.has_value());
            TaskContext second{*std::move(secondBuilt.session), *secondBuilt.recorder};

            auto const firstTicket = first.openCycle();
            REQUIRE(firstTicket.has_value());
            auto const secondTicket = second.openCycle();
            REQUIRE(secondTicket.has_value());
            REQUIRE(firstTicket->ordinal == secondTicket->ordinal);

            auto const crossed = second.cycleRead(*firstTicket, test::pixelRect(0, 0, 1, 1));
            REQUIRE_FALSE(crossed.has_value());
            CHECK(
                automationErrorKind(crossed.error())
                == AutomationErrorKind::StaleObservation
            );

            // The rejection is the stamp and not a dead ledger: the second
            // generation's own ticket of the same ordinal reaches the engine,
            // which refuses it for its own reason (no OCR adapter is bound) and
            // never for the ledger's.
            auto const own = second.cycleRead(*secondTicket, test::pixelRect(0, 0, 1, 1));
            REQUIRE_FALSE(own.has_value());
            CHECK(
                automationErrorKind(own.error())
                == AutomationErrorKind::UnsupportedCapability
            );
        }

        TEST_CASE("The task binding returns nil for a match that completes without a hit")
        {
            // The template is larger than the region it is searched in, which is
            // what a completed miss looks like: no candidate position at all, so
            // an ordinary answer about the screen rather than a search that
            // stopped. A template that FITS always reports its best position and
            // the distance there, because judging a distance is layer two's.
            auto frames = std::vector<Frame>{};
            frames.emplace_back(
                grayFrame(fixtureFingerprint(), resolvedTargetlessPixels(), FrameId{26})
            );
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const source = "local TEMPLATE = " + oversizedTemplateLiteral()
                + "\n" + R"lua(
                local template = ctx:template_load(TEMPLATE)
                local cycle = ctx:cycle_open()
                local hit = ctx:cycle_match(cycle, template, 0, 0, 3, 1)
                ctx:cycle_close(cycle)
                return (hit == nil) and 1 or 0
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 0);
        }

        TEST_CASE("The task binding runs a poll loop that opens and closes one cycle a turn")
        {
            // Twenty frames the template is not on, then one it is. The loop opens
            // a cycle, inspects it and closes it every turn, so it runs far past
            // anything a retention cap once allowed -- and needs no forced
            // collection to do it, because nothing is ever pinned past the close.
            auto const fingerprint = fixtureFingerprint();
            auto       frames      = std::vector<Frame>{};
            for (int index = 0; index < 20; ++index)
            {
                frames.emplace_back(grayFrame(fingerprint, unknownPixels(), FrameId{40}));
            }
            frames.emplace_back(grayFrame(fingerprint, resolvingPixels(), FrameId{41}));
            auto built = buildBinding(std::move(frames));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const source = withTemplate(R"lua(
                local template = ctx:template_load(TEMPLATE)
                while true do
                    local cycle = ctx:cycle_open()
                    local hit = ctx:cycle_match(cycle, template, 0, 0, 3, 1)
                    if hit ~= nil then
                        ctx:cycle_click(cycle, hit)
                        break
                    end
                    ctx:cycle_close(cycle)
                end
                return 1
            )lua");

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
            // The last cycle was consumed by the click and every earlier one was
            // closed, so nothing stays pinned in the host.
            CHECK_FALSE(context.hasOpenCycle());
        }

        TEST_CASE("A deadline out of the host's range is refused before any capture")
        {
            auto built = buildBinding(resolvingFrames(FrameId{29}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // 1e15 ms cleared the old <= 1e15 bound yet overflowed the nanosecond
            // tick rep inside duration_cast (undefined behaviour). It is now a
            // clean Tier B InvalidResource raised while the deadline is minted,
            // before anything is observed, so the frame source is never touched.
            constexpr std::string_view source = R"lua(
                local ok, err = pcall(function()
                    return ctx:deadline(1e15)
                end)
                if ok then return 0 end
                if err.kind ~= 'invalid_resource' then return 0 end
                if getmetatable(err) ~= 'uf.error' then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 0);
            CHECK_FALSE(context.hasOpenCycle());
        }

        TEST_CASE("ctx:try catches a Tier B automation error and returns the carrier")
        {
            auto built = buildBinding(resolvingFrames(FrameId{30}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // The first click consumes the cycle; a second click inside try is a
            // Tier B stale_observation, returned as (false, carrier). A plainly
            // successful function returns (true, nil).
            auto const source = withTemplate(R"lua(
                local template = ctx:template_load(TEMPLATE)
                local cycle = ctx:cycle_open()
                local hit = ctx:cycle_match(cycle, template, 0, 0, 3, 1)
                ctx:cycle_click(cycle, hit)

                local ok, err = ctx:try(function() ctx:cycle_click(cycle, hit) end)
                if ok ~= false then return 0 end
                if err == nil or err.kind ~= 'stale_observation' then return 0 end
                if err.retryable ~= true then return 0 end
                -- The carrier is host-minted userdata, not a table: that is what
                -- a project script has no way to produce.
                if type(err) ~= 'userdata' then return 0 end
                if getmetatable(err) ~= 'uf.error' then return 0 end

                local okDone, errDone = ctx:try(function() return 7 end)
                if okDone ~= true or errDone ~= nil then return 0 end
                return 1
            )lua");

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
        }

        TEST_CASE("A project script cannot forge a Tier B error that ctx:try accepts")
        {
            auto built = buildBinding(resolvingFrames(FrameId{33}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // Forgery is not a check to defeat any more: the carrier is a
            // userdata under a host tag, and a project script has no way to mint
            // a userdata at all. Every route it does have is exercised here,
            // each against the control that the GENUINE carrier is accepted by
            // the same path -- so a try that classified nothing would fail the
            // control rather than pass this vacuously.
            //
            // The second route is the one that matters most: it is the forgery
            // the frozen error TABLE could not refuse. With identity resting on
            // the __metatable label, a hand-built table wearing that label was
            // accepted; with identity resting on being userdata, it is not.
            auto const source = withTemplate(R"lua(
                local template = ctx:template_load(TEMPLATE)
                local cycle = ctx:cycle_open()
                local hit = ctx:cycle_match(cycle, template, 0, 0, 3, 1)
                ctx:cycle_click(cycle, hit)

                local ok, real = ctx:try(function() ctx:cycle_click(cycle, hit) end)
                if ok ~= false or real == nil then return 0 end
                if real.kind ~= 'stale_observation' then return 0 end

                -- Control: re-raised, the genuine carrier comes back as a Tier B
                -- result, unchanged.
                local okReal, sameErr = ctx:try(function() error(real) end)
                if okReal ~= false or sameErr ~= real then return 0 end

                local fields = function()
                    return {
                        kind = real.kind,
                        message = real.message,
                        retryable = real.retryable,
                    }
                end

                -- 1. A plain table carrying the same fields.
                local plain = fields()
                -- 2. The same table wearing a metatable that answers
                --    getmetatable with exactly the host's own label.
                local labelled = setmetatable(
                    fields(),
                    { __metatable = getmetatable(real) }
                )
                -- Control: the disguise really is complete on the axis the old
                -- carrier was identified by.
                if getmetatable(labelled) ~= getmetatable(real) then return 0 end

                -- 3. table.clone of a real one yields no value to dress up.
                if pcall(table.clone, real) then return 0 end
                -- 4. newproxy, the one base-library way to mint a userdata, is
                --    absent from the project environment.
                if newproxy ~= nil then return 0 end

                for _, forgery in ipairs({ plain, labelled }) do
                    -- try refuses to classify it and re-raises it, so the OUTER
                    -- pcall is what catches, and it catches the forgery itself
                    -- rather than a Tier B result.
                    local through, back = pcall(function()
                        ctx:try(function() error(forgery) end)
                    end)
                    if through then return 0 end
                    if back ~= forgery then return 0 end
                end
                return 1
            )lua");

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
        }

        TEST_CASE("The host names an uncaught Tier B error by its tag, never by its fields")
        {
            // The C++ half of the same guarantee. A value that escapes a run
            // uncaught is classified by the carrier's userdata tag, so the kind
            // in the run report -- and so in run.finished -- is the one that
            // really failed. A forged look-alike carries no tag and therefore
            // cannot choose the kind its run is reported under.
            SUBCASE("a genuine carrier names its kind")
            {
                auto built = buildBinding(resolvingFrames(FrameId{38}));
                REQUIRE(built.session.has_value());
                TaskContext context{*std::move(built.session), *built.recorder};

                auto const source = withTemplate(R"lua(
                    local template = ctx:template_load(TEMPLATE)
                    local cycle = ctx:cycle_open()
                    local hit = ctx:cycle_match(cycle, template, 0, 0, 3, 1)
                    ctx:cycle_click(cycle, hit)
                    -- Unguarded: the stale click raises and nothing catches it.
                    ctx:cycle_click(cycle, hit)
                    return 1
                )lua");

                auto const result = runBoundResult(context, built, source);
                REQUIRE_FALSE(result.has_value());
                CHECK(
                    automationErrorKind(result.error())
                    == AutomationErrorKind::StaleObservation
                );
            }

            SUBCASE("a forged look-alike names nothing")
            {
                auto built = buildBinding(resolvingFrames(FrameId{39}));
                REQUIRE(built.session.has_value());
                TaskContext context{*std::move(built.session), *built.recorder};

                // Same fields and the same label, read off a real error so the
                // disguise cannot go stale, raised the same way -- and the host
                // reports the script's own failure instead of the claimed kind.
                auto const source = withTemplate(R"lua(
                    local template = ctx:template_load(TEMPLATE)
                    local cycle = ctx:cycle_open()
                    local hit = ctx:cycle_match(cycle, template, 0, 0, 3, 1)
                    ctx:cycle_click(cycle, hit)

                    local ok, real = ctx:try(function() ctx:cycle_click(cycle, hit) end)
                    if ok ~= false or real == nil then return 1 end
                    error(setmetatable(
                        {
                            kind = real.kind,
                            message = real.message,
                            retryable = real.retryable,
                        },
                        { __metatable = getmetatable(real) }
                    ))
                )lua");

                auto const result = runBoundResult(context, built, source);
                REQUIRE_FALSE(result.has_value());
                CHECK(
                    automationErrorKind(result.error())
                    == AutomationErrorKind::InvalidResource
                );
            }
        }

        TEST_CASE("A Tier B error is immutable and hands out nothing a forgery could wear")
        {
            auto built = buildBinding(resolvingFrames(FrameId{40}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // The carrier a script catches is the host's own object and stays
            // the host's: its fields are readable and every write route is
            // refused, its metatable can neither be replaced nor obtained, and
            // its printed form names the kind rather than an address.
            //
            // The read controls are load-bearing. Without them an object that
            // simply answered nothing would pass every refusal below.
            auto const source = withTemplate(R"lua(
                local template = ctx:template_load(TEMPLATE)
                local cycle = ctx:cycle_open()
                local hit = ctx:cycle_match(cycle, template, 0, 0, 3, 1)
                ctx:cycle_click(cycle, hit)

                local ok, err = ctx:try(function() ctx:cycle_click(cycle, hit) end)
                if ok ~= false or err == nil then return 0 end

                -- Control: the fields ARE readable.
                if err.kind ~= 'stale_observation' then return 0 end
                if err.retryable ~= true then return 0 end
                if type(err.message) ~= 'string' then return 0 end
                if string.find(tostring(err), 'stale_observation', 1, true) == nil then
                    return 0
                end

                -- Every write route fails ...
                if pcall(function() err.kind = 'timeout' end) then return 0 end
                if pcall(function() err.injected = 1 end) then return 0 end
                if pcall(rawset, err, 'kind', 'timeout') then return 0 end
                -- ... and none of them took.
                if err.kind ~= 'stale_observation' then return 0 end
                if err.injected ~= nil then return 0 end

                -- The metatable cannot be replaced ...
                if pcall(setmetatable, err, {}) then return 0 end
                -- ... and cannot be read either: getmetatable hands back the
                -- label, so the fields table behind __index is unreachable and
                -- the value handed out is not something setmetatable accepts.
                local exposed = getmetatable(err)
                if type(exposed) ~= 'string' then return 0 end
                if pcall(setmetatable, {}, exposed) then return 0 end
                return 1
            )lua");

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 1);
        }

        TEST_CASE("ctx:try lets a script's own error propagate instead of swallowing it")
        {
            auto built = buildBinding(resolvingFrames(FrameId{31}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // error('boom') is the script's own failure, not a Tier B automation
            // error: try must re-raise it, so the outer pcall -- not try -- is what
            // catches it, the statement after try never runs, and the caught value
            // is the raw string rather than an error table.
            constexpr std::string_view source = R"lua(
                local reachedAfter = false
                local ok, err = pcall(function()
                    ctx:try(function() error('boom') end)
                    reachedAfter = true
                end)
                if ok then return 0 end
                if reachedAfter then return 0 end
                if type(err) ~= 'string' then return 0 end
                if string.find(err, 'boom') == nil then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 0);
        }

        TEST_CASE("A cancellation is unrecoverable through pcall or try, and mark never runs")
        {
            // The shared stop source drives both the engine (returns Cancelled) and
            // the VM interrupt (hard-breaks the thread). A capture requests the stop,
            // so whatever the script wraps the call in, no statement after it runs:
            // the host-visible mark() -- a non-automation witness -- stays at zero
            // and the run ends Cancelled.
            auto const cancelledRun = [](std::string_view guarded) -> void
            {
                auto stop  = std::stop_source{};
                auto frame = grayFrame(fixtureFingerprint(), resolvingPixels(), FrameId{32});
                auto built = buildBindingWith(
                    std::make_unique<StopOnCaptureFrameSource>(std::move(frame), stop),
                    stop.get_token(),
                    std::make_unique<DiscardingTraceSink>()
                );
                REQUIRE(built.session.has_value());
                TaskContext context{
                    *std::move(built.session),
                    *built.recorder,
                    TaskContextConfig{.cancellation = stop.get_token()},
                };

                auto const run = runWithMark(context, stop.get_token(), guarded);
                REQUIRE_FALSE(run.result.has_value());
                CHECK(
                    automationErrorKind(run.result.error())
                    == AutomationErrorKind::Cancelled
                );
                CHECK(run.markCount == 0);
                CHECK(built.clicks->clickCount() == 0);
            };

            SUBCASE("wrapped in the native pcall")
            {
                cancelledRun(R"lua(
                    pcall(function() ctx:cycle_open() end)
                    mark()
                    return 1
                )lua");
            }
            SUBCASE("wrapped in ctx:try")
            {
                cancelledRun(R"lua(
                    ctx:try(function() ctx:cycle_open() end)
                    mark()
                    return 1
                )lua");
            }
        }

        TEST_CASE("A swallowed cancellation still leaves the next primitive refused")
        {
            // The Tier C contract, split from the interrupt that normally hides
            // it: a project pcall MAY observe the sentinel, and observing it buys
            // nothing, because the terminal latch is checked at the C guard entry
            // of every primitive rather than by ctx:try.
            SUBCASE("the latch refuses every later primitive, without capturing")
            {
                auto frameSource = std::make_unique<CancelOnceFrameSource>(
                    grayFrame(
                        fixtureFingerprint(),
                        resolvingPixels(),
                        FrameId{34}
                    )
                );
                auto* const p_frames = frameSource.get();
                auto built = buildBindingWith(
                    std::move(frameSource),
                    std::stop_token{},
                    std::make_unique<DiscardingTraceSink>()
                );
                REQUIRE(built.session.has_value());
                TaskContext context{*std::move(built.session), *built.recorder};

                constexpr std::string_view source = R"lua(
                    -- The first primitive is cancelled. The host latches the
                    -- generation terminal and raises the Tier C sentinel, which
                    -- is a plain string a project pcall can catch.
                    local caught, err = pcall(function() return ctx:cycle_open() end)
                    if caught then return 0 end
                    if type(err) ~= 'string' then return 0 end

                    -- Catching it changed nothing: the next call is refused.
                    local again = pcall(function() return ctx:cycle_open() end)
                    if again then return 0 end

                    -- Including one routed through ctx:try, which is pure Luau
                    -- and consults no latch of its own -- it re-raises the
                    -- sentinel, so the outer pcall is what catches it.
                    local through = pcall(function()
                        ctx:try(function() return ctx:cycle_open() end)
                    end)
                    if through then return 0 end
                    return 1
                )lua";

                CHECK(runBound(context, built, source) == doctest::Approx(1.0));
                // One capture: the cancelled one. Neither refusal reached the
                // frame source, so the guard ran before the engine, not after.
                CHECK(p_frames->captureCount() == 1U);
                CHECK(built.clicks->clickCount() == 0);
            }

            SUBCASE("control: with nothing latched the second open succeeds")
            {
                // Without this the case above would also pass on a binding that
                // refuses every second cycle_open for some unrelated reason.
                auto frames = std::vector<Frame>{};
                frames.emplace_back(
                    grayFrame(
                        fixtureFingerprint(),
                        resolvingPixels(),
                        FrameId{35}
                    )
                );
                auto frameSource = std::make_unique<FakeFrameSource>(
                    std::move(frames)
                );
                auto* const p_frames = frameSource.get();
                auto built = buildBindingWith(
                    std::move(frameSource),
                    std::stop_token{},
                    std::make_unique<DiscardingTraceSink>()
                );
                REQUIRE(built.session.has_value());
                TaskContext context{*std::move(built.session), *built.recorder};

                constexpr std::string_view source = R"lua(
                    local first = ctx:cycle_open()
                    ctx:cycle_close(first)
                    local second = ctx:cycle_open()
                    ctx:cycle_close(second)
                    return 1
                )lua";

                CHECK(runBound(context, built, source) == doctest::Approx(1.0));
                CHECK(p_frames->captureCount() == 2U);
            }
        }

        TEST_CASE("The retired root spelling names nothing on a real task VM")
        {
            auto built = buildBinding(resolvingFrames(FrameId{37}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // `umbra` was this root's global name before 2026-07-29. The project
            // environment is a whitelist with no metatable, so a name the
            // installer no longer registers is structurally absent: it reads nil
            // and indexing it raises. The control is `uf` itself, which must
            // still carry the error table.
            constexpr std::string_view source = R"lua(
                if umbra ~= nil then return 0 end
                if pcall(function() return umbra.errors.timeout end) then return 0 end

                if uf.errors.timeout ~= 'timeout' then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
        }

        TEST_CASE("No project route reaches the private capability surface")
        {
            auto frameSource = std::make_unique<FakeFrameSource>(
                resolvingFrames(FrameId{36})
            );
            auto* const p_frames = frameSource.get();
            auto built = buildBindingWith(
                std::move(frameSource),
                std::stop_token{},
                std::make_unique<DiscardingTraceSink>()
            );
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // The primitives are upvalues of the framework's closures. A project
            // script must not reach one as a global, as a field of uf, through
            // rawget, through a table.clone of uf, or by walking values, keys
            // and metatables from anything it can name.
            //
            // ctx is excluded from the walk by identity, and only by identity:
            // its methods are the framework's own Luau wrappers, which ARE the
            // published surface. The claim is about the native table behind them.
            //
            // Three controls keep the claim from being vacuous: the scanner is
            // shown finding a planted primitive by the same route; the two
            // by-name routes are shown finding a key that really is on uf; and
            // the run ends by driving a real capture through ctx, so the
            // framework demonstrably holds what no project route can name.
            constexpr std::string_view source = R"lua(
                local target = 'cycle_open'

                local function scan(value, depth, seen)
                    if depth > 6 then return false end
                    if type(value) ~= 'table' then return false end
                    if seen[value] then return false end
                    seen[value] = true
                    if value ~= ctx and rawget(value, target) ~= nil then
                        return true
                    end
                    for key, entry in pairs(value) do
                        if scan(key, depth + 1, seen) then return true end
                        if scan(entry, depth + 1, seen) then return true end
                    end
                    return scan(getmetatable(value), depth + 1, seen)
                end

                -- Control: the scanner really does find one when it is there.
                if not scan({ nest = { { cycle_open = print } } }, 0, {}) then
                    return 0
                end

                -- No primitive is a project global.
                if cycle_open ~= nil or cycle_close ~= nil then return 0 end
                if cycle_match ~= nil or cycle_read ~= nil then return 0 end
                if cycle_click ~= nil or raise ~= nil then return 0 end
                if deadline ~= nil or wait ~= nil or settle ~= nil then return 0 end
                if random ~= nil or try ~= nil then return 0 end

                -- Nor a field of uf, by index or by rawget.
                local names = {
                    'cycle_open', 'cycle_close', 'cycle_match', 'cycle_read',
                    'cycle_click', 'template_load', 'raise', 'deadline', 'wait',
                    'settle', 'random', 'try',
                }
                for _, name in ipairs(names) do
                    if uf[name] ~= nil then return 0 end
                    if rawget(uf, name) ~= nil then return 0 end
                end
                -- Control: both routes DO see a key that is on uf.
                if uf.errors == nil or rawget(uf, 'errors') == nil then
                    return 0
                end
                -- And the old method spelling now calls a nil.
                if pcall(function() return uf:cycle_open() end) then return 0 end

                -- No walk from anything nameable reaches one, uf's clone and
                -- the framework's own ctx included.
                if scan({
                    uf, ctx, task, table.clone(uf), getmetatable(uf),
                    _G, getfenv, setfenv, newproxy, gcinfo, coroutine, debug,
                    _VERSION, assert, error, getmetatable, ipairs, next, pairs,
                    pcall, print, rawequal, rawget, rawlen, rawset, select,
                    setmetatable, tonumber, tostring, type, typeof, unpack,
                    xpcall, bit32, buffer, math, os, string, table, utf8, vector,
                }, 0, {}) then
                    return 0
                end

                -- Control: the framework DOES hold the surface, and can spend a
                -- real capture with it.
                local cycle = ctx:cycle_open()
                if cycle == nil then return 0 end
                ctx:cycle_close(cycle)
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(p_frames->captureCount() == 1U);
        }

        TEST_CASE("A failing verb reports its own cause, not the trace sink's failure")
        {
            // A failing verb records its native call before it raises. If that
            // record raised the sink's own IoFailure instead, an author debugging a
            // failed click would be told the trace file was unwritable rather than
            // why the click failed. The verb's cause wins; the lost evidence is
            // latched on the context so the host still learns the trace is
            // incomplete.
            //
            // Clicking consumes the cycle, so the second click fails
            // StaleObservation -- a Tier B failure ctx:try hands back rather than
            // re-raising, which is what makes the substitution observable.
            auto built = buildBindingWith(
                std::make_unique<FakeFrameSource>(resolvingFrames(FrameId{88})),
                std::stop_token{},
                std::make_unique<FailOnFailedTraceSink>()
            );
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const source = withTemplate(R"lua(
                local template = ctx:template_load(TEMPLATE)
                local cycle = ctx:cycle_open()
                local hit = ctx:cycle_match(cycle, template, 0, 0, 3, 1)
                if hit == nil then return 0 end
                ctx:cycle_click(cycle, hit)

                local ok, err = ctx:try(function() ctx:cycle_click(cycle, hit) end)
                if ok then return 0 end
                if err.kind ~= 'stale_observation' then return 0 end
                return 1
            )lua");

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(context.traceFailed());
            CHECK(built.clicks->clickCount() == 1);
        }

        TEST_CASE("ctx:random is the task's only RNG, covers its interval, and rejects empty ones")
        {
            auto built = buildBinding(resolvingFrames(FrameId{61}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // The sandbox removed the native clocks and RNG, so ctx:random is the
            // sole source of randomness; no-argument random() is a float in [0, 1);
            // random(1, 3) stays in the closed interval, is always integral, and
            // over enough draws reaches both endpoints; and an empty or non-integer
            // interval is rejected exactly as math.random would reject it.
            constexpr std::string_view source = R"lua(
                if os.time ~= nil or os.clock ~= nil then return 0 end
                if math.random ~= nil or math.randomseed ~= nil then return 0 end

                local f = ctx:random()
                if type(f) ~= 'number' or f < 0 or f >= 1 then return 0 end

                local lo, hi = 3, 1
                for _ = 1, 400 do
                    local r = ctx:random(1, 3)
                    if type(r) ~= 'number' or r < 1 or r > 3 then return 0 end
                    if r ~= math.floor(r) then return 0 end
                    if r < lo then lo = r end
                    if r > hi then hi = r end
                end
                if lo ~= 1 or hi ~= 3 then return 0 end

                if pcall(function() return ctx:random(0) end) then return 0 end
                if pcall(function() return ctx:random(5, 2) end) then return 0 end
                if pcall(function() return ctx:random(1.5) end) then return 0 end

                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
        }

        TEST_CASE("an open-to-click cycle produces one ordered stream both layers wrote")
        {
            auto traceSink       = std::make_unique<RecordingTraceSink>();
            auto* const p_traces = traceSink.get();
            auto built = buildBindingWith(
                std::make_unique<FakeFrameSource>(resolvingFrames(FrameId{70})),
                std::stop_token{},
                std::move(traceSink)
            );
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const source = withTemplate(R"lua(
                local template = ctx:template_load(TEMPLATE)
                local cycle = ctx:cycle_open()
                local hit = ctx:cycle_match(cycle, template, 0, 0, 3, 1)
                ctx:cycle_click(cycle, hit)
                ctx:cycle_close(cycle)
                return 1
            )lua");

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));

            auto const& events = p_traces->events();

            // Every event of the run carries the same run identity and the next
            // sequence number, whichever layer wrote it. This is what the two old
            // schemas could not do: they had no join key at all.
            REQUIRE_FALSE(events.empty());
            for (auto index = std::size_t{0}; index < events.size(); ++index)
            {
                CHECK(events[index].sequence() == index + 1U);
                CHECK(events[index].runId() == k_fixtureRunId);
                CHECK(events[index].generationId() == k_fixtureGenerationId);
            }

            // The engine event and the task.native_call it caused sit adjacent in
            // one stream, so a reader can correlate them: each primitive's native
            // call immediately follows the engine work it drove, and the click's
            // engine.action_delivered names the frame the open observed. The
            // trailing cycle_close reached the host and found nothing to release,
            // which is the deterministic-release path being audited rather than
            // inferred.
            auto const kinds = kindsOf(events);
            auto const expected = std::vector<trace::TraceEventKind>{
                trace::TraceEventKind::TaskNativeCall,
                trace::TraceEventKind::EngineObserved,
                trace::TraceEventKind::TaskNativeCall,
                trace::TraceEventKind::EngineActionFound,
                trace::TraceEventKind::TaskNativeCall,
                trace::TraceEventKind::EngineActionAuthorized,
                trace::TraceEventKind::EngineActionDelivered,
                trace::TraceEventKind::EngineObservationInvalidated,
                trace::TraceEventKind::TaskNativeCall,
                trace::TraceEventKind::TaskNativeCall,
            };
            CHECK(kinds == expected);

            auto const observed = events[1].event();
            REQUIRE(observed.frame.has_value());
            CHECK(observed.frame->frameId() == FrameId{70});
            auto const delivered = events[6].event();
            REQUIRE(delivered.frame.has_value());
            CHECK(delivered.frame->frameId() == FrameId{70});

            // Each primitive emits exactly one native call, in call order. The
            // template load comes first and reaches no engine verb at all, which
            // is why its line stands alone.
            auto const verbs = nativeCallVerbs(events);
            CHECK(
                verbs
                == std::vector<std::string>{
                    "template_load",
                    "cycle_open",
                    "cycle_match",
                    "cycle_click",
                    "cycle_close",
                }
            );

            auto const* p_click = findNativeCall(events, "cycle_click");
            REQUIRE(p_click != nullptr);
            CHECK(p_click->outcome == trace::NativeCallOutcome::Succeeded);
            // Both ordinals name the one open cycle, which is what a delivered
            // click always looks like now.
            CHECK(p_click->cycleOrdinal == p_click->hitCycleOrdinal);

            // The close after a consuming click had nothing left to release.
            auto const* p_close = findNativeCall(events, "cycle_close");
            REQUIRE(p_close != nullptr);
            CHECK(p_close->outcome == trace::NativeCallOutcome::Empty);
        }

        TEST_CASE("The task binding emits an Empty native call for a match that finds nothing")
        {
            auto const fingerprint = fixtureFingerprint();
            auto frames            = std::vector<Frame>{};
            frames.emplace_back(grayFrame(fingerprint, unknownPixels(), FrameId{71}));
            frames.emplace_back(
                grayFrame(fingerprint, resolvedTargetlessPixels(), FrameId{72})
            );
            auto traceSink       = std::make_unique<RecordingTraceSink>();
            auto* const p_traces = traceSink.get();
            auto built = buildBindingWith(
                std::make_unique<FakeFrameSource>(std::move(frames)),
                std::stop_token{},
                std::move(traceSink)
            );
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // The template is larger than the region, so both searches COMPLETE
            // with nowhere to have looked. Each records Empty (Tier A) rather
            // than Succeeded or Failed: an absence is an ordinary answer about
            // the screen, and the distinction is what a reader tells a miss from
            // a stop by.
            auto const source = "local TEMPLATE = " + oversizedTemplateLiteral()
                + "\n" + R"lua(
                local template = ctx:template_load(TEMPLATE)
                local first = ctx:cycle_open()
                local miss1 = ctx:cycle_match(first, template, 0, 0, 3, 1)
                ctx:cycle_close(first)

                local second = ctx:cycle_open()
                local miss2 = ctx:cycle_match(second, template, 0, 0, 3, 1)
                ctx:cycle_close(second)
                return (miss1 == nil and miss2 == nil) and 1 or 0
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));

            auto const& events = p_traces->events();
            CHECK(
                nativeCallVerbs(events)
                == std::vector<std::string>{
                    "template_load",
                    "cycle_open",
                    "cycle_match",
                    "cycle_close",
                    "cycle_open",
                    "cycle_match",
                    "cycle_close",
                }
            );

            auto const* p_match = findNativeCall(events, "cycle_match");
            REQUIRE(p_match != nullptr);
            CHECK(p_match->outcome == trace::NativeCallOutcome::Empty);

            // The close DID release a frame, so it is the one Succeeded call here.
            auto const* p_close = findNativeCall(events, "cycle_close");
            REQUIRE(p_close != nullptr);
            CHECK(p_close->outcome == trace::NativeCallOutcome::Succeeded);
        }

        TEST_CASE("The task binding aborts a verb as Tier B io_failure when the trace sink fails")
        {
            auto built = buildBindingWith(
                std::make_unique<FakeFrameSource>(resolvingFrames(FrameId{72})),
                std::stop_token{},
                std::make_unique<FailOnNativeCallTraceSink>()
            );
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // cycle_open succeeds in the engine, but recording its native call
            // fails; losing the trace evidence aborts the primitive as a Tier B
            // io_failure carrying the protected uf.error metatable, rather than
            // dropping the record silently (the trace's throw-instant discipline).
            constexpr std::string_view source = R"lua(
                local ok, err = pcall(function() return ctx:cycle_open() end)
                if ok then return 0 end
                if err.kind ~= 'io_failure' then return 0 end
                if getmetatable(err) ~= 'uf.error' then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 0);
        }
    }
}
