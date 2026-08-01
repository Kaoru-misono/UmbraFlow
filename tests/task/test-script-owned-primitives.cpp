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
// gates. Every case here drives the host contract directly, because that
// contract is what the Luau layer will be built on and the Luau layer does not
// exist yet.
namespace uf::task
{
    namespace
    {
        // What a fake OCR engine was asked for and what it answers.
        //
        // The layout it records is load-bearing: the adapter this project ships
        // refuses TextLayout::Block outright, so a host that quietly asked for
        // Block would work against a fake and fail on the real engine.
        class FakeOcrEngine final : public ocr::IOcrEngine
        {
            ocr::Readout                       m_readout;
            std::optional<AutomationErrorKind> m_failure{};
            std::vector<ocr::TextLayout>       m_layouts{};
            std::vector<PixelRect>             m_rects{};

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
                    return fail(
                        AutomationErrorKind::UnsupportedCapability,
                        "this adapter does not run the detection model"
                    );
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

        // How one harness differs from the default. Everything here exists
        // because some case has to move it: a mismatched fingerprint, an already
        // dead lease, a budget of zero comparisons, or an OCR adapter.
        struct HarnessSpec final
        {
            std::optional<ProjectFingerprint> liveFingerprint{};
            uint64                            maximumPixelComparisons{1'000};
            MonotonicInstant::Duration maxActionFrameAge{k_defaultMaxActionFrameAge};
            std::unique_ptr<ocr::IOcrEngine> ocrEngine{};
        };

        struct Harness final
        {
            std::unique_ptr<trace::TraceRecorder> recorder;
            Result<engine::EngineSession>         session;
            CountingActionSink*                   clicks;
        };

        [[nodiscard]]
        auto buildHarness(std::vector<Frame> frames, HarnessSpec spec) -> Harness
        {
            auto const fingerprint = fixtureFingerprint();
            auto actionSink      = std::make_unique<CountingActionSink>();
            auto* const p_clicks = actionSink.get();
            auto recorder        = std::make_unique<trace::TraceRecorder>(
                std::make_unique<DiscardingTraceSink>(),
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

        // The PNG bytes of the one-by-one grey template the fixture runtime uses,
        // taken through the same encoder so a script-loaded template and a
        // catalog template are the same bytes.
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

            // Determinism, and the whole reason the verb is handle-based: the same
            // bytes name the same template however often a script loads them, so a
            // model that loads its templates in a different order still holds the
            // same handles.
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
            // Zero comparisons stops the search before it has decided anything.
            // The distinction this pins is the three-layer fail-closed rule: a
            // stop reported as nil would tell a script the template is absent
            // from a screen nothing ever looked at. Remove the stop branch in
            // EngineSession::matchTemplate and this case goes red -- the failure
            // becomes an empty optional.
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
            // The budget is a dimension of its own -- see
            // k_defaultMaximumReadsPerCycle -- and exhausting it must not read as
            // "there was no text here". Remove the charge in
            // TaskContext::cycleRead and the second read succeeds, so this case
            // goes red.
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

            // No page was resolved on this cycle, and that is the point: the page
            // requirement moved up a layer with the model, so a match delivers
            // without one.
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
            // The same-frame requirement, which is one of the four the host keeps.
            // Remove the requireOpenOrdinal call in cycleClickPoint and the second
            // cycle happily delivers a coordinate the first frame produced, so
            // this case goes red.
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
                // Remove the lease check in EngineSession::clickPoint and the
                // click is delivered against a frame whose coordinate has already
                // expired, so this case goes red.
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
            // decoded by template_load, matched, judged in Luau, and clicked --
            // which is the whole layer-one surface the script-owned page model
            // stands on, driven the way the framework will drive it.
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
