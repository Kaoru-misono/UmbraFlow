#include "binding-fixture.hpp"

#include <task/script-bindings.hpp>
#include <task/cycle-ledger.hpp>
#include <task/project-files.hpp>
#include <task/task-context.hpp>
#include <task/template-store.hpp>

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/detection.hpp>
#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <engine/session.hpp>

#include <image/png.hpp>

#include <script/engine.hpp>

#include <ocr/engine.hpp>
#include <ocr/text.hpp>

#include <trace/event.hpp>
#include <trace/recorder.hpp>

#include <vision/bgra-image.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// The layer-one half of the script-owned page model: raw template matching, text
// reading, project file I/O, and the bare-point click the trusted framework
// gates. Cases here drive the host contract directly, because that contract is
// what the Luau layer is built on.
namespace uf::task
{
    namespace
    {
        // What a fake OCR engine was asked for and what it answers. The layout it
        // records is load-bearing: the two layouts cost different things and refuse
        // on different terms, so a host that quietly asked for the wrong one would
        // work against a fake and misbehave on the real engine. The line ceiling is
        // recorded because a block read that did not hand its remaining budget down
        // would let the engine recognise a region the cycle cannot pay for.
        class FakeOcrEngine final : public ocr::IOcrEngine
        {
            ocr::Readout                       m_readout;
            ocr::Readout                       m_block{};
            std::optional<AutomationErrorKind> m_failure{};
            std::vector<ocr::TextLayout>       m_layouts{};
            std::vector<PixelRect>             m_rects{};
            std::vector<std::optional<uint32>> m_ceilings{};
            bool                               m_answersBlock{false};

        public:
            explicit FakeOcrEngine(ocr::Readout readout) noexcept
                : m_readout{std::move(readout)}
            {
            }

            FakeOcrEngine(
                ocr::Readout readout,
                AutomationErrorKind failure
            ) noexcept
                : m_readout{std::move(readout)}
                , m_failure{failure}
            {
            }

            // The two-layout fake. `block` is what a Block read answers with, and
            // supplying one is what makes this engine claim to have a detector.
            FakeOcrEngine(ocr::Readout readout, ocr::Readout block) noexcept
                : m_readout{std::move(readout)}
                , m_block{std::move(block)}
                , m_answersBlock{true}
            {
            }

            [[nodiscard]]
            auto identity() const noexcept -> std::string_view override
            {
                return "fake/single-line";
            }

            [[nodiscard]]
            auto read(
                BgraImage const& /*image*/,
                ocr::ReadSpec const& spec
            ) -> Result<ocr::Readout> override
            {
                m_layouts.emplace_back(spec.layout);
                if (spec.rect.has_value())
                {
                    m_rects.emplace_back(*spec.rect);
                }
                if (spec.layout == ocr::TextLayout::Block)
                {
                    m_ceilings.emplace_back(spec.maximumLines);
                    if (!m_answersBlock)
                    {
                        return fail(
                            AutomationErrorKind::UnsupportedCapability,
                            "this adapter does not run the detection model"
                        );
                    }
                    if (
                        spec.maximumLines.has_value()
                        && m_block.lines.size() > *spec.maximumLines
                    )
                    {
                        return fail(
                            AutomationErrorKind::RecognitionIncomplete,
                            "the region holds more lines than this read may take"
                        );
                    }
                    return m_block;
                }
                if (m_failure.has_value())
                {
                    return fail(*m_failure, "the fake ocr engine refused");
                }
                return m_readout;
            }

            [[nodiscard]] auto layouts() const noexcept UF_LIFETIME_BOUND
                -> std::vector<ocr::TextLayout> const&
            {
                return m_layouts;
            }

            [[nodiscard]] auto rects() const noexcept UF_LIFETIME_BOUND
                -> std::vector<PixelRect> const&
            {
                return m_rects;
            }

            [[nodiscard]] auto ceilings() const noexcept UF_LIFETIME_BOUND
                -> std::vector<std::optional<uint32>> const&
            {
                return m_ceilings;
            }
        };

        [[nodiscard]]
        auto oneLineReadout(std::string text, uint32 confidenceBp) -> ocr::Readout
        {
            auto readout = ocr::Readout{};
            readout.lines.emplace_back(
                ocr::TextLine{
                    .text         = std::move(text),
                    .bounds       = test::pixelRect(0, 0, 3, 1),
                    .confidenceBp = confidenceBp,
                }
            );
            return readout;
        }

        // How one harness differs from the default. Everything here exists because
        // some case has to move it.
        struct HarnessSpec final
        {
            std::optional<ProjectFingerprint> liveFingerprint{};
            uint64                            maximumPixelComparisons{1'000};
            MonotonicInstant::Duration maxActionFrameAge{k_defaultMaxActionFrameAge};
            std::unique_ptr<ocr::IOcrEngine> ocrEngine{};

            bool recordsTrace{false};
        };

        struct Harness final
        {
            std::unique_ptr<trace::TraceRecorder> recorder;
            Result<engine::EngineSession>         session;
            CountingActionSink*                   clicks;

            // Null unless the case asked for a recording sink: only a case that
            // asserts what the run WROTE needs to keep every line in memory.
            RecordingTraceSink* traces;
        };

        [[nodiscard]]
        auto buildHarness(std::vector<Frame> frames, HarnessSpec spec) -> Harness
        {
            auto const fingerprint = fixtureFingerprint();
            auto actionSink      = std::make_unique<CountingActionSink>();
            auto* const p_clicks = actionSink.get();

            auto traceSink       = std::make_unique<RecordingTraceSink>();
            auto* const p_traces = spec.recordsTrace ? traceSink.get() : nullptr;
            auto recorder        = std::make_unique<trace::TraceRecorder>(
                spec.recordsTrace
                    ? std::unique_ptr<trace::ITraceSink>{std::move(traceSink)}
                    : std::unique_ptr<trace::ITraceSink>{
                          std::make_unique<DiscardingTraceSink>()
                      },
                k_fixtureRunId,
                k_fixtureGenerationId,
                trace::FrontEnd::Task
            );
            auto session = engine::EngineSession::create(
                std::make_unique<FakeFrameSource>(std::move(frames)),
                std::move(actionSink),
                *recorder,
                engine::EngineSessionConfig{
                    .liveFingerprint         = spec.liveFingerprint.value_or(fingerprint),
                    .projectFingerprint      = fingerprint,
                    .maximumPixelComparisons = spec.maximumPixelComparisons,
                    .recognitionTimeout      = std::chrono::duration_cast<
                        MonotonicInstant::Duration
                    >(std::chrono::seconds{5}),
                    .maxActionFrameAge = spec.maxActionFrameAge,
                },
                std::move(spec.ocrEngine)
            );
            return Harness{
                .recorder = std::move(recorder),
                .session  = std::move(session),
                .clicks   = p_clicks,
                .traces   = p_traces,
            };
        }

        [[nodiscard]]
        auto matchableFrames() -> std::vector<Frame>
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(
                grayFrame(
                    fixtureFingerprint(),
                    resolvingPixels(),
                    FrameId{91}
                )
            );
            frames.emplace_back(
                grayFrame(
                    fixtureFingerprint(),
                    resolvingPixels(),
                    FrameId{92}
                )
            );
            return frames;
        }

        // The PNG bytes of the one-by-one grey template, through the same encoder
        // so a script-loaded template and a catalog template are the same bytes.
        [[nodiscard]]
        auto templateBlob(uint8 gray) -> std::vector<std::byte>
        {
            return encodedTemplate(gray).pngBytes;
        }

        // A template larger than any region a three-by-one frame can offer, so a
        // completed search has no candidate position at all.
        [[nodiscard]]
        auto oversizedTemplateBlob() -> std::vector<std::byte>
        {
            auto rgba = std::vector<std::byte>{};
            for (auto index = 0; index < 4; ++index)
            {
                rgba.emplace_back(asByte(5));
                rgba.emplace_back(asByte(5));
                rgba.emplace_back(asByte(5));
                rgba.emplace_back(asByte(255));
            }
            auto encoded = image::encodeRgbaPng("oversized.png", 2, 2, rgba);
            REQUIRE(encoded.has_value());
            return *std::move(encoded);
        }

        [[nodiscard]]
        auto hexOf(std::span<std::byte const> bytes) -> std::string
        {
            auto const hash = sha256(bytes);
            REQUIRE(hash.has_value());
            return hash->toString();
        }

        [[nodiscard]]
        auto kindOfError(Error const& error) -> AutomationErrorKind
        {
            auto const kind = automationErrorKind(error);
            REQUIRE(kind.has_value());
            return *kind;
        }

        // One directory that exists for the life of one case, removed on the way
        // out however the case ended.
        class TemporaryDirectory final
        {
            std::filesystem::path m_path;

        public:
            explicit TemporaryDirectory(std::string_view label)
                : m_path{std::filesystem::temp_directory_path() / label}
            {
                auto error = std::error_code{};
                std::filesystem::remove_all(m_path, error);
                REQUIRE(std::filesystem::create_directories(m_path, error));
            }

            TemporaryDirectory(TemporaryDirectory const&) = delete;
            TemporaryDirectory(TemporaryDirectory&&) = delete;
            auto operator=(TemporaryDirectory const&) -> TemporaryDirectory& = delete;
            auto operator=(TemporaryDirectory&&) -> TemporaryDirectory& = delete;

            ~TemporaryDirectory()
            {
                auto error = std::error_code{};
                std::filesystem::remove_all(m_path, error);
            }

            [[nodiscard]] auto path() const noexcept UF_LIFETIME_BOUND
                -> std::filesystem::path const&
            {
                return m_path;
            }
        };

        TEST_CASE("template_load decodes once and names the same pixels once")
        {
            auto built = buildHarness(matchableFrames(), HarnessSpec{});
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const blob  = templateBlob(k_targetActionGray);
            auto const first = context.loadTemplate(blob);
            REQUIRE(first.has_value());
            CHECK(first->hash.toString() == hexOf(blob));

            // The same bytes name the same template however often a script loads
            // them, so a model that loads its templates in a different order still
            // holds the same handles.
            auto const second = context.loadTemplate(blob);
            REQUIRE(second.has_value());
            CHECK(second->ticket.generation == first->ticket.generation);
            CHECK(second->ticket.ordinal == first->ticket.ordinal);

            auto const other = context.loadTemplate(templateBlob(k_targetAnchorGray));
            REQUIRE(other.has_value());
            CHECK(other->ticket.ordinal != first->ticket.ordinal);
        }

        TEST_CASE("template_load refuses a blob that is not a decodable template")
        {
            auto built = buildHarness(matchableFrames(), HarnessSpec{});
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const garbage = std::vector<std::byte>{
                asByte(1),
                asByte(2),
                asByte(3),
            };
            auto const loaded = context.loadTemplate(garbage);
            REQUIRE_FALSE(loaded.has_value());
            CHECK(kindOfError(loaded.error()) == AutomationErrorKind::InvalidResource);

            auto const empty = context.loadTemplate(std::span<std::byte const>{});
            REQUIRE_FALSE(empty.has_value());
            CHECK(kindOfError(empty.error()) == AutomationErrorKind::InvalidResource);
        }

        TEST_CASE("cycle_match reports a position, its score and its ceiling")
        {
            auto built = buildHarness(matchableFrames(), HarnessSpec{});
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const loaded = context.loadTemplate(templateBlob(k_targetActionGray));
            REQUIRE(loaded.has_value());
            auto const ticket = context.openCycle();
            REQUIRE(ticket.has_value());

            auto const found = context.cycleMatch(
                *ticket,
                loaded->ticket,
                test::pixelRect(0, 0, 3, 1)
            );
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());

            // The grey-5 pixel sits at x = 1 of [2, 5, 0], so an exact match is
            // score zero against a one-pixel ceiling of 255. Both numbers reach
            // the caller because judging them is the trusted framework's job.
            CHECK((*found)->matchedRect.x() == 1U);
            CHECK((*found)->sadScore == uint64{0});
            CHECK((*found)->maximumSad == uint64{255});
            CHECK((*found)->clickPixel.x() == 1U);
        }

        TEST_CASE("A cycle_match that cannot fit its template completes as a miss")
        {
            auto built = buildHarness(matchableFrames(), HarnessSpec{});
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const loaded = context.loadTemplate(oversizedTemplateBlob());
            REQUIRE(loaded.has_value());
            auto const ticket = context.openCycle();
            REQUIRE(ticket.has_value());

            auto const found = context.cycleMatch(
                *ticket,
                loaded->ticket,
                test::pixelRect(0, 0, 3, 1)
            );
            REQUIRE(found.has_value());
            CHECK_FALSE(found->has_value());
        }

        TEST_CASE("A cycle_match stopped by its budget is a failure, never a miss")
        {
            // Zero comparisons stops the search before it has decided anything, and
            // a stop reported as nil would tell a script the template is absent from
            // a screen nothing ever looked at. Remove the stop branch in
            // EngineSession::matchTemplate and this case goes red.
            auto built = buildHarness(
                matchableFrames(),
                HarnessSpec{.maximumPixelComparisons = 0}
            );
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const loaded = context.loadTemplate(templateBlob(k_targetActionGray));
            REQUIRE(loaded.has_value());
            auto const ticket = context.openCycle();
            REQUIRE(ticket.has_value());

            auto const found = context.cycleMatch(
                *ticket,
                loaded->ticket,
                test::pixelRect(0, 0, 3, 1)
            );
            REQUIRE_FALSE(found.has_value());
            CHECK(
                kindOfError(found.error())
                == AutomationErrorKind::RecognitionIncomplete
            );
        }

        TEST_CASE("cycle_match refuses a spent ticket and an unknown template")
        {
            auto built = buildHarness(matchableFrames(), HarnessSpec{});
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const loaded = context.loadTemplate(templateBlob(k_targetActionGray));
            REQUIRE(loaded.has_value());
            auto const ticket = context.openCycle();
            REQUIRE(ticket.has_value());
            CHECK(context.closeCycle(*ticket));

            auto const stale = context.cycleMatch(
                *ticket,
                loaded->ticket,
                test::pixelRect(0, 0, 3, 1)
            );
            REQUIRE_FALSE(stale.has_value());
            CHECK(
                kindOfError(stale.error()) == AutomationErrorKind::StaleObservation
            );

            auto const reopened = context.openCycle();
            REQUIRE(reopened.has_value());
            auto const foreign = context.cycleMatch(
                *reopened,
                TemplateTicket{.generation = 0, .ordinal = 99},
                test::pixelRect(0, 0, 3, 1)
            );
            REQUIRE_FALSE(foreign.has_value());
            CHECK(
                kindOfError(foreign.error()) == AutomationErrorKind::InvalidResource
            );
        }

        TEST_CASE("cycle_read returns the text, its confidence and the region read")
        {
            auto ocrEngine = std::make_unique<FakeOcrEngine>(
                oneLineReadout("battle", 9'000)
            );
            auto* const p_ocr = ocrEngine.get();
            auto built = buildHarness(
                matchableFrames(),
                HarnessSpec{.ocrEngine = std::move(ocrEngine)}
            );
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const ticket = context.openCycle();
            REQUIRE(ticket.has_value());
            auto const reading = context.cycleRead(
                *ticket,
                test::pixelRect(0, 0, 3, 1)
            );
            REQUIRE(reading.has_value());
            REQUIRE(reading->has_value());
            CHECK((*reading)->text == "battle");
            CHECK((*reading)->confidenceBp == 9'000U);
            CHECK((*reading)->rect.width() == 3U);

            // Single line and never Block. The shipped adapter refuses Block, so a
            // host that asked for it would pass here and fail on a real engine.
            REQUIRE(p_ocr->layouts().size() == 1U);
            CHECK(p_ocr->layouts().front() == ocr::TextLayout::SingleLine);
            REQUIRE(p_ocr->rects().size() == 1U);
            CHECK(p_ocr->rects().front() == test::pixelRect(0, 0, 3, 1));
        }

        TEST_CASE("A cycle_read past its per-cycle budget is a failure, never a miss")
        {
            // Exhausting the per-cycle read budget must not read as "there was no
            // text here". Remove the charge in TaskContext::cycleRead and the
            // second read succeeds, so this case goes red.
            auto built = buildHarness(
                matchableFrames(),
                HarnessSpec{
                    .ocrEngine = std::make_unique<FakeOcrEngine>(
                        oneLineReadout("battle", 9'000)
                    ),
                }
            );
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.maximumReadsPerCycle = 1},
            };

            auto const ticket = context.openCycle();
            REQUIRE(ticket.has_value());
            auto const rect = test::pixelRect(0, 0, 3, 1);

            auto const first = context.cycleRead(*ticket, rect);
            REQUIRE(first.has_value());
            CHECK(first->has_value());

            auto const second = context.cycleRead(*ticket, rect);
            REQUIRE_FALSE(second.has_value());
            CHECK(
                kindOfError(second.error())
                == AutomationErrorKind::RecognitionIncomplete
            );

            // The budget belongs to the cycle, so a fresh cycle reads again.
            CHECK(context.closeCycle(*ticket));
            auto const next = context.openCycle();
            REQUIRE(next.has_value());
            auto const third = context.cycleRead(*next, rect);
            REQUIRE(third.has_value());
            CHECK(third->has_value());
        }

        // Three lines at three different places inside a 3x1 frame's only row. The
        // rectangles differ from each other and from any region a case reads, which
        // is what makes "every line carries its OWN place" checkable at all.
        [[nodiscard]]
        auto threeLineReadout() -> ocr::Readout
        {
            auto readout = ocr::Readout{};
            readout.lines.emplace_back(
                ocr::TextLine{
                    .text   = "battle",
                    .bounds = test::pixelRect(0, 0, 1, 1),
                    .confidenceBp = 9'000,
                }
            );
            readout.lines.emplace_back(
                ocr::TextLine{
                    .text   = "rest",
                    .bounds = test::pixelRect(1, 0, 1, 1),
                    .confidenceBp = 8'000,
                }
            );
            readout.lines.emplace_back(
                ocr::TextLine{
                    .text   = "shop",
                    .bounds = test::pixelRect(2, 0, 1, 1),
                    .confidenceBp = 7'000,
                }
            );
            return readout;
        }

        TEST_CASE("cycle_read_lines reports every line with its own place on the frame")
        {
            auto ocrEngine = std::make_unique<FakeOcrEngine>(
                oneLineReadout("battle", 9'000),
                threeLineReadout()
            );
            auto* const p_ocr = ocrEngine.get();
            auto built = buildHarness(
                matchableFrames(),
                HarnessSpec{.ocrEngine = std::move(ocrEngine)}
            );
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const ticket = context.openCycle();
            REQUIRE(ticket.has_value());
            auto const lines = context.cycleReadLines(
                *ticket,
                test::pixelRect(0, 0, 3, 1)
            );
            REQUIRE(lines.has_value());
            REQUIRE(lines->size() == 3U);

            // Each line's OWN rectangle, and not the region that was read. Feed the
            // requested rect back onto every reading and this goes red on line two.
            CHECK((*lines)[0].text == "battle");
            CHECK((*lines)[0].rect == test::pixelRect(0, 0, 1, 1));
            CHECK((*lines)[1].text == "rest");
            CHECK((*lines)[1].rect == test::pixelRect(1, 0, 1, 1));
            CHECK((*lines)[2].confidenceBp == 7'000U);

            // Block and never SingleLine, with the region the caller named.
            REQUIRE(p_ocr->layouts().size() == 1U);
            CHECK(p_ocr->layouts().front() == ocr::TextLayout::Block);
            REQUIRE(p_ocr->rects().size() == 1U);
            CHECK(p_ocr->rects().front() == test::pixelRect(0, 0, 3, 1));
        }

        TEST_CASE("A block read costs one for locating and one for each line found")
        {
            // The budget decision, and the only place it is observable. Charge one
            // flat read per block read instead and the second call below succeeds,
            // so this goes red.
            auto built = buildHarness(
                matchableFrames(),
                HarnessSpec{
                    .ocrEngine = std::make_unique<FakeOcrEngine>(
                        oneLineReadout("battle", 9'000),
                        threeLineReadout()
                    ),
                }
            );
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.maximumReadsPerCycle = 5},
            };

            auto const ticket = context.openCycle();
            REQUIRE(ticket.has_value());
            auto const rect = test::pixelRect(0, 0, 3, 1);

            // One for the detection pass plus three lines is four of the five.
            auto const first = context.cycleReadLines(*ticket, rect);
            REQUIRE(first.has_value());
            CHECK(first->size() == 3U);

            // The fifth pays for the next detection pass, leaving nothing for
            // the lines it would find -- so it refuses rather than reading some.
            auto const second = context.cycleReadLines(*ticket, rect);
            REQUIRE_FALSE(second.has_value());
            CHECK(
                kindOfError(second.error())
                == AutomationErrorKind::RecognitionIncomplete
            );

            // And the pool is shared with the single-line verb, not a second
            // dimension beside it.
            auto const line = context.cycleRead(*ticket, rect);
            REQUIRE_FALSE(line.has_value());
            CHECK(
                kindOfError(line.error())
                == AutomationErrorKind::RecognitionIncomplete
            );
        }

        TEST_CASE("A block read hands its remaining budget down to the engine")
        {
            // What stops a region from being recognised line by line before the
            // budget can object: the ceiling travels with the read, so the engine
            // refuses having located rather than having read. Stop passing
            // `remaining` and the ceiling arrives absent, so this goes red.
            auto ocrEngine = std::make_unique<FakeOcrEngine>(
                oneLineReadout("battle", 9'000),
                threeLineReadout()
            );
            auto* const p_ocr = ocrEngine.get();
            auto built = buildHarness(
                matchableFrames(),
                HarnessSpec{.ocrEngine = std::move(ocrEngine)}
            );
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.maximumReadsPerCycle = 3},
            };

            auto const ticket = context.openCycle();
            REQUIRE(ticket.has_value());
            auto const lines = context.cycleReadLines(
                *ticket,
                test::pixelRect(0, 0, 3, 1)
            );

            REQUIRE(p_ocr->ceilings().size() == 1U);
            REQUIRE(p_ocr->ceilings().front().has_value());
            CHECK(*p_ocr->ceilings().front() == 2U);

            // Three lines against a ceiling of two is a failure and never the
            // first two lines.
            REQUIRE_FALSE(lines.has_value());
            CHECK(
                kindOfError(lines.error())
                == AutomationErrorKind::RecognitionIncomplete
            );
        }

        TEST_CASE("A block read writes one engine line carrying every line it found")
        {
            // A single-line read's region and its answer are one rectangle, so one
            // `readRect` says both; a block read's are not, and a reader checking
            // that a delivered click landed on text this frame actually held has
            // only the per-line rectangles to check it against. Drop the `lines`
            // the engine fills in and this goes red.
            auto built = buildHarness(
                matchableFrames(),
                HarnessSpec{
                    .ocrEngine = std::make_unique<FakeOcrEngine>(
                        oneLineReadout("battle", 9'000),
                        threeLineReadout()
                    ),
                    .recordsTrace = true,
                }
            );
            REQUIRE(built.session.has_value());
            auto* const p_traces = built.traces;
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const ticket = context.openCycle();
            REQUIRE(ticket.has_value());
            REQUIRE(
                context.cycleReadLines(*ticket, test::pixelRect(0, 0, 3, 1))
                    .has_value()
            );

            auto const* p_read = static_cast<trace::TraceEvent const*>(nullptr);
            for (auto const& stamped : p_traces->events())
            {
                if (stamped.event().kind == trace::TraceEventKind::EngineTextRead)
                {
                    p_read = &stamped.event();
                }
            }
            REQUIRE(p_read != nullptr);
            REQUIRE(p_read->reading.has_value());

            // The region on the event itself, and each line inside it.
            CHECK(p_read->reading->rect == test::pixelRect(0, 0, 3, 1));
            CHECK(p_read->reading->text.empty());
            CHECK(p_read->reading->engineId == "fake/single-line");
            REQUIRE(p_read->reading->lines.size() == 3U);
            CHECK(p_read->reading->lines[1].text == "rest");
            CHECK(p_read->reading->lines[1].rect == test::pixelRect(1, 0, 1, 1));
            CHECK(p_read->reading->lines[1].confidenceBp == 8'000U);
        }

        TEST_CASE("A single-line read still writes the line it always wrote")
        {
            // The control for the case above: adding a per-line list must not change
            // what the older verb records, because a stream written before block
            // reads existed has to keep meaning what it meant.
            auto built = buildHarness(
                matchableFrames(),
                HarnessSpec{
                    .ocrEngine = std::make_unique<FakeOcrEngine>(
                        oneLineReadout("battle", 9'000)
                    ),
                    .recordsTrace = true,
                }
            );
            REQUIRE(built.session.has_value());
            auto* const p_traces = built.traces;
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const ticket = context.openCycle();
            REQUIRE(ticket.has_value());
            REQUIRE(
                context.cycleRead(*ticket, test::pixelRect(0, 0, 3, 1)).has_value()
            );

            auto const* p_read = static_cast<trace::TraceEvent const*>(nullptr);
            for (auto const& stamped : p_traces->events())
            {
                if (stamped.event().kind == trace::TraceEventKind::EngineTextRead)
                {
                    p_read = &stamped.event();
                }
            }
            REQUIRE(p_read != nullptr);
            REQUIRE(p_read->reading.has_value());
            CHECK(p_read->reading->text == "battle");
            CHECK(p_read->reading->confidenceBp == 9'000U);
            CHECK(p_read->reading->lines.empty());
        }

        TEST_CASE("cycle_read_lines refuses without an adapter and on a spent ticket")
        {
            auto built = buildHarness(matchableFrames(), HarnessSpec{});
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const ticket = context.openCycle();
            REQUIRE(ticket.has_value());
            auto const rect     = test::pixelRect(0, 0, 3, 1);
            auto const unbacked = context.cycleReadLines(*ticket, rect);
            REQUIRE_FALSE(unbacked.has_value());
            CHECK(
                kindOfError(unbacked.error())
                == AutomationErrorKind::UnsupportedCapability
            );

            CHECK(context.closeCycle(*ticket));
            auto const stale = context.cycleReadLines(*ticket, rect);
            REQUIRE_FALSE(stale.has_value());
            CHECK(
                kindOfError(stale.error()) == AutomationErrorKind::StaleObservation
            );
        }

        TEST_CASE("cycle_read_lines hands a script a frozen array of readings")
        {
            // The array is frozen because the layer above treats what the host
            // returned as evidence; drop the deepFreeze and the first half goes red.
            constexpr std::string_view source = R"lua(
                local cycle = ctx:cycle_open()
                local lines = ctx:cycle_read_lines(cycle, 0, 0, 3, 1)
                if #lines ~= 3 then return 0 end
                if lines[1].text ~= "battle" then return 0 end
                if lines[2].x ~= 1 then return 0 end
                if lines[3].confidence ~= 7000 then return 0 end
                if pcall(function() lines[4] = lines[1] end) then return 0 end
                if pcall(function() lines[1].text = "forged" end) then return 0 end
                ctx:cycle_close(cycle)
                return 1
            )lua";

            SUBCASE("a run VM reaches it")
            {
                auto built = buildHarness(
                    matchableFrames(),
                    HarnessSpec{
                        .ocrEngine = std::make_unique<FakeOcrEngine>(
                            oneLineReadout("battle", 9'000),
                            threeLineReadout()
                        ),
                    }
                );
                REQUIRE(built.session.has_value());
                TaskContext context{*std::move(built.session), *built.recorder};

                auto engineVm = script::Engine::create(taskVmConfig(context));
                REQUIRE(engineVm.has_value());
                auto const result = engineVm->runNumber(source, "read-lines-run");
                REQUIRE(result.has_value());
                CHECK(*result == 1.0);
            }

            SUBCASE("an exploration VM reaches the same one")
            {
                auto built = buildHarness(
                    matchableFrames(),
                    HarnessSpec{
                        .ocrEngine = std::make_unique<FakeOcrEngine>(
                            oneLineReadout("battle", 9'000),
                            threeLineReadout()
                        ),
                    }
                );
                REQUIRE(built.session.has_value());
                TaskContext context{*std::move(built.session), *built.recorder};

                auto engineVm = script::Engine::create(explorationVmConfig(context));
                REQUIRE(engineVm.has_value());
                auto const result = engineVm->runNumber(
                    source,
                    "read-lines-exploration"
                );
                REQUIRE(result.has_value());
                CHECK(*result == 1.0);
            }
        }

        TEST_CASE("cycle_read surfaces an adapter refusal instead of reporting no text")
        {
            auto built = buildHarness(
                matchableFrames(),
                HarnessSpec{
                    .ocrEngine = std::make_unique<FakeOcrEngine>(
                        ocr::Readout{},
                        AutomationErrorKind::UnsupportedCapability
                    ),
                }
            );
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const ticket = context.openCycle();
            REQUIRE(ticket.has_value());
            auto const reading = context.cycleRead(
                *ticket,
                test::pixelRect(0, 0, 3, 1)
            );
            REQUIRE_FALSE(reading.has_value());
            CHECK(
                kindOfError(reading.error())
                == AutomationErrorKind::UnsupportedCapability
            );
        }

        TEST_CASE("cycle_read refuses without an adapter and on a spent ticket")
        {
            auto built = buildHarness(matchableFrames(), HarnessSpec{});
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const ticket = context.openCycle();
            REQUIRE(ticket.has_value());
            auto const rect      = test::pixelRect(0, 0, 3, 1);
            auto const unbacked = context.cycleRead(*ticket, rect);
            REQUIRE_FALSE(unbacked.has_value());
            CHECK(
                kindOfError(unbacked.error())
                == AutomationErrorKind::UnsupportedCapability
            );

            CHECK(context.closeCycle(*ticket));
            auto const stale = context.cycleRead(*ticket, rect);
            REQUIRE_FALSE(stale.has_value());
            CHECK(
                kindOfError(stale.error()) == AutomationErrorKind::StaleObservation
            );
        }

        TEST_CASE("A degenerate rectangle latches the generation before it is raised")
        {
            // PixelRect::create reports an empty rectangle as InternalInvariant,
            // and a framework bug has to latch the terminal BEFORE it is raised:
            // send that arm of checkPixelRect back to raiseTierB alone and the
            // carrier a script swallows buys it another primitive, so this goes
            // red. No OCR adapter is needed -- the rect is decoded before any
            // engine verb is reached.
            auto built = buildHarness(matchableFrames(), HarnessSpec{});
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            // cycle_close is the follow-up probe rather than a second cycle_open,
            // which would refuse on its own terms and mask the latch.
            constexpr std::string_view source = R"lua(
                local cycle = ctx:cycle_open()

                local swallowed, err = pcall(function()
                    return ctx:cycle_read(cycle, 0, 0, 0, 1)
                end)
                if swallowed then return 0 end
                if type(err) ~= 'userdata' then return 0 end
                if err.kind ~= uf.errors.internal_invariant then return 0 end

                -- Caught, and worth nothing: the next primitive stops at the
                -- guard before it reaches the engine.
                if pcall(function() ctx:cycle_close(cycle) end) then return 0 end
                return 1
            )lua";

            auto engineVm = script::Engine::create(taskVmConfig(context));
            REQUIRE(engineVm.has_value());
            auto const result = engineVm->runNumber(source, "degenerate-rect");
            REQUIRE(result.has_value());
            CHECK(*result == 1.0);
            CHECK(context.fatal());
            CHECK(context.terminalKind() == AutomationErrorKind::InternalInvariant);
        }

        TEST_CASE("A match clicks through the same fence a hit does")
        {
            auto built = buildHarness(matchableFrames(), HarnessSpec{});
            REQUIRE(built.session.has_value());
            auto* const p_clicks = built.clicks;
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const loaded = context.loadTemplate(templateBlob(k_targetActionGray));
            REQUIRE(loaded.has_value());
            auto const ticket = context.openCycle();
            REQUIRE(ticket.has_value());
            auto const found = context.cycleMatch(
                *ticket,
                loaded->ticket,
                test::pixelRect(0, 0, 3, 1)
            );
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());

            // No page was resolved on this cycle: the page requirement moved up a
            // layer with the model, so a match delivers without one.
            auto const receipt = context.cycleClickPoint(
                *ticket,
                ticket->ordinal,
                (*found)->clickPixel
            );
            REQUIRE(receipt.has_value());
            CHECK(p_clicks->clickCount() == 1U);

            // One observation delivers at most one input: the cycle is spent, so
            // the same ticket cannot click twice.
            auto const again = context.cycleClickPoint(
                *ticket,
                std::nullopt,
                (*found)->clickPixel
            );
            REQUIRE_FALSE(again.has_value());
            CHECK(
                kindOfError(again.error()) == AutomationErrorKind::StaleObservation
            );
            CHECK(p_clicks->clickCount() == 1U);
        }

        TEST_CASE("A match from a spent cycle cannot click on the next frame")
        {
            // The same-frame requirement. Remove the requireOpenOrdinal call in
            // cycleClickPoint and the second cycle happily delivers a coordinate
            // the first frame produced, so this case goes red.
            auto built = buildHarness(matchableFrames(), HarnessSpec{});
            REQUIRE(built.session.has_value());
            auto* const p_clicks = built.clicks;
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const loaded = context.loadTemplate(templateBlob(k_targetActionGray));
            REQUIRE(loaded.has_value());
            auto const first = context.openCycle();
            REQUIRE(first.has_value());
            auto const found = context.cycleMatch(
                *first,
                loaded->ticket,
                test::pixelRect(0, 0, 3, 1)
            );
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            CHECK(context.closeCycle(*first));

            auto const second = context.openCycle();
            REQUIRE(second.has_value());
            auto const carried = context.cycleClickPoint(
                *second,
                first->ordinal,
                (*found)->clickPixel
            );
            REQUIRE_FALSE(carried.has_value());
            CHECK(
                kindOfError(carried.error())
                == AutomationErrorKind::StaleObservation
            );
            CHECK(p_clicks->clickCount() == 0U);
        }

        TEST_CASE("A bare-point click still honours the lease and the fingerprint")
        {
            SUBCASE("an expired lease refuses the click")
            {
                // Remove the lease check in EngineSession::clickPoint and the click
                // is delivered against a frame whose coordinate has already expired,
                // so this case goes red.
                auto built = buildHarness(
                    matchableFrames(),
                    HarnessSpec{
                        .maxActionFrameAge = MonotonicInstant::Duration::zero(),
                    }
                );
                REQUIRE(built.session.has_value());
                auto* const p_clicks = built.clicks;
                TaskContext context{*std::move(built.session), *built.recorder};

                auto const ticket = context.openCycle();
                REQUIRE(ticket.has_value());
                auto const receipt = context.cycleClickPoint(
                    *ticket,
                    std::nullopt,
                    PixelPoint{1, 0}
                );
                REQUIRE_FALSE(receipt.has_value());
                CHECK(
                    kindOfError(receipt.error())
                    == AutomationErrorKind::StaleObservation
                );
                CHECK(p_clicks->clickCount() == 0U);
            }

            SUBCASE("a mismatched fingerprint refuses the click")
            {
                // Remove the fingerprint check and a project authored at one size
                // delivers coordinates onto a target of another, so this goes red.
                auto built = buildHarness(
                    matchableFrames(),
                    HarnessSpec{
                        .liveFingerprint = test::fingerprint(3, 1, 120, 120),
                    }
                );
                REQUIRE(built.session.has_value());
                auto* const p_clicks = built.clicks;
                TaskContext context{*std::move(built.session), *built.recorder};

                auto const ticket = context.openCycle();
                REQUIRE(ticket.has_value());
                auto const receipt = context.cycleClickPoint(
                    *ticket,
                    std::nullopt,
                    PixelPoint{1, 0}
                );
                REQUIRE_FALSE(receipt.has_value());
                CHECK(
                    kindOfError(receipt.error())
                    == AutomationErrorKind::TargetCompatibilityUnverified
                );
                CHECK(p_clicks->clickCount() == 0U);
            }
        }

        TEST_CASE("A scroll spends its cycle and needs no hit to deliver")
        {
            auto built = buildHarness(matchableFrames(), HarnessSpec{});
            REQUIRE(built.session.has_value());
            auto* const p_clicks = built.clicks;
            TaskContext context{*std::move(built.session), *built.recorder};

            // No template loaded, no match found, no page resolved: a scroll names
            // no screen position, so an open cycle is the whole of what it needs.
            auto const ticket = context.openCycle();
            REQUIRE(ticket.has_value());
            REQUIRE(context.cycleScroll(*ticket, int32{-3}).has_value());
            REQUIRE(p_clicks->scrolls().size() == 1U);
            CHECK(p_clicks->scrolls().front() == int32{-3});

            // The cycle is spent, exactly as a keystroke spends it: the screen
            // moved, so the frame this ticket named no longer describes it. Drop the
            // spend in TaskContext::cycleScroll and one frame delivers as many wheel
            // messages as a script asks for, so this goes red.
            CHECK_FALSE(context.hasOpenCycle());
            auto const again = context.cycleScroll(*ticket, int32{-3});
            REQUIRE_FALSE(again.has_value());
            CHECK(
                kindOfError(again.error()) == AutomationErrorKind::StaleObservation
            );
            CHECK(p_clicks->scrolls().size() == 1U);
        }

        TEST_CASE("cycle_scroll refuses every ticket that names no open cycle")
        {
            auto built = buildHarness(matchableFrames(), HarnessSpec{});
            REQUIRE(built.session.has_value());
            auto* const p_clicks = built.clicks;
            TaskContext context{*std::move(built.session), *built.recorder};

            SUBCASE("with no cycle ever opened")
            {
                auto const never = context.cycleScroll(
                    CycleTicket{.generation = 0, .ordinal = 1},
                    int32{1}
                );
                REQUIRE_FALSE(never.has_value());
                CHECK(
                    kindOfError(never.error())
                    == AutomationErrorKind::StaleObservation
                );
            }

            SUBCASE("on a cycle the caller already closed")
            {
                auto const ticket = context.openCycle();
                REQUIRE(ticket.has_value());
                CHECK(context.closeCycle(*ticket));

                auto const closed = context.cycleScroll(*ticket, int32{1});
                REQUIRE_FALSE(closed.has_value());
                CHECK(
                    kindOfError(closed.error())
                    == AutomationErrorKind::StaleObservation
                );
            }

            SUBCASE("on a ticket minted by another generation's ledger")
            {
                auto const ticket = context.openCycle();
                REQUIRE(ticket.has_value());

                // The live cycle's ordinal under a stamp this ledger never minted: a
                // ticket left over from a spent generation must be rejected rather
                // than collide with the ordinal that is open now.
                auto const foreign = context.cycleScroll(
                    CycleTicket{
                        .generation = ticket->generation + 1,
                        .ordinal    = ticket->ordinal,
                    },
                    int32{1}
                );
                REQUIRE_FALSE(foreign.has_value());
                CHECK(
                    kindOfError(foreign.error())
                    == AutomationErrorKind::StaleObservation
                );

                // And the refusal left the frame where it was, so the framework's
                // own close still has a cycle to close.
                CHECK(context.hasOpenCycle());
            }

            // Every refusal above is fail-closed: no wheel message escaped.
            CHECK(p_clicks->scrolls().empty());
        }

        TEST_CASE("cycle_scroll is on the run surface and the exploration surface")
        {
            // Decision (b) of the capability split: scrolling a list too long to fit
            // is ordinary business work, so unlike cycle_crop and probe this
            // primitive is bound on both surfaces. Move its installation inside
            // buildPrivateSurface's Exploration branch and the first half goes red.
            constexpr std::string_view source = R"lua(
                local cycle = ctx:cycle_open()
                ctx:cycle_scroll(cycle, -2)
                return 1
            )lua";

            SUBCASE("a run VM reaches it")
            {
                auto built = buildHarness(matchableFrames(), HarnessSpec{});
                REQUIRE(built.session.has_value());
                auto* const p_clicks = built.clicks;
                TaskContext context{*std::move(built.session), *built.recorder};

                auto engineVm = script::Engine::create(taskVmConfig(context));
                REQUIRE(engineVm.has_value());
                auto const result = engineVm->runNumber(source, "cycle-scroll-run");
                REQUIRE(result.has_value());
                CHECK(*result == 1.0);
                REQUIRE(p_clicks->scrolls().size() == 1U);
                CHECK(p_clicks->scrolls().front() == int32{-2});
                CHECK_FALSE(context.hasOpenCycle());
            }

            SUBCASE("an exploration VM reaches the same one")
            {
                auto built = buildHarness(matchableFrames(), HarnessSpec{});
                REQUIRE(built.session.has_value());
                auto* const p_clicks = built.clicks;
                TaskContext context{*std::move(built.session), *built.recorder};

                auto engineVm = script::Engine::create(explorationVmConfig(context));
                REQUIRE(engineVm.has_value());
                auto const result = engineVm->runNumber(
                    source,
                    "cycle-scroll-exploration"
                );
                REQUIRE(result.has_value());
                CHECK(*result == 1.0);
                REQUIRE(p_clicks->scrolls().size() == 1U);
                CHECK(p_clicks->scrolls().front() == int32{-2});
            }
        }

        TEST_CASE("cycle_scroll refuses a notch count that is not a whole number")
        {
            // Refused BEFORE the cycle is spent, which is what makes it worth
            // checking here rather than leaving to the delivery layer: a script that
            // passed a string or a fraction still holds its frame.
            auto built = buildHarness(matchableFrames(), HarnessSpec{});
            REQUIRE(built.session.has_value());
            auto* const p_clicks = built.clicks;
            TaskContext context{*std::move(built.session), *built.recorder};

            constexpr std::string_view source = R"lua(
                local cycle = ctx:cycle_open()
                local ok = pcall(function() ctx:cycle_scroll(cycle, 1.5) end)
                if ok then return 0 end
                local typed = pcall(function() ctx:cycle_scroll(cycle, "down") end)
                if typed then return 0 end
                ctx:cycle_scroll(cycle, 1)
                return 1
            )lua";

            auto engineVm = script::Engine::create(taskVmConfig(context));
            REQUIRE(engineVm.has_value());
            auto const result = engineVm->runNumber(source, "cycle-scroll-typing");
            REQUIRE(result.has_value());
            CHECK(*result == 1.0);
            REQUIRE(p_clicks->scrolls().size() == 1U);
            CHECK(p_clicks->scrolls().front() == int32{1});
        }

        TEST_CASE("A pointer move spends its cycle and presses nothing")
        {
            auto built = buildHarness(matchableFrames(), HarnessSpec{});
            REQUIRE(built.session.has_value());
            auto* const p_clicks = built.clicks;
            TaskContext context{*std::move(built.session), *built.recorder};

            // No template loaded, no match found, no page resolved: a move needs no
            // hit, exactly as a scroll needs none.
            auto const ticket = context.openCycle();
            REQUIRE(ticket.has_value());
            auto const receipt = context.cycleMovePointer(*ticket, PixelPoint{1, 0});
            REQUIRE(receipt.has_value());

            // One pointer message, and nothing pressed. Route cycleMovePointer to
            // cycleClickPoint and the two counts swap.
            REQUIRE(p_clicks->moves().size() == 1U);
            CHECK(p_clicks->moves().front().x() == doctest::Approx(1.0));
            CHECK(p_clicks->clickCount() == 0U);
            CHECK(p_clicks->longPresses().empty());

            // The cycle is spent, exactly as a scroll spends it: the target's idea
            // of what is hovered moved, so the frame this ticket named no longer
            // describes it. Drop the spend in TaskContext::cycleMovePointer and one
            // frame delivers as many moves as a script asks for, so this goes red.
            CHECK_FALSE(context.hasOpenCycle());
            auto const again = context.cycleMovePointer(*ticket, PixelPoint{1, 0});
            REQUIRE_FALSE(again.has_value());
            CHECK(
                kindOfError(again.error()) == AutomationErrorKind::StaleObservation
            );
            CHECK(p_clicks->moves().size() == 1U);
        }

        TEST_CASE("cycle_move_pointer refuses every ticket that names no open cycle")
        {
            auto built = buildHarness(matchableFrames(), HarnessSpec{});
            REQUIRE(built.session.has_value());
            auto* const p_clicks = built.clicks;
            TaskContext context{*std::move(built.session), *built.recorder};

            SUBCASE("with no cycle ever opened")
            {
                auto const never = context.cycleMovePointer(
                    CycleTicket{.generation = 0, .ordinal = 1},
                    PixelPoint{1, 0}
                );
                REQUIRE_FALSE(never.has_value());
                CHECK(
                    kindOfError(never.error())
                    == AutomationErrorKind::StaleObservation
                );
            }

            SUBCASE("on a cycle the caller already closed")
            {
                auto const ticket = context.openCycle();
                REQUIRE(ticket.has_value());
                CHECK(context.closeCycle(*ticket));

                auto const closed = context.cycleMovePointer(*ticket, PixelPoint{1, 0});
                REQUIRE_FALSE(closed.has_value());
                CHECK(
                    kindOfError(closed.error())
                    == AutomationErrorKind::StaleObservation
                );
            }

            SUBCASE("on a ticket minted by another generation's ledger")
            {
                auto const ticket = context.openCycle();
                REQUIRE(ticket.has_value());

                auto const foreign = context.cycleMovePointer(
                    CycleTicket{
                        .generation = ticket->generation + 1,
                        .ordinal    = ticket->ordinal,
                    },
                    PixelPoint{1, 0}
                );
                REQUIRE_FALSE(foreign.has_value());
                CHECK(
                    kindOfError(foreign.error())
                    == AutomationErrorKind::StaleObservation
                );

                // And the refusal left the frame where it was, so the framework's
                // own close still has a cycle to close.
                CHECK(context.hasOpenCycle());
            }

            // Every refusal above is fail-closed: no pointer message escaped.
            CHECK(p_clicks->moves().empty());
        }

        TEST_CASE("cycle_move_pointer is on the run surface and the exploration surface")
        {
            // The move is published like cycle_scroll and not like
            // cycle_click_point: what makes a bare coordinate privileged is that it
            // ACTIVATES something the page never authorised, and a move activates
            // nothing. Move its installation inside buildPrivateSurface's
            // Exploration branch, or drop the ctx forward, and the first half goes
            // red -- which is the whole point of the work, since a run-mode task
            // that cannot nudge cannot scroll a list.
            constexpr std::string_view source = R"lua(
                local cycle = ctx:cycle_open()
                ctx:cycle_move_pointer(cycle, 1, 0)
                return 1
            )lua";

            SUBCASE("a run VM reaches it")
            {
                auto built = buildHarness(matchableFrames(), HarnessSpec{});
                REQUIRE(built.session.has_value());
                auto* const p_clicks = built.clicks;
                TaskContext context{*std::move(built.session), *built.recorder};

                auto engineVm = script::Engine::create(taskVmConfig(context));
                REQUIRE(engineVm.has_value());
                auto const result = engineVm->runNumber(source, "cycle-move-run");
                REQUIRE(result.has_value());
                CHECK(*result == 1.0);
                REQUIRE(p_clicks->moves().size() == 1U);
                CHECK(p_clicks->clickCount() == 0U);
                CHECK_FALSE(context.hasOpenCycle());
            }

            SUBCASE("an exploration VM reaches the same one")
            {
                auto built = buildHarness(matchableFrames(), HarnessSpec{});
                REQUIRE(built.session.has_value());
                auto* const p_clicks = built.clicks;
                TaskContext context{*std::move(built.session), *built.recorder};

                auto engineVm = script::Engine::create(explorationVmConfig(context));
                REQUIRE(engineVm.has_value());
                auto const result = engineVm->runNumber(
                    source,
                    "cycle-move-exploration"
                );
                REQUIRE(result.has_value());
                CHECK(*result == 1.0);
                REQUIRE(p_clicks->moves().size() == 1U);
            }
        }

        TEST_CASE("cycle_move_pointer refuses a coordinate that is not a whole pixel")
        {
            // Refused BEFORE the cycle is spent, cycle_scroll's rule: a script that
            // passed a string or a fraction still holds its frame.
            auto built = buildHarness(matchableFrames(), HarnessSpec{});
            REQUIRE(built.session.has_value());
            auto* const p_clicks = built.clicks;
            TaskContext context{*std::move(built.session), *built.recorder};

            constexpr std::string_view source = R"lua(
                local cycle = ctx:cycle_open()
                local fractional = pcall(function()
                    ctx:cycle_move_pointer(cycle, 1.5, 0)
                end)
                if fractional then return 0 end
                local typed = pcall(function()
                    ctx:cycle_move_pointer(cycle, "left", 0)
                end)
                if typed then return 0 end
                local missing = pcall(function()
                    ctx:cycle_move_pointer(cycle, 1)
                end)
                if missing then return 0 end
                ctx:cycle_move_pointer(cycle, 1, 0)
                return 1
            )lua";

            auto engineVm = script::Engine::create(taskVmConfig(context));
            REQUIRE(engineVm.has_value());
            auto const result = engineVm->runNumber(source, "cycle-move-typing");
            REQUIRE(result.has_value());
            CHECK(*result == 1.0);
            REQUIRE(p_clicks->moves().size() == 1U);
        }

        TEST_CASE("A long press reaches the sink at its point with the hold named")
        {
            auto built = buildHarness(matchableFrames(), HarnessSpec{});
            REQUIRE(built.session.has_value());
            auto* const p_clicks = built.clicks;
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const ticket = context.openCycle();
            REQUIRE(ticket.has_value());
            auto const hold = MonotonicInstant::Duration{
                std::chrono::milliseconds{250}
            };
            auto const receipt =
                context.cycleLongPress(*ticket, PixelPoint{1, 0}, hold);
            REQUIRE(receipt.has_value());

            // The hold is asserted and not merely the delivery: replace `hold` with
            // anything else in the sink call inside EngineSession::longPress -- a
            // zero, a constant, the argument dropped -- and every layer still
            // delivers a press while this goes red.
            REQUIRE(p_clicks->longPresses().size() == 1U);
            CHECK(p_clicks->longPresses().front().hold == hold);
            CHECK(receipt->hold == hold);

            // The frame is 3x1 at scale 1, so the client point is the frame
            // point; what matters is that the coordinate travelled at all.
            CHECK(p_clicks->longPresses().front().point.x() == doctest::Approx(1.0));

            // It is NOT a click, on the port or on the wire: a long press folded
            // into click() would leave a reader counting delivered clicks counting
            // acts that magnified a card instead of pressing a button.
            CHECK(p_clicks->clickCount() == 0U);
        }

        TEST_CASE("A long press spends its cycle")
        {
            auto built = buildHarness(matchableFrames(), HarnessSpec{});
            REQUIRE(built.session.has_value());
            auto* const p_clicks = built.clicks;
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const ticket = context.openCycle();
            REQUIRE(ticket.has_value());
            auto const hold = MonotonicInstant::Duration{
                std::chrono::milliseconds{200}
            };
            REQUIRE(
                context.cycleLongPress(*ticket, PixelPoint{1, 0}, hold).has_value()
            );

            // A long press spends its frame like every other delivered input: the
            // press magnifies what it pressed, so the frame that authorised it
            // describes a screen the press has already replaced. Drop the spend in
            // TaskContext::cycleLongPress and one frame delivers as many presses as
            // a script asks for, so this goes red.
            CHECK_FALSE(context.hasOpenCycle());
            auto const again =
                context.cycleLongPress(*ticket, PixelPoint{1, 0}, hold);
            REQUIRE_FALSE(again.has_value());
            CHECK(
                kindOfError(again.error()) == AutomationErrorKind::StaleObservation
            );
            CHECK(p_clicks->longPresses().size() == 1U);
        }

        TEST_CASE("A long press honours the lease and the fingerprint like a click")
        {
            SUBCASE("an expired lease refuses the press")
            {
                // Paired with "A bare-point click still honours the lease and the
                // fingerprint" above: the same harness, the same expiry, the same
                // refusal. Remove the lease check from EngineSession::longPress --
                // a second and laxer path to the same window -- and this goes red
                // while the click case stays green.
                auto built = buildHarness(
                    matchableFrames(),
                    HarnessSpec{
                        .maxActionFrameAge = MonotonicInstant::Duration::zero(),
                    }
                );
                REQUIRE(built.session.has_value());
                auto* const p_clicks = built.clicks;
                TaskContext context{*std::move(built.session), *built.recorder};

                auto const ticket = context.openCycle();
                REQUIRE(ticket.has_value());
                auto const receipt = context.cycleLongPress(
                    *ticket,
                    PixelPoint{1, 0},
                    MonotonicInstant::Duration{std::chrono::milliseconds{200}}
                );
                REQUIRE_FALSE(receipt.has_value());
                CHECK(
                    kindOfError(receipt.error())
                    == AutomationErrorKind::StaleObservation
                );
                CHECK(p_clicks->longPresses().empty());
            }

            SUBCASE("a mismatched fingerprint refuses the press")
            {
                auto built = buildHarness(
                    matchableFrames(),
                    HarnessSpec{
                        .liveFingerprint = test::fingerprint(3, 1, 120, 120),
                    }
                );
                REQUIRE(built.session.has_value());
                auto* const p_clicks = built.clicks;
                TaskContext context{*std::move(built.session), *built.recorder};

                auto const ticket = context.openCycle();
                REQUIRE(ticket.has_value());
                auto const receipt = context.cycleLongPress(
                    *ticket,
                    PixelPoint{1, 0},
                    MonotonicInstant::Duration{std::chrono::milliseconds{200}}
                );
                REQUIRE_FALSE(receipt.has_value());
                CHECK(
                    kindOfError(receipt.error())
                    == AutomationErrorKind::TargetCompatibilityUnverified
                );
                CHECK(p_clicks->longPresses().empty());
            }
        }

        TEST_CASE("cycle_long_press refuses a hold it cannot honour")
        {
            // Every refusal below happens BEFORE the cycle is spent, so a script
            // that mistyped a hold still holds its frame and the target is never
            // left mid-press. Delete the k_maxLongPressHold check in
            // cycleLongPressFn and the third line stops raising, so this goes red.
            auto built = buildHarness(matchableFrames(), HarnessSpec{});
            REQUIRE(built.session.has_value());
            auto* const p_clicks = built.clicks;
            TaskContext context{*std::move(built.session), *built.recorder};

            constexpr std::string_view source = R"lua(
                local cycle = ctx:cycle_open()
                local missing = pcall(function()
                    explore.long_press(cycle, 1, 0)
                end)
                if missing then return 0 end
                local typed = pcall(function()
                    explore.long_press(cycle, 1, 0, "a while")
                end)
                if typed then return 0 end
                local huge = pcall(function()
                    explore.long_press(cycle, 1, 0, 60000)
                end)
                if huge then return 0 end
                local backwards = pcall(function()
                    explore.long_press(cycle, 1, 0, -50)
                end)
                if backwards then return 0 end
                explore.long_press(cycle, 1, 0, 200)
                return 1
            )lua";

            auto engineVm = script::Engine::create(explorationVmConfig(context));
            REQUIRE(engineVm.has_value());
            auto const result = engineVm->runNumber(source, "long-press-holds");
            REQUIRE(result.has_value());
            CHECK(*result == 1.0);
            REQUIRE(p_clicks->longPresses().size() == 1U);
            CHECK(
                p_clicks->longPresses().front().hold
                == MonotonicInstant::Duration{std::chrono::milliseconds{200}}
            );
        }

        TEST_CASE("cycle_long_press is bound on both surfaces and forwarded on one")
        {
            // The capability split for this verb. A long press names a BARE
            // COORDINATE, so it carries cycle_click_point's privilege exactly: the
            // primitive is installed on both private surfaces, because the trusted
            // framework needs it in run mode for an element the page model placed --
            // and no project environment may name it, because a business task
            // clicking or pressing wherever it likes is the hole `ctx` closed.
            //
            // A case asserting only that `ctx` has no such key would pass just as
            // well against a build where the primitive did not exist at all, so each
            // subcase pairs the absence with something that shows the primitive
            // really is there. Add a forward to ctx.luau --
            //
            //     function ctx:cycle_long_press(ticket, x, y, hold)
            //         return native.cycle_long_press(ticket, x, y, hold)
            //     end
            //
            // -- and BOTH subcases go red, which is the fence being HELD rather than
            // merely absent. That the run surface really binds it is proved by
            // delivering one in test-script-owned-model.cpp's "observe.long_press
            // presses a page-positioned element", because a run VM publishes no
            // `explore` to ask the question with.
            SUBCASE("no project environment can name it on ctx")
            {
                auto built = buildHarness(matchableFrames(), HarnessSpec{});
                REQUIRE(built.session.has_value());
                auto* const p_clicks = built.clicks;
                TaskContext context{*std::move(built.session), *built.recorder};

                // Asserted in the EXPLORATION VM, where `explore.long_press` proves
                // in the same breath that the primitive is bound and reachable.
                // `ctx` is published into both project environments, so its silence
                // here is its silence in a run VM too.
                constexpr std::string_view source = R"lua(
                    if rawget(ctx, "cycle_long_press") ~= nil then return 0 end
                    if rawget(ctx, "cycle_click_point") ~= nil then return 0 end
                    if not explore.has("cycle_long_press") then return 0 end
                    local cycle = ctx:cycle_open()
                    explore.long_press(cycle, 1, 0, 150)
                    return 1
                )lua";

                auto engineVm = script::Engine::create(explorationVmConfig(context));
                REQUIRE(engineVm.has_value());
                auto const result = engineVm->runNumber(source, "long-press-ctx");
                REQUIRE(result.has_value());
                CHECK(*result == 1.0);
                CHECK(p_clicks->longPresses().size() == 1U);
            }

            SUBCASE("a run VM has neither the ctx forward nor the explore module")
            {
                // The other half of the confinement: `explore` publishes the forward
                // and is in the exploration environment's list and no other, so a
                // business task has no `ctx` key AND no module to reach it through.
                // Publish `explore` into the run environment and this goes red.
                auto built = buildHarness(matchableFrames(), HarnessSpec{});
                REQUIRE(built.session.has_value());
                auto* const p_clicks = built.clicks;
                TaskContext context{*std::move(built.session), *built.recorder};

                constexpr std::string_view source = R"lua(
                    if rawget(ctx, "cycle_long_press") ~= nil then return 0 end
                    if explore ~= nil then return 0 end
                    return 1
                )lua";

                auto engineVm = script::Engine::create(taskVmConfig(context));
                REQUIRE(engineVm.has_value());
                auto const result = engineVm->runNumber(source, "long-press-run");
                REQUIRE(result.has_value());
                CHECK(*result == 1.0);
                CHECK(p_clicks->longPresses().empty());
            }
        }

        TEST_CASE("Project file I/O round trips inside the project directory")
        {
            auto const directory = TemporaryDirectory{"uf-project-files-roundtrip"};
            auto built = buildHarness(matchableFrames(), HarnessSpec{});
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const payload = std::vector<std::byte>{
                asByte(uint8{0x70}),
                asByte(uint8{0x61}),
                asByte(uint8{0x67}),
                asByte(uint8{0x65}),
            };
            REQUIRE(context.projectWrite("model.toml", payload).has_value());

            auto const read = context.projectRead("model.toml");
            REQUIRE(read.has_value());
            CHECK(*read == payload);

            // Rewriting in place is the difference from the input agent's output
            // confinement: a page model is saved over and over.
            auto const replacement = std::vector<std::byte>{asByte(uint8{0x78})};
            REQUIRE(context.projectWrite("model.toml", replacement).has_value());
            auto const reread = context.projectRead("model.toml");
            REQUIRE(reread.has_value());
            CHECK(*reread == replacement);
        }

        TEST_CASE("Project file I/O refuses every name that leaves the project")
        {
            // The falsification: the escape target really exists and really is
            // readable. Drop the parent-traversal refusal or the containment
            // check and the first read below succeeds, so this case goes red.
            auto const outer = TemporaryDirectory{"uf-project-files-confinement"};
            auto const root  = outer.path() / "project";
            auto error       = std::error_code{};
            REQUIRE(std::filesystem::create_directories(root, error));

            auto const escapeTarget = outer.path() / "secret.txt";
            {
                auto stream = std::ofstream{escapeTarget, std::ios::binary};
                REQUIRE(stream.is_open());
                stream << "not yours";
            }
            REQUIRE(std::filesystem::is_regular_file(escapeTarget));

            auto built = buildHarness(matchableFrames(), HarnessSpec{});
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = root},
            };

            auto const traversal = context.projectRead("../secret.txt");
            REQUIRE_FALSE(traversal.has_value());
            CHECK(
                kindOfError(traversal.error())
                == AutomationErrorKind::InvalidResource
            );

            auto const absolute = context.projectRead(escapeTarget.string());
            REQUIRE_FALSE(absolute.has_value());
            CHECK(
                kindOfError(absolute.error())
                == AutomationErrorKind::InvalidResource
            );

            auto const empty = context.projectRead("");
            REQUIRE_FALSE(empty.has_value());
            CHECK(kindOfError(empty.error()) == AutomationErrorKind::InvalidResource);

            auto const written = context.projectWrite(
                "../escaped.txt",
                std::span<std::byte const>{}
            );
            REQUIRE_FALSE(written.has_value());
            CHECK(kindOfError(written.error()) == AutomationErrorKind::InvalidResource);
            CHECK_FALSE(std::filesystem::exists(outer.path() / "escaped.txt"));
        }

        TEST_CASE("A generation with no project directory reaches no file at all")
        {
            auto built = buildHarness(matchableFrames(), HarnessSpec{});
            REQUIRE(built.session.has_value());
            TaskContext context{*std::move(built.session), *built.recorder};

            auto const read = context.projectRead("model.toml");
            REQUIRE_FALSE(read.has_value());
            CHECK(kindOfError(read.error()) == AutomationErrorKind::InvalidResource);
        }

        TEST_CASE("The private surface exposes the script-owned primitives")
        {
            auto const directory = TemporaryDirectory{"uf-project-files-surface"};
            auto built = buildHarness(matchableFrames(), HarnessSpec{});
            REQUIRE(built.session.has_value());
            auto* const p_clicks = built.clicks;
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            // The blob is written and read back through the project verbs, then
            // decoded by template_load, matched, judged in Luau and clicked -- the
            // whole layer-one surface, driven the way the framework will drive it.
            auto const blob = templateBlob(k_targetActionGray);
            REQUIRE(context.projectWrite("action.png", blob).has_value());

            constexpr std::string_view source = R"lua(
                local blob = ctx:project_read("action.png")
                local template = ctx:template_load(blob)
                local cycle = ctx:cycle_open()
                local match = ctx:cycle_match(cycle, template, 0, 0, 3, 1)
                if match == nil then return 0 end
                if match.score ~= 0 then return 0 end
                if match.maximum ~= 255 then return 0 end
                if match.x ~= 1 then return 0 end
                ctx:cycle_click(cycle, match)
                return 1
            )lua";

            auto engineVm = script::Engine::create(taskVmConfig(context));
            REQUIRE(engineVm.has_value());
            auto const result = engineVm->runNumber(source, "script-owned-primitives");
            REQUIRE(result.has_value());
            CHECK(*result == 1.0);
            CHECK(p_clicks->clickCount() == 1U);
            CHECK_FALSE(context.hasOpenCycle());
        }
    }
}
