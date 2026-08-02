#pragma once

#include "../domain/test-helpers.hpp"

#include <task/framework-bundle.hpp>
#include <task/script-bindings.hpp>
#include <task/task-context.hpp>

#include <core/error/result.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/key.hpp>
#include <domain/space.hpp>

#include <engine/ports.hpp>
#include <engine/session.hpp>

#include <image/png.hpp>

#include <script/engine.hpp>
#include <script/testing/cancel-probe.hpp>

#include <trace/event.hpp>
#include <trace/recorder.hpp>
#include <trace/sink.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Shared test fixture for every modules/task suite that boots a real task VM:
// the fixture geometry, the grey frame builders, the template blobs, the fake
// frame sources, the recording sinks, and the bound-VM runners.
// test-task-binding.cpp, test-adversarial-surface.cpp,
// test-determinism-harness.cpp, test-framework-context.cpp,
// test-framework-surface.cpp and test-veto-blocking.cpp all build sessions from
// these, so they live here once rather than being copied into each translation
// unit. Everything is inline or a header-defined type, so including it in more
// than one TU is safe.
//
// IT BUILDS NO RECOGNITION RUNTIME ANY MORE. It used to compile a one-page and a
// two-page annotation catalog so a session could resolve a page and find an
// element; both verbs retired with the C++ page model
// (docs/plans/2026-07-31-script-owned-page-model.md 9), and an engine session now
// takes a fingerprint and nothing else. What is left of the old fixture is the
// part that was always about pixels: a three-by-one grey frame, and the
// one-by-one grey template blobs a script matches against it.
namespace uf::task
{
    // The grey levels the fixture frames are painted with. They are distinct, so
    // a template cut from one scores zero where it was painted and nonzero
    // everywhere else -- which is what lets a case say "this template is on this
    // frame" and "this one is not" without a page model.
    inline constexpr auto k_targetAnchorGray = uint8{2};
    inline constexpr auto k_targetActionGray = uint8{5};

    [[nodiscard]]
    constexpr auto asByte(uint8 value) noexcept -> std::byte
    {
        return static_cast<std::byte>(value);
    }

    // One template PNG beside the content hash that addresses it on disk, which
    // is the pair `assets/templates/<hex>.png` is named from.
    struct FixtureTemplate final
    {
        ContentHash            hash;
        std::vector<std::byte> pngBytes{};
    };

    // A one-by-one grey RGBA template addressed by its content hash, encoded
    // exactly as an authored template is, so a script that loads it searches for
    // the same pixels the real loop would.
    [[nodiscard]]
    inline auto encodedTemplate(uint8 gray) -> FixtureTemplate
    {
        auto const rgba = std::vector<std::byte>{
            asByte(gray),
            asByte(gray),
            asByte(gray),
            asByte(255),
        };
        auto encoded = image::encodeRgbaPng("task-binding-template.png", 1, 1, rgba);
        REQUIRE(encoded.has_value());
        auto const hash = sha256(*encoded);
        REQUIRE(hash.has_value());
        return FixtureTemplate{
            .hash     = *hash,
            .pngBytes = *std::move(encoded),
        };
    }

    // The geometry every fixture frame is captured at, and therefore the
    // fingerprint every fixture session is configured with on both sides. Three
    // by one is the smallest frame a one-by-one template can be searched in at
    // more than one position.
    [[nodiscard]]
    inline auto fixtureFingerprint() -> ProjectFingerprint
    {
        auto const fingerprint = ProjectFingerprint::create(3, 1, 96, 96);
        REQUIRE(fingerprint.has_value());
        return *fingerprint;
    }

    [[nodiscard]]
    inline auto grayFrame(
        ProjectFingerprint fingerprint,
        std::vector<std::byte> pixels,
        FrameId frameId
    ) -> Frame
    {
        auto const transform = CoordinateTransform::create(
            Point<DesktopSpace>{0.0F, 0.0F},
            static_cast<float>(fingerprint.width()),
            static_cast<float>(fingerprint.height()),
            fingerprint.width(),
            fingerprint.height()
        );
        REQUIRE(transform.has_value());
        auto const width = checkedCast<std::size_t>(fingerprint.width());
        REQUIRE(width.has_value());
        auto const stride = checkedMultiply(
            width.value_or(std::size_t{0}),
            bytesPerPixel(PixelFormat::Gray8)
        );
        REQUIRE(stride.has_value());
        auto const buffer = std::shared_ptr<FrameBuffer const>{
            std::make_shared<FrameBuffer>(std::move(pixels))
        };
        auto frame = Frame::create(
            frameId,
            CaptureSessionId{7},
            TargetGeneration::fromValue(3),
            MonotonicInstant::now(),
            fingerprint.width(),
            fingerprint.height(),
            stride.value_or(std::size_t{0}),
            PixelFormat::Gray8,
            buffer,
            *transform
        );
        REQUIRE(frame.has_value());
        return *std::move(frame);
    }

    [[nodiscard]]
    inline auto resolvingPixels() -> std::vector<std::byte>
    {
        return std::vector<std::byte>{
            asByte(k_targetAnchorGray),
            asByte(k_targetActionGray),
            asByte(0),
        };
    }

    // A frame carrying the anchor grey and nothing else, so a search for the
    // action template COMPLETES and matches nothing.
    [[nodiscard]]
    inline auto resolvedTargetlessPixels() -> std::vector<std::byte>
    {
        return std::vector<std::byte>{
            asByte(k_targetAnchorGray),
            asByte(0),
            asByte(0),
        };
    }

    [[nodiscard]]
    inline auto unknownPixels() -> std::vector<std::byte>
    {
        return std::vector<std::byte>{asByte(0), asByte(0), asByte(0)};
    }

    // Replays a fixed sequence of frames, repeating the last once exhausted.
    //
    // It returns without blocking, which is how it honours the capture budget:
    // a source that never waits cannot outlive a deadline and has no wait for a
    // stop to interrupt. Every fake below that ignores its budget does so for
    // this reason; the one that does not is DeadlineHonouringFrameSource in
    // tests/engine/test-session.cpp, which is where the budget's own contract is
    // exercised.
    class FakeFrameSource final : public engine::IFrameSource
    {
        std::vector<Frame> m_frames;
        std::size_t        m_index{0};

    public:
        explicit FakeFrameSource(std::vector<Frame> frames) noexcept
            : m_frames{std::move(frames)}
        {
        }

        [[nodiscard]]
        auto capture(CaptureBudget const& /*budget*/) -> Result<Frame> override
        {
            if (m_frames.empty())
            {
                return fail(
                    AutomationErrorKind::CaptureUnavailable,
                    "fake frame source has no frames to replay"
                );
            }
            auto const index = std::min(m_index, m_frames.size() - 1U);
            ++m_index;
            return m_frames[index];
        }

        [[nodiscard]] auto validateTargetInstance() -> Status override
        {
            return ok();
        }

        // How many captures this source served. A test that must prove the host
        // refused an operation BEFORE observing reads it: a refusal that still
        // spent a frame is a different, weaker guarantee.
        [[nodiscard]] auto captureCount() const noexcept -> std::size_t
        {
            return m_index;
        }
    };

    // Counts delivered clicks so fail-closed cases can assert none escaped.
    class CountingActionSink final : public engine::IActionSink
    {
    public:
        // One long press this sink was asked for. Both halves are recorded
        // because a case proving the duration reached the port has to read the
        // duration, and a point alone would pass just as well against a sink that
        // dropped it.
        struct LongPressDelivery final
        {
            Point<ClientSpace>         point;
            MonotonicInstant::Duration hold{};
        };

    private:
        uint32               m_clickCount{0};
        std::vector<KeyName> m_keys{};
        std::vector<int32>   m_scrolls{};

        std::vector<LongPressDelivery> m_longPresses{};

    public:
        [[nodiscard]]
        auto click(
            Point<ClientSpace> /*point*/,
            ObservationLease const& /*lease*/
        ) -> Status override
        {
            ++m_clickCount;
            return ok();
        }

        [[nodiscard]]
        auto pressKey(
            KeyName key,
            TargetGeneration /*actionGeneration*/
        ) -> Status override
        {
            m_keys.emplace_back(key);
            return ok();
        }

        [[nodiscard]]
        auto scroll(
            int32 notches,
            ObservationLease const& /*lease*/
        ) -> Status override
        {
            m_scrolls.emplace_back(notches);
            return ok();
        }

        [[nodiscard]]
        auto longPress(
            Point<ClientSpace> point,
            MonotonicInstant::Duration hold,
            ObservationLease const& /*lease*/
        ) -> Status override
        {
            m_longPresses.emplace_back(
                LongPressDelivery{
                    .point = point,
                    .hold  = hold,
                }
            );
            return ok();
        }

        [[nodiscard]] auto clickCount() const noexcept -> uint32
        {
            return m_clickCount;
        }

        // The keys this sink was asked to deliver, in order. A test that must prove
        // a keystroke was refused reads it: a refusal that still posted input would
        // be a different, weaker guarantee.
        [[nodiscard]] auto keys() const noexcept UF_LIFETIME_BOUND
            -> std::vector<KeyName> const&
        {
            return m_keys;
        }

        // The detent counts this sink was asked to deliver, in order. A refusal
        // that still posted a wheel message would be a weaker guarantee than the
        // one under test, which is why the cases read the deliveries rather than
        // only the returned error.
        [[nodiscard]] auto scrolls() const noexcept UF_LIFETIME_BOUND
            -> std::vector<int32> const&
        {
            return m_scrolls;
        }

        // The long presses this sink was asked to deliver, in order, with the
        // hold each one named.
        [[nodiscard]] auto longPresses() const noexcept UF_LIFETIME_BOUND
            -> std::vector<LongPressDelivery> const&
        {
            return m_longPresses;
        }
    };

    // Records every delivered click point in order, so a determinism run can read
    // back the exact coordinate sequence a script produced and fold it into the
    // canonical action trace.
    class RecordingActionSink final : public engine::IActionSink
    {
        std::vector<Point<ClientSpace>> m_points{};

    public:
        [[nodiscard]]
        auto click(
            Point<ClientSpace> point,
            ObservationLease const& /*lease*/
        ) -> Status override
        {
            m_points.emplace_back(point);
            return ok();
        }

        [[nodiscard]]
        auto pressKey(
            KeyName /*key*/,
            TargetGeneration /*actionGeneration*/
        ) -> Status override
        {
            return ok();
        }

        [[nodiscard]]
        auto scroll(
            int32 /*notches*/,
            ObservationLease const& /*lease*/
        ) -> Status override
        {
            return ok();
        }

        // Recorded into the same point list a click is: a determinism run's
        // canonical action trace is the sequence of coordinates a script aimed
        // at, and a long press aims at one exactly as a click does.
        [[nodiscard]]
        auto longPress(
            Point<ClientSpace> point,
            MonotonicInstant::Duration /*hold*/,
            ObservationLease const& /*lease*/
        ) -> Status override
        {
            m_points.emplace_back(point);
            return ok();
        }

        [[nodiscard]] auto points() const noexcept UF_LIFETIME_BOUND
            -> std::vector<Point<ClientSpace>> const&
        {
            return m_points;
        }
    };

    class DiscardingTraceSink final : public trace::ITraceSink
    {
    public:
        [[nodiscard]]
        auto emit(trace::StampedTraceEvent const& /*event*/) -> Status override
        {
            return ok();
        }
    };

    // Records every stamped event a run emits, so a test can assert both the
    // sequence of events and the identity stamped onto each one. The sink owns
    // its buffer, and the recorder owns the sink through a unique_ptr, so an
    // observing pointer to the sink stays valid for the recorder's life.
    class RecordingTraceSink final : public trace::ITraceSink
    {
        std::vector<trace::StampedTraceEvent> m_events{};

    public:
        [[nodiscard]]
        auto emit(trace::StampedTraceEvent const& event) -> Status override
        {
            m_events.emplace_back(event);
            return ok();
        }

        [[nodiscard]]
        auto events() const noexcept UF_LIFETIME_BOUND
            -> std::vector<trace::StampedTraceEvent> const&
        {
            return m_events;
        }
    };

    // The verb of every native call a run recorded, in order. Two suites read
    // it -- the binding suite for the cycle verbs, the time-primitive suite for
    // settle -- so it lives here rather than in either.
    [[nodiscard]]
    inline auto nativeCallVerbs(std::vector<trace::StampedTraceEvent> const& events)
        -> std::vector<std::string>
    {
        auto verbs = std::vector<std::string>{};
        for (auto const& event : events)
        {
            if (event.event().nativeCall.has_value())
            {
                verbs.emplace_back(event.event().nativeCall->verb);
            }
        }
        return verbs;
    }

    // The first native call recorded for `verb`, or null when the run made
    // none. The returned pointer observes storage owned by the recording sink
    // behind `events`, which the annotation on that parameter states.
    [[nodiscard]]
    inline auto findNativeCall(
        std::vector<trace::StampedTraceEvent> const& events UF_LIFETIME_BOUND,
        std::string_view verb
    ) noexcept -> trace::TraceEvent::NativeCall const*
    {
        for (auto const& event : events)
        {
            auto const& call = event.event().nativeCall;
            if (call.has_value() && call->verb == verb)
            {
                return &*call;
            }
        }
        return nullptr;
    }

    // The run identity these fixtures stamp. The binding layer never authors it
    // -- TaskHost does -- so a fixed pair is enough to prove every event of a run
    // lands under one identity.
    inline constexpr auto k_fixtureRunId        = TaskRunId{5};
    inline constexpr auto k_fixtureGenerationId = GenerationId{1};

    // The EngineConfig a real task VM boots with, assembled in one place so no
    // test can accidentally assert against a surface shape the host does not
    // ship: the real framework bundle under the framework environment, the
    // private capability surface handed to it as a chunk argument, the uf data
    // table as a project global, and the framework's own ctx published beside it.
    //
    // `context` must outlive the Engine built from the returned config, because
    // the private surface holds its address (see task/script-bindings.hpp).
    [[nodiscard]]
    inline auto taskVmConfig(TaskContext& context) -> script::EngineConfig
    {
        return script::EngineConfig{
            .frameworkModules  = frameworkScriptModules(),
            .installHostTables = scriptHostTableInstaller(),
            .installPrivateCapabilities = scriptPrivateCapabilities(
                context,
                ScriptTrustMode::Run
            ),
            .projectGlobals          = scriptProjectGlobals(),
            .frameworkProjectGlobals = frameworkProjectGlobals(),
            .classifyRaisedError     = scriptRaisedErrorClassifier(),
        };
    }

    // The VM the agent front-end boots: the wider private surface, and the two
    // modules only an exploration environment publishes.
    //
    // It is the same assembly ExplorationSession::create performs, spelled here
    // so a test asserts against the surface the host actually ships rather than
    // one a fixture invented.
    [[nodiscard]]
    inline auto explorationVmConfig(TaskContext& context) -> script::EngineConfig
    {
        return script::EngineConfig{
            .frameworkModules  = frameworkScriptModules(),
            .installHostTables = scriptHostTableInstaller(),
            .installPrivateCapabilities = scriptPrivateCapabilities(
                context,
                ScriptTrustMode::Exploration
            ),
            .projectGlobals          = scriptProjectGlobals(),
            .frameworkProjectGlobals = explorationProjectGlobals(),
            .classifyRaisedError     = scriptRaisedErrorClassifier(),
        };
    }

    [[nodiscard]]
    inline auto baseConfig(ProjectFingerprint fingerprint)
        -> engine::EngineSessionConfig
    {
        return engine::EngineSessionConfig{
            .liveFingerprint    = fingerprint,
            .projectFingerprint = fingerprint,
            .maximumPixelComparisons = 1'000,
            .recognitionTimeout      = std::chrono::duration_cast<
                MonotonicInstant::Duration
            >(std::chrono::seconds{5}),
        };
    }

    // Fails the FIRST capture with Cancelled and serves a good frame on every
    // one after it, with no stop token armed anywhere.
    //
    // That combination is what isolates the terminal latch. In a real cancel
    // the VM interrupt also breaks the thread, so a test cannot tell which
    // layer refused the next call. Here the interrupt never fires and the
    // engine would happily capture again, so the only thing that can refuse
    // the second primitive is the fatal latch the first one set -- which is
    // exactly the guarantee that has to survive a script that swallowed the
    // Tier C sentinel and kept running.
    class CancelOnceFrameSource final : public engine::IFrameSource
    {
        Frame       m_frame;
        std::size_t m_captureCount{0};

    public:
        explicit CancelOnceFrameSource(Frame frame) noexcept
            : m_frame{std::move(frame)}
        {
        }

        [[nodiscard]]
        auto capture(CaptureBudget const& /*budget*/) -> Result<Frame> override
        {
            ++m_captureCount;
            if (m_captureCount == 1U)
            {
                return fail(AutomationErrorKind::Cancelled, "capture cancelled once");
            }
            return m_frame;
        }

        [[nodiscard]] auto validateTargetInstance() -> Status override
        {
            return ok();
        }

        // How many captures this source served. A refusal that still spent a
        // frame would be a weaker guarantee than the one under test.
        [[nodiscard]] auto captureCount() const noexcept -> std::size_t
        {
            return m_captureCount;
        }
    };

    // The run's recorder, a constructed EngineSession over it, and a non-owning
    // observer of the click sink.
    //
    // The recorder is declared first and held through a unique_ptr: the
    // session borrows it (see engine/session.hpp), so it must outlive the
    // session and keep a stable address when this struct is moved.
    struct Built final
    {
        std::unique_ptr<trace::TraceRecorder> recorder;
        Result<engine::EngineSession>         session;
        CountingActionSink*                   clicks;
    };

    // Builds a session over the fixture geometry from `frameSource` with
    // `cancellation` armed on the engine config, recording into `traceSink`. One
    // recorder serves both the engine session and the TaskContext built over it,
    // which is what puts their events into a single ordered stream.
    [[nodiscard]]
    inline auto buildBindingWith(
        std::unique_ptr<engine::IFrameSource> frameSource,
        std::stop_token cancellation,
        std::unique_ptr<trace::ITraceSink> traceSink
    ) -> Built
    {
        auto actionSink      = std::make_unique<CountingActionSink>();
        auto* const p_clicks = actionSink.get();
        auto config         = baseConfig(fixtureFingerprint());
        config.cancellation = std::move(cancellation);
        auto recorder        = std::make_unique<trace::TraceRecorder>(
            std::move(traceSink),
            k_fixtureRunId,
            k_fixtureGenerationId,
            trace::FrontEnd::Task
        );
        auto session = engine::EngineSession::create(
            std::move(frameSource),
            std::move(actionSink),
            *recorder,
            config
        );
        return Built{
            .recorder = std::move(recorder),
            .session  = std::move(session),
            .clicks   = p_clicks,
        };
    }

    [[nodiscard]]
    inline auto buildBinding(std::vector<Frame> frames) -> Built
    {
        return buildBindingWith(
            std::make_unique<FakeFrameSource>(std::move(frames)),
            std::stop_token{},
            std::make_unique<DiscardingTraceSink>()
        );
    }

    // One frame carrying both the anchor and the action grey, so a search for
    // either template matches on it.
    [[nodiscard]]
    inline auto resolvingFrames(FrameId frameId) -> std::vector<Frame>
    {
        auto frames = std::vector<Frame>{};
        frames.emplace_back(grayFrame(fixtureFingerprint(), resolvingPixels(), frameId));
        return frames;
    }

    // The PNG bytes of the one-by-one grey template a script loads through
    // `template_load` to search a fixture frame for. It goes through the same
    // encoder the fixture frames are painted from, so a match is exact.
    [[nodiscard]]
    inline auto templateBlob(uint8 gray) -> std::vector<std::byte>
    {
        return encodedTemplate(gray).pngBytes;
    }

    // Those PNG bytes as a Luau string literal, so a script can load the
    // template without a project directory on disk.
    //
    // Every byte is written as a three-digit decimal escape, PADDED. The padding
    // is not cosmetic: an unpadded escape followed by a digit byte would be read
    // as one larger number, so some blobs would silently decode to different
    // pixels and others would not.
    [[nodiscard]]
    inline auto templateLiteral(uint8 gray) -> std::string
    {
        auto const blob = templateBlob(gray);
        auto       out  = std::string{"\""};
        for (auto const byte : blob)
        {
            out += std::format("\\{:03}", static_cast<unsigned>(byte));
        }
        out += '"';
        return out;
    }

    // `body` with the fixture template's blob bound as the Luau local TEMPLATE,
    // so a script can load it with `ctx:template_load(TEMPLATE)` and search a
    // fixture frame for it. Spelled once here because almost every case that
    // matches or clicks has to load one first.
    [[nodiscard]]
    inline auto withTemplate(
        std::string_view body,
        uint8 gray = k_targetActionGray
    ) -> std::string
    {
        return "local TEMPLATE = " + templateLiteral(gray) + "\n"
            + std::string{body};
    }

    // A template larger than any region a three-by-one fixture frame can offer,
    // as a Luau string literal.
    //
    // It is what a COMPLETED search with no candidate position needs: a template
    // that fits always reports its best position and a distance, so an empty
    // answer from cycle_match means the region could hold the template nowhere
    // at all. Judging a distance is layer two's, which is why "not found" is not
    // something this layer can say.
    [[nodiscard]]
    inline auto oversizedTemplateLiteral() -> std::string
    {
        auto rgba = std::vector<std::byte>{};
        for (auto index = 0; index < 4; ++index)
        {
            rgba.emplace_back(asByte(k_targetActionGray));
            rgba.emplace_back(asByte(k_targetActionGray));
            rgba.emplace_back(asByte(k_targetActionGray));
            rgba.emplace_back(asByte(255));
        }
        auto encoded = image::encodeRgbaPng("oversized.png", 2, 2, rgba);
        REQUIRE(encoded.has_value());

        auto out = std::string{"\""};
        for (auto const byte : *encoded)
        {
            out += std::format("\\{:03}", static_cast<unsigned>(byte));
        }
        out += '"';
        return out;
    }

    // Runs `source` on a real task VM bound to `built`'s session and returns
    // the script's numeric result. The VM is created and destroyed inside
    // this call, so anything the host still holds afterwards is held by the
    // host, not by a live Lua handle.
    [[nodiscard]]
    inline auto runBound(TaskContext& context, Built& /*built*/, std::string_view source)
        -> double
    {
        auto engine = script::Engine::create(taskVmConfig(context));
        REQUIRE(engine.has_value());
        auto const result = engine->runNumber(source, "task-binding");
        REQUIRE(result.has_value());
        return *result;
    }

    // The same run, without requiring it to succeed. A test that asks how
    // the HOST classified a value the script let escape needs the failure
    // itself, which runBound deliberately refuses to hand back.
    [[nodiscard]]
    inline auto runBoundResult(
        TaskContext& context,
        Built& /*built*/,
        std::string_view source
    ) -> Result<double>
    {
        auto engine = script::Engine::create(taskVmConfig(context));
        REQUIRE(engine.has_value());
        return engine->runNumber(source, "task-binding");
    }

    // Outcome of a discriminator run: the run result (an error on a
    // cancellation) and how many times the host mark() ran.
    struct DiscriminatorRun final
    {
        Result<double> result;
        uint64         markCount{0};
    };

    // Runs `source` on a task VM bound to `surface` and `context` with
    // `cancellation` armed on the VM interrupt (the session already shares the
    // same token), plus a host mark() the script can call. Returns the run
    // result and how many times mark() reached. markCount is declared before the
    // Engine, so it outlives the VM and the closure's pointer into it stays
    // valid for every call.
    [[nodiscard]]
    inline auto runWithMark(
        TaskContext& context,
        std::stop_token cancellation,
        std::string_view source
    ) -> DiscriminatorRun
    {
        uint64 markCount        = 0;
        auto   config           = taskVmConfig(context);
        auto   surfaceInstaller = std::move(config.installHostTables);
        config.cancellation     = std::move(cancellation);
        config.installHostTables =
            [surfaceInstaller = std::move(surfaceInstaller), &markCount](
                lua_State* state
            ) -> Status
        {
            UF_TRY(surfaceInstaller(state));
            script::testing::installMarkCounter(state, &markCount);
            return ok();
        };
        config.projectGlobals.emplace_back("mark");

        auto engine = script::Engine::create(config);
        REQUIRE(engine.has_value());
        auto result = engine->runNumber(source, "task-tier-c");
        return DiscriminatorRun{
            .result    = std::move(result),
            .markCount = markCount,
        };
    }
}
