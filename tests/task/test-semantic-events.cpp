#include "binding-fixture.hpp"

#include <task/script-bindings.hpp>
#include <task/framework-bundle.hpp>
#include <task/task-context.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>

#include <script/engine.hpp>

#include <trace/event.hpp>
#include <trace/recorder.hpp>
#include <trace/stream-validator.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The framework semantic events and the two primitives behind them, driven
// through a real task VM: what the trusted Luau framework asks the host to
// record, what the host refuses, and what a refusal costs. Everything runs off
// fake ports and asserts on host-visible counts -- frames served, clicks
// delivered, lines recorded -- so no case depends on machine load or a clock.
namespace uf::task
{
    namespace
    {
        // A framework module that emits BADLY on purpose, loaded beside the real
        // bundle and published to the project environment as `probe`. No project
        // script can reach `emit` -- it is a closure upvalue of the trusted
        // framework -- so the only honest way to test what the host does about a
        // framework bug is to boot a framework that has one. The source is a
        // string literal, satisfying FrameworkModule's process-long lifetime
        // contract.
        constexpr auto k_probeSource = std::string_view{R"lua(
            local native = ...
            local probe = {}

            function probe.finish_unopened_step()
                native.emit("step_finished", "never_opened")
            end

            function probe.leak_step()
                native.emit("step_started", "leaked")
            end

            function probe.close_leaked_step()
                native.emit("step_finished", "leaked")
            end

            return probe
        )lua"};

        [[nodiscard]]
        auto probeVmConfig(TaskContext& context) -> script::EngineConfig
        {
            auto config = taskVmConfig(context);
            config.frameworkModules.emplace_back(
                script::FrameworkModule{.name = "probe", .source = k_probeSource}
            );
            config.frameworkProjectGlobals.emplace_back("probe");
            return config;
        }

        [[nodiscard]]
        auto runWithProbe(
            TaskContext& context,
            Built& /*built*/,
            std::string_view source
        ) -> double
        {
            auto engine = script::Engine::create(probeVmConfig(context));
            REQUIRE(engine.has_value());
            auto const result = engine->runNumber(source, "semantic-events");
            REQUIRE(result.has_value());
            return *result;
        }

        // A build whose trace sink keeps every stamped line, plus observing
        // pointers to the frame source and that sink.
        struct SemanticBuild final
        {
            Built               built;
            FakeFrameSource*    frames{};
            RecordingTraceSink* traces{};
        };

        [[nodiscard]]
        auto buildRecording(std::vector<Frame> frames) -> SemanticBuild
        {
            auto frameSource     = std::make_unique<FakeFrameSource>(std::move(frames));
            auto* const p_frames = frameSource.get();
            auto traceSink       = std::make_unique<RecordingTraceSink>();
            auto* const p_traces = traceSink.get();
            auto built           = buildBindingWith(
                std::move(frameSource),
                std::stop_token{},
                std::move(traceSink)
            );
            return SemanticBuild{
                .built  = std::move(built),
                .frames = p_frames,
                .traces = p_traces,
            };
        }

        [[nodiscard]]
        auto unknownFrames(std::size_t count, FrameId frameId) -> std::vector<Frame>
        {
            auto const fingerprint = test::fingerprint(3, 1, 96, 96);
            auto frames            = std::vector<Frame>{};
            for (auto index = std::size_t{0}; index < count; ++index)
            {
                frames.emplace_back(grayFrame(fingerprint, unknownPixels(), frameId));
            }
            return frames;
        }

        [[nodiscard]]
        auto semanticKinds(
            std::vector<trace::StampedTraceEvent> const& events
        ) -> std::vector<trace::TraceEventKind>
        {
            auto kinds = std::vector<trace::TraceEventKind>{};
            for (auto const& event : events)
            {
                if (event.event().framework.has_value())
                {
                    kinds.emplace_back(event.event().kind);
                }
            }
            return kinds;
        }

        [[nodiscard]]
        auto semanticLabels(
            std::vector<trace::StampedTraceEvent> const& events
        ) -> std::vector<std::string>
        {
            auto labels = std::vector<std::string>{};
            for (auto const& event : events)
            {
                if (
                    event.event().framework.has_value()
                    && !event.event().framework->label.empty()
                )
                {
                    labels.emplace_back(event.event().framework->label);
                }
            }
            return labels;
        }

        // The serialized line the run's first event of `kind` produced, or an
        // empty string when it recorded none. A replay checker reads the wire
        // rather than the struct behind it, so a case about what reaches a
        // consumer asserts on the JSON.
        [[nodiscard]]
        auto serializedLineOfKind(
            std::vector<trace::StampedTraceEvent> const& events,
            trace::TraceEventKind kind
        ) -> std::string
        {
            for (auto const& event : events)
            {
                if (event.event().kind == kind)
                {
                    return trace::serializeTraceEvent(event);
                }
            }
            return std::string{};
        }

        // The first line the run recorded for `verb`, so a test can read the step
        // scope stamped onto it. The returned pointer observes the recording
        // sink's own storage, which the annotation on the parameter states.
        [[nodiscard]]
        auto findStampedCall(
            std::vector<trace::StampedTraceEvent> const& events UF_LIFETIME_BOUND,
            std::string_view verb
        ) noexcept -> trace::StampedTraceEvent const*
        {
            for (auto const& event : events)
            {
                auto const& call = event.event().nativeCall;
                if (call.has_value() && call->verb == verb)
                {
                    return &event;
                }
            }
            return nullptr;
        }

        TEST_CASE("the framework records the structure it maintains, in order")
        {
            auto semantic = buildRecording(unknownFrames(3, FrameId{300}));
            REQUIRE(semantic.built.session.has_value());
            TaskContext context{
                *std::move(semantic.built.session),
                *semantic.built.recorder,
            };

            // One nested pair of steps with a native call inside them, one
            // declared pause, and one retry that exhausts its two attempts on a
            // retryable failure -- between them, every framework event a task
            // can produce.
            constexpr std::string_view source = R"lua(
                ctx:step('outer', function()
                    ctx:step('inner', function()
                        local ticket = ctx:cycle_open()
                        ctx:cycle_close(ticket)
                    end)
                end)

                ctx:settle(0)

                local tries = 0
                local ok = ctx:try(function()
                    ctx:retry({ attempts = 2, backoff_ms = 0 }, function()
                        tries += 1
                        local ticket = ctx:cycle_open()
                        ctx:cycle_close(ticket)
                        -- A closed ticket names no open cycle, so this raises the
                        -- retryable StaleObservation the retry policy is waiting
                        -- for.
                        ctx:cycle_read(ticket, 0, 0, 1, 1)
                    end)
                end)
                if ok ~= false then return 0 end
                if tries ~= 2 then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, semantic.built, source) == doctest::Approx(1.0));

            auto const expectedKinds = std::vector<trace::TraceEventKind>{
                trace::TraceEventKind::FrameworkStepStarted,
                trace::TraceEventKind::FrameworkStepStarted,
                trace::TraceEventKind::FrameworkStepFinished,
                trace::TraceEventKind::FrameworkStepFinished,
                trace::TraceEventKind::FrameworkSettled,
                trace::TraceEventKind::FrameworkRetryAttempt,
                trace::TraceEventKind::FrameworkRetryBackoff,
                trace::TraceEventKind::FrameworkRetryAttempt,
            };
            CHECK(semanticKinds(semantic.traces->events()) == expectedKinds);

            CHECK(
                semanticLabels(semantic.traces->events())
                == std::vector<std::string>{"outer", "inner", "inner", "outer"}
            );

            // The two attempts name the same declared total and count up.
            auto attempts = std::vector<uint64>{};
            for (auto const& event : semantic.traces->events())
            {
                if (event.event().kind == trace::TraceEventKind::FrameworkRetryAttempt)
                {
                    REQUIRE(event.event().framework->attempt.has_value());
                    REQUIRE(event.event().framework->attempts.has_value());
                    CHECK(*event.event().framework->attempts == 2U);
                    attempts.emplace_back(*event.event().framework->attempt);
                }
            }
            CHECK(attempts == std::vector<uint64>{1U, 2U});

            // Design section 12's "every task.native_call falls inside the step
            // scope open at the time": the call made inside the two steps carries
            // both of them, stamped by the host rather than claimed by the caller.
            auto const* const p_call =
                findStampedCall(semantic.traces->events(), "cycle_open");
            REQUIRE(p_call != nullptr);
            REQUIRE(p_call->openSteps().size() == 2U);
            CHECK(p_call->openSteps()[0] == "outer");
            CHECK(p_call->openSteps()[1] == "inner");

            // The control that keeps the stamp from being trivially non-empty:
            // the pause taken outside any step carries no scope at all.
            auto const* const p_settle =
                findStampedCall(semantic.traces->events(), "settle");
            REQUIRE(p_settle != nullptr);
            CHECK(p_settle->openSteps().empty());

            CHECK(semantic.frames->captureCount() == 3U);
            CHECK_FALSE(context.fatal());
        }

        // Two pages over one frame that carries the action grey at x = 1 and
        // nothing at x = 2: the first page's anchor is there and the second
        // page's is not, so one resolution succeeds and one fails on the same
        // ticket. Both are asked, because "only a success is recorded" is not
        // observable from a script that only ever resolves.
        constexpr std::string_view k_twoPageSource = R"lua(
            local function anchorAt(name, x)
                return model.Element.new{
                    name = name,
                    form = "fixed",
                    capabilities = { "identify" },
                    rect = { x = x, y = 0, width = 1, height = 1 },
                    appearances = {
                        {
                            name = "lit",
                            source = "lit.png",
                            template = ctx:template_load(TEMPLATE),
                            threshold = 10000,
                        },
                    },
                }
            end

            local function pageOver(name, element)
                return model.Page.new{
                    name = name,
                    references = {
                        {
                            element = element,
                            holding = "owned",
                            exercised = { "identify" },
                            identify = "required",
                        },
                    },
                }
            end

            local battle = pageOver("battle", anchorAt("here", 1))
            local menu   = pageOver("menu", anchorAt("elsewhere", 2))

            local ticket = ctx:cycle_open()
            local missed = observe.resolve_page(ctx, ticket, menu)
            local receipt = observe.resolve_page(ctx, ticket, battle)
            ctx:cycle_close(ticket)

            if missed ~= nil then return 0 end
            if receipt == nil then return 0 end
            if receipt.page ~= battle then return 0 end
            return 1
        )lua";

        TEST_CASE("a resolved page is recorded under its own name, and only then")
        {
            auto semantic = buildRecording(resolvingFrames(FrameId{304}));
            REQUIRE(semantic.built.session.has_value());
            TaskContext context{
                *std::move(semantic.built.session),
                *semantic.built.recorder,
            };

            CHECK(
                runBound(context, semantic.built, withTemplate(k_twoPageSource))
                == doctest::Approx(1.0)
            );

            // One line for two resolutions attempted: the page that answered.
            // Without it the run's page identities survive nowhere in the
            // stream, which is what this event exists to fix.
            CHECK(
                semanticKinds(semantic.traces->events())
                == std::vector<trace::TraceEventKind>{
                    trace::TraceEventKind::FrameworkPageResolved,
                }
            );
            CHECK(
                semanticLabels(semantic.traces->events())
                == std::vector<std::string>{"battle"}
            );

            auto const line = serializedLineOfKind(
                semantic.traces->events(),
                trace::TraceEventKind::FrameworkPageResolved
            );
            CHECK(line.find(R"("kind":"framework.page_resolved")") != std::string::npos);
            CHECK(line.find(R"("label":"battle")") != std::string::npos);

            // One capture: both resolutions read the same ticket, so the count
            // says the failed one really did run against the same frame.
            CHECK(semantic.frames->captureCount() == 1U);
            CHECK_FALSE(context.fatal());
        }

        TEST_CASE("an unusable step name is refused whole and the run carries on")
        {
            auto semantic = buildRecording(unknownFrames(1, FrameId{301}));
            REQUIRE(semantic.built.session.has_value());
            TaskContext context{
                *std::move(semantic.built.session),
                *semantic.built.recorder,
            };

            // Length and character set are the host's, and a refusal is Tier B:
            // the name came from a project string literal, and the invariant
            // kind is reserved for failures a project cannot cause -- so the
            // author catches it, and the generation is still theirs to use.
            constexpr std::string_view source = R"lua(
                local tooLong = string.rep('a', 65)
                local ok, err = ctx:try(function()
                    ctx:step(tooLong, function() end)
                end)
                if ok ~= false then return 0 end
                if type(err) ~= 'userdata' then return 0 end
                if err.kind ~= uf.errors.invalid_resource then return 0 end
                if #ctx:step_path() ~= 0 then return 0 end

                local bad, badErr = ctx:try(function()
                    ctx:step('wait\nhere', function() end)
                end)
                if bad ~= false then return 0 end
                if badErr.kind ~= uf.errors.invalid_resource then return 0 end
                if #ctx:step_path() ~= 0 then return 0 end

                -- The control: the generation is live, so a legal step still runs
                -- and the primitive inside it still reaches the engine.
                local ran = 0
                ctx:step('daily', function()
                    ran = 1
                    local ticket = ctx:cycle_open()
                    ctx:cycle_close(ticket)
                end)
                if ran ~= 1 then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, semantic.built, source) == doctest::Approx(1.0));

            // Nothing was truncated into the stream: the only step the trace
            // knows about is the one that was accepted whole. A host that had
            // clipped the long name to its ceiling would show a third label here.
            CHECK(
                semanticLabels(semantic.traces->events())
                == std::vector<std::string>{"daily", "daily"}
            );
            CHECK(semantic.frames->captureCount() == 1U);
            CHECK_FALSE(context.fatal());
        }

        TEST_CASE("a framework invariant failure is latched before it is raised")
        {
            auto semantic = buildRecording(unknownFrames(2, FrameId{302}));
            REQUIRE(semantic.built.session.has_value());
            TaskContext context{
                *std::move(semantic.built.session),
                *semantic.built.recorder,
            };

            // The value CAN be caught -- there is no way to make a Lua raise
            // uncatchable -- so what is under test is that catching it buys no
            // control: the latch is already set when the raise happens, and every
            // later primitive stops at the guard before it reaches the engine.
            constexpr std::string_view source = R"lua(
                -- Control: the primitive works, and costs a frame, before the
                -- framework misbehaves. Without it the refusals below would pass
                -- against a session that never worked at all.
                local first = ctx:cycle_open()
                ctx:cycle_close(first)

                local swallowed, err = pcall(function()
                    probe.finish_unopened_step()
                end)
                if swallowed then return 0 end
                if type(err) ~= 'userdata' then return 0 end
                if err.kind ~= uf.errors.internal_invariant then return 0 end

                -- Swallowed, and worth nothing: the next primitive refuses.
                if pcall(function() return ctx:cycle_open() end) then return 0 end
                if pcall(function() ctx:step('after', function() end) end) then
                    return 0
                end
                if ctx:try(function() return ctx:cycle_open() end) ~= false then
                    return 0
                end
                return 1
            )lua";

            CHECK(runWithProbe(context, semantic.built, source) == doctest::Approx(1.0));

            CHECK(context.fatal());
            CHECK(context.terminalKind() == AutomationErrorKind::InternalInvariant);

            // One capture: the control's. Every refusal after the violation
            // stopped at the guard, so none of them observed anything.
            CHECK(semantic.frames->captureCount() == 1U);
            CHECK(semantic.built.clicks->clickCount() == 0);

            // The refused event never reached the stream, and neither did the
            // step the refused ctx:step would have opened.
            CHECK(semanticKinds(semantic.traces->events()).empty());
        }

        TEST_CASE("a step the framework never closed refuses the run bracket")
        {
            auto semantic = buildRecording(unknownFrames(1, FrameId{303}));
            REQUIRE(semantic.built.session.has_value());
            TaskContext context{
                *std::move(semantic.built.session),
                *semantic.built.recorder,
            };

            REQUIRE(semantic.built.recorder->requireScopesClosed().has_value());

            constexpr std::string_view leak = R"lua(
                probe.leak_step()
                return 1
            )lua";
            CHECK(runWithProbe(context, semantic.built, leak) == doctest::Approx(1.0));

            // The script returned cleanly, so nothing else would have failed this
            // run: the unclosed step itself is the failure, under the kind
            // reserved for a framework bug.
            auto const scopes = semantic.built.recorder->requireScopesClosed();
            REQUIRE_FALSE(scopes.has_value());
            CHECK(
                automationErrorKind(scopes.error())
                == AutomationErrorKind::InternalInvariant
            );

            // The control: closing it makes the bracket closable again, so the
            // refusal is about the open step rather than the check never passing.
            constexpr std::string_view close = R"lua(
                probe.close_leaked_step()
                return 1
            )lua";
            CHECK(runWithProbe(context, semantic.built, close) == doctest::Approx(1.0));
            CHECK(semantic.built.recorder->requireScopesClosed().has_value());
        }
    }
}
