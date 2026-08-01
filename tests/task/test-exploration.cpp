#include "binding-fixture.hpp"

#include <task/framework-bundle.hpp>
#include <task/pixel-probe.hpp>
#include <task/script-bindings.hpp>
#include <task/task-context.hpp>

#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/space.hpp>

#include <engine/session.hpp>

#include <image/png.hpp>

#include <script/engine.hpp>

#include <trace/event.hpp>
#include <trace/recorder.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The agent front-end's two halves: the primitives only it carries, and the
// environment split that decides who carries them.
//
// EVERY CASE HERE DRIVES REAL LUAU AGAINST A REAL SESSION. The claim under test
// is about what a script can and cannot reach, and a C++-only harness would
// prove things about a table nobody runs against. The absence cases in
// particular have to be asked from INSIDE the environment they are about: "the
// run environment cannot name a bare click" is a statement about a Luau
// whitelist, and only Luau can ask it.
namespace uf::task
{
    namespace
    {
        // A colour frame, because the two new primitives are about colour and the
        // shared fixture frame is Gray8.
        //
        // Four by two, painted in two solid halves: the left column pair is one
        // colour and the right pair another. That is the smallest frame on which
        // a crop can differ from the whole frame, a colour key can select part of
        // a rect and miss the rest, and a census has something to be dominant.
        inline constexpr auto k_leftBlue  = uint8{200};
        inline constexpr auto k_leftGreen = uint8{40};
        inline constexpr auto k_leftRed   = uint8{10};

        inline constexpr auto k_rightBlue  = uint8{10};
        inline constexpr auto k_rightGreen = uint8{220};
        inline constexpr auto k_rightRed   = uint8{30};

        [[nodiscard]]
        auto colourFingerprint() -> ProjectFingerprint
        {
            auto const fingerprint = ProjectFingerprint::create(4, 2, 96, 96);
            REQUIRE(fingerprint.has_value());
            return *fingerprint;
        }

        [[nodiscard]]
        auto colourPixels() -> std::vector<std::byte>
        {
            auto pixels = std::vector<std::byte>{};
            for (auto row = 0; row < 2; ++row)
            {
                for (auto column = 0; column < 4; ++column)
                {
                    auto const left = column < 2;
                    pixels.emplace_back(asByte(left ? k_leftBlue : k_rightBlue));
                    pixels.emplace_back(asByte(left ? k_leftGreen : k_rightGreen));
                    pixels.emplace_back(asByte(left ? k_leftRed : k_rightRed));
                    pixels.emplace_back(asByte(255));
                }
            }
            return pixels;
        }

        [[nodiscard]]
        auto colourFrame(FrameId frameId) -> Frame
        {
            auto const fingerprint = colourFingerprint();
            auto const transform   = CoordinateTransform::create(
                Point<DesktopSpace>{0.0F, 0.0F},
                static_cast<float>(fingerprint.width()),
                static_cast<float>(fingerprint.height()),
                fingerprint.width(),
                fingerprint.height()
            );
            REQUIRE(transform.has_value());

            auto const buffer = std::shared_ptr<FrameBuffer const>{
                std::make_shared<FrameBuffer>(colourPixels())
            };
            auto frame = Frame::create(
                frameId,
                CaptureSessionId{7},
                TargetGeneration::fromValue(3),
                MonotonicInstant::now(),
                fingerprint.width(),
                fingerprint.height(),
                std::size_t{4} * 4U,
                PixelFormat::Bgra8,
                buffer,
                *transform
            );
            REQUIRE(frame.has_value());
            return *std::move(frame);
        }

        // A session over the colour frame, recording into `sink`, stamped with
        // `frontEnd`.
        //
        // The front-end is a parameter because half the cases here are about it:
        // the same click through the same code has to land under two different
        // trace kinds depending on which stream it belongs to, and a fixture that
        // fixed the front-end could not ask that question.
        struct ColourHarness final
        {
            std::unique_ptr<trace::TraceRecorder> recorder;
            Result<engine::EngineSession>         session;
            CountingActionSink*                   clicks;
        };

        [[nodiscard]]
        auto buildColourHarness(
            trace::FrontEnd frontEnd,
            std::unique_ptr<trace::ITraceSink> sink
        ) -> ColourHarness
        {
            auto const fingerprint = colourFingerprint();

            auto frames = std::vector<Frame>{};
            frames.emplace_back(colourFrame(FrameId{900}));
            frames.emplace_back(colourFrame(FrameId{901}));
            frames.emplace_back(colourFrame(FrameId{902}));

            auto actionSink      = std::make_unique<CountingActionSink>();
            auto* const p_clicks = actionSink.get();
            auto recorder        = std::make_unique<trace::TraceRecorder>(
                std::move(sink),
                k_fixtureRunId,
                k_fixtureGenerationId,
                frontEnd
            );
            auto session = engine::EngineSession::create(
                std::make_unique<FakeFrameSource>(std::move(frames)),
                std::move(actionSink),
                *recorder,
                engine::EngineSessionConfig{
                    .liveFingerprint    = fingerprint,
                    .projectFingerprint = fingerprint,
                    .maximumPixelComparisons = 1'000,
                    .recognitionTimeout      = std::chrono::duration_cast<
                        MonotonicInstant::Duration
                    >(std::chrono::seconds{5}),
                }
            );
            return ColourHarness{
                .recorder = std::move(recorder),
                .session  = std::move(session),
                .clicks   = p_clicks,
            };
        }

        [[nodiscard]]
        auto runExploration(TaskContext& context, std::string_view source)
            -> Result<double>
        {
            auto engine = script::Engine::create(explorationVmConfig(context));
            REQUIRE(engine.has_value());
            return engine->runNumber(source, "exploration");
        }

        // A RUN VM that nonetheless publishes `explore`.
        //
        // This is the falsification harness for the environment split, and it is
        // deliberately not something the product ever builds. The claim is "the
        // run surface does not carry these keys"; asked through a verb that
        // raises, the claim would pass just as well against a surface that
        // carried the keys and refused the calls -- which is exactly the shape
        // the design rejects. So the module that names the surface is published
        // into a run VM here, and asked directly whether the keys are there.
        [[nodiscard]]
        auto runWithExploreOnRunSurface(
            TaskContext& context,
            std::string_view source
        ) -> Result<double>
        {
            auto config = taskVmConfig(context);
            config.frameworkProjectGlobals.emplace_back("explore");

            auto engine = script::Engine::create(config);
            REQUIRE(engine.has_value());
            return engine->runNumber(source, "run-surface-probe");
        }

        [[nodiscard]]
        auto kindsOf(std::vector<trace::StampedTraceEvent> const& events)
            -> std::vector<trace::TraceEventKind>
        {
            auto kinds = std::vector<trace::TraceEventKind>{};
            for (auto const& event : events)
            {
                kinds.emplace_back(event.event().kind);
            }
            return kinds;
        }

        TEST_CASE("A business environment cannot name a bare-coordinate click")
        {
            auto built = buildBinding(resolvingFrames(FrameId{40}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // The whole of the rule, asked as an absence rather than as a
            // refusal. `ctx` used to forward the primitive, which made the bare
            // click something every business task could call directly; the
            // project environment is a whitelist with no metatable, so a name
            // nothing publishes is not reachable by any route at all.
            constexpr std::string_view source = R"lua(
                if ctx.cycle_click_point ~= nil then return 0 end
                if explore ~= nil then return 0 end
                if scribe ~= nil then return 0 end

                -- The control: the environment is otherwise the real one, and
                -- the verbs a task IS meant to have are still here.
                if type(ctx.cycle_open) ~= "function" then return 0 end
                if type(observe.click) ~= "function" then return 0 end
                return 1
            )lua";

            CHECK(runBound(context, built, source) == doctest::Approx(1.0));
        }

        TEST_CASE("The run surface does not carry the crop and probe primitives")
        {
            auto built = buildBinding(resolvingFrames(FrameId{41}));
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // Asked of the TABLE, not of a call. See runWithExploreOnRunSurface.
            constexpr std::string_view source = R"lua(
                if explore.has("cycle_crop") then return 0 end
                if explore.has("probe") then return 0 end

                -- The bare click IS on a run surface, and that is not a hole:
                -- observe.click needs it for an element verified by its text,
                -- and it reaches it as a closure upvalue no environment names.
                if not explore.has("cycle_click_point") then return 0 end

                -- Controls: the ordinary primitives are all still there.
                if not explore.has("cycle_open") then return 0 end
                if not explore.has("cycle_read") then return 0 end
                return 1
            )lua";

            auto const result = runWithExploreOnRunSurface(context, source);
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("The exploration surface carries all three privileged verbs")
        {
            auto harness = buildColourHarness(
                trace::FrontEnd::Annotation,
                std::make_unique<DiscardingTraceSink>()
            );
            REQUIRE(harness.session.has_value());
            TaskContext context{*std::move(harness.session), *harness.recorder};

            constexpr std::string_view source = R"lua(
                if not explore.has("cycle_crop") then return 0 end
                if not explore.has("probe") then return 0 end
                if not explore.has("cycle_click_point") then return 0 end
                if type(scribe.measure) ~= "function" then return 0 end
                return 1
            )lua";

            auto const result = runExploration(context, source);
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("A crop hands back the region's pixels and their content hash")
        {
            auto harness = buildColourHarness(
                trace::FrontEnd::Annotation,
                std::make_unique<DiscardingTraceSink>()
            );
            REQUIRE(harness.session.has_value());
            TaskContext context{*std::move(harness.session), *harness.recorder};

            constexpr std::string_view source = R"lua(
                local ticket = ctx:cycle_open()
                local blob, hash = explore.crop(ticket, 0, 0, 2, 2)
                ctx:cycle_close(ticket)

                if type(blob) ~= "string" or #blob == 0 then return 0 end
                if type(hash) ~= "string" or #hash ~= 64 then return 0 end
                if string.match(hash, "^[0-9a-f]+$") == nil then return 0 end

                -- The blob is a PNG of exactly the region asked for, which the
                -- probe reads back off the bytes rather than being told.
                local stats = explore.probe(blob, 0, 0, 2, 2)
                if stats.image_width ~= 2 then return 0 end
                if stats.image_height ~= 2 then return 0 end
                return 1
            )lua";

            auto const result = runExploration(context, source);
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("The crop budget is per cycle and an exhausted one is never a miss")
        {
            auto harness = buildColourHarness(
                trace::FrontEnd::Annotation,
                std::make_unique<DiscardingTraceSink>()
            );
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.maximumCropsPerCycle = 2},
            };

            constexpr std::string_view source = R"lua(
                local ticket = ctx:cycle_open()
                explore.crop(ticket, 0, 0, 1, 1)
                explore.crop(ticket, 0, 0, 1, 1)

                local ok, err = pcall(function()
                    return explore.crop(ticket, 0, 0, 1, 1)
                end)
                if ok then return 0 end

                -- RecognitionIncomplete and never an empty answer: the host
                -- stopped looking, so nothing was established about the screen.
                if err.kind ~= uf.errors.recognition_incomplete then return 0 end
                ctx:cycle_close(ticket)

                -- The budget belongs to the CYCLE, so the next one starts fresh.
                local second = ctx:cycle_open()
                explore.crop(second, 0, 0, 1, 1)
                ctx:cycle_close(second)
                return 1
            )lua";

            auto const result = runExploration(context, source);
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("A crop is confined to the frame it was taken from")
        {
            auto harness = buildColourHarness(
                trace::FrontEnd::Annotation,
                std::make_unique<DiscardingTraceSink>()
            );
            REQUIRE(harness.session.has_value());
            TaskContext context{*std::move(harness.session), *harness.recorder};

            constexpr std::string_view source = R"lua(
                local ticket = ctx:cycle_open()

                -- One pixel past the right edge of a four-wide frame.
                local ok, err = pcall(function()
                    return explore.crop(ticket, 3, 0, 2, 1)
                end)
                if ok then return 0 end
                if err.kind ~= uf.errors.invalid_resource then return 0 end

                -- And past the bottom edge of a two-high one.
                local tallOk = pcall(function()
                    return explore.crop(ticket, 0, 1, 1, 2)
                end)
                if tallOk then return 0 end

                -- The control: a region that DOES fit is served.
                local blob = explore.crop(ticket, 3, 1, 1, 1)
                if type(blob) ~= "string" then return 0 end
                ctx:cycle_close(ticket)
                return 1
            )lua";

            auto const result = runExploration(context, source);
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("Two crops of one region on one frame are byte-identical")
        {
            auto harness = buildColourHarness(
                trace::FrontEnd::Annotation,
                std::make_unique<DiscardingTraceSink>()
            );
            REQUIRE(harness.session.has_value());
            TaskContext context{*std::move(harness.session), *harness.recorder};

            // Determinism is what lets a template asset be NAMED by its hash: two
            // crops of the same pixels have to be the same file, or an agent
            // re-measuring an element would litter the project with duplicates
            // that differ in nothing.
            constexpr std::string_view source = R"lua(
                local ticket = ctx:cycle_open()
                local first, firstHash   = explore.crop(ticket, 0, 0, 2, 2)
                local second, secondHash = explore.crop(ticket, 0, 0, 2, 2)
                ctx:cycle_close(ticket)

                if first ~= second then return 0 end
                if firstHash ~= secondHash then return 0 end

                -- A DIFFERENT region must not collide with it.
                local next = ctx:cycle_open()
                local other, otherHash = explore.crop(next, 2, 0, 2, 2)
                ctx:cycle_close(next)
                if other == first then return 0 end
                if otherHash == firstHash then return 0 end
                return 1
            )lua";

            auto const result = runExploration(context, source);
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("A crop does not consume the cycle that produced it")
        {
            auto harness = buildColourHarness(
                trace::FrontEnd::Annotation,
                std::make_unique<DiscardingTraceSink>()
            );
            REQUIRE(harness.session.has_value());
            TaskContext context{*std::move(harness.session), *harness.recorder};

            // Reading pixels changes nothing on the target, so the frame still
            // describes it afterwards and the same cycle may go on to click.
            constexpr std::string_view source = R"lua(
                local ticket = ctx:cycle_open()
                explore.crop(ticket, 0, 0, 1, 1)
                explore.click_point(ticket, 1, 1)

                -- The click DID spend it, which is the other half of the same
                -- statement.
                local ok = pcall(function()
                    return explore.crop(ticket, 0, 0, 1, 1)
                end)
                if ok then return 0 end
                return 1
            )lua";

            auto const result = runExploration(context, source);
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
            CHECK(harness.clicks->clickCount() == 1U);
        }

        TEST_CASE("probe reports a census with no key and a selection with one")
        {
            auto harness = buildColourHarness(
                trace::FrontEnd::Annotation,
                std::make_unique<DiscardingTraceSink>()
            );
            REQUIRE(harness.session.has_value());
            TaskContext context{*std::move(harness.session), *harness.recorder};

            // The frame is two solid halves, so a crop of the whole four-by-two
            // holds exactly two colours, four pixels each. A key on the left
            // colour must take exactly those four at full weight.
            auto const source = std::string{R"lua(
                local ticket = ctx:cycle_open()
                local blob = explore.crop(ticket, 0, 0, 4, 2)
                ctx:cycle_close(ticket)

                local census = explore.probe(blob, 0, 0, 4, 2)
                if census.rect_pixels ~= 8 then return 0 end
                if census.distinct_colours ~= 2 then return 0 end

                -- No key means no selection fields at all. Zeroes would read as
                -- "the key selected nothing", which is a different answer.
                if census.fully_selected_pixels ~= nil then return 0 end
                if census.selected_weight ~= nil then return 0 end

                local keyed = explore.probe(blob, 0, 0, 4, 2, {
                    red = )lua"} + std::to_string(k_leftRed) + R"lua(,
                    green = )lua" + std::to_string(k_leftGreen) + R"lua(,
                    blue = )lua" + std::to_string(k_leftBlue) + R"lua(,
                }, 0)
                if keyed.fully_selected_pixels ~= 4 then return 0 end
                if keyed.ramp_selected_pixels ~= 0 then return 0 end
                if keyed.selected_weight ~= 4 * 255 then return 0 end

                -- A key nothing on this crop wears selects nothing, and says so
                -- with a zero rather than with an absence.
                local miss = explore.probe(blob, 0, 0, 4, 2, {
                    red = 1, green = 2, blue = 3,
                }, 0)
                if miss.fully_selected_pixels ~= 0 then return 0 end
                if miss.rect_pixels ~= 8 then return 0 end
                return 1
            )lua";

            auto const result = runExploration(context, source);
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("probe is confined to the blob it was handed")
        {
            auto harness = buildColourHarness(
                trace::FrontEnd::Annotation,
                std::make_unique<DiscardingTraceSink>()
            );
            REQUIRE(harness.session.has_value());
            TaskContext context{*std::move(harness.session), *harness.recorder};

            constexpr std::string_view source = R"lua(
                local ticket = ctx:cycle_open()
                local blob = explore.crop(ticket, 0, 0, 2, 2)
                ctx:cycle_close(ticket)

                local ok, err = pcall(function()
                    return explore.probe(blob, 0, 0, 3, 3)
                end)
                if ok then return 0 end
                if err.kind ~= uf.errors.invalid_resource then return 0 end

                -- A blob that is not a PNG at all is refused rather than
                -- measured, which is what keeps "probe reports pixels" true.
                local junkOk = pcall(function()
                    return explore.probe("not a png", 0, 0, 1, 1)
                end)
                if junkOk then return 0 end
                return 1
            )lua";

            auto const result = runExploration(context, source);
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("probe needs no cycle, because it establishes nothing about the target")
        {
            auto harness = buildColourHarness(
                trace::FrontEnd::Annotation,
                std::make_unique<DiscardingTraceSink>()
            );
            REQUIRE(harness.session.has_value());
            TaskContext context{*std::move(harness.session), *harness.recorder};

            constexpr std::string_view source = R"lua(
                local ticket = ctx:cycle_open()
                local blob = explore.crop(ticket, 0, 0, 2, 2)
                ctx:cycle_close(ticket)

                -- The cycle is gone, and the measurement is still available:
                -- the bytes are the caller's now and reading them is arithmetic.
                local stats = explore.probe(blob, 0, 0, 2, 2)
                if stats.rect_pixels ~= 4 then return 0 end
                return 1
            )lua";

            auto const result = runExploration(context, source);
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("A bare click on the exploration stream is never engine.action_delivered")
        {
            auto sink        = std::make_unique<RecordingTraceSink>();
            auto* const p_sink = sink.get();
            auto harness     = buildColourHarness(
                trace::FrontEnd::Annotation,
                std::move(sink)
            );
            REQUIRE(harness.session.has_value());
            TaskContext context{*std::move(harness.session), *harness.recorder};

            constexpr std::string_view source = R"lua(
                local ticket = ctx:cycle_open()
                explore.click_point(ticket, 2, 1)
                return 1
            )lua";

            auto const result = runExploration(context, source);
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));

            auto const kinds = kindsOf(p_sink->events());
            CHECK(
                std::ranges::find(kinds, trace::TraceEventKind::AnnotationClickDelivered)
                != kinds.end()
            );
            CHECK(
                std::ranges::find(kinds, trace::TraceEventKind::EngineActionDelivered)
                == kinds.end()
            );

            // The point the AGENT named reaches the wire, not only the client
            // point the desktop received: on this stream the first of the two is
            // what the agent believed it was doing.
            auto const found = std::ranges::find_if(
                p_sink->events(),
                [](trace::StampedTraceEvent const& event)
                {
                    return event.event().kind
                        == trace::TraceEventKind::AnnotationClickDelivered;
                }
            );
            REQUIRE(found != p_sink->events().end());
            REQUIRE(found->event().annotation.has_value());
            REQUIRE(found->event().annotation->point.has_value());
            CHECK(found->event().annotation->point->x() == 2U);
            CHECK(found->event().annotation->point->y() == 1U);
            CHECK(found->event().clickClient.has_value());
            CHECK(found->frontEnd() == trace::FrontEnd::Annotation);
        }

        TEST_CASE("A crop writes the region it copied and the hash of what it produced")
        {
            auto sink          = std::make_unique<RecordingTraceSink>();
            auto* const p_sink = sink.get();
            auto harness       = buildColourHarness(
                trace::FrontEnd::Annotation,
                std::move(sink)
            );
            REQUIRE(harness.session.has_value());
            TaskContext context{*std::move(harness.session), *harness.recorder};

            constexpr std::string_view source = R"lua(
                local ticket = ctx:cycle_open()
                local blob, hash = explore.crop(ticket, 1, 0, 2, 2)
                ctx:cycle_close(ticket)
                return 1
            )lua";

            auto const result = runExploration(context, source);
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));

            auto const found = std::ranges::find_if(
                p_sink->events(),
                [](trace::StampedTraceEvent const& event)
                {
                    return event.event().kind
                        == trace::TraceEventKind::AnnotationRegionSaved;
                }
            );
            REQUIRE(found != p_sink->events().end());

            auto const& event = found->event();
            REQUIRE(event.annotation.has_value());
            REQUIRE(event.annotation->rect.has_value());
            CHECK(event.annotation->rect->x() == 1U);
            CHECK(event.annotation->rect->width() == 2U);
            CHECK(event.annotation->rect->height() == 2U);
            REQUIRE(event.annotation->contentHash.has_value());
            // The trace's own spelling of a content hash, which carries the
            // algorithm prefix every other content-hash line carries. The bare
            // hex the script was handed is the same value; see cycle_crop.
            CHECK(event.annotation->contentHash->starts_with("sha256:"));
            CHECK(event.annotation->contentHash->size() == 71U);
            REQUIRE(event.annotation->byteCount.has_value());
            CHECK(*event.annotation->byteCount > 0U);

            // The frame identity is on the line, which is the only thing that
            // ties a template asset back to the capture it was cut from.
            CHECK(event.frame.has_value());

            // And a native call names the verb, so the crop is joinable to the
            // cycle it was charged against.
            auto const* p_call = findNativeCall(p_sink->events(), "cycle_crop");
            REQUIRE(p_call != nullptr);
            CHECK(p_call->contentHash.has_value());
        }

        TEST_CASE("A crop measures the frame's own pixels")
        {
            // The C++ half of the same claim, asked without a VM: an agent that
            // cropped the right half must get the right half's colour, or every
            // measurement above is about a rectangle nobody chose.
            auto harness = buildColourHarness(
                trace::FrontEnd::Annotation,
                std::make_unique<DiscardingTraceSink>()
            );
            REQUIRE(harness.session.has_value());
            TaskContext context{*std::move(harness.session), *harness.recorder};

            auto const ticket = context.openCycle();
            REQUIRE(ticket.has_value());

            auto const rect = PixelRect::create(2, 0, 2, 2);
            REQUIRE(rect.has_value());
            auto const crop = context.cycleCrop(*ticket, *rect);
            REQUIRE(crop.has_value());

            auto const measured = probePngRegion(crop->png, *rect, std::nullopt);
            // The rect is in FRAME coordinates and the blob is the crop, so a
            // probe of the frame rect must not fit inside the two-by-two blob.
            CHECK(!measured.has_value());

            auto const blobRect = PixelRect::create(0, 0, 2, 2);
            REQUIRE(blobRect.has_value());
            auto const inside = probePngRegion(
                crop->png,
                *blobRect,
                ProbeColourKey{
                    .red       = k_rightRed,
                    .green     = k_rightGreen,
                    .blue      = k_rightBlue,
                    .tolerance = 0,
                }
            );
            REQUIRE(inside.has_value());
            CHECK(inside->rectPixels == 4U);
            REQUIRE(inside->fullySelectedPixels.has_value());
            CHECK(*inside->fullySelectedPixels == 4U);
            CHECK(inside->distinctColours == 1U);
            CHECK(inside->dominantRed == k_rightRed);
            CHECK(inside->dominantGreen == k_rightGreen);
            CHECK(inside->dominantBlue == k_rightBlue);
        }

        TEST_CASE("A Gray8 frame is cropped rather than refused")
        {
            // The capture format is not something the agent chose, so it must not
            // change which verbs answer. The shared fixture frame is Gray8.
            auto built = buildBindingWith(
                std::make_unique<FakeFrameSource>(resolvingFrames(FrameId{44})),
                std::stop_token{},
                std::make_unique<DiscardingTraceSink>()
            );
            REQUIRE(built.session.has_value());

            // The fixture recorder is a Task stream, and a crop writes an
            // annotation.* line, which that stream refuses by protocol. So this
            // case asks the question on an annotation-stamped recorder of its
            // own over the same grey frame.
            auto recorder = trace::TraceRecorder{
                std::make_unique<DiscardingTraceSink>(),
                k_fixtureRunId,
                k_fixtureGenerationId,
                trace::FrontEnd::Annotation,
            };
            auto session = engine::EngineSession::create(
                std::make_unique<FakeFrameSource>(resolvingFrames(FrameId{45})),
                std::make_unique<CountingActionSink>(),
                recorder,
                baseConfig(fixtureFingerprint())
            );
            REQUIRE(session.has_value());
            TaskContext context{*std::move(session), recorder};

            auto const ticket = context.openCycle();
            REQUIRE(ticket.has_value());
            auto const rect = PixelRect::create(0, 0, 3, 1);
            REQUIRE(rect.has_value());

            auto const crop = context.cycleCrop(*ticket, *rect);
            REQUIRE(crop.has_value());

            auto const decoded = image::decodePng(crop->png, "gray crop");
            REQUIRE(decoded.has_value());
            CHECK(decoded->width == 3U);
            CHECK(decoded->height == 1U);
        }

        TEST_CASE("A chunk that leaks a cycle does not poison the next one")
        {
            auto harness = buildColourHarness(
                trace::FrontEnd::Annotation,
                std::make_unique<DiscardingTraceSink>()
            );
            REQUIRE(harness.session.has_value());
            TaskContext context{*std::move(harness.session), *harness.recorder};

            // One queue line is one bracket, and the HOST owns it. An agent that
            // raised between cycle_open and cycle_close leaves the ledger holding
            // a frame; without the sweep the next chunk's cycle_open is an
            // InternalInvariant, latched terminal -- so one mistyped line would
            // end the whole session instead of costing one result line.
            auto const leaked = context.openCycle();
            REQUIRE(leaked.has_value());
            CHECK(context.hasOpenCycle());

            CHECK(context.sweepOpenCycle());
            CHECK(!context.hasOpenCycle());

            auto const next = context.openCycle();
            REQUIRE(next.has_value());

            // The swept ordinal is never reissued, so a ticket the chunk somehow
            // kept hold of stays dead rather than naming the new cycle.
            CHECK(next->ordinal != leaked->ordinal);

            // Sweeping when nothing is open says so rather than pretending.
            CHECK(context.sweepOpenCycle());
            CHECK(!context.sweepOpenCycle());
        }

        TEST_CASE("An annotation event is refused on a stream no agent drove")
        {
            // The mirror of the framework.* rule, and the reason the vocabulary
            // is worth anything: a stream's front-end does not merely label it,
            // it decides which events the stream may hold.
            auto sink          = std::make_unique<RecordingTraceSink>();
            auto* const p_sink = sink.get();
            auto recorder      = trace::TraceRecorder{
                std::move(sink),
                k_fixtureRunId,
                k_fixtureGenerationId,
                trace::FrontEnd::Task,
            };

            auto const point = PixelPoint{1, 1};
            auto const event = trace::TraceEvent{
                .kind       = trace::TraceEventKind::AnnotationClickDelivered,
                .annotation = trace::TraceEvent::Annotation{.point = point},
            };
            auto const admitted = recorder.emit(event);
            CHECK(!admitted.has_value());
            CHECK(p_sink->events().empty());
        }
    }
}
