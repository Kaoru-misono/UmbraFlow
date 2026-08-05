#include "binding-fixture.hpp"

#include <task/framework-bundle.hpp>
#include <task/pixel-probe.hpp>
#include <task/script-bindings.hpp>
#include <task/task-context.hpp>

#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/space.hpp>

#include <engine/session.hpp>

#include <image/png.hpp>

#include <script/engine.hpp>

#include <trace/recorder.hpp>

#include <vision/frame-analysis.hpp>
#include <vision/template-match.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

// Colour-key masking through the AUTHORING half: a crop cut under a key, the
// alpha plane it bakes, the counts it reports, and the two authoring mistakes
// the host is willing to say something about. The scores are the test rather
// than the alpha bytes, because a mask that reaches the PNG but never reaches
// the matcher would satisfy any assertion about pixels -- so each case cuts one
// template two ways from the same pixels and compares what the matcher reports.
namespace uf::task
{
    namespace
    {
        // The fixture screen: a glyph that is a small part of its own rectangle,
        // on a background that is most of it. Forty by eight so BOTH ends of the
        // mask floor are reachable on one frame -- the glyph takes 60 of 320
        // pixels (18.8%), inside the 6.6-25.8% band every element that survived
        // cross-page falsification in the reference project measured; the speck
        // takes 10, under the floor; the background 250, over the share.
        inline constexpr auto k_backgroundLevel = uint8{16};
        inline constexpr auto k_glyphLevel      = uint8{240};

        // The background repainted, for the frame where the glyph holds still
        // and the scenery does not. Eighty grey levels away: far enough that an
        // unmasked template cannot ignore it, near enough that nothing saturates.
        inline constexpr auto k_sceneryLevel    = uint8{96};
        inline constexpr auto k_sceneryDistance = uint64{80};

        // How far the glyph moves when it is gone: the level it wears minus the
        // background that replaces it.
        inline constexpr auto k_glyphDistance = uint64{224};

        inline constexpr auto k_speckBlue  = uint8{60};
        inline constexpr auto k_speckGreen = uint8{60};
        inline constexpr auto k_speckRed   = uint8{200};

        inline constexpr auto k_screenWidth  = uint32{40};
        inline constexpr auto k_screenHeight = uint32{8};

        inline constexpr auto k_rectPixels       = uint64{320};
        inline constexpr auto k_glyphPixels      = uint64{60};
        inline constexpr auto k_speckPixels      = uint64{10};
        inline constexpr auto k_backgroundPixels = uint64{250};

        [[nodiscard]]
        auto isGlyph(uint32 x, uint32 y) noexcept -> bool
        {
            return x >= 2U && x < 12U && y >= 1U && y < 7U;
        }

        [[nodiscard]]
        auto isSpeck(uint32 x, uint32 y) noexcept -> bool
        {
            return y == 0U && x >= 30U;
        }

        // The three frames this file needs. One closed set rather than two
        // booleans because exactly one thing varies at a time, which is what
        // makes each score attributable.
        enum class Scene : uint8
        {
            // The screen the template is cut from.
            Authored,

            // The glyph is gone and the scenery is untouched: the empty cell an
            // element must MISS.
            GlyphGone,

            // The glyph is byte-identical and the scenery behind it is
            // repainted: the same element on a different map, which an element
            // must still HIT. The frame the measured minimap failure is about.
            SceneryChanged,
        };

        [[nodiscard]]
        auto backgroundOf(Scene scene) noexcept -> uint8
        {
            return scene == Scene::SceneryChanged
                ? k_sceneryLevel
                : k_backgroundLevel;
        }

        [[nodiscard]]
        auto screenPixels(Scene scene) -> std::vector<std::byte>
        {
            auto const background = backgroundOf(scene);
            auto pixels = std::vector<std::byte>{};
            for (auto y = uint32{0}; y < k_screenHeight; ++y)
            {
                for (auto x = uint32{0}; x < k_screenWidth; ++x)
                {
                    // The speck is painted identically on every scene: it exists
                    // for the mask floor below, and holding it still keeps each
                    // score attributable to one group of pixels.
                    if (isSpeck(x, y))
                    {
                        pixels.emplace_back(asByte(k_speckBlue));
                        pixels.emplace_back(asByte(k_speckGreen));
                        pixels.emplace_back(asByte(k_speckRed));
                        pixels.emplace_back(asByte(255));
                        continue;
                    }
                    auto const glyph =
                        scene != Scene::GlyphGone && isGlyph(x, y);
                    auto const level = glyph ? k_glyphLevel : background;
                    pixels.emplace_back(asByte(level));
                    pixels.emplace_back(asByte(level));
                    pixels.emplace_back(asByte(level));
                    pixels.emplace_back(asByte(255));
                }
            }
            return pixels;
        }

        [[nodiscard]]
        auto screenFingerprint() -> ProjectFingerprint
        {
            auto const fingerprint = ProjectFingerprint::create(
                k_screenWidth,
                k_screenHeight,
                96,
                96
            );
            REQUIRE(fingerprint.has_value());
            return *fingerprint;
        }

        [[nodiscard]]
        auto screenFrame(Scene scene, FrameId frameId) -> Frame
        {
            auto const fingerprint = screenFingerprint();
            auto const transform   = CoordinateTransform::create(
                Point<DesktopSpace>{0.0F, 0.0F},
                static_cast<float>(fingerprint.width()),
                static_cast<float>(fingerprint.height()),
                fingerprint.width(),
                fingerprint.height()
            );
            REQUIRE(transform.has_value());

            auto const buffer = std::shared_ptr<FrameBuffer const>{
                std::make_shared<FrameBuffer>(screenPixels(scene))
            };
            auto frame = Frame::create(
                frameId,
                CaptureSessionId{7},
                TargetGeneration::fromValue(3),
                MonotonicInstant::now(),
                fingerprint.width(),
                fingerprint.height(),
                std::size_t{k_screenWidth} * 4U,
                PixelFormat::Bgra8,
                buffer,
                *transform
            );
            REQUIRE(frame.has_value());
            return *std::move(frame);
        }

        struct ScreenHarness final
        {
            std::unique_ptr<trace::TraceRecorder> recorder;
            Result<engine::EngineSession>         session;
        };

        [[nodiscard]]
        auto buildScreenHarness() -> ScreenHarness
        {
            auto frames = std::vector<Frame>{};
            for (auto id = uint64{800}; id < 820U; ++id)
            {
                frames.emplace_back(screenFrame(Scene::Authored, FrameId{id}));
            }

            auto recorder = std::make_unique<trace::TraceRecorder>(
                std::make_unique<DiscardingTraceSink>(),
                k_fixtureRunId,
                k_fixtureGenerationId,
                trace::FrontEnd::Annotation
            );
            auto session = engine::EngineSession::create(
                std::make_unique<FakeFrameSource>(std::move(frames)),
                std::make_unique<CountingActionSink>(),
                *recorder,
                baseConfig(screenFingerprint())
            );
            return ScreenHarness{
                .recorder = std::move(recorder),
                .session  = std::move(session),
            };
        }

        [[nodiscard]]
        auto runExploration(TaskContext& context, std::string_view source)
            -> Result<double>
        {
            auto engine = script::Engine::create(explorationVmConfig(context));
            REQUIRE(engine.has_value());
            return engine->runNumber(source, "colour-key-crop");
        }

        [[nodiscard]]
        auto wholeScreen() -> PixelRect
        {
            auto const rect =
                PixelRect::create(0, 0, k_screenWidth, k_screenHeight);
            REQUIRE(rect.has_value());
            return *rect;
        }

        [[nodiscard]]
        auto glyphKey(uint32 tolerance) -> ProbeColourKey
        {
            return ProbeColourKey{
                .red       = k_glyphLevel,
                .green     = k_glyphLevel,
                .blue      = k_glyphLevel,
                .tolerance = tolerance,
            };
        }

        // The score one decoded template gets on one scene; a raw match takes no
        // threshold, so it is the whole of what two spellings of one template
        // can be compared on. The search ROI is the whole screen and the
        // template covers it, so there is exactly one candidate position and the
        // score is not an argmin over offsets -- a search with room to wander
        // would measure a tiny mask finding SOME lucky offset instead of what
        // the mask excludes.
        [[nodiscard]]
        auto scoreAgainst(GrayTemplateImage const& templateImage, Scene scene)
            -> uint64
        {
            auto const frame = screenFrame(scene, FrameId{999});
            auto const attempt = matchTemplateOnFrame(
                frame,
                templateImage,
                wholeScreen(),
                RecognitionPolicy{.maximumPixelComparisons = 100'000}
            );
            REQUIRE(attempt.has_value());
            auto const* p_found =
                std::get_if<std::optional<TemplateMatch>>(&attempt->result);
            REQUIRE(p_found != nullptr);
            REQUIRE(p_found->has_value());
            return (*p_found)->sadScore;
        }

        TEST_CASE("A keyed crop bakes the key into the template the matcher reads")
        {
            auto harness = buildScreenHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{*std::move(harness.session), *harness.recorder};

            auto const ticket = context.openCycle();
            REQUIRE(ticket.has_value());

            auto const keyed =
                context.cycleCrop(*ticket, wholeScreen(), glyphKey(0));
            REQUIRE(keyed.has_value());

            // The crop says what the key took, in the counts probe uses.
            REQUIRE(keyed->mask.has_value());
            CHECK(keyed->mask->rectPixels == k_rectPixels);
            CHECK(keyed->mask->selectedPixels == k_glyphPixels);
            CHECK(keyed->mask->rampSelectedPixels == 0U);
            CHECK(keyed->mask->warning.empty());

            // And the same counts come back out of `probe`, measured
            // independently off the encoded bytes: the two verbs are one
            // measurement or they are two numbers that can drift.
            auto const probed = probePngRegion(
                keyed->png,
                wholeScreen(),
                glyphKey(0)
            );
            REQUIRE(probed.has_value());
            REQUIRE(probed->fullySelectedPixels.has_value());
            CHECK(*probed->fullySelectedPixels == keyed->mask->selectedPixels);

            // The mask survives the encode: decodeTemplateImage is what the
            // matcher loads every template through, and one it reads as empty
            // was never there.
            auto const decodedKeyed = decodeTemplateImage(keyed->png, "keyed");
            REQUIRE(decodedKeyed.has_value());
            REQUIRE(!decodedKeyed->mask.empty());
            CHECK(decodedKeyed->mask.size() == k_rectPixels);

            auto const unkeyed =
                context.cycleCrop(*ticket, wholeScreen(), std::nullopt);
            REQUIRE(unkeyed.has_value());
            CHECK(!unkeyed->mask.has_value());
            auto const decodedPlain =
                decodeTemplateImage(unkeyed->png, "unkeyed");
            REQUIRE(decodedPlain.has_value());
            CHECK(decodedPlain->mask.empty());

            // Two spellings of one template on one frame: the mask REACHES the
            // matcher and changes the number it reports. On the empty cell the
            // pixels that changed ARE the pixels the key kept, so this shows
            // not exclusion but the rescaling onto the template's own pixel
            // count, 320/60 of the unmasked distance. The case below shows the
            // exclusion, and the two together bracket the claim.
            auto const plainScore =
                scoreAgainst(*decodedPlain, Scene::GlyphGone);
            auto const maskedScore =
                scoreAgainst(*decodedKeyed, Scene::GlyphGone);
            CHECK(plainScore != maskedScore);
            CHECK(maskedScore > plainScore);

            // The exact numbers, because "differs" alone would pass on a mask
            // that weighted the wrong pixels.
            CHECK(plainScore == k_glyphPixels * k_glyphDistance);
            CHECK(
                maskedScore
                == (k_glyphPixels * k_glyphDistance * k_rectPixels)
                    / k_glyphPixels
            );
        }

        TEST_CASE("A masked template ignores the scenery an unmasked one matches on")
        {
            // The glyph is byte-identical to the pixels the template was cut
            // from and only the scenery behind it moved. An element that cannot
            // survive that is the measured minimap icon, which scored
            // 8885 / 8549 / 8582 for a lit node, a dim node and an EMPTY cell
            // because the grid was doing the matching
            // (docs/pitfalls/colour-key-annotation.md).
            auto harness = buildScreenHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{*std::move(harness.session), *harness.recorder};

            auto const ticket = context.openCycle();
            REQUIRE(ticket.has_value());

            auto const keyed =
                context.cycleCrop(*ticket, wholeScreen(), glyphKey(0));
            REQUIRE(keyed.has_value());
            auto const unkeyed =
                context.cycleCrop(*ticket, wholeScreen(), std::nullopt);
            REQUIRE(unkeyed.has_value());

            auto const decodedKeyed = decodeTemplateImage(keyed->png, "keyed");
            REQUIRE(decodedKeyed.has_value());
            auto const decodedPlain =
                decodeTemplateImage(unkeyed->png, "unkeyed");
            REQUIRE(decodedPlain.has_value());

            auto const plainScore =
                scoreAgainst(*decodedPlain, Scene::SceneryChanged);
            auto const maskedScore =
                scoreAgainst(*decodedKeyed, Scene::SceneryChanged);

            // Unmasked, the template reports on scenery it does not care about:
            // all 250 background pixels moved 80 grey levels, and all 250 vote.
            CHECK(plainScore == k_backgroundPixels * k_sceneryDistance);

            // Masked, none of the changed pixels carry weight. Exactly zero
            // rather than "small": the mask either excludes the background or
            // it does not.
            CHECK(maskedScore == 0U);
            CHECK(maskedScore < plainScore);

            // Unmasked, this template scores WORSE on the frame its glyph is
            // present on (20000) than on the frame it is ABSENT from (13440):
            // it prefers the wrong screen, which is how an element comes to hit
            // every state it was meant to tell apart. Masked, the same two
            // frames come out 0 and 71680, in the order it should rank them.
            auto const plainOnEmpty =
                scoreAgainst(*decodedPlain, Scene::GlyphGone);
            auto const maskedOnEmpty =
                scoreAgainst(*decodedKeyed, Scene::GlyphGone);
            CHECK(plainScore > plainOnEmpty);
            CHECK(maskedScore < maskedOnEmpty);
        }

        TEST_CASE("An unkeyed crop is the bytes it always was")
        {
            auto harness = buildScreenHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{*std::move(harness.session), *harness.recorder};

            auto const ticket = context.openCycle();
            REQUIRE(ticket.has_value());

            auto const plain =
                context.cycleCrop(*ticket, wholeScreen(), std::nullopt);
            REQUIRE(plain.has_value());

            // Rebuilt from the frame this fixture painted, through the same
            // encoder and nothing else, so the hash stops matching the moment
            // the unkeyed path grows a step.
            auto const bgra     = screenPixels(Scene::Authored);
            auto expectedRgba = std::vector<std::byte>{};
            expectedRgba.reserve(bgra.size());
            for (auto index = std::size_t{0}; index < bgra.size(); index += 4U)
            {
                expectedRgba.emplace_back(bgra[index + 2U]);
                expectedRgba.emplace_back(bgra[index + 1U]);
                expectedRgba.emplace_back(bgra[index]);
                expectedRgba.emplace_back(bgra[index + 3U]);
            }
            auto const expected = image::encodeRgbaPng(
                "expected",
                k_screenWidth,
                k_screenHeight,
                expectedRgba
            );
            REQUIRE(expected.has_value());
            auto const expectedHash = sha256(*expected);
            REQUIRE(expectedHash.has_value());
            CHECK(plain->hash.hex() == expectedHash->hex());

            // A key that takes EVERY pixel is the other end of the same claim:
            // an alpha plane of nothing but 255 is what an unkeyed crop already
            // wrote, so the bytes are the same file.
            auto const total = context.cycleCrop(
                *ticket,
                wholeScreen(),
                ProbeColourKey{
                    .red       = 0,
                    .green     = 0,
                    .blue      = 0,
                    .tolerance = k_maximumColourKeyTolerance,
                }
            );
            REQUIRE(total.has_value());
            CHECK(total->hash.hex() == plain->hash.hex());
            REQUIRE(total->mask.has_value());
            CHECK(total->mask->selectedPixels == k_rectPixels);
        }

        TEST_CASE("A key that selects nothing is refused where it was chosen")
        {
            auto harness = buildScreenHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{*std::move(harness.session), *harness.recorder};

            auto const ticket = context.openCycle();
            REQUIRE(ticket.has_value());

            // A colour this screen does not wear. Persisted, it becomes a fully
            // transparent template, and every later match fails
            // InternalInvariant on a run that no longer knows which key was
            // chosen or over what rectangle.
            auto const nothing = context.cycleCrop(
                *ticket,
                wholeScreen(),
                ProbeColourKey{
                    .red       = 1,
                    .green     = 2,
                    .blue      = 3,
                    .tolerance = 0,
                }
            );
            REQUIRE(!nothing.has_value());
            CHECK(
                automationErrorKind(nothing.error())
                == AutomationErrorKind::InvalidResource
            );

            // The sentence has to name the key and the rectangle; one that
            // named neither is a failure an agent cannot act on.
            auto const message = std::string{nothing.error().message()};
            CHECK(message.contains("1,2,3"));
            CHECK(message.contains("40x8"));

            // The control: the same rectangle with a key the screen does wear is
            // served, so the refusal is about the selection, not the region.
            auto const served =
                context.cycleCrop(*ticket, wholeScreen(), glyphKey(0));
            REQUIRE(served.has_value());
        }

        TEST_CASE("The crop warns about a mask that can measure nothing")
        {
            auto harness = buildScreenHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{*std::move(harness.session), *harness.recorder};

            // Three keys on one screen, one per verdict. A mask in the band that
            // works must not carry a warning, or the hint says nothing.
            auto const source = std::string{R"lua(
                local ticket = ctx:cycle_open()

                local _, _, glyph = explore.crop(ticket, 0, 0, 40, 8, {
                    red = )lua"} + std::to_string(k_glyphLevel) + R"lua(,
                    green = )lua" + std::to_string(k_glyphLevel) + R"lua(,
                    blue = )lua" + std::to_string(k_glyphLevel) + R"lua(,
                    removes = false,
                }, 0)
                if glyph == nil then return 0 end
                if glyph.selected_pixels ~= )lua"
                + std::to_string(k_glyphPixels) + R"lua( then return 0 end
                if glyph.rect_pixels ~= )lua"
                + std::to_string(k_rectPixels) + R"lua( then return 0 end
                if glyph.warning ~= nil then return 0 end

                -- Under the floor: ten saturated pixels find some offset in any
                -- busy region, so the element hits every screen.
                local _, _, speck = explore.crop(ticket, 0, 0, 40, 8, {
                    red = )lua" + std::to_string(k_speckRed) + R"lua(,
                    green = )lua" + std::to_string(k_speckGreen) + R"lua(,
                    blue = )lua" + std::to_string(k_speckBlue) + R"lua(,
                    removes = false,
                }, 0)
                if speck.selected_pixels ~= )lua"
                + std::to_string(k_speckPixels) + R"lua( then return 0 end
                if speck.warning == nil then return 0 end
                if string.find(speck.warning, "under the 50", 1, true) == nil then
                    return 0
                end

                -- At or above the share: a mask this large is a solid patch of
                -- one colour, and any patch of it the same size matches.
                local _, _, flat = explore.crop(ticket, 0, 0, 40, 8, {
                    red = )lua" + std::to_string(k_backgroundLevel) + R"lua(,
                    green = )lua" + std::to_string(k_backgroundLevel) + R"lua(,
                    blue = )lua" + std::to_string(k_backgroundLevel) + R"lua(,
                    removes = false,
                }, 0)
                if flat.selected_pixels ~= )lua"
                + std::to_string(k_backgroundPixels) + R"lua( then return 0 end
                if flat.warning == nil then return 0 end
                if string.find(flat.warning, "percent", 1, true) == nil then
                    return 0
                end

                -- A warning is a hint and never a refusal: all three crops
                -- produced bytes, and the agent is the one that decides.
                ctx:cycle_close(ticket)
                return 1
            )lua";

            auto const result = runExploration(context, source);
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("An unkeyed crop through the script surface carries no mask")
        {
            auto harness = buildScreenHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{*std::move(harness.session), *harness.recorder};

            constexpr std::string_view source = R"lua(
                local ticket = ctx:cycle_open()
                local blob, hash, mask = explore.crop(ticket, 0, 0, 40, 8)
                ctx:cycle_close(ticket)

                if type(blob) ~= "string" or #blob == 0 then return 0 end
                if type(hash) ~= "string" or #hash ~= 64 then return 0 end

                -- Absent, not zeroed. A zeroed handle would read as a key that
                -- took nothing, which is an answer this verb never gives.
                if mask ~= nil then return 0 end
                return 1
            )lua";

            auto const result = runExploration(context, source);
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        class ProjectDirectory final
        {
            std::filesystem::path m_path;

        public:
            explicit ProjectDirectory(std::string_view label)
                : m_path{std::filesystem::temp_directory_path() / label}
            {
                auto error = std::error_code{};
                std::filesystem::remove_all(m_path, error);
                REQUIRE(
                    std::filesystem::create_directories(
                        m_path / "assets" / "templates",
                        error
                    )
                );

                auto const model =
                    std::string{"schema = \"umbraflow-project/l2-v2\"\n"};
                auto stream = std::ofstream{
                    m_path / "page-model.toml",
                    std::ios::binary | std::ios::trunc
                };
                REQUIRE(stream.is_open());
                stream << model;
                REQUIRE(stream.good());
            }

            ProjectDirectory(ProjectDirectory const&)                    = delete;
            ProjectDirectory(ProjectDirectory&&)                         = delete;
            auto operator=(ProjectDirectory const&) -> ProjectDirectory& = delete;
            auto operator=(ProjectDirectory&&) -> ProjectDirectory&      = delete;

            ~ProjectDirectory()
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

        TEST_CASE("An authored element records the key its pixels were cut under")
        {
            auto const directory = ProjectDirectory{"uf-colour-key-round-trip"};

            auto harness = buildScreenHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            // The authoring loop as an agent drives it, with a key. The key it
            // wrote down is the one the HOST applied, tolerance included, so the
            // file records what really carved the alpha plane.
            auto const author = std::string{R"lua(
                local built  = project.load_project(ctx)
                local ticket = ctx:cycle_open()
                local measured = scribe.measure(
                    ctx,
                    ticket,
                    { x = 0, y = 0, width = 40, height = 8 },
                    {
                        red = )lua"} + std::to_string(k_glyphLevel) + R"lua(,
                        green = )lua" + std::to_string(k_glyphLevel) + R"lua(,
                        blue = )lua" + std::to_string(k_glyphLevel) + R"lua(,
                        removes = false,
                    },
                    0
                )
                ctx:cycle_close(ticket)

                if measured.key == nil then return 0 end
                if measured.key.red ~= )lua"
                + std::to_string(k_glyphLevel) + R"lua( then return 0 end
                if measured.key.tolerance ~= 0 then return 0 end

                -- The crop and the probe measured the same pixels, which is the
                -- half that used to be false: the key reached only the probe,
                -- so the counts described a mask the stored template did not
                -- have.
                if measured.mask.selected_pixels ~= 60 then return 0 end
                if measured.stats.fully_selected_pixels ~= 60 then return 0 end
                if measured.mask.warning ~= nil then return 0 end

                local element = scribe.author_element(ctx, measured, {
                    name         = "glyph",
                    capabilities = { "identify" },
                    threshold    = 9900,
                })
                if element.appearances[1].key == nil then return 0 end

                scribe.add_element(built, element)
                scribe.save(ctx, built)
                return 1
            )lua";

            auto const authored = runExploration(context, author);
            REQUIRE(authored.has_value());
            CHECK(*authored == doctest::Approx(1.0));

            // A FRESH session and a FRESH VM, so everything below comes off the
            // disk rather than out of the run that wrote it.
            auto reloadHarness = buildScreenHarness();
            REQUIRE(reloadHarness.session.has_value());
            TaskContext reloaded{
                *std::move(reloadHarness.session),
                *reloadHarness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const verify = std::string{R"lua(
                local built   = project.load_project(ctx)
                local element = built.element_by_name["glyph"]
                if element == nil then return 0 end

                local key = element.appearances[1].key
                if key == nil then return 0 end
                if key.red ~= )lua"} + std::to_string(k_glyphLevel)
                + R"lua( then return 0 end
                if key.green ~= )lua" + std::to_string(k_glyphLevel)
                + R"lua( then return 0 end
                if key.blue ~= )lua" + std::to_string(k_glyphLevel)
                + R"lua( then return 0 end
                if key.tolerance ~= 0 then return 0 end
                return 1
            )lua";

            auto const verified = runExploration(reloaded, verify);
            REQUIRE(verified.has_value());
            CHECK(*verified == doctest::Approx(1.0));

            // The key reached the FILE, not merely the model: an appearance the
            // writer dropped would reload as nil, and the check above would be
            // measuring one run's memory.
            auto stream = std::ifstream{
                directory.path() / "page-model.toml",
                std::ios::binary
            };
            REQUIRE(stream.is_open());
            auto const text = std::string{
                std::istreambuf_iterator<char>{stream},
                std::istreambuf_iterator<char>{}
            };
            CHECK(text.contains("key = [240, 240, 240, 0]"));
        }

        TEST_CASE("A key can name the backdrop instead of the mark")
        {
            // What a multi-coloured mark needs. The fixture's glyph is one
            // colour, which is what lets the two directions be compared exactly:
            // whatever the glyph key keeps, the backdrop key must take the rest.
            auto const directory = ProjectDirectory{"uf-colour-key-removes"};

            auto harness = buildScreenHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const author = std::string{R"lua(
                local built  = project.load_project(ctx)
                local ticket = ctx:cycle_open()
                local measured = scribe.measure(
                    ctx,
                    ticket,
                    { x = 0, y = 0, width = 40, height = 8 },
                    {
                        red = )lua"} + std::to_string(k_glyphLevel) + R"lua(,
                        green = )lua" + std::to_string(k_glyphLevel) + R"lua(,
                        blue = )lua" + std::to_string(k_glyphLevel) + R"lua(,
                        removes = true,
                    },
                    0
                )
                ctx:cycle_close(ticket)
                local ticket2 = ctx:cycle_open()

                -- The complement of the same key on the same pixels: the other
                -- direction takes 60 of these 320, so this one takes 260. A
                -- tolerance of zero leaves no ramp for either.
                if measured.mask.rect_pixels ~= 320 then return 0 end
                if measured.mask.selected_pixels ~= 260 then return 0 end
                if measured.mask.ramp_selected_pixels ~= 0 then return 0 end
                if measured.stats.fully_selected_pixels ~= 260 then return 0 end

                -- The direction comes back off the MASK, which is the host's
                -- report of what it applied -- not off the argument, which is
                -- what a caller believed.
                if measured.mask.key_removes ~= true then return 0 end
                if measured.key.removes ~= true then return 0 end

                -- 260 of 320 is 81 percent, well past the ceiling a KEPT mask
                -- is warned at. Not warned here, and the reason is the whole
                -- point of the flag: that ceiling says a large mask is a solid
                -- patch of one colour, and this mask is everything BUT one
                -- colour. A warning that fired on the ordinary use of a feature
                -- would teach a reader to ignore the one that matters.
                if measured.mask.warning ~= nil then return 0 end

                -- The control, so the silence above is this key's doing and not
                -- a warning nothing can trip in this direction. A colour the
                -- rect does not hold removes nothing, which leaves the template
                -- the unmasked rectangle a mask exists to avoid.
                local hollow = scribe.measure(
                    ctx,
                    ticket2,
                    { x = 0, y = 0, width = 40, height = 8 },
                    { red = 0, green = 255, blue = 0, removes = true },
                    0
                )
                ctx:cycle_close(ticket2)
                if hollow.mask.selected_pixels ~= 320 then return 0 end
                if hollow.mask.warning == nil then return 0 end
                if string.find(hollow.mask.warning, "removed almost nothing") == nil
                then
                    return 0
                end

                local element = scribe.author_element(ctx, measured, {
                    name         = "backdrop_cut",
                    capabilities = { "identify" },
                    threshold    = 9900,
                })
                scribe.add_element(built, element)
                scribe.save(ctx, built)
                return 1
            )lua";

            auto const authored = runExploration(context, author);
            REQUIRE(authored.has_value());
            CHECK(*authored == doctest::Approx(1.0));

            auto stream = std::ifstream{
                directory.path() / "page-model.toml",
                std::ios::binary
            };
            REQUIRE(stream.is_open());
            auto const text = std::string{
                std::istreambuf_iterator<char>{stream},
                std::istreambuf_iterator<char>{}
            };
            CHECK(text.contains("key = [240, 240, 240, 0]"));
            CHECK(text.contains("key_removes = true"));

            // A fresh session and a fresh VM: the direction has to survive the
            // file, because a mask re-cut the other way round selects the
            // complement of its subject and every score it reports is about the
            // wrong pixels.
            auto reloadHarness = buildScreenHarness();
            REQUIRE(reloadHarness.session.has_value());
            TaskContext reloaded{
                *std::move(reloadHarness.session),
                *reloadHarness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const verify = std::string{R"lua(
                local built = project.load_project(ctx)
                local key   = built.element_by_name["backdrop_cut"].appearances[1].key
                if key == nil then return 0 end
                if key.removes ~= true then return 0 end

                -- And writing it back is a fixpoint: a load and a save of an
                -- unchanged model reproduce the same line rather than dropping
                -- the flag or writing it twice.
                local text = project.encode(built)
                local _, count = string.gsub(text, "key_removes = true", "")
                if count ~= 1 then return 0 end
                return 1
            )lua"};

            auto const verified = runExploration(reloaded, verify);
            REQUIRE(verified.has_value());
            CHECK(*verified == doctest::Approx(1.0));
        }
    }
}
