#include "binding-fixture.hpp"

#include <task/script-bindings.hpp>
#include <task/task-context.hpp>

#include <core/error/result.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>

#include <engine/ports.hpp>
#include <engine/session.hpp>

#include <trace/event.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <memory>
#include <stop_token>
#include <string_view>
#include <utility>
#include <vector>

// The trusted Luau framework's policy layer, driven through a real task VM:
// ctx:step and ctx:cycle over the real cycle ledger.
//
// Every case is driven from fake ENGINE ports and asserts only on facts the host
// can see -- how many frames were served, how many clicks were delivered,
// whether the ledger still holds a cycle -- so a green run finishes in
// milliseconds and nothing here depends on how loaded the machine is. The two
// cancellation cases arm their stop from inside the frame source rather than
// from a timer thread, so even "the stop landed mid-wait" is a fixed point in
// the sequence rather than a race.
//
// The companion file is test-framework-surface.cpp, which drives the SAME
// framework against a scripted stand-in for the private capability surface and
// asserts the primitive call sequence instead. What stays here is what a fake
// surface cannot reach: the real ledger's own answer about the frame it is
// holding, and the real terminal latch.
namespace uf::task
{
    namespace
    {
        // A build over the fixture geometry, with observing pointers to the
        // frame source and the click sink. The frame count is what several cases
        // below assert against: a refusal that still spent a capture would be a
        // weaker guarantee.
        struct FrameworkBuild final
        {
            Built               built;
            FakeFrameSource*    frames{};
            RecordingTraceSink* traces{};
        };

        // Every case records its trace rather than discarding it: the
        // framework's semantic events pass the host's validation state machine
        // on their way to this sink, so a run that completes here is also a run
        // whose event sequence the host accepted.
        [[nodiscard]]
        auto buildOver(std::vector<Frame> frames) -> FrameworkBuild
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
            return FrameworkBuild{
                .built  = std::move(built),
                .frames = p_frames,
                .traces = p_traces,
            };
        }

        // The kinds of the framework semantic events a run recorded, in order.
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
        auto unknownFrames(std::size_t count, FrameId frameId) -> std::vector<Frame>
        {
            auto const fingerprint = fixtureFingerprint();
            auto frames            = std::vector<Frame>{};
            for (std::size_t index = 0; index < count; ++index)
            {
                frames.emplace_back(grayFrame(fingerprint, unknownPixels(), frameId));
            }
            return frames;
        }

        // Requests the run's stop from inside the first capture, so a
        // cancellation landing WHILE the framework's wait loop runs is a fixed
        // point: exactly one frame was served when the stop arrived, and any
        // later capture is one the terminal guard failed to prevent.
        class StopOnFirstCaptureFrameSource final : public engine::IFrameSource
        {
            Frame            m_frame;
            std::stop_source m_stop;
            std::size_t      m_captureCount{0};

        public:
            StopOnFirstCaptureFrameSource(Frame frame, std::stop_source stop) noexcept
                : m_frame{std::move(frame)}
                , m_stop{std::move(stop)}
            {
            }

            [[nodiscard]]
            auto capture(CaptureBudget const& /*budget*/) -> Result<Frame> override
            {
                ++m_captureCount;
                if (m_captureCount == 1U)
                {
                    static_cast<void>(m_stop.request_stop());
                }
                return m_frame;
            }

            [[nodiscard]] auto validateTargetInstance() -> Status override
            {
                return ok();
            }

            [[nodiscard]] auto captureCount() const noexcept -> std::size_t
            {
                return m_captureCount;
            }
        };

        TEST_CASE("ctx:cycle closes its cycle on every exit path")
        {
            auto framework = buildOver(resolvingFrames(FrameId{60}));
            REQUIRE(framework.built.session.has_value());
            TaskContext context{
                *std::move(framework.built.session),
                *framework.built.recorder,
            };

            // Three exits, asserted on the host side rather than by asking Lua:
            // hasOpenCycle() is the ledger's own answer, and it holds the frame.
            constexpr std::string_view normal = R"lua(
                return ctx:cycle(function(cycle)
                    return 1
                end)
            )lua";
            CHECK(runBound(context, framework.built, normal) == doctest::Approx(1.0));
            CHECK_FALSE(context.hasOpenCycle());

            constexpr std::string_view luauError = R"lua(
                local ok, err = pcall(function()
                    ctx:cycle(function() error('boom', 0) end)
                end)
                if ok or err ~= 'boom' then return 0 end
                return 1
            )lua";
            CHECK(runBound(context, framework.built, luauError) == doctest::Approx(1.0));
            CHECK_FALSE(context.hasOpenCycle());

            // A Tier B raise from a primitive INSIDE the block: a match handed
            // something that is not a cycle ticket.
            constexpr std::string_view tierB = R"lua(
                local ok, err = ctx:try(function()
                    ctx:cycle(function() ctx:cycle_match(nil, nil, 0, 0, 1, 1) end)
                end)
                if ok ~= false then return 0 end
                if type(err) ~= 'userdata' then return 0 end
                if err.kind ~= uf.errors.invalid_resource then return 0 end
                return 1
            )lua";
            CHECK(runBound(context, framework.built, tierB) == doctest::Approx(1.0));
            CHECK_FALSE(context.hasOpenCycle());

            // The control that keeps the three closed-ness checks from passing
            // vacuously: each run really did open a cycle to close.
            CHECK(framework.frames->captureCount() == 3U);
            CHECK(framework.built.clicks->clickCount() == 0);
        }

        TEST_CASE("A nested ctx:cycle fails in Luau, not at the host's invariant")
        {
            auto framework = buildOver(resolvingFrames(FrameId{61}));
            REQUIRE(framework.built.session.has_value());
            TaskContext context{
                *std::move(framework.built.session),
                *framework.built.recorder,
            };

            // The framework refuses the second open itself, raising a STRING an
            // author can act on. The control below reaches the host's
            // InternalInvariant through the primitive-level surface the
            // framework's bookkeeping cannot see, and that one is a Tier B
            // userdata carrier -- two different types, so this case cannot be
            // passing because the C++ backstop produced the failure.
            constexpr std::string_view source = R"lua(
                local ok, err = pcall(function()
                    ctx:cycle(function()
                        ctx:cycle(function() end)
                    end)
                end)
                if ok then return 0 end
                if type(err) ~= 'string' then return 0 end
                local sentence =
                    'cannot open an observation cycle while one is open'
                if string.find(err, sentence, 1, true) == nil then return 0 end

                local ticket = ctx:cycle_open()
                local hostOk, hostErr = pcall(function()
                    return ctx:cycle_open()
                end)
                ctx:cycle_close(ticket)
                if hostOk then return 0 end
                if type(hostErr) ~= 'userdata' then return 0 end
                if hostErr.kind ~= uf.errors.internal_invariant then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, framework.built, source) == doctest::Approx(1.0));
            CHECK_FALSE(context.hasOpenCycle());

            // Two captures: the outer ctx:cycle and the control's bare open.
            // Neither refusal spent a frame -- the framework refuses before it
            // calls cycle_open at all, and the host before it observes.
            CHECK(framework.frames->captureCount() == 2U);
        }

        TEST_CASE("ctx:step nests strictly and leaves no step open behind a raise")
        {
            auto framework = buildOver(unknownFrames(1, FrameId{70}));
            REQUIRE(framework.built.session.has_value());
            TaskContext context{
                *std::move(framework.built.session),
                *framework.built.recorder,
            };

            // The open-step path is what makes well-nestedness observable, and
            // reading it while nested is the control: an always-empty path would
            // pass the "closed afterwards" checks for free.
            constexpr std::string_view source = R"lua(
                local function path()
                    return table.concat(ctx:step_path(), '/')
                end

                if path() ~= '' then return 0 end

                local inside = ''
                local outside = ''
                ctx:step('outer', function()
                    outside = path()
                    ctx:step('inner', function()
                        inside = path()
                    end)
                    if path() ~= 'outer' then error('inner leaked', 0) end
                end)

                if outside ~= 'outer' then return 0 end
                if inside ~= 'outer/inner' then return 0 end
                if path() ~= '' then return 0 end

                -- A step whose body raises, with the error swallowed above it,
                -- still closes: an unclosed step is what the host's coming
                -- run.finished check refuses, and it must not be reachable from
                -- ordinary script failure.
                -- A raw pcall, not ctx:try: a plain Luau error is the script's
                -- own failure, so try re-raises it rather than reporting it.
                local ok, err = pcall(function()
                    ctx:step('raises', function() error('boom', 0) end)
                end)
                if ok or err ~= 'boom' then return 0 end
                if path() ~= '' then return 0 end

                -- The same for a Tier B raise from a primitive.
                local caught = ctx:try(function()
                    ctx:step('tier_b', function()
                        ctx:cycle_match(nil, nil, 0, 0, 1, 1)
                    end)
                end)
                if caught ~= false then return 0 end
                if path() ~= '' then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, framework.built, source) == doctest::Approx(1.0));
        }

        TEST_CASE("A cancel inside an observation loop ends the run and no retry recovers it")
        {
            auto stop = std::stop_source{};
            auto frameSource = std::make_unique<StopOnFirstCaptureFrameSource>(
                grayFrame(fixtureFingerprint(), unknownPixels(), FrameId{71}),
                stop
            );
            auto* const p_frames = frameSource.get();

            auto built = buildBindingWith(
                std::move(frameSource),
                stop.get_token(),
                std::make_unique<DiscardingTraceSink>()
            );
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.cancellation = stop.get_token()},
            };

            // The stop is requested from inside the first capture, so it lands
            // while the script's own observation loop is running. mark() is the
            // discriminator: it is reached only if the script kept executing
            // past the cancel, and five retry attempts is the shape that would.
            constexpr std::string_view source = R"lua(
                local swallowed = pcall(function()
                    ctx:retry({ attempts = 5 }, function()
                        for _ = 1, 10 do
                            ctx:cycle(function() end)
                        end
                    end)
                end)
                mark()
                return 1
            )lua";

            auto const run = runWithMark(context, stop.get_token(), source);
            REQUIRE_FALSE(run.result.has_value());
            CHECK(
                automationErrorKind(run.result.error())
                == AutomationErrorKind::Cancelled
            );
            CHECK(run.markCount == 0U);
            CHECK(p_frames->captureCount() == 1U);
            CHECK(built.clicks->clickCount() == 0);
        }

        TEST_CASE("Swallowing the cancel inside a retry still refuses the next primitive")
        {
            auto stop = std::stop_source{};
            auto frameSource = std::make_unique<StopOnFirstCaptureFrameSource>(
                grayFrame(fixtureFingerprint(), unknownPixels(), FrameId{72}),
                stop
            );
            auto* const p_frames = frameSource.get();

            // No stop token on the ENGINE config and none on the VM interrupt:
            // the engine would happily capture again and the interrupt never
            // fires, so only the terminal latch the loop itself set can refuse
            // the primitives that follow.
            auto built = buildBindingWith(
                std::move(frameSource),
                std::stop_token{},
                std::make_unique<DiscardingTraceSink>()
            );
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.cancellation = stop.get_token()},
            };

            constexpr std::string_view source = R"lua(
                local tries = 0
                local swallowed, sentinel = pcall(function()
                    ctx:retry({ attempts = 5 }, function()
                        tries += 1
                        for _ = 1, 10 do
                            ctx:cycle(function() end)
                            -- The time verbs are what consult the run's cancel
                            -- source; the observation verbs reach the engine,
                            -- whose own token is deliberately unarmed here.
                            ctx:wait(ctx:deadline(10), 10)
                        end
                    end)
                end)
                if swallowed then return 0 end

                -- The Tier C sentinel is a plain string, so retry re-raised it
                -- instead of counting it as an attempt worth repeating.
                if type(sentinel) ~= 'string' then return 0 end
                if tries ~= 1 then return 0 end

                -- And swallowing it bought nothing: the next primitive refuses
                -- before it reaches the engine.
                if pcall(function() return ctx:cycle_open() end) then return 0 end
                local scoped = pcall(function()
                    ctx:cycle(function() end)
                end)
                if scoped then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
            CHECK(context.fatal());

            // One capture, the one the cancel arrived during. Every refusal
            // after it stopped at the guard, so none of them cost a frame.
            CHECK(p_frames->captureCount() == 1U);
            CHECK(built.clicks->clickCount() == 0);
        }
    }
}
