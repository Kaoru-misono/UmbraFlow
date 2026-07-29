#pragma once

#include "../annotation/test-helpers.hpp"

#include <task/capability-surface.hpp>
#include <task/framework-bundle.hpp>
#include <task/task-context.hpp>

#include <annotation/resource.hpp>
#include <annotation/content-hash.hpp>
#include <annotation/recognition.hpp>
#include <annotation/recognition-runtime.hpp>
#include <annotation/runtime-manifest.hpp>

#include <core/error/result.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <engine/ports.hpp>
#include <engine/runtime-loader.hpp>
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
#include <string_view>
#include <utility>
#include <vector>

// Shared test fixture for the modules/task binding, adversarial and determinism
// suites: the single-page recognition runtime, the grey frame builders, the fake
// frame sources, the recording sinks, and the bound-VM runners every suite drives
// a real task VM through. test-task-binding.cpp, test-adversarial-surface.cpp and
// test-determinism-harness.cpp all build sessions from these, so they live here
// once rather than being copied into each translation unit. Everything is inline
// or a header-defined type, so including it in more than one TU is safe.
namespace uf::task
{
    namespace anno = annotation;

    inline constexpr auto k_anchorId = "00000000-0000-0000-0000-000000000011";
    inline constexpr auto k_actionId = "00000000-0000-0000-0000-000000000013";
    inline constexpr auto k_pageId   = "00000000-0000-0000-0000-000000000111";

    [[nodiscard]]
    constexpr auto asByte(uint8 value) noexcept -> std::byte
    {
        return static_cast<std::byte>(value);
    }

    // A one-by-one grey RGBA template addressed by its content hash, mirroring
    // the engine session fixtures so page and action evaluation behave exactly
    // as they do in the real loop.
    [[nodiscard]]
    inline auto encodedTemplate(uint8 gray) -> anno::EncodedRuntimeTemplate
    {
        auto const rgba = std::vector<std::byte>{
            asByte(gray),
            asByte(gray),
            asByte(gray),
            asByte(255),
        };
        auto encoded = image::encodeRgbaPng("task-binding-template.png", 1, 1, rgba);
        REQUIRE(encoded.has_value());
        auto const hash = anno::sha256(*encoded);
        REQUIRE(hash.has_value());
        return anno::EncodedRuntimeTemplate{
            .hash     = *hash,
            .pngBytes = *std::move(encoded),
        };
    }

    struct RuntimeParts final
    {
        engine::LoadedRuntime    loaded;
        anno::ProjectFingerprint fingerprint;
    };

    // One page (page_a, required anchor grey 2) plus one action target
    // (grey 5) authorized for that page. A grey [2,5,0] frame resolves page_a
    // and the target hits at position 1; a grey [0,0,0] frame resolves nothing
    // and the target misses.
    [[nodiscard]]
    inline auto singlePageRuntime() -> RuntimeParts
    {
        auto const fingerprint = anno::test::fingerprint(3, 1, 96, 96);
        auto const anchorA     = anno::test::recognizerId(k_anchorId);
        auto const actionT     = anno::test::recognizerId(k_actionId);
        auto const pageA       = anno::test::pageId(k_pageId);
        auto anchorTemplate = encodedTemplate(2);
        auto actionTemplate = encodedTemplate(5);
        auto const sourceBytes = std::array{asByte(42)};
        auto const sourceHash  = anno::sha256(sourceBytes);
        REQUIRE(sourceHash.has_value());

        auto manifest = anno::RuntimeManifest::create(
            anno::test::projectId("personal.task_binding"),
            fingerprint,
            {
                anno::RuntimeRecognizerSpec{
                    .definition = anno::test::recognizer(
                        fingerprint,
                        anchorA,
                        "anchor_a",
                        anno::AnnotationType::PageAnchor,
                        anno::test::pixelRect(0, 0, 1, 1),
                        anno::test::pixelRect(0, 0, 3, 1),
                        {},
                        std::nullopt,
                        anno::test::threshold(10'000)
                    ),
                    .templateHash = anchorTemplate.hash,
                    .sourceHash   = *sourceHash,
                },
                anno::RuntimeRecognizerSpec{
                    .definition = anno::test::recognizer(
                        fingerprint,
                        actionT,
                        "action_target",
                        anno::AnnotationType::ActionTarget,
                        anno::test::pixelRect(0, 0, 1, 1),
                        anno::test::pixelRect(0, 0, 3, 1),
                        {pageA},
                        std::nullopt,
                        anno::test::threshold(10'000)
                    ),
                    .templateHash = actionTemplate.hash,
                    .sourceHash   = *sourceHash,
                },
            },
            {anno::test::page(pageA, "page_a", {anchorA})}
        );
        REQUIRE(manifest.has_value());
        auto templates = std::vector<anno::EncodedRuntimeTemplate>{};
        templates.emplace_back(std::move(anchorTemplate));
        templates.emplace_back(std::move(actionTemplate));
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
    inline auto grayFrame(
        anno::ProjectFingerprint fingerprint,
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
        return std::vector<std::byte>{asByte(2), asByte(5), asByte(0)};
    }

    [[nodiscard]]
    inline auto unknownPixels() -> std::vector<std::byte>
    {
        return std::vector<std::byte>{asByte(0), asByte(0), asByte(0)};
    }

    // Replays a fixed sequence of frames, repeating the last once exhausted.
    class FakeFrameSource final : public engine::IFrameSource
    {
        std::vector<Frame> m_frames;
        std::size_t        m_index{0};

    public:
        explicit FakeFrameSource(std::vector<Frame> frames) noexcept
            : m_frames{std::move(frames)}
        {
        }

        [[nodiscard]] auto capture() -> Result<Frame> override
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
        uint32 m_clickCount{0};

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

        [[nodiscard]] auto clickCount() const noexcept -> uint32
        {
            return m_clickCount;
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

    // The run identity these fixtures stamp. The binding layer never authors it
    // -- TaskHost does -- so a fixed pair is enough to prove every event of a run
    // lands under one identity.
    inline constexpr auto k_fixtureRunId        = TaskRunId{5};
    inline constexpr auto k_fixtureGenerationId = GenerationId{1};

    // The EngineConfig a real task VM boots with, assembled in one place so no
    // test can accidentally assert against a surface shape the host does not
    // ship: the real framework bundle under the framework environment, the
    // private capability surface handed to it as a chunk argument, the uf
    // data tables as a project global, and the framework's own ctx published
    // beside them.
    //
    // `context` must outlive the Engine built from the returned config, because
    // the private surface holds its address (see CapabilitySurface).
    [[nodiscard]]
    inline auto taskVmConfig(CapabilitySurface const& surface, TaskContext& context)
        -> script::EngineConfig
    {
        return script::EngineConfig{
            .frameworkModules  = frameworkScriptModules(),
            .installHostTables = surface.installer(),
            .installPrivateCapabilities =
                CapabilitySurface::privateCapabilities(context),
            .projectGlobals          = CapabilitySurface::projectGlobals(),
            .frameworkProjectGlobals = frameworkProjectGlobals(),
            .classifyRaisedError     = CapabilitySurface::raisedErrorClassifier(),
        };
    }

    [[nodiscard]]
    inline auto baseConfig(anno::ProjectFingerprint fingerprint)
        -> engine::EngineSessionConfig
    {
        return engine::EngineSessionConfig{
            .liveFingerprint         = fingerprint,
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

        [[nodiscard]] auto capture() -> Result<Frame> override
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

    // The run's recorder, a constructed EngineSession over it, the surface
    // built from its own catalog, and a non-owning observer of the click sink.
    // The surface is captured before the runtime moves into the session, so
    // both name the same recognizer and page identities.
    //
    // The recorder is declared first and held through a unique_ptr: the
    // session borrows it (see engine/session.hpp), so it must outlive the
    // session and keep a stable address when this struct is moved.
    struct Built final
    {
        std::unique_ptr<trace::TraceRecorder> recorder;
        Result<engine::EngineSession>         session;
        CapabilitySurface                     surface;
        CountingActionSink*                   clicks;
    };

    // Builds the session from `frameSource` with `cancellation` armed on the
    // engine config, recording into `traceSink`. One recorder serves both the
    // engine session and the TaskContext built over it, which is what puts
    // their events into a single ordered stream.
    [[nodiscard]]
    inline auto buildBindingWith(
        std::unique_ptr<engine::IFrameSource> frameSource,
        std::stop_token cancellation,
        std::unique_ptr<trace::ITraceSink> traceSink
    ) -> Built
    {
        auto parts   = singlePageRuntime();
        auto surface = CapabilitySurface::create(
            parts.loaded.runtime.manifest().catalog()
        );
        REQUIRE(surface.has_value());

        auto actionSink      = std::make_unique<CountingActionSink>();
        auto* const p_clicks = actionSink.get();
        auto config         = baseConfig(parts.fingerprint);
        config.cancellation = std::move(cancellation);
        auto recorder        = std::make_unique<trace::TraceRecorder>(
            std::move(traceSink),
            k_fixtureRunId,
            k_fixtureGenerationId
        );
        auto session = engine::EngineSession::create(
            std::move(parts.loaded),
            std::move(frameSource),
            std::move(actionSink),
            *recorder,
            config
        );
        return Built{
            .recorder = std::move(recorder),
            .session  = std::move(session),
            .surface  = *std::move(surface),
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

    // One frame that resolves page_a and hits the action target.
    [[nodiscard]]
    inline auto resolvingFrames(FrameId frameId) -> std::vector<Frame>
    {
        auto frames = std::vector<Frame>{};
        frames.emplace_back(
            grayFrame(anno::test::fingerprint(3, 1, 96, 96), resolvingPixels(), frameId)
        );
        return frames;
    }

    // Runs `source` on a real task VM bound to `built`'s session and returns
    // the script's numeric result. The VM is created and destroyed inside
    // this call, so anything the host still holds afterwards is held by the
    // host, not by a live Lua handle.
    [[nodiscard]]
    inline auto runBound(TaskContext& context, Built& built, std::string_view source)
        -> double
    {
        auto engine = script::Engine::create(taskVmConfig(built.surface, context));
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
        Built& built,
        std::string_view source
    ) -> Result<double>
    {
        auto engine = script::Engine::create(taskVmConfig(built.surface, context));
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

    // Runs `source` on a task VM bound to `built` with `cancellation` armed on
    // the VM interrupt (the session already shares the same token), plus a host
    // mark() the script can call. Returns the run result and how many times
    // mark() reached. markCount is declared before the Engine, so it outlives
    // the VM and the closure's pointer into it stays valid for every call.
    [[nodiscard]]
    inline auto runWithMark(
        TaskContext& context,
        Built& built,
        std::stop_token cancellation,
        std::string_view source
    ) -> DiscriminatorRun
    {
        uint64 markCount        = 0;
        auto   config           = taskVmConfig(built.surface, context);
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
