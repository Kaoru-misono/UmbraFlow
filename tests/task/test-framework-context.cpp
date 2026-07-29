#include "binding-fixture.hpp"

#include <task/capability-surface.hpp>
#include <task/task-context.hpp>

#include <annotation/content-hash.hpp>
#include <annotation/recognition.hpp>
#include <annotation/recognition-runtime.hpp>
#include <annotation/resource.hpp>
#include <annotation/runtime-manifest.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>

#include <engine/ports.hpp>
#include <engine/runtime-loader.hpp>
#include <engine/session.hpp>

#include <trace/event.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <string_view>
#include <utility>
#include <vector>

// The trusted Luau framework's policy layer, driven through a real task VM:
// ctx:step, ctx:cycle, ctx:wait_for_page, ctx:retry and the interrupt registry.
//
// Every case is driven from fake ports and asserts only on counts the host can
// see -- how many frames were served, how many clicks were delivered, whether a
// cycle is still open -- so a green run finishes in milliseconds and nothing
// here depends on how loaded the machine is. The two cancellation cases arm
// their stop from inside the frame source rather than from a timer thread, so
// even "the stop landed mid-wait" is a fixed point in the sequence rather than
// a race.
namespace uf::task
{
    namespace
    {
        // Two more recognizers and one more page than singlePageRuntime, so a
        // popup is a page in its own right and a target page exists for the wait
        // to be waiting FOR. Distinct grey levels with an exact-match threshold
        // keep the two pages mutually exclusive: a frame resolves one of them or
        // neither, never both.
        constexpr auto k_popupAnchorId      = "00000000-0000-0000-0000-000000000012";
        constexpr auto k_closeDialogId      = "00000000-0000-0000-0000-000000000014";
        constexpr auto k_popupPageId        = "00000000-0000-0000-0000-000000000112";

        constexpr auto k_targetAnchorGray = uint8{2};
        constexpr auto k_targetActionGray = uint8{5};
        constexpr auto k_popupAnchorGray  = uint8{20};
        constexpr auto k_popupCloseGray   = uint8{40};

        [[nodiscard]]
        auto interruptRuntime() -> RuntimeParts
        {
            auto const fingerprint  = anno::test::fingerprint(3, 1, 96, 96);
            auto const targetAnchor = anno::test::recognizerId(k_anchorId);
            auto const targetAction = anno::test::recognizerId(k_actionId);
            auto const popupAnchor  = anno::test::recognizerId(k_popupAnchorId);
            auto const popupClose   = anno::test::recognizerId(k_closeDialogId);
            auto const targetPage   = anno::test::pageId(k_pageId);
            auto const popupPage    = anno::test::pageId(k_popupPageId);

            auto targetAnchorTemplate = encodedTemplate(k_targetAnchorGray);
            auto targetActionTemplate = encodedTemplate(k_targetActionGray);
            auto popupAnchorTemplate  = encodedTemplate(k_popupAnchorGray);
            auto popupCloseTemplate   = encodedTemplate(k_popupCloseGray);

            auto const sourceBytes = std::array{asByte(42)};
            auto const sourceHash  = anno::sha256(sourceBytes);
            REQUIRE(sourceHash.has_value());

            auto const anchorSpec = [&](anno::RecognizerId id,
                                        std::string_view name,
                                        anno::ContentHash templateHash)
            {
                return anno::RuntimeRecognizerSpec{
                    .definition = anno::test::recognizer(
                        fingerprint,
                        id,
                        std::string{name},
                        anno::AnnotationType::PageAnchor,
                        anno::test::pixelRect(0, 0, 1, 1),
                        anno::test::pixelRect(0, 0, 3, 1),
                        {},
                        std::nullopt,
                        anno::test::threshold(10'000)
                    ),
                    .templateHash = templateHash,
                    .sourceHash   = *sourceHash,
                };
            };
            auto const actionSpec = [&](anno::RecognizerId id,
                                        std::string_view name,
                                        anno::PageId page,
                                        anno::ContentHash templateHash)
            {
                return anno::RuntimeRecognizerSpec{
                    .definition = anno::test::recognizer(
                        fingerprint,
                        id,
                        std::string{name},
                        anno::AnnotationType::ActionTarget,
                        anno::test::pixelRect(0, 0, 1, 1),
                        anno::test::pixelRect(0, 0, 3, 1),
                        {page},
                        std::nullopt,
                        anno::test::threshold(10'000)
                    ),
                    .templateHash = templateHash,
                    .sourceHash   = *sourceHash,
                };
            };

            auto manifest = anno::RuntimeManifest::create(
                anno::test::projectId("personal.framework_context"),
                fingerprint,
                {
                    anchorSpec(targetAnchor, "anchor_a", targetAnchorTemplate.hash),
                    actionSpec(
                        targetAction,
                        "action_target",
                        targetPage,
                        targetActionTemplate.hash
                    ),
                    anchorSpec(popupAnchor, "anchor_popup", popupAnchorTemplate.hash),
                    actionSpec(
                        popupClose,
                        "close_dialog",
                        popupPage,
                        popupCloseTemplate.hash
                    ),
                },
                {
                    anno::test::page(targetPage, "page_a", {targetAnchor}),
                    anno::test::page(popupPage, "popup", {popupAnchor}),
                }
            );
            REQUIRE(manifest.has_value());

            auto templates = std::vector<anno::EncodedRuntimeTemplate>{};
            templates.emplace_back(std::move(targetAnchorTemplate));
            templates.emplace_back(std::move(targetActionTemplate));
            templates.emplace_back(std::move(popupAnchorTemplate));
            templates.emplace_back(std::move(popupCloseTemplate));

            auto runtime = anno::RecognitionRuntime::create(
                *std::move(manifest),
                std::move(templates)
            );
            REQUIRE(runtime.has_value());
            return RuntimeParts{
                .loaded      = engine::LoadedRuntime{.runtime = *std::move(runtime)},
                .fingerprint = fingerprint,
            };
        }

        [[nodiscard]]
        auto popupPixels() -> std::vector<std::byte>
        {
            return std::vector<std::byte>{
                asByte(k_popupAnchorGray),
                asByte(k_popupCloseGray),
                asByte(0),
            };
        }

        // A build over one of the two runtimes, with observing pointers to the
        // frame source and the click sink. The frame count is what several cases
        // below assert against: a refusal that still spent a capture would be a
        // weaker guarantee than the one under test.
        struct FrameworkBuild final
        {
            Built               built;
            FakeFrameSource*    frames{};
            RecordingTraceSink* traces{};
        };

        // Every case records its trace rather than discarding it. Most read only
        // the counts below, but the framework's semantic events pass the host's
        // validation state machine on their way to this sink, so a run that
        // completes here is also a run whose step, retry and interrupt sequence
        // the host accepted -- and the interrupt case reads the sink directly.
        [[nodiscard]]
        auto buildOver(RuntimeParts parts, std::vector<Frame> frames)
            -> FrameworkBuild
        {
            auto frameSource     = std::make_unique<FakeFrameSource>(std::move(frames));
            auto* const p_frames = frameSource.get();
            auto traceSink       = std::make_unique<RecordingTraceSink>();
            auto* const p_traces = traceSink.get();
            auto built           = buildBindingOver(
                std::move(parts),
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
            auto const fingerprint = anno::test::fingerprint(3, 1, 96, 96);
            auto frames            = std::vector<Frame>{};
            for (std::size_t index = 0; index < count; ++index)
            {
                frames.emplace_back(grayFrame(fingerprint, unknownPixels(), frameId));
            }
            return frames;
        }

        // Requests the run's stop from inside the first capture, so a
        // cancellation that lands WHILE the framework's wait loop is running is
        // a fixed point of the sequence: exactly one frame was served when the
        // stop arrived, and every later capture would be a capture the terminal
        // guard failed to prevent.
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
            auto framework = buildOver(singlePageRuntime(), resolvingFrames(FrameId{60}));
            REQUIRE(framework.built.session.has_value());
            TaskContext context{
                *std::move(framework.built.session),
                *framework.built.recorder,
            };

            // Three exits, asserted on the host side rather than by asking Lua:
            // hasOpenCycle() is the ledger's own answer, and the ledger is what
            // holds the 8 MiB frame.
            constexpr std::string_view normal = R"lua(
                return ctx:cycle(function(cycle)
                    local page = cycle:page()
                    if page == nil or not page:is(uf.pages.page_a) then return 0 end
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

            // A Tier B raise from a primitive INSIDE the block: a find handed
            // something that is not a recognizer.
            constexpr std::string_view tierB = R"lua(
                local ok, err = ctx:try(function()
                    ctx:cycle(function(cycle) cycle:find(nil) end)
                end)
                if ok ~= false then return 0 end
                if type(err) ~= 'userdata' then return 0 end
                if err.kind ~= uf.errors.invalid_resource then return 0 end
                return 1
            )lua";
            CHECK(runBound(context, framework.built, tierB) == doctest::Approx(1.0));
            CHECK_FALSE(context.hasOpenCycle());

            // The control that keeps the three closed-ness checks from passing
            // vacuously: each run really did open a cycle, so each really had
            // one to close.
            CHECK(framework.frames->captureCount() == 3U);
            CHECK(framework.built.clicks->clickCount() == 0);
        }

        TEST_CASE("A nested ctx:cycle fails in Luau, not at the host's invariant")
        {
            auto framework = buildOver(singlePageRuntime(), resolvingFrames(FrameId{61}));
            REQUIRE(framework.built.session.has_value());
            TaskContext context{
                *std::move(framework.built.session),
                *framework.built.recorder,
            };

            // The framework refuses the second open itself, with a sentence an
            // author can act on, and the value it raises is a STRING. The
            // control below reaches the host's InternalInvariant deliberately,
            // through the primitive-level surface the framework's bookkeeping
            // cannot see, and that one is a Tier B userdata carrier. The two are
            // different values of different types, so this case cannot be
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
            // Neither refusal spent a frame -- the framework's refusal happens
            // before it calls cycle_open at all, and the host's happens before
            // it observes.
            CHECK(framework.frames->captureCount() == 2U);
        }

        TEST_CASE("ctx:wait_for_page resolves on a later frame and runs its block there")
        {
            auto frames = unknownFrames(2, FrameId{62});
            frames.emplace_back(
                grayFrame(
                    anno::test::fingerprint(3, 1, 96, 96),
                    resolvingPixels(),
                    FrameId{63}
                )
            );
            auto framework = buildOver(singlePageRuntime(), std::move(frames));
            REQUIRE(framework.built.session.has_value());
            TaskContext context{
                *std::move(framework.built.session),
                *framework.built.recorder,
            };

            constexpr std::string_view source = R"lua(
                local ran = 0
                task.define {
                    run = function(ctx)
                        ctx:step('daily', function()
                            ctx:wait_for_page(
                                uf.pages.page_a,
                                { timeout_ms = 60000, poll_ms = 0 },
                                function(home)
                                    local hit = home:find(uf.recognizers.action_target)
                                    if hit == nil then return end
                                    home:click(hit)
                                    ran = 1
                                end
                            )
                        end)
                    end,
                }
                return ran
            )lua";

            CHECK(runBound(context, framework.built, source) == doctest::Approx(1.0));
            CHECK(framework.built.clicks->clickCount() == 1);
            CHECK(framework.frames->captureCount() == 3U);
            CHECK_FALSE(context.hasOpenCycle());
        }

        TEST_CASE("ctx:wait_for_page times out as a Tier B error the host minted")
        {
            auto framework = buildOver(singlePageRuntime(), unknownFrames(1, FrameId{64}));
            REQUIRE(framework.built.session.has_value());
            TaskContext context{
                *std::move(framework.built.session),
                *framework.built.recorder,
            };

            // A zero budget takes exactly one turn: the loop observes once, the
            // deadline is already spent, and the timeout is raised. It is a real
            // automation error rather than a Luau string -- host-minted
            // userdata, the domain's own `timeout` spelling, retryable false
            // (Timeout's FailureResponse is Abort) -- which is what lets a
            // project catch it and a retry policy name it.
            constexpr std::string_view source = R"lua(
                local ok, err = ctx:try(function()
                    ctx:wait_for_page(
                        uf.pages.page_a,
                        { timeout_ms = 0 },
                        function() end
                    )
                end)
                if ok ~= false then return 0 end
                if type(err) ~= 'userdata' then return 0 end
                if err.kind ~= uf.errors.timeout then return 0 end
                if err.retryable ~= false then return 0 end
                if getmetatable(err) ~= 'uf.error' then return 0 end

                -- A timed-out wait left nothing open, so a bare open still works.
                local ticket = ctx:cycle_open()
                ctx:cycle_close(ticket)
                return 1
            )lua";

            CHECK(runBound(context, framework.built, source) == doctest::Approx(1.0));
            CHECK(framework.frames->captureCount() == 2U);
            CHECK_FALSE(context.hasOpenCycle());
        }

        TEST_CASE("An interrupt handles a popup during a wait and the wait continues")
        {
            // The capability the whole three-layer design exists for. The popup
            // appears on the first frame, in the middle of a wait for another
            // page; its handler dismisses it; the wait then keeps polling and
            // finds its target two frames later.
            auto const fingerprint = anno::test::fingerprint(3, 1, 96, 96);
            auto frames            = std::vector<Frame>{};
            frames.emplace_back(grayFrame(fingerprint, popupPixels(), FrameId{65}));
            frames.emplace_back(grayFrame(fingerprint, unknownPixels(), FrameId{66}));
            frames.emplace_back(grayFrame(fingerprint, resolvingPixels(), FrameId{67}));

            auto framework = buildOver(interruptRuntime(), std::move(frames));
            REQUIRE(framework.built.session.has_value());
            TaskContext context{
                *std::move(framework.built.session),
                *framework.built.recorder,
            };

            constexpr std::string_view source = R"lua(
                local handled = 0
                local clicked = 0

                local popup = task.interrupt {
                    id = 'popup',
                    when = uf.pages.popup,
                    max_hits = 3,
                    handle = function(ctx, cycle)
                        handled += 1
                        local close = cycle:find(uf.recognizers.close_dialog)
                        if close ~= nil then
                            cycle:click(close)
                        end
                    end,
                }

                task.define {
                    interrupts = { popup },
                    run = function(ctx)
                        ctx:wait_for_page(
                            uf.pages.page_a,
                            { timeout_ms = 60000, poll_ms = 0 },
                            function(home)
                                local hit = home:find(uf.recognizers.action_target)
                                if hit ~= nil then
                                    home:click(hit)
                                    clicked += 1
                                end
                            end
                        )
                    end,
                }

                if handled ~= 1 then return 0 end
                if clicked ~= 1 then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, framework.built, source) == doctest::Approx(1.0));

            // Two clicks: the popup's close button and the target's own. One
            // alone would mean either the popup was never handled or the wait
            // never continued past it.
            CHECK(framework.built.clicks->clickCount() == 2);
            CHECK(framework.frames->captureCount() == 3U);
            CHECK_FALSE(context.hasOpenCycle());

            // And it is in the record as a match that was handled. The pair is
            // what the host's state machine checks -- a close must follow a match
            // of the same id -- so a framework that emitted one without the other
            // would have failed this run rather than reaching these lines.
            CHECK(
                semanticKinds(framework.traces->events())
                == std::vector<trace::TraceEventKind>{
                    trace::TraceEventKind::FrameworkInterruptMatched,
                    trace::TraceEventKind::FrameworkInterruptHandled,
                }
            );
        }

        TEST_CASE("An interrupt stops firing once its hit budget is spent")
        {
            // A popup whose handler never actually dismisses it. Without a hit
            // budget it would be re-handled on every poll for the whole wait;
            // max_hits is what turns "this handler is not working" into a bounded
            // number of attempts and then a timeout.
            auto const fingerprint = anno::test::fingerprint(3, 1, 96, 96);
            auto frames            = std::vector<Frame>{};
            frames.emplace_back(grayFrame(fingerprint, popupPixels(), FrameId{68}));

            auto framework = buildOver(interruptRuntime(), std::move(frames));
            REQUIRE(framework.built.session.has_value());
            TaskContext context{
                *std::move(framework.built.session),
                *framework.built.recorder,
            };

            constexpr std::string_view source = R"lua(
                local handled = 0

                local popup = task.interrupt {
                    id = 'popup',
                    when = uf.pages.popup,
                    max_hits = 2,
                    handle = function() handled += 1 end,
                }

                local timedOut = 0
                task.define {
                    interrupts = { popup },
                    run = function(ctx)
                        local ok, err = ctx:try(function()
                            ctx:wait_for_page(
                                uf.pages.page_a,
                                { timeout_ms = 200, poll_ms = 0 },
                                function() end
                            )
                        end)
                        if ok == false and err.kind == uf.errors.timeout then
                            timedOut = 1
                        end
                    end,
                }

                if timedOut ~= 1 then return 0 end
                if handled ~= 2 then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, framework.built, source) == doctest::Approx(1.0));
            CHECK(framework.built.clicks->clickCount() == 0);
            CHECK_FALSE(context.hasOpenCycle());

            // A popup that keeps matching after its budget is spent is recorded
            // as matched-and-exhausted rather than silently skipped, which is the
            // whole difference between "the handler is not working" and "the
            // popup went away".
            auto const kinds = semanticKinds(framework.traces->events());
            REQUIRE(kinds.size() >= 3U);
            CHECK(kinds[0] == trace::TraceEventKind::FrameworkInterruptMatched);
            CHECK(kinds[1] == trace::TraceEventKind::FrameworkInterruptHandled);
            CHECK(kinds.back() == trace::TraceEventKind::FrameworkInterruptExhausted);
        }

        TEST_CASE("ctx:retry follows the on list over retryable, and retryable without one")
        {
            auto framework = buildOver(singlePageRuntime(), unknownFrames(1, FrameId{69}));
            REQUIRE(framework.built.session.has_value());
            TaskContext context{
                *std::move(framework.built.session),
                *framework.built.recorder,
            };

            // Four directions over two kinds whose retryable flags differ:
            // `timeout` is Abort and therefore retryable = false, while
            // `stale_observation` is Retry and therefore true.
            //
            //   1. `on` names timeout            -> retried, though not retryable
            //   2. no `on`, timeout              -> not retried, retryable rules
            //   3. no `on`, stale_observation    -> retried, retryable rules
            //   4. `on` names timeout only, and
            //      the failure is retryable      -> NOT retried
            //
            // The fourth is what pins the semantics: with `on` present,
            // `retryable` is not consulted at all.
            constexpr std::string_view source = R"lua(
                local function timeoutOnce()
                    ctx:wait_for_page(
                        uf.pages.page_a,
                        { timeout_ms = 0 },
                        function() end
                    )
                end

                local function staleOnce()
                    local ticket = ctx:cycle_open()
                    ctx:cycle_close(ticket)
                    ctx:cycle_page(ticket)
                end

                local function count(policy, body)
                    local tries = 0
                    local ok, err = ctx:try(function()
                        ctx:retry(policy, function()
                            tries += 1
                            body()
                        end)
                    end)
                    if ok ~= false then return -1 end
                    return tries
                end

                local overridden =
                    count({ attempts = 3, on = { uf.errors.timeout } }, timeoutOnce)
                if overridden ~= 3 then return 0 end

                local defaulted = count({ attempts = 3 }, timeoutOnce)
                if defaulted ~= 1 then return 0 end

                local retryable = count({ attempts = 2 }, staleOnce)
                if retryable ~= 2 then return 0 end

                local excluded =
                    count({ attempts = 3, on = { uf.errors.timeout } }, staleOnce)
                if excluded ~= 1 then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, framework.built, source) == doctest::Approx(1.0));
            CHECK(framework.built.clicks->clickCount() == 0);
            CHECK_FALSE(context.hasOpenCycle());
        }

        TEST_CASE("ctx:step nests strictly and leaves no step open behind a raise")
        {
            auto framework = buildOver(singlePageRuntime(), unknownFrames(1, FrameId{70}));
            REQUIRE(framework.built.session.has_value());
            TaskContext context{
                *std::move(framework.built.session),
                *framework.built.recorder,
            };

            // The open-step path is what makes well-nestedness observable at all,
            // and observing it while nested is the control: a path that were
            // always empty would pass the "closed afterwards" checks for free.
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
                    ctx:step('tier_b', function() ctx:cycle_page(nil) end)
                end)
                if caught ~= false then return 0 end
                if path() ~= '' then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, framework.built, source) == doctest::Approx(1.0));
        }

        TEST_CASE("A cancel inside the wait loop ends the run and no retry recovers it")
        {
            auto stop = std::stop_source{};
            auto frameSource = std::make_unique<StopOnFirstCaptureFrameSource>(
                grayFrame(
                    anno::test::fingerprint(3, 1, 96, 96),
                    unknownPixels(),
                    FrameId{71}
                ),
                stop
            );
            auto* const p_frames = frameSource.get();

            auto built = buildBindingOver(
                singlePageRuntime(),
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
            // while the framework's wait loop is running. mark() is the
            // discriminator: it is reached only if the script kept executing
            // past the cancel, and a retry policy with five attempts is exactly
            // the shape that would try to.
            constexpr std::string_view source = R"lua(
                local swallowed = pcall(function()
                    ctx:retry({ attempts = 5 }, function()
                        ctx:wait_for_page(
                            uf.pages.page_a,
                            { timeout_ms = 60000, poll_ms = 10 },
                            function() end
                        )
                    end)
                end)
                mark()
                return 1
            )lua";

            auto const run = runWithMark(context, built, stop.get_token(), source);
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
                grayFrame(
                    anno::test::fingerprint(3, 1, 96, 96),
                    unknownPixels(),
                    FrameId{72}
                ),
                stop
            );
            auto* const p_frames = frameSource.get();

            // No stop token on the ENGINE config and none on the VM interrupt,
            // deliberately: the engine would happily capture again and the
            // interrupt never fires, so the only thing that can refuse the
            // primitives after the cancelled wait is the terminal latch the
            // wait itself set.
            auto built = buildBindingOver(
                singlePageRuntime(),
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
                        ctx:wait_for_page(
                            uf.pages.page_a,
                            { timeout_ms = 60000, poll_ms = 10 },
                            function() end
                        )
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
                local waited = pcall(function()
                    ctx:wait_for_page(uf.pages.page_a, nil, function() end)
                end)
                if waited then return 0 end
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
