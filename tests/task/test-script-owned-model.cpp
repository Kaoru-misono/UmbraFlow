#include "binding-fixture.hpp"

#include <task/capability-surface.hpp>
#include <task/framework-bundle.hpp>
#include <task/task-context.hpp>

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/space.hpp>

#include <engine/session.hpp>

#include <ocr/engine.hpp>
#include <ocr/text.hpp>

#include <script/engine.hpp>

#include <trace/event.hpp>
#include <trace/recorder.hpp>

#include <vision/bgra-image.hpp>

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
#include <vector>

// The layer-two half of the script-owned page model: the element and page
// vocabulary in modules/task/runtime/model.luau, the verbs in observe.luau, and
// the project file in project.luau.
//
// Everything here drives real Luau against a real session, because the whole
// claim of that layer is that a model built from Luau tables plus the four host
// primitives behaves the way the C++ model used to. A pure-Luau harness would
// prove the tables are shaped right and nothing about what they do with pixels.
namespace uf::task
{
    namespace
    {
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

            TemporaryDirectory(TemporaryDirectory const&)                    = delete;
            TemporaryDirectory(TemporaryDirectory&&)                         = delete;
            auto operator=(TemporaryDirectory const&) -> TemporaryDirectory& = delete;
            auto operator=(TemporaryDirectory&&) -> TemporaryDirectory&      = delete;

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

        auto writeFile(
            std::filesystem::path const& path,
            std::span<std::byte const>   bytes
        ) -> void
        {
            auto stream = std::ofstream{path, std::ios::binary};
            REQUIRE(stream.is_open());
            // Byte by byte rather than one write of a recast pointer: a test is
            // not a place to open a cast boundary, and a PNG template is small.
            for (auto const value : bytes)
            {
                stream.put(static_cast<char>(value));
            }
            REQUIRE(stream.good());
        }

        [[nodiscard]]
        auto readFileText(std::filesystem::path const& path) -> std::string
        {
            auto stream = std::ifstream{path, std::ios::binary};
            REQUIRE(stream.is_open());
            return std::string{
                std::istreambuf_iterator<char>{stream},
                std::istreambuf_iterator<char>{},
            };
        }

        // The one-by-one grey templates every model in this file is built from,
        // written into the project directory under the names the Luau side reads
        // them back by. A model's appearances are project files, so seeding them
        // is what makes template_load reachable from a script at all.
        auto seedTemplates(std::filesystem::path const& root) -> void
        {
            for (auto const gray :
                 {uint8{2}, uint8{3}, uint8{5}, uint8{20}, uint8{40}})
            {
                auto const blob = encodedTemplate(gray).pngBytes;
                writeFile(
                    root / ("gray" + std::to_string(gray) + ".png"),
                    std::span<std::byte const>{blob}
                );
            }
        }

        // A fake OCR adapter that answers one line, so the read verb has
        // something to compare against. It refuses Block exactly as the shipped
        // adapter does, so a host that quietly asked for Block fails here too.
        class FakeOcrEngine final : public ocr::IOcrEngine
        {
            ocr::Readout m_readout;

        public:
            explicit FakeOcrEngine(ocr::Readout readout) noexcept
                : m_readout{std::move(readout)}
            {
            }

            [[nodiscard]]
            auto identity() const noexcept -> std::string_view override
            {
                return "fake/single-line";
            }

            [[nodiscard]]
            auto read(BgraImage const& /*image*/, ocr::ReadSpec const& spec)
                -> Result<ocr::Readout> override
            {
                if (spec.layout == ocr::TextLayout::Block)
                {
                    return fail(
                        AutomationErrorKind::UnsupportedCapability,
                        "this adapter does not run the detection model"
                    );
                }
                return m_readout;
            }
        };

        [[nodiscard]]
        auto oneLineReadout(std::string text, uint32 confidenceBp) -> ocr::Readout
        {
            auto readout = ocr::Readout{};
            readout.lines.emplace_back(
                ocr::TextLine{
                    .text         = std::move(text),
                    .bounds       = anno::test::pixelRect(0, 0, 3, 1),
                    .confidenceBp = confidenceBp,
                }
            );
            return readout;
        }

        struct HarnessSpec final
        {
            // One entry per frame, three grey bytes each. The last frame repeats
            // once the source is exhausted, so a wait loop cannot run dry.
            std::vector<std::vector<std::byte>> framePixels{};
            uint64                              maximumPixelComparisons{1'000};
            std::unique_ptr<ocr::IOcrEngine>    ocrEngine{};
            std::filesystem::path               projectRoot{};
        };

        struct Harness final
        {
            std::unique_ptr<trace::TraceRecorder> recorder;
            Result<engine::EngineSession>         session;
            CapabilitySurface                     surface;
            CountingActionSink*                   clicks;
            FakeFrameSource*                      frames;
        };

        [[nodiscard]]
        auto buildHarness(HarnessSpec spec) -> Harness
        {
            auto parts = singlePageRuntime();
            auto surface =
                CapabilitySurface::create(parts.loaded.runtime.manifest().catalog());
            REQUIRE(surface.has_value());

            auto const fingerprint = anno::test::fingerprint(3, 1, 96, 96);
            auto       frames  = std::vector<Frame>{};
            auto       frameId = uint64{500};
            for (auto& pixels : spec.framePixels)
            {
                frames.emplace_back(
                    grayFrame(fingerprint, std::move(pixels), FrameId{frameId})
                );
                ++frameId;
            }

            auto frameSource       = std::make_unique<FakeFrameSource>(std::move(frames));
            auto* const p_frames   = frameSource.get();
            auto actionSink        = std::make_unique<CountingActionSink>();
            auto* const p_clicks   = actionSink.get();
            auto recorder          = std::make_unique<trace::TraceRecorder>(
                std::make_unique<DiscardingTraceSink>(),
                k_fixtureRunId,
                k_fixtureGenerationId,
                trace::FrontEnd::Task
            );
            auto session = engine::EngineSession::create(
                std::move(parts.loaded),
                std::move(frameSource),
                std::move(actionSink),
                *recorder,
                engine::EngineSessionConfig{
                    .liveFingerprint         = parts.fingerprint,
                    .maximumPixelComparisons = spec.maximumPixelComparisons,
                    .recognitionTimeout      = std::chrono::duration_cast<
                        MonotonicInstant::Duration
                    >(std::chrono::seconds{5}),
                },
                std::move(spec.ocrEngine)
            );
            return Harness{
                .recorder = std::move(recorder),
                .session  = std::move(session),
                .surface  = *std::move(surface),
                .clicks   = p_clicks,
                .frames   = p_frames,
            };
        }

        // The VM a layer-three script would boot, plus the framework modules the
        // script-owned page model added, published under their own names.
        //
        // They are published HERE rather than in frameworkProjectGlobals()
        // because wiring them into every task VM is a host decision that belongs
        // with whoever lands the layer-three loader; the modules themselves are
        // in the bundle already, and a test that publishes them is exactly what
        // a project environment will do once that wiring exists.
        //
        // `mint` is deliberately NOT among them. It is the spec-checking toolkit
        // the constructors share, so every one of its functions is reachable
        // through a constructor that already calls it; publishing it would put a
        // second, unchecked way to shape a model in front of a project script
        // for no gain.
        [[nodiscard]]
        auto modelVmConfig(CapabilitySurface const& surface, TaskContext& context)
            -> script::EngineConfig
        {
            auto config = taskVmConfig(surface, context);
            config.frameworkProjectGlobals.emplace_back("model");
            config.frameworkProjectGlobals.emplace_back("navigation");
            config.frameworkProjectGlobals.emplace_back("observe");
            config.frameworkProjectGlobals.emplace_back("project");
            return config;
        }

        [[nodiscard]]
        auto runModel(TaskContext& context, Harness& harness, std::string_view source)
            -> Result<double>
        {
            auto engine = script::Engine::create(modelVmConfig(harness.surface, context));
            REQUIRE(engine.has_value());
            return engine->runNumber(source, "script-owned-model");
        }

        // Frames, by what the model built on them sees.
        //
        // The fixture frame is three grey pixels. gray 2 sits at x = 0 and gray 5
        // at x = 1 on the resolving frame, so a one-by-one template of either
        // grey is an element that hits at a known pixel and misses on a frame
        // that does not carry it.
        [[nodiscard]]
        auto pixels(uint8 first, uint8 second, uint8 third) -> std::vector<std::byte>
        {
            return std::vector<std::byte>{asByte(first), asByte(second), asByte(third)};
        }

        // The prelude every behaviour script starts from: one helper that turns a
        // project file name into a template handle.
        constexpr std::string_view k_prelude = R"lua(
            local function template(name)
                return ctx:template_load(ctx:project_read(name))
            end
        )lua";

        [[nodiscard]]
        auto script(std::string_view body) -> std::string
        {
            return std::string{k_prelude} + std::string{body};
        }

        // A constructor refusal, expressed as the smallest script that proves it:
        // the call raises, and the sentence it raised names the thing that is
        // wrong. Returning 1 only on both counts is what keeps a refusal for the
        // WRONG reason from passing.
        //
        // The fragment goes in a long-bracket literal because the sentences under
        // test quote the field names they are about, so half of them carry a
        // quote character of one kind or the other.
        [[nodiscard]]
        auto refusalScript(std::string_view body, std::string_view fragment)
            -> std::string
        {
            return script(
                std::string{"local ok, err = pcall(function()\n"} + std::string{body}
                + "\nend)\n"
                  "if ok then return 0 end\n"
                  "if type(err) ~= 'string' then return 0 end\n"
                  "if string.find(err, [==["
                + std::string{fragment}
                + "]==], 1, true) == nil then return 0 end\n"
                  "return 1\n"
            );
        }

        struct Refusal final
        {
            std::string_view label;
            std::string_view body;
            std::string_view fragment;
        };

        // Element.new's invariants, one row each. Every row is otherwise a valid
        // element, so exactly one check can be the one that fires: delete that
        // check and the row's call succeeds, `ok` is true, and the row goes red.
        [[nodiscard]]
        auto elementRefusals() -> std::vector<Refusal>
        {
            return {
                Refusal{
                    .label = "an unnamed element",
                    .body  = R"lua(
                        return model.Element.new{
                            name = "",
                            capabilities = { "interact" },
                            rect = { x = 0, y = 0, width = 3, height = 1 },
                        }
                    )lua",
                    .fragment = "non-empty name",
                },
                Refusal{
                    .label = "a field this layer does not own",
                    .body  = R"lua(
                        return model.Element.new{
                            name = "back",
                            capabilities = { "interact" },
                            rect = { x = 0, y = 0, width = 3, height = 1 },
                            my_grid_stride = 5,
                        }
                    )lua",
                    .fragment = "extra = { my_grid_stride",
                },
                Refusal{
                    .label = "an element with no capability at all",
                    .body  = R"lua(
                        return model.Element.new{
                            name = "back",
                            capabilities = {},
                            rect = { x = 0, y = 0, width = 3, height = 1 },
                        }
                    )lua",
                    .fragment = "at least one capability",
                },
                Refusal{
                    .label = "a capability that is not one of the three",
                    .body  = R"lua(
                        return model.Element.new{
                            name = "back",
                            capabilities = { "interact", "scroll" },
                            rect = { x = 0, y = 0, width = 3, height = 1 },
                        }
                    )lua",
                    .fragment = "is not a capability",
                },
                Refusal{
                    .label = "the same capability twice",
                    .body  = R"lua(
                        return model.Element.new{
                            name = "back",
                            capabilities = { "interact", "interact" },
                            rect = { x = 0, y = 0, width = 3, height = 1 },
                        }
                    )lua",
                    .fragment = "twice",
                },
                Refusal{
                    .label = "a rectangle with no height",
                    .body  = R"lua(
                        return model.Element.new{
                            name = "back",
                            capabilities = { "interact" },
                            rect = { x = 0, y = 0, width = 3, height = 0 },
                        }
                    )lua",
                    .fragment = "at least one pixel wide",
                },
                Refusal{
                    .label = "identify with no appearances to identify by",
                    .body  = R"lua(
                        return model.Element.new{
                            name = "slot",
                            capabilities = { "identify", "interact" },
                            rect = { x = 0, y = 0, width = 3, height = 1 },
                            appearances = {},
                        }
                    )lua",
                    .fragment = "declares identify with no appearances",
                },
                Refusal{
                    .label = "two verification sources at once",
                    .body  = R"lua(
                        return model.Element.new{
                            name = "sortie",
                            capabilities = { "interact", "read" },
                            rect = { x = 0, y = 0, width = 3, height = 1 },
                            expected_text = "battle",
                            appearances = {
                                {
                                    name = "only",
                                    source = "gray2.png",
                                    template = template("gray2.png"),
                                    threshold = 10000,
                                },
                            },
                        }
                    )lua",
                    .fragment = "both appearances and expected_text",
                },
                Refusal{
                    .label = "expected text on an element nothing may read",
                    .body  = R"lua(
                        return model.Element.new{
                            name = "sortie",
                            capabilities = { "interact" },
                            rect = { x = 0, y = 0, width = 3, height = 1 },
                            expected_text = "battle",
                        }
                    )lua",
                    .fragment = "does not declare the read capability",
                },
                Refusal{
                    .label = "an appearance with no source file",
                    .body  = R"lua(
                        return model.Element.new{
                            name = "back",
                            capabilities = { "interact" },
                            rect = { x = 0, y = 0, width = 3, height = 1 },
                            appearances = {
                                { name = "only", template = template("gray2.png"), threshold = 10000 },
                            },
                        }
                    )lua",
                    .fragment = "needs source =",
                },
                Refusal{
                    .label = "an appearance holding the blob instead of the handle",
                    .body  = R"lua(
                        return model.Element.new{
                            name = "back",
                            capabilities = { "interact" },
                            rect = { x = 0, y = 0, width = 3, height = 1 },
                            appearances = {
                                {
                                    name = "only",
                                    source = "gray2.png",
                                    template = ctx:project_read("gray2.png"),
                                    threshold = 10000,
                                },
                            },
                        }
                    )lua",
                    .fragment = "not the blob itself",
                },
                Refusal{
                    .label = "a threshold outside basis points",
                    .body  = R"lua(
                        return model.Element.new{
                            name = "back",
                            capabilities = { "interact" },
                            rect = { x = 0, y = 0, width = 3, height = 1 },
                            appearances = {
                                {
                                    name = "only",
                                    source = "gray2.png",
                                    template = template("gray2.png"),
                                    threshold = 10001,
                                },
                            },
                        }
                    )lua",
                    .fragment = "between 0 and 10000",
                },
                Refusal{
                    .label = "two appearances sharing one name",
                    .body  = R"lua(
                        return model.Element.new{
                            name = "back",
                            capabilities = { "interact" },
                            rect = { x = 0, y = 0, width = 3, height = 1 },
                            appearances = {
                                {
                                    name = "twin",
                                    source = "gray2.png",
                                    template = template("gray2.png"),
                                    threshold = 10000,
                                },
                                {
                                    name = "twin",
                                    source = "gray5.png",
                                    template = template("gray5.png"),
                                    threshold = 10000,
                                },
                            },
                        }
                    )lua",
                    .fragment = "repeats the appearance name",
                },
            };
        }

        // A valid anchor and a valid click target, so a page refusal row differs
        // from a good page in exactly one field.
        constexpr std::string_view k_pagePrelude = R"lua(
            local anchor = model.Element.new{
                name = "anchor",
                capabilities = { "identify", "interact" },
                rect = { x = 0, y = 0, width = 3, height = 1 },
                appearances = {
                    {
                        name = "on_dark",
                        source = "gray2.png",
                        template = template("gray2.png"),
                        threshold = 10000,
                    },
                },
            }
            local slot = model.Element.new{
                name = "slot",
                capabilities = { "interact", "read" },
                rect = { x = 0, y = 0, width = 3, height = 1 },
                expected_text = "battle",
            }
            local function anchorRow(overrides)
                local row = {
                    element = anchor,
                    holding = "owned",
                    exercised = { "identify" },
                    identify = "required",
                }
                for key, value in overrides do
                    row[key] = value
                end
                return row
            end
        )lua";

        [[nodiscard]]
        auto pageRefusals() -> std::vector<Refusal>
        {
            return {
                Refusal{
                    .label = "a page with no references",
                    .body  = R"lua(
                        return model.Page.new{ name = "battle", references = {} }
                    )lua",
                    .fragment = "has no references",
                },
                Refusal{
                    .label = "a page with no required identify reference",
                    .body  = R"lua(
                        return model.Page.new{
                            name = "battle",
                            references = { anchorRow({ identify = "forbidden" }) },
                        }
                    )lua",
                    .fragment = "no required identify reference",
                },
                Refusal{
                    .label = "an element table nobody minted",
                    .body  = R"lua(
                        return model.Page.new{
                            name = "battle",
                            references = {
                                {
                                    element = {
                                        name = "anchor",
                                        capabilities = { identify = true },
                                        rect = { x = 0, y = 0, width = 3, height = 1 },
                                        appearances = {},
                                    },
                                    holding = "owned",
                                    exercised = { "identify" },
                                    identify = "required",
                                },
                            },
                        }
                    )lua",
                    .fragment = "an element built by Element.new",
                },
                Refusal{
                    .label = "a holding that is neither owned nor referenced",
                    .body  = R"lua(
                        return model.Page.new{
                            name = "battle",
                            references = { anchorRow({ holding = "borrowed" }) },
                        }
                    )lua",
                    .fragment = R"(holding = "owned" or "referenced")",
                },
                Refusal{
                    .label = "exercising more than the element declares",
                    .body  = R"lua(
                        return model.Page.new{
                            name = "battle",
                            references = {
                                anchorRow({ exercised = { "identify", "read" } }),
                            },
                        }
                    )lua",
                    .fragment = "does not declare it",
                },
                Refusal{
                    .label = "identify without a polarity",
                    .body  = R"lua(
                        return model.Page.new{
                            name = "battle",
                            references = {
                                {
                                    element = anchor,
                                    holding = "owned",
                                    exercised = { "identify" },
                                },
                            },
                        }
                    )lua",
                    .fragment = "exercises identify, so it needs",
                },
                Refusal{
                    .label = "a polarity on a reference that is not a signature",
                    .body  = R"lua(
                        return model.Page.new{
                            name = "battle",
                            references = {
                                anchorRow({}),
                                {
                                    element = slot,
                                    holding = "owned",
                                    exercised = { "interact" },
                                    identify = "required",
                                },
                            },
                        }
                    )lua",
                    .fragment = "sets a polarity without exercising identify",
                },
                Refusal{
                    .label = "a pin on a reference that never clicks",
                    .body  = R"lua(
                        return model.Page.new{
                            name = "battle",
                            references = {
                                anchorRow({ pinned_appearance = "on_dark" }),
                            },
                        }
                    )lua",
                    .fragment = "pins an appearance without exercising interact",
                },
                Refusal{
                    .label = "a pin naming an appearance the element does not have",
                    .body  = R"lua(
                        return model.Page.new{
                            name = "battle",
                            references = {
                                anchorRow({
                                    exercised = { "identify", "interact" },
                                    pinned_appearance = "on_light",
                                }),
                            },
                        }
                    )lua",
                    .fragment = "which element 'anchor' does not have",
                },
                Refusal{
                    .label = "an identify reference refining the search rectangle",
                    .body  = R"lua(
                        return model.Page.new{
                            name = "battle",
                            references = {
                                anchorRow({
                                    rect_override = { x = 0, y = 0, width = 1, height = 1 },
                                }),
                            },
                        }
                    )lua",
                    .fragment = "exercises identify and also refines",
                },
                Refusal{
                    .label = "two references to one element",
                    .body  = R"lua(
                        return model.Page.new{
                            name = "battle",
                            references = {
                                anchorRow({}),
                                {
                                    element = anchor,
                                    holding = "referenced",
                                    exercised = { "interact" },
                                },
                            },
                        }
                    )lua",
                    .fragment = "second reference to element 'anchor'",
                },
                Refusal{
                    .label = "a page field this layer does not own",
                    .body  = R"lua(
                        return model.Page.new{
                            name = "battle",
                            references = { anchorRow({}) },
                            my_page_kind = "overlay",
                        }
                    )lua",
                    .fragment = "extra = { my_page_kind",
                },
                Refusal{
                    .label = "an overlay flag that is not a boolean",
                    .body  = R"lua(
                        return model.Page.new{
                            name = "battle",
                            references = { anchorRow({}) },
                            overlay = "yes",
                        }
                    )lua",
                    .fragment = "overlay must be true or false",
                },
                Refusal{
                    .label = "an interrupt flag that is not a boolean",
                    .body  = R"lua(
                        return model.Page.new{
                            name = "battle",
                            references = { anchorRow({}) },
                            interrupt = 1,
                        }
                    )lua",
                    .fragment = "interrupt must be true or false",
                },
            };
        }

        [[nodiscard]]
        auto refusalHarness(std::filesystem::path const& root) -> Harness
        {
            return buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(2, 5, 0)},
                    .projectRoot = root,
                }
            );
        }

        TEST_CASE("Element.new refuses every malformed element by name")
        {
            auto const directory = TemporaryDirectory{"uf-model-element-refusals"};
            seedTemplates(directory.path());

            for (auto const& refusal : elementRefusals())
            {
                auto built = refusalHarness(directory.path());
                REQUIRE(built.session.has_value());
                TaskContext context{
                    *std::move(built.session),
                    *built.recorder,
                    TaskContextConfig{.projectRoot = directory.path()},
                };

                INFO("Element.new refuses ", refusal.label);
                auto const result = runModel(
                    context,
                    built,
                    refusalScript(refusal.body, refusal.fragment)
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }
        }

        TEST_CASE("Page.new refuses every malformed page by name")
        {
            auto const directory = TemporaryDirectory{"uf-model-page-refusals"};
            seedTemplates(directory.path());

            for (auto const& refusal : pageRefusals())
            {
                auto built = refusalHarness(directory.path());
                REQUIRE(built.session.has_value());
                TaskContext context{
                    *std::move(built.session),
                    *built.recorder,
                    TaskContextConfig{.projectRoot = directory.path()},
                };

                INFO("Page.new refuses ", refusal.label);
                auto const body =
                    std::string{k_pagePrelude} + std::string{refusal.body};
                auto const result =
                    runModel(context, built, refusalScript(body, refusal.fragment));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }
        }

        TEST_CASE("Page.new accepts an element layer three derived from ours")
        {
            // The inheritance ruling, made observable: layer three extends a
            // layer-two element with setmetatable and __index, and the page that
            // references the derived table is built rather than refused. Tighten
            // the check to plain identity and this goes red, which is what keeps
            // the mint from becoming a ban on extending anything.
            auto const directory = TemporaryDirectory{"uf-model-derived-element"};
            seedTemplates(directory.path());
            auto built = refusalHarness(directory.path());
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const source = script(R"lua(
                local base = model.Element.new{
                    name = "anchor",
                    capabilities = { "identify" },
                    rect = { x = 0, y = 0, width = 3, height = 1 },
                    appearances = {
                        {
                            name = "on_dark",
                            source = "gray2.png",
                            template = template("gray2.png"),
                            threshold = 10000,
                        },
                    },
                }
                local derived = setmetatable(
                    { my_own_note = "layer three" },
                    { __index = base }
                )
                local page = model.Page.new{
                    name = "battle",
                    references = {
                        {
                            element = derived,
                            holding = "owned",
                            exercised = { "identify" },
                            identify = "required",
                        },
                    },
                }
                if page.references[1].element.name ~= "anchor" then return 0 end
                if page.references[1].element.my_own_note ~= "layer three" then return 0 end
                return 1
            )lua");

            auto const result = runModel(context, built, source);
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        // The model both behaviour halves are written against: an anchor with two
        // appearances, a page-positioned slot, and a page that requires the
        // anchor, pins its dark appearance for clicking, and exercises the slot.
        constexpr std::string_view k_battleModel = R"lua(
            local anchor = model.Element.new{
                name = "anchor",
                capabilities = { "identify", "interact" },
                rect = { x = 0, y = 0, width = 3, height = 1 },
                appearances = {
                    {
                        name = "on_dark",
                        source = "gray2.png",
                        template = template("gray2.png"),
                        threshold = 10000,
                    },
                    {
                        name = "on_light",
                        source = "gray5.png",
                        template = template("gray5.png"),
                        threshold = 10000,
                    },
                },
            }
            local slot = model.Element.new{
                name = "slot",
                capabilities = { "interact", "read" },
                rect = { x = 0, y = 0, width = 3, height = 1 },
                expected_text = "battle",
            }
            local battle = model.Page.new{
                name = "battle",
                references = {
                    {
                        element = anchor,
                        holding = "owned",
                        exercised = { "identify", "interact" },
                        identify = "required",
                        pinned_appearance = "on_dark",
                    },
                    {
                        element = slot,
                        holding = "owned",
                        exercised = { "interact", "read" },
                    },
                },
            }
        )lua";

        [[nodiscard]]
        auto battleScript(std::string_view body) -> std::string
        {
            return script(std::string{k_battleModel} + std::string{body});
        }

        TEST_CASE("resolve_page answers on the required and forbidden clauses")
        {
            auto const directory = TemporaryDirectory{"uf-model-resolve"};
            seedTemplates(directory.path());

            SUBCASE("a frame carrying the required anchor resolves the page")
            {
                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(2, 5, 0)},
                        .projectRoot = directory.path(),
                    }
                );
                REQUIRE(built.session.has_value());
                TaskContext context{
                    *std::move(built.session),
                    *built.recorder,
                    TaskContextConfig{.projectRoot = directory.path()},
                };

                auto const result = runModel(context, built, battleScript(R"lua(
                    local ticket = ctx:cycle_open()
                    local ok, why = observe.resolve_page(ctx, ticket, battle)
                    ctx:cycle_close(ticket)
                    if ok ~= true then return 0 end
                    if why ~= nil then return 0 end
                    return 1
                )lua"));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("a frame without it says which clause failed")
            {
                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(0, 0, 0)},
                        .projectRoot = directory.path(),
                    }
                );
                REQUIRE(built.session.has_value());
                TaskContext context{
                    *std::move(built.session),
                    *built.recorder,
                    TaskContextConfig{.projectRoot = directory.path()},
                };

                auto const result = runModel(context, built, battleScript(R"lua(
                    local ticket = ctx:cycle_open()
                    local ok, why = observe.resolve_page(ctx, ticket, battle)
                    ctx:cycle_close(ticket)
                    if ok ~= false then return 0 end
                    if string.find(why, "requires element 'anchor'", 1, true) == nil then
                        return 0
                    end
                    return 1
                )lua"));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("a forbidden element that is present refuses the page")
            {
                // The overlay case, in miniature: the page is only this page
                // while the grey-5 element is absent. Delete the forbidden branch
                // and the page resolves on a frame that carries it, so this goes
                // red.
                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(2, 5, 0)},
                        .projectRoot = directory.path(),
                    }
                );
                REQUIRE(built.session.has_value());
                TaskContext context{
                    *std::move(built.session),
                    *built.recorder,
                    TaskContextConfig{.projectRoot = directory.path()},
                };

                auto const result = runModel(context, built, script(R"lua(
                    local anchor = model.Element.new{
                        name = "anchor",
                        capabilities = { "identify" },
                        rect = { x = 0, y = 0, width = 3, height = 1 },
                        appearances = {
                            {
                                name = "on_dark",
                                source = "gray2.png",
                                template = template("gray2.png"),
                                threshold = 10000,
                            },
                        },
                    }
                    local overlay = model.Element.new{
                        name = "overlay",
                        capabilities = { "identify" },
                        rect = { x = 0, y = 0, width = 3, height = 1 },
                        appearances = {
                            {
                                name = "lit",
                                source = "gray5.png",
                                template = template("gray5.png"),
                                threshold = 10000,
                            },
                        },
                    }
                    local plain = model.Page.new{
                        name = "plain_battle",
                        references = {
                            {
                                element = anchor,
                                holding = "owned",
                                exercised = { "identify" },
                                identify = "required",
                            },
                            {
                                element = overlay,
                                holding = "referenced",
                                exercised = { "identify" },
                                identify = "forbidden",
                            },
                        },
                    }
                    local ticket = ctx:cycle_open()
                    local ok, why = observe.resolve_page(ctx, ticket, plain)
                    ctx:cycle_close(ticket)
                    if ok ~= false then return 0 end
                    if string.find(why, "forbids element 'overlay'", 1, true) == nil then
                        return 0
                    end
                    return 1
                )lua"));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }
        }

        TEST_CASE("identify folds across every appearance and ignores the page's pin")
        {
            // The correction that had to be made when the C++ model was built and
            // survives the move to Luau: the identify sweep runs BEFORE the page
            // is known, so it cannot consult a page's pin. The page below pins
            // on_dark; the frame carries only the on_light pixels; the page must
            // still resolve.
            //
            // Honour the pin inside resolve_page and this goes red immediately,
            // which is the whole reason the pin is documented as interact-only.
            auto const directory = TemporaryDirectory{"uf-model-identify-fold"};
            seedTemplates(directory.path());
            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(0, 5, 0)},
                    .projectRoot = directory.path(),
                }
            );
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runModel(context, built, battleScript(R"lua(
                local ticket = ctx:cycle_open()
                local ok = observe.resolve_page(ctx, ticket, battle)
                if ok ~= true then return 0 end

                -- The same cycle, the same pin: the CLICK path honours it, so a
                -- find of the anchor on this frame comes back empty.
                local hit = observe.find(ctx, ticket, battle, anchor)
                ctx:cycle_close(ticket)
                if hit ~= nil then return 0 end
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("A match stopped by its budget is a control error, never a miss")
        {
            // The fail-closed rule, one layer up. cycle_match raises when a
            // budget stopped the search, and no verb here may turn that into
            // "the element is not on screen": a page that reported itself absent
            // because a number ran out would make the model's answer a function
            // of a configuration value.
            //
            // Wrap the cycle_match call in observe.foldAppearances with a pcall
            // and this case goes red -- resolve_page starts returning false.
            auto const directory = TemporaryDirectory{"uf-model-budget-stop"};
            seedTemplates(directory.path());
            auto built = buildHarness(
                HarnessSpec{
                    .framePixels             = {pixels(2, 5, 0)},
                    .maximumPixelComparisons = 0,
                    .projectRoot             = directory.path(),
                }
            );
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runModel(context, built, battleScript(R"lua(
                local ticket = ctx:cycle_open()
                local ok, err = ctx:try(function()
                    return observe.resolve_page(ctx, ticket, battle)
                end)
                if ok ~= false then return 0 end
                if type(err) ~= 'userdata' then return 0 end
                if err.kind ~= uf.errors.recognition_incomplete then return 0 end
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("find folds appearances by margin and lets declaration order break ties only")
        {
            auto const directory = TemporaryDirectory{"uf-model-appearance-fold"};
            seedTemplates(directory.path());

            SUBCASE("the better margin wins even when a worse appearance is declared first")
            {
                // The case that turns "declaration order only breaks ties" from a
                // sentence into a fact. On a [2, 5, 0] frame the grey-3 template
                // is declared first and DOES clear its own threshold, at x = 0
                // with a score of one; the grey-5 template scores zero at x = 1.
                // Under "first past the threshold wins" the hit would name
                // wide_first and point at x = 0, and a click would land on the
                // wrong pixel with nothing downstream noticing.
                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(2, 5, 0)},
                        .projectRoot = directory.path(),
                    }
                );
                REQUIRE(built.session.has_value());
                TaskContext context{
                    *std::move(built.session),
                    *built.recorder,
                    TaskContextConfig{.projectRoot = directory.path()},
                };

                auto const result = runModel(context, built, script(R"lua(
                    local speed = model.Element.new{
                        name = "speed",
                        capabilities = { "identify", "interact" },
                        rect = { x = 0, y = 0, width = 3, height = 1 },
                        appearances = {
                            {
                                name = "wide_first",
                                source = "gray3.png",
                                template = template("gray3.png"),
                                threshold = 9000,
                            },
                            {
                                name = "exact_second",
                                source = "gray5.png",
                                template = template("gray5.png"),
                                threshold = 9000,
                            },
                        },
                    }
                    local page = model.Page.new{
                        name = "battle",
                        references = {
                            {
                                element = speed,
                                holding = "owned",
                                exercised = { "identify", "interact" },
                                identify = "required",
                            },
                        },
                    }
                    local ticket = ctx:cycle_open()
                    local hit = observe.find(ctx, ticket, page, speed)
                    ctx:cycle_close(ticket)
                    if hit == nil then return 0 end
                    if hit.appearance ~= "exact_second" then return 0 end
                    if hit.x ~= 1 then return 0 end
                    if hit.score ~= 0 then return 0 end
                    if hit.positioned_by ~= "pixels" then return 0 end
                    return 1
                )lua"));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("an exact tie goes to the appearance declared first")
            {
                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(2, 5, 0)},
                        .projectRoot = directory.path(),
                    }
                );
                REQUIRE(built.session.has_value());
                TaskContext context{
                    *std::move(built.session),
                    *built.recorder,
                    TaskContextConfig{.projectRoot = directory.path()},
                };

                auto const result = runModel(context, built, script(R"lua(
                    local twin = model.Element.new{
                        name = "twin",
                        capabilities = { "identify", "interact" },
                        rect = { x = 0, y = 0, width = 3, height = 1 },
                        appearances = {
                            {
                                name = "first",
                                source = "gray2.png",
                                template = template("gray2.png"),
                                threshold = 10000,
                            },
                            {
                                name = "second",
                                source = "gray2.png",
                                template = template("gray2.png"),
                                threshold = 10000,
                            },
                        },
                    }
                    local page = model.Page.new{
                        name = "battle",
                        references = {
                            {
                                element = twin,
                                holding = "owned",
                                exercised = { "identify", "interact" },
                                identify = "required",
                            },
                        },
                    }
                    local ticket = ctx:cycle_open()
                    local hit = observe.find(ctx, ticket, page, twin)
                    ctx:cycle_close(ticket)
                    if hit == nil then return 0 end
                    if hit.appearance ~= "first" then return 0 end
                    return 1
                )lua"));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }
        }

        TEST_CASE("A page-positioned element is located by the page and clicked at its centre")
        {
            auto const directory = TemporaryDirectory{"uf-model-page-positioned"};
            seedTemplates(directory.path());
            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(2, 5, 0)},
                    .projectRoot = directory.path(),
                }
            );
            REQUIRE(built.session.has_value());
            auto* const p_clicks = built.clicks;
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runModel(context, built, battleScript(R"lua(
                local ticket = ctx:cycle_open()
                if observe.resolve_page(ctx, ticket, battle) ~= true then return 0 end
                local hit = observe.find(ctx, ticket, battle, slot)
                if hit == nil then return 0 end
                if hit.positioned_by ~= "page" then return 0 end
                if hit.match ~= nil then return 0 end
                if hit.click_x ~= 1 or hit.click_y ~= 0 then return 0 end
                observe.click(ctx, ticket, hit)
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
            CHECK(p_clicks->clickCount() == 1U);
        }

        TEST_CASE("A pixel hit clicks through the match the same cycle produced")
        {
            auto const directory = TemporaryDirectory{"uf-model-pixel-click"};
            seedTemplates(directory.path());
            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(2, 5, 0)},
                    .projectRoot = directory.path(),
                }
            );
            REQUIRE(built.session.has_value());
            auto* const p_clicks = built.clicks;
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runModel(context, built, battleScript(R"lua(
                local ticket = ctx:cycle_open()
                if observe.resolve_page(ctx, ticket, battle) ~= true then return 0 end
                local hit = observe.find(ctx, ticket, battle, anchor)
                if hit == nil then return 0 end
                if hit.appearance ~= "on_dark" then return 0 end
                if hit.x ~= 0 then return 0 end
                observe.click(ctx, ticket, hit)
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
            CHECK(p_clicks->clickCount() == 1U);
        }

        TEST_CASE("Finding is allowed without interact and clicking is not")
        {
            // The enforcement that moved out of C++ with the model. A state
            // element the page only identifies must stay queryable -- reading
            // which appearance hit is how a script knows what state the game is
            // in -- while the click is refused HERE, by the trusted framework,
            // because the host's four remaining guarantees no longer include
            // "the page authorises this element".
            //
            // Drop the interact test in observe.click and the click is delivered,
            // so the counter below goes to one and this case goes red.
            auto const directory = TemporaryDirectory{"uf-model-find-without-click"};
            seedTemplates(directory.path());
            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(2, 5, 0)},
                    .projectRoot = directory.path(),
                }
            );
            REQUIRE(built.session.has_value());
            auto* const p_clicks = built.clicks;
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runModel(context, built, script(R"lua(
                local state = model.Element.new{
                    name = "auto_button",
                    capabilities = { "identify" },
                    rect = { x = 0, y = 0, width = 3, height = 1 },
                    appearances = {
                        {
                            name = "auto_on",
                            source = "gray2.png",
                            template = template("gray2.png"),
                            threshold = 10000,
                        },
                    },
                }
                local page = model.Page.new{
                    name = "battle",
                    references = {
                        {
                            element = state,
                            holding = "owned",
                            exercised = { "identify" },
                            identify = "required",
                        },
                    },
                }
                local ticket = ctx:cycle_open()
                local hit = observe.find(ctx, ticket, page, state)
                if hit == nil then return 0 end
                if hit.appearance ~= "auto_on" then return 0 end
                if hit.interact ~= false then return 0 end

                local ok, err = pcall(function()
                    observe.click(ctx, ticket, hit)
                end)
                ctx:cycle_close(ticket)
                if ok then return 0 end
                if string.find(err, "does not exercise interact", 1, true) == nil then
                    return 0
                end
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
            CHECK(p_clicks->clickCount() == 0U);
        }

        TEST_CASE("observe.click refuses a hit no find on this layer produced")
        {
            auto const directory = TemporaryDirectory{"uf-model-forged-hit"};
            seedTemplates(directory.path());
            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(2, 5, 0)},
                    .projectRoot = directory.path(),
                }
            );
            REQUIRE(built.session.has_value());
            auto* const p_clicks = built.clicks;
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runModel(context, built, battleScript(R"lua(
                local ticket = ctx:cycle_open()
                local ok, err = pcall(function()
                    observe.click(ctx, ticket, {
                        element = slot,
                        interact = true,
                        match = nil,
                        click_x = 1,
                        click_y = 0,
                    })
                end)
                ctx:cycle_close(ticket)
                if ok then return 0 end
                if string.find(err, "observe.find returned", 1, true) == nil then
                    return 0
                end
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
            CHECK(p_clicks->clickCount() == 0U);
        }

        TEST_CASE("read_element compares the reading with the policy it was handed")
        {
            auto const directory = TemporaryDirectory{"uf-model-read"};
            seedTemplates(directory.path());

            SUBCASE("the expected text is data and exact_text is the comparison")
            {
                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(2, 5, 0)},
                        .ocrEngine   = std::make_unique<FakeOcrEngine>(
                            oneLineReadout("battle", 9'000)
                        ),
                        .projectRoot = directory.path(),
                    }
                );
                REQUIRE(built.session.has_value());
                TaskContext context{
                    *std::move(built.session),
                    *built.recorder,
                    TaskContextConfig{.projectRoot = directory.path()},
                };

                auto const result = runModel(context, built, battleScript(R"lua(
                    local ticket = ctx:cycle_open()
                    local reading = observe.read_element(
                        ctx, ticket, slot, observe.exact_text
                    )
                    ctx:cycle_close(ticket)
                    if reading == nil then return 0 end
                    if reading.text ~= "battle" then return 0 end
                    if reading.confidence ~= 9000 then return 0 end
                    if reading.expected ~= "battle" then return 0 end
                    if reading.matches ~= true then return 0 end
                    return 1
                )lua"));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("a different comparison is a different answer on the same pixels")
            {
                // The point of passing the rule in: the reading is the same and
                // the verdict is not, because "what counts as a match" is this
                // layer's policy and never the host's. cycle_read takes no
                // expected text for exactly this reason.
                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(2, 5, 0)},
                        .ocrEngine   = std::make_unique<FakeOcrEngine>(
                            oneLineReadout("battle now", 9'000)
                        ),
                        .projectRoot = directory.path(),
                    }
                );
                REQUIRE(built.session.has_value());
                TaskContext context{
                    *std::move(built.session),
                    *built.recorder,
                    TaskContextConfig{.projectRoot = directory.path()},
                };

                auto const result = runModel(context, built, battleScript(R"lua(
                    local ticket = ctx:cycle_open()
                    local strict = observe.read_element(
                        ctx, ticket, slot, observe.exact_text
                    )
                    local loose = observe.read_element(
                        ctx, ticket, slot, function(actual, expected)
                            return string.find(actual, expected, 1, true) ~= nil
                        end
                    )
                    ctx:cycle_close(ticket)
                    if strict == nil or loose == nil then return 0 end
                    if strict.matches ~= false then return 0 end
                    if loose.matches ~= true then return 0 end
                    return 1
                )lua"));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("expected text with no comparison function is refused")
            {
                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(2, 5, 0)},
                        .ocrEngine   = std::make_unique<FakeOcrEngine>(
                            oneLineReadout("battle", 9'000)
                        ),
                        .projectRoot = directory.path(),
                    }
                );
                REQUIRE(built.session.has_value());
                TaskContext context{
                    *std::move(built.session),
                    *built.recorder,
                    TaskContextConfig{.projectRoot = directory.path()},
                };

                auto const result = runModel(context, built, battleScript(R"lua(
                    local ticket = ctx:cycle_open()
                    local ok, err = pcall(function()
                        return observe.read_element(ctx, ticket, slot, nil)
                    end)
                    ctx:cycle_close(ticket)
                    if ok then return 0 end
                    if string.find(err, "pass observe.exact_text", 1, true) == nil then
                        return 0
                    end
                    return 1
                )lua"));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("an element that declares no read capability is refused")
            {
                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(2, 5, 0)},
                        .ocrEngine   = std::make_unique<FakeOcrEngine>(
                            oneLineReadout("battle", 9'000)
                        ),
                        .projectRoot = directory.path(),
                    }
                );
                REQUIRE(built.session.has_value());
                TaskContext context{
                    *std::move(built.session),
                    *built.recorder,
                    TaskContextConfig{.projectRoot = directory.path()},
                };

                auto const result = runModel(context, built, battleScript(R"lua(
                    local ticket = ctx:cycle_open()
                    local ok, err = pcall(function()
                        return observe.read_element(
                            ctx, ticket, anchor, observe.exact_text
                        )
                    end)
                    ctx:cycle_close(ticket)
                    if ok then return 0 end
                    if string.find(err, "does not declare the read capability", 1, true) == nil then
                        return 0
                    end
                    return 1
                )lua"));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }
        }

        TEST_CASE("wait_until needs K agreeing observations in a row")
        {
            // Four frames: hold, break, hold, hold. With K = 2 the streak the
            // first frame opened is broken by the second, so the wait ends on the
            // FOURTH capture and not the third.
            //
            // Return on the first hold -- drop the streak entirely -- and the run
            // ends after one capture, so the count below goes red. Decrement the
            // streak instead of resetting it and the wait ends on the third, so
            // it goes red the other way.
            auto const directory = TemporaryDirectory{"uf-model-wait-consecutive"};
            seedTemplates(directory.path());
            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {
                        pixels(2, 0, 0),
                        pixels(0, 0, 0),
                        pixels(2, 0, 0),
                        pixels(2, 0, 0),
                    },
                    .projectRoot = directory.path(),
                }
            );
            REQUIRE(built.session.has_value());
            auto* const p_frames = built.frames;
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runModel(context, built, battleScript(R"lua(
                return observe.wait_until(ctx, {
                    page = battle,
                    consecutive = 2,
                    timeout_ms = 60000,
                    interval_ms = 0,
                }) and 1 or 0
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
            CHECK(p_frames->captureCount() == 4U);
            CHECK_FALSE(context.hasOpenCycle());
        }

        TEST_CASE("wait_until that runs out of budget raises rather than reporting a no")
        {
            // A zero budget takes exactly one turn: one observation, a deadline
            // already spent, and a host-minted Tier B timeout. Returning false
            // here instead would tell a script the page is not up when in truth
            // nothing waited long enough to know.
            //
            // Replace the raise with `return false` and this goes red; the run
            // still terminates, which is what keeps proving it red cheap.
            auto const directory = TemporaryDirectory{"uf-model-wait-timeout"};
            seedTemplates(directory.path());
            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(0, 0, 0)},
                    .projectRoot = directory.path(),
                }
            );
            REQUIRE(built.session.has_value());
            auto* const p_frames = built.frames;
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runModel(context, built, battleScript(R"lua(
                local ok, err = ctx:try(function()
                    return observe.wait_until(ctx, {
                        page = battle,
                        consecutive = 1,
                        timeout_ms = 0,
                        interval_ms = 0,
                    })
                end)
                if ok ~= false then return 0 end
                if type(err) ~= 'userdata' then return 0 end
                if err.kind ~= uf.errors.timeout then return 0 end
                if getmetatable(err) ~= 'uf.error' then return 0 end

                -- The wait left nothing open, so a bare cycle still opens.
                local ticket = ctx:cycle_open()
                ctx:cycle_close(ticket)
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
            CHECK(p_frames->captureCount() == 2U);
            CHECK_FALSE(context.hasOpenCycle());
        }

        TEST_CASE("wait_until has no default budget, streak or interval")
        {
            auto const directory = TemporaryDirectory{"uf-model-wait-explicit"};
            seedTemplates(directory.path());
            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(2, 0, 0)},
                    .projectRoot = directory.path(),
                }
            );
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runModel(context, built, battleScript(R"lua(
                local missing = { "consecutive", "timeout_ms", "interval_ms" }
                for _, field in missing do
                    local options = {
                        page = battle,
                        consecutive = 1,
                        timeout_ms = 0,
                        interval_ms = 0,
                    }
                    options[field] = nil
                    local ok, err = pcall(function()
                        return observe.wait_until(ctx, options)
                    end)
                    if ok then return 0 end
                    if string.find(err, "needs " .. field .. " =", 1, true) == nil then
                        return 0
                    end
                end
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        // ------------------------------------------------------ the page graph

        // Five pages over five one-by-one greys, which is the smallest model the
        // 2026-08-01 graph ruling can be exercised against: a base screen, an
        // overlay over it, a second overlay over that, a popup that can appear
        // over anything, and a screen the game can jump to on its own.
        //
        // Each page is recognised by one grey, so a three-pixel frame decides
        // exactly which of them are on screen. `pixels(2, 20, 0)` carries the
        // base marker AND the overlay marker at once, which is not an accident:
        // an overlay COVERS the page underneath rather than replacing it, and the
        // page underneath is still recognisable and still clickable while it is
        // up (docs/plans/2026-07-31-script-owned-page-model.md 1).
        constexpr std::string_view k_graphModel = R"lua(
            local function marker(name, source)
                return model.Element.new{
                    name = name,
                    capabilities = { "identify", "interact" },
                    rect = { x = 0, y = 0, width = 3, height = 1 },
                    appearances = {
                        {
                            name = "lit",
                            source = source,
                            template = template(source),
                            threshold = 10000,
                        },
                    },
                }
            end
            local mark_base   = marker("mark_base", "gray2.png")
            local mark_over   = marker("mark_over", "gray20.png")
            local mark_zoom   = marker("mark_zoom", "gray40.png")
            local mark_alert  = marker("mark_alert", "gray5.png")
            local mark_result = marker("mark_result", "gray3.png")

            local function pageOn(name, element, flags)
                local spec = {
                    name = name,
                    references = {
                        {
                            element = element,
                            holding = "owned",
                            exercised = { "identify", "interact" },
                            identify = "required",
                        },
                    },
                }
                for key, value in flags do
                    spec[key] = value
                end
                return model.Page.new(spec)
            end

            local base   = pageOn("base", mark_base, {})
            local detail = pageOn("detail", mark_over, { overlay = true })
            local zoom   = pageOn("zoom", mark_zoom, { overlay = true })
            local alert  =
                pageOn("alert", mark_alert, { overlay = true, interrupt = true })
            local result = pageOn("result", mark_result, {})

            local open_detail = navigation.Edge.new{
                from = base,
                to = { detail },
                via = "click",
                via_element = mark_base,
                kind = "push",
            }
            local end_turn = navigation.Edge.new{
                from = base,
                to = { base },
                via = "key",
                via_key = "E",
                kind = "navigate",
            }
            local close_detail = navigation.Edge.new{
                from = detail,
                via = "click",
                via_element = mark_over,
                kind = "pop",
            }
            local battle_over = navigation.Edge.new{
                from = detail,
                to = { result },
                via = "spontaneous",
                kind = "navigate",
            }
            local zoom_in = navigation.Edge.new{
                from = detail,
                to = { zoom },
                via = "key",
                via_key = "Z",
                kind = "push",
            }
            local zoom_out = navigation.Edge.new{
                from = zoom,
                via = "click",
                via_element = mark_zoom,
                kind = "pop",
            }
            local graph = navigation.Graph.new{
                pages = { base, detail, zoom, alert, result },
                edges = {
                    open_detail,
                    end_turn,
                    close_detail,
                    battle_over,
                    zoom_in,
                    zoom_out,
                },
            }
        )lua";

        [[nodiscard]]
        auto graphScript(std::string_view body) -> std::string
        {
            return script(std::string{k_graphModel} + std::string{body});
        }

        // The pages an edge refusal needs and nothing more: a base page that
        // exercises interact on its marker, an overlay, a plain page, and a page
        // that references the base marker WITHOUT interact so an edge can try to
        // leave by clicking something it may only look at.
        constexpr std::string_view k_edgePrelude = R"lua(
            local mark_base = model.Element.new{
                name = "mark_base",
                capabilities = { "identify", "interact" },
                rect = { x = 0, y = 0, width = 3, height = 1 },
                appearances = {
                    {
                        name = "lit",
                        source = "gray2.png",
                        template = template("gray2.png"),
                        threshold = 10000,
                    },
                },
            }
            local mark_over = model.Element.new{
                name = "mark_over",
                capabilities = { "identify", "interact" },
                rect = { x = 0, y = 0, width = 3, height = 1 },
                appearances = {
                    {
                        name = "lit",
                        source = "gray20.png",
                        template = template("gray20.png"),
                        threshold = 10000,
                    },
                },
            }
            local function pageOn(name, element, holding, exercised, flags)
                local spec = {
                    name = name,
                    references = {
                        {
                            element = element,
                            holding = holding,
                            exercised = exercised,
                            identify = "required",
                        },
                    },
                }
                for key, value in flags do
                    spec[key] = value
                end
                return model.Page.new(spec)
            end
            local base =
                pageOn("base", mark_base, "owned", { "identify", "interact" }, {})
            local detail = pageOn(
                "detail", mark_over, "owned", { "identify", "interact" },
                { overlay = true }
            )
            local watch =
                pageOn("watch", mark_base, "referenced", { "identify" }, {})
        )lua";

        // Edge.new's rulings, one row each. Every row is otherwise a valid edge,
        // so exactly one check can be the one that fires: delete that check and
        // the row's call succeeds and the row goes red.
        [[nodiscard]]
        auto edgeRefusals() -> std::vector<Refusal>
        {
            return {
                Refusal{
                    .label = "an edge leaving a table nobody minted",
                    .body  = R"lua(
                        return navigation.Edge.new{
                            from = { name = "base", references = {} },
                            to = { detail },
                            via = "spontaneous",
                            kind = "navigate",
                        }
                    )lua",
                    .fragment = "from = the page it leaves",
                },
                Refusal{
                    .label = "an edge field this layer does not own",
                    .body  = R"lua(
                        return navigation.Edge.new{
                            from = base,
                            to = { detail },
                            via = "click",
                            via_element = mark_base,
                            kind = "push",
                            my_edge_weight = 3,
                        }
                    )lua",
                    .fragment = "extra = { my_edge_weight",
                },
                Refusal{
                    .label = "a trigger that is not one of the three",
                    .body  = R"lua(
                        return navigation.Edge.new{
                            from = base,
                            to = { detail },
                            via = "scroll",
                            kind = "push",
                        }
                    )lua",
                    .fragment = "three ways a screen changes",
                },
                Refusal{
                    .label = "a kind that is not one of the three",
                    .body  = R"lua(
                        return navigation.Edge.new{
                            from = base,
                            to = { detail },
                            via = "spontaneous",
                            kind = "cover",
                        }
                    )lua",
                    .fragment = "three different things to believe afterwards",
                },
                Refusal{
                    .label = "a click edge naming no element",
                    .body  = R"lua(
                        return navigation.Edge.new{
                            from = base,
                            to = { detail },
                            via = "click",
                            kind = "push",
                        }
                    )lua",
                    .fragment = "needs via_element =",
                },
                Refusal{
                    .label = "a click edge naming an element the page never declared",
                    .body  = R"lua(
                        return navigation.Edge.new{
                            from = base,
                            to = { detail },
                            via = "click",
                            via_element = mark_over,
                            kind = "push",
                        }
                    )lua",
                    .fragment = "which page 'base' does not reference",
                },
                Refusal{
                    .label = "a click edge leaving by an element the page may not click",
                    .body  = R"lua(
                        return navigation.Edge.new{
                            from = watch,
                            to = { detail },
                            via = "click",
                            via_element = mark_base,
                            kind = "push",
                        }
                    )lua",
                    .fragment = "references without exercising interact",
                },
                Refusal{
                    .label = "a click edge that also names a key",
                    .body  = R"lua(
                        return navigation.Edge.new{
                            from = base,
                            to = { detail },
                            via = "click",
                            via_element = mark_base,
                            via_key = "E",
                            kind = "push",
                        }
                    )lua",
                    .fragment = "cannot also name a key",
                },
                Refusal{
                    .label = "a key edge naming no key",
                    .body  = R"lua(
                        return navigation.Edge.new{
                            from = base,
                            to = { detail },
                            via = "key",
                            kind = "push",
                        }
                    )lua",
                    .fragment = "needs via_key =",
                },
                Refusal{
                    .label = "a key edge that also names an element",
                    .body  = R"lua(
                        return navigation.Edge.new{
                            from = base,
                            to = { detail },
                            via = "key",
                            via_key = "E",
                            via_element = mark_base,
                            kind = "push",
                        }
                    )lua",
                    .fragment = "cannot also name an element",
                },
                Refusal{
                    .label = "a spontaneous edge that names a trigger anyway",
                    .body  = R"lua(
                        return navigation.Edge.new{
                            from = base,
                            to = { detail },
                            via = "spontaneous",
                            via_key = "E",
                            kind = "push",
                        }
                    )lua",
                    .fragment = "names neither an element to click nor a key",
                },
                Refusal{
                    .label = "a pop the game is supposed to take on its own",
                    .body  = R"lua(
                        return navigation.Edge.new{
                            from = detail,
                            via = "spontaneous",
                            kind = "pop",
                        }
                    )lua",
                    .fragment = "there is no instance to design it against",
                },
                Refusal{
                    .label = "a pop that writes down where it lands",
                    .body  = R"lua(
                        return navigation.Edge.new{
                            from = detail,
                            to = { base },
                            via = "click",
                            via_element = mark_over,
                            kind = "pop",
                        }
                    )lua",
                    .fragment = "pops, so it declares no to",
                },
                Refusal{
                    .label = "an edge that is not a pop and names nowhere to land",
                    .body  = R"lua(
                        return navigation.Edge.new{
                            from = base,
                            via = "click",
                            via_element = mark_base,
                            kind = "push",
                        }
                    )lua",
                    .fragment = "needs to = { page, ... }",
                },
                Refusal{
                    .label = "an edge whose destination set is empty",
                    .body  = R"lua(
                        return navigation.Edge.new{
                            from = base,
                            to = {},
                            via = "click",
                            via_element = mark_base,
                            kind = "push",
                        }
                    )lua",
                    .fragment = "has an empty to",
                },
                Refusal{
                    .label = "a destination that is not a page",
                    .body  = R"lua(
                        return navigation.Edge.new{
                            from = base,
                            to = { detail, { name = "ghost" } },
                            via = "click",
                            via_element = mark_base,
                            kind = "push",
                        }
                    )lua",
                    .fragment = "destination 2 is not a page built by Page.new",
                },
                Refusal{
                    .label = "one destination named twice",
                    .body  = R"lua(
                        return navigation.Edge.new{
                            from = base,
                            to = { detail, detail },
                            via = "click",
                            via_element = mark_base,
                            kind = "push",
                        }
                    )lua",
                    .fragment = "twice; to is a set",
                },
                Refusal{
                    .label = "a push onto a page that replaces the screen",
                    .body  = R"lua(
                        return navigation.Edge.new{
                            from = watch,
                            to = { base },
                            via = "spontaneous",
                            kind = "push",
                        }
                    )lua",
                    .fragment = "is not declared overlay = true",
                },
                Refusal{
                    .label = "a navigate onto a page that only covers the screen",
                    .body  = R"lua(
                        return navigation.Edge.new{
                            from = base,
                            to = { detail },
                            via = "click",
                            via_element = mark_base,
                            kind = "navigate",
                        }
                    )lua",
                    .fragment = "push it instead",
                },
            };
        }

        // Graph.new's rulings: the ones a single Edge.new cannot see because they
        // are about the edges and pages TOGETHER.
        [[nodiscard]]
        auto graphRefusals() -> std::vector<Refusal>
        {
            return {
                Refusal{
                    .label = "a graph with no edge list at all",
                    .body  = R"lua(
                        return navigation.Graph.new{ pages = { base, detail } }
                    )lua",
                    .fragment = "a project that has drawn none passes an empty list",
                },
                Refusal{
                    .label = "one page named twice",
                    .body  = R"lua(
                        return navigation.Graph.new{
                            pages = { base, detail, base },
                            edges = {},
                        }
                    )lua",
                    .fragment = "a graph names each page once",
                },
                Refusal{
                    .label = "a table in the edge list that nobody minted",
                    .body  = R"lua(
                        return navigation.Graph.new{
                            pages = { base, detail },
                            edges = { { from = base, to = { detail } } },
                        }
                    )lua",
                    .fragment = "edge 1 is not an edge built by Edge.new",
                },
                Refusal{
                    .label = "an edge leaving a page the graph never declared",
                    .body  = R"lua(
                        local stray = navigation.Edge.new{
                            from = watch,
                            to = { detail },
                            via = "spontaneous",
                            kind = "push",
                        }
                        return navigation.Graph.new{
                            pages = { base, detail },
                            edges = { stray },
                        }
                    )lua",
                    .fragment = "names page 'watch', which this graph does not declare",
                },
                Refusal{
                    .label = "an edge into a page the graph never declared",
                    .body  = R"lua(
                        local stray = navigation.Edge.new{
                            from = base,
                            to = { detail },
                            via = "spontaneous",
                            kind = "push",
                        }
                        return navigation.Graph.new{
                            pages = { base, watch },
                            edges = { stray },
                        }
                    )lua",
                    .fragment = "names page 'detail', which this graph does not declare",
                },
                Refusal{
                    .label = "two edges racing to describe one click",
                    .body  = R"lua(
                        local first = navigation.Edge.new{
                            from = base,
                            to = { detail },
                            via = "click",
                            via_element = mark_base,
                            kind = "push",
                        }
                        local second = navigation.Edge.new{
                            from = base,
                            to = { detail },
                            via = "click",
                            via_element = mark_base,
                            kind = "push",
                        }
                        return navigation.Graph.new{
                            pages = { base, detail },
                            edges = { first, second },
                        }
                    )lua",
                    .fragment = "repeats a trigger page 'base' already leaves by",
                },
            };
        }

        // Runs a table of refusal rows over a shared Luau prelude, one fresh
        // session each, and requires every row to raise for its own reason.
        auto checkRefusals(
            std::string_view              label,
            std::string_view              directory,
            std::string_view              prelude,
            std::vector<Refusal> const&   rows
        ) -> void
        {
            auto const root = TemporaryDirectory{directory};
            seedTemplates(root.path());

            for (auto const& refusal : rows)
            {
                auto built = refusalHarness(root.path());
                REQUIRE(built.session.has_value());
                TaskContext context{
                    *std::move(built.session),
                    *built.recorder,
                    TaskContextConfig{.projectRoot = root.path()},
                };

                INFO(label, " refuses ", refusal.label);
                auto const body =
                    std::string{prelude} + std::string{refusal.body};
                auto const result =
                    runModel(context, built, refusalScript(body, refusal.fragment));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }
        }

        TEST_CASE("Edge.new refuses every malformed edge by name")
        {
            checkRefusals(
                "Edge.new",
                "uf-model-edge-refusals",
                k_edgePrelude,
                edgeRefusals()
            );
        }

        TEST_CASE("Graph.new refuses every inconsistent graph by name")
        {
            checkRefusals(
                "Graph.new",
                "uf-model-graph-refusals",
                k_edgePrelude,
                graphRefusals()
            );
        }

        TEST_CASE("A page stack is an explicit object with a required ceiling")
        {
            auto const directory = TemporaryDirectory{"uf-model-stack-shape"};
            seedTemplates(directory.path());
            auto built = refusalHarness(directory.path());
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            // Every refusal here is about the STACK rather than about pixels, so
            // one script covers them all without a frame in sight: no graph, no
            // ceiling, a ceiling that is not a whole count, a page from another
            // model, and the empty stack a fresh one starts as.
            auto const result = runModel(context, built, graphScript(R"lua(
                local function refused(fragment, fn)
                    local ok, err = pcall(fn)
                    if ok then return false end
                    if type(err) ~= "string" then return false end
                    return string.find(err, fragment, 1, true) ~= nil
                end

                if not refused("needs graph =", function()
                    return navigation.stack_new{ max_depth = 4 }
                end) then return 0 end

                if not refused("needs max_depth =", function()
                    return navigation.stack_new{ graph = graph }
                end) then return 0 end

                if not refused("needs max_depth =", function()
                    return navigation.stack_new{ graph = graph, max_depth = 0 }
                end) then return 0 end

                if not refused("does not have a field named", function()
                    return navigation.stack_new{
                        graph = graph, max_depth = 4, start = base,
                    }
                end) then return 0 end

                local stack = navigation.stack_new{ graph = graph, max_depth = 4 }
                if navigation.stack_depth(stack) ~= 0 then return 0 end
                if navigation.stack_top(stack) ~= nil then return 0 end

                -- A page from a model this stack is not a stack of.
                local elsewhere = model.Page.new{
                    name = "elsewhere",
                    references = {
                        {
                            element = mark_base,
                            holding = "referenced",
                            exercised = { "identify" },
                            identify = "required",
                        },
                    },
                }
                if not refused("is not in the graph this stack", function()
                    navigation.stack_reset(stack, elsewhere)
                end) then return 0 end

                navigation.stack_reset(stack, base)
                if navigation.stack_depth(stack) ~= 1 then return 0 end
                if navigation.stack_top(stack).name ~= "base" then return 0 end

                -- The copy is a copy: truncating it cannot truncate the belief.
                local pages = navigation.stack_pages(stack)
                table.remove(pages)
                if navigation.stack_depth(stack) ~= 1 then return 0 end

                -- A stack from one graph refuses an edge from another.
                local other = navigation.Graph.new{
                    pages = { base, detail },
                    edges = {},
                }
                local lonely = navigation.stack_new{ graph = other, max_depth = 4 }
                navigation.stack_reset(lonely, base)
                if not refused("not in the graph this stack was built over", function()
                    return observe.walk_edge(ctx, lonely, open_detail, {
                        consecutive = 1, timeout_ms = 0, interval_ms = 0,
                    })
                end) then return 0 end

                if not refused("needs the page stack", function()
                    return observe.walk_edge(ctx, { entries = {} }, open_detail, {
                        consecutive = 1, timeout_ms = 0, interval_ms = 0,
                    })
                end) then return 0 end

                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
            CHECK(built.clicks->clickCount() == 0U);
        }

        TEST_CASE("walk_edge performs the trigger the edge declared and nothing else")
        {
            auto const directory = TemporaryDirectory{"uf-model-walk-via"};
            seedTemplates(directory.path());

            SUBCASE("a click edge clicks through the page's own authorisation")
            {
                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(2, 0, 0), pixels(2, 20, 0)},
                        .projectRoot = directory.path(),
                    }
                );
                REQUIRE(built.session.has_value());
                auto* const p_clicks = built.clicks;
                TaskContext context{
                    *std::move(built.session),
                    *built.recorder,
                    TaskContextConfig{.projectRoot = directory.path()},
                };

                auto const result = runModel(context, built, graphScript(R"lua(
                    local stack = navigation.stack_new{ graph = graph, max_depth = 4 }
                    navigation.stack_reset(stack, base)
                    local outcome = observe.walk_edge(ctx, stack, open_detail, {
                        consecutive = 1, timeout_ms = 2000, interval_ms = 0,
                    })
                    if outcome.outcome ~= "arrived" then return 0 end
                    if outcome.page.name ~= "detail" then return 0 end
                    if navigation.stack_depth(stack) ~= 2 then return 0 end
                    if navigation.stack_top(stack).name ~= "detail" then return 0 end
                    return 1
                )lua"));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
                CHECK(p_clicks->clickCount() == 1U);
                CHECK(p_clicks->keys().empty());
            }

            SUBCASE("a key edge presses the key and clicks nothing")
            {
                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(2, 0, 0)},
                        .projectRoot = directory.path(),
                    }
                );
                REQUIRE(built.session.has_value());
                auto* const p_clicks = built.clicks;
                TaskContext context{
                    *std::move(built.session),
                    *built.recorder,
                    TaskContextConfig{.projectRoot = directory.path()},
                };

                auto const result = runModel(context, built, graphScript(R"lua(
                    local stack = navigation.stack_new{ graph = graph, max_depth = 4 }
                    navigation.stack_reset(stack, base)
                    local outcome = observe.walk_edge(ctx, stack, end_turn, {
                        consecutive = 1, timeout_ms = 2000, interval_ms = 0,
                    })
                    if outcome.outcome ~= "arrived" then return 0 end
                    if outcome.page.name ~= "base" then return 0 end
                    if navigation.stack_depth(stack) ~= 1 then return 0 end
                    return 1
                )lua"));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
                CHECK(p_clicks->clickCount() == 0U);
                REQUIRE(p_clicks->keys().size() == 1U);
                CHECK(p_clicks->keys().front().value() == "E");
            }

            SUBCASE("a spontaneous edge delivers no input at all")
            {
                // The whole of a spontaneous walk is the wait, so this is also
                // the case that pins the capture count: one observation, no
                // trigger cycle before it.
                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(3, 0, 0)},
                        .projectRoot = directory.path(),
                    }
                );
                REQUIRE(built.session.has_value());
                auto* const p_clicks = built.clicks;
                auto* const p_frames = built.frames;
                TaskContext context{
                    *std::move(built.session),
                    *built.recorder,
                    TaskContextConfig{.projectRoot = directory.path()},
                };

                auto const result = runModel(context, built, graphScript(R"lua(
                    local stack = navigation.stack_new{ graph = graph, max_depth = 4 }
                    navigation.stack_reset(stack, base)
                    local outcome = observe.walk_edge(ctx, stack, battle_over, {
                        consecutive = 1, timeout_ms = 2000, interval_ms = 0,
                    })
                    if outcome.outcome ~= "arrived" then return 0 end
                    if outcome.page.name ~= "result" then return 0 end
                    return 1
                )lua"));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
                CHECK(p_clicks->clickCount() == 0U);
                CHECK(p_clicks->keys().empty());
                CHECK(p_frames->captureCount() == 1U);
            }

            SUBCASE("a click edge whose element is not on screen refuses to walk")
            {
                // Fail closed BEFORE the wait: nothing was clicked, so nothing
                // can have moved, and the sentence names the element rather than
                // reporting a timeout on a destination that was never triggered.
                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(0, 0, 0)},
                        .projectRoot = directory.path(),
                    }
                );
                REQUIRE(built.session.has_value());
                auto* const p_clicks = built.clicks;
                TaskContext context{
                    *std::move(built.session),
                    *built.recorder,
                    TaskContextConfig{.projectRoot = directory.path()},
                };

                auto const result = runModel(context, built, graphScript(R"lua(
                    local stack = navigation.stack_new{ graph = graph, max_depth = 4 }
                    navigation.stack_reset(stack, base)
                    local ok, err = pcall(function()
                        return observe.walk_edge(ctx, stack, open_detail, {
                            consecutive = 1, timeout_ms = 0, interval_ms = 0,
                        })
                    end)
                    if ok then return 0 end
                    if type(err) ~= "string" then return 0 end
                    if string.find(err, "nothing on this frame matched it", 1, true) == nil then
                        return 0
                    end
                    if navigation.stack_depth(stack) ~= 1 then return 0 end
                    return 1
                )lua"));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
                CHECK(p_clicks->clickCount() == 0U);
            }
        }

        TEST_CASE("push and pop keep the page underneath and give it back")
        {
            // The first touchstone the ruling named: open an overlay, open a
            // second one over it, close that one, and land back where the stack
            // said you were. The destination of each pop is never written in the
            // model -- it is read off the stack at walk time -- so replacing the
            // pop branch with the edge's own `to` cannot even compile a
            // destination, and dropping the entry the pop removes lands the run
            // one page too shallow.
            auto const directory = TemporaryDirectory{"uf-model-push-pop"};
            seedTemplates(directory.path());
            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {
                        pixels(2, 0, 0),   // click mark_base on the base page
                        pixels(2, 20, 0),  // detail resolves, over the base
                        pixels(20, 40, 0), // the Z keystroke's own cycle
                        pixels(20, 40, 0), // zoom resolves, over the detail
                        pixels(20, 40, 0), // click mark_zoom to close it
                        pixels(20, 20, 0), // detail resolves again underneath
                    },
                    .projectRoot = directory.path(),
                }
            );
            REQUIRE(built.session.has_value());
            auto* const p_clicks = built.clicks;
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runModel(context, built, graphScript(R"lua(
                local stack = navigation.stack_new{ graph = graph, max_depth = 4 }
                navigation.stack_reset(stack, base)

                local opened = observe.walk_edge(ctx, stack, open_detail, {
                    consecutive = 1, timeout_ms = 2000, interval_ms = 0,
                })
                if opened.page.name ~= "detail" then return 0 end
                if navigation.stack_depth(stack) ~= 2 then return 0 end

                local zoomed = observe.walk_edge(ctx, stack, zoom_in, {
                    consecutive = 1, timeout_ms = 2000, interval_ms = 0,
                })
                if zoomed.page.name ~= "zoom" then return 0 end
                if navigation.stack_depth(stack) ~= 3 then return 0 end

                -- The pop names no destination. What it waits for is the page
                -- the stack has underneath the zoom, which is the detail overlay
                -- and NOT the base page at the bottom.
                local closed = observe.walk_edge(ctx, stack, zoom_out, {
                    consecutive = 1, timeout_ms = 2000, interval_ms = 0,
                })
                if closed.outcome ~= "arrived" then return 0 end
                if closed.page.name ~= "detail" then return 0 end
                if navigation.stack_depth(stack) ~= 2 then return 0 end
                if navigation.stack_top(stack).name ~= "detail" then return 0 end

                local remembered = navigation.stack_pages(stack)
                if remembered[1].name ~= "base" then return 0 end
                if remembered[2].name ~= "detail" then return 0 end
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
            CHECK(p_clicks->clickCount() == 2U);
            REQUIRE(p_clicks->keys().size() == 1U);
            CHECK(p_clicks->keys().front().value() == "Z");
        }

        TEST_CASE("A pop with nothing underneath is refused rather than guessed at")
        {
            auto const directory = TemporaryDirectory{"uf-model-pop-floor"};
            seedTemplates(directory.path());
            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(2, 20, 0)},
                    .projectRoot = directory.path(),
                }
            );
            REQUIRE(built.session.has_value());
            auto* const p_clicks = built.clicks;
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runModel(context, built, graphScript(R"lua(
                local stack = navigation.stack_new{ graph = graph, max_depth = 4 }
                navigation.stack_reset(stack, detail)
                local ok, err = pcall(function()
                    return observe.walk_edge(ctx, stack, close_detail, {
                        consecutive = 1, timeout_ms = 0, interval_ms = 0,
                    })
                end)
                if ok then return 0 end
                if type(err) ~= "string" then return 0 end
                if string.find(err, "needs a page underneath", 1, true) == nil then
                    return 0
                end
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
            CHECK(p_clicks->clickCount() == 0U);
        }

        TEST_CASE("A base page that resolves resets the stack, whatever it believed")
        {
            // The ruling's own instance, made mechanical: a run standing in a
            // battle with the card detail open, when the battle ends and the game
            // jumps to the result screen on its own. The overlay AND the page it
            // covered are gone together, so belief has to be thrown away rather
            // than popped once.
            //
            // Make the arrival always push instead and the stack ends three deep;
            // make it always pop and it ends two deep with the wrong base. Both
            // go red here.
            auto const directory = TemporaryDirectory{"uf-model-belief-reset"};
            seedTemplates(directory.path());
            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {
                        pixels(2, 0, 0),
                        pixels(2, 20, 0),
                        pixels(3, 0, 0),
                    },
                    .projectRoot = directory.path(),
                }
            );
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runModel(context, built, graphScript(R"lua(
                local stack = navigation.stack_new{ graph = graph, max_depth = 4 }
                navigation.stack_reset(stack, base)
                observe.walk_edge(ctx, stack, open_detail, {
                    consecutive = 1, timeout_ms = 2000, interval_ms = 0,
                })
                if navigation.stack_depth(stack) ~= 2 then return 0 end

                local ended = observe.walk_edge(ctx, stack, battle_over, {
                    consecutive = 1, timeout_ms = 2000, interval_ms = 0,
                })
                if ended.page.name ~= "result" then return 0 end
                if navigation.stack_depth(stack) ~= 1 then return 0 end
                if navigation.stack_top(stack).name ~= "result" then return 0 end
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("The depth cap refuses the push before the input is delivered")
        {
            // A runaway push is a script bug, so the cap raises a plain Luau
            // error rather than a Tier B automation failure -- nothing about the
            // target went wrong.
            //
            // The ORDER is the other half of the case. Move the check to the
            // moment the belief is applied and the keystroke is delivered first:
            // the target moves, and only then does the run refuse to reason about
            // where it is. The key count below is what goes red.
            auto const directory = TemporaryDirectory{"uf-model-depth-cap"};
            seedTemplates(directory.path());
            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(2, 0, 0), pixels(2, 20, 0)},
                    .projectRoot = directory.path(),
                }
            );
            REQUIRE(built.session.has_value());
            auto* const p_clicks = built.clicks;
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runModel(context, built, graphScript(R"lua(
                local stack = navigation.stack_new{ graph = graph, max_depth = 2 }
                navigation.stack_reset(stack, base)
                observe.walk_edge(ctx, stack, open_detail, {
                    consecutive = 1, timeout_ms = 2000, interval_ms = 0,
                })
                if navigation.stack_depth(stack) ~= 2 then return 0 end

                local ok, err = pcall(function()
                    return observe.walk_edge(ctx, stack, zoom_in, {
                        consecutive = 1, timeout_ms = 2000, interval_ms = 0,
                    })
                end)
                if ok then return 0 end
                if type(err) ~= "string" then return 0 end
                if string.find(err, "past the max_depth of 2", 1, true) == nil then
                    return 0
                end
                if navigation.stack_depth(stack) ~= 2 then return 0 end
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
            CHECK(p_clicks->clickCount() == 1U);
            CHECK(p_clicks->keys().empty());
        }

        TEST_CASE("A walk that times out reports the interrupt page it found instead")
        {
            auto const directory = TemporaryDirectory{"uf-model-interrupt"};
            seedTemplates(directory.path());

            SUBCASE("an interrupt page on screen becomes the walk's outcome")
            {
                // The popup is declared ONCE, as a flag on its own page, and is
                // reached by no inbound edge at all. Delete the interrupt sweep
                // and this walk raises a timeout instead, which is the whole
                // difference the flag buys: a sentence naming what is actually on
                // screen rather than one naming what is not.
                //
                // The walk does not dismiss it. Closing a popup is a policy some
                // projects answer differently, so the framework's job ends at
                // handing the page over -- and at recording that an overlay went
                // onto the stack, because that is what was observed.
                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(2, 5, 0)},
                        .projectRoot = directory.path(),
                    }
                );
                REQUIRE(built.session.has_value());
                auto* const p_clicks = built.clicks;
                TaskContext context{
                    *std::move(built.session),
                    *built.recorder,
                    TaskContextConfig{.projectRoot = directory.path()},
                };

                auto const result = runModel(context, built, graphScript(R"lua(
                    local stack = navigation.stack_new{ graph = graph, max_depth = 4 }
                    navigation.stack_reset(stack, base)
                    local outcome = observe.walk_edge(ctx, stack, open_detail, {
                        consecutive = 1, timeout_ms = 0, interval_ms = 0,
                    })
                    if outcome.outcome ~= "interrupted" then return 0 end
                    if outcome.page.name ~= "alert" then return 0 end
                    if navigation.stack_depth(stack) ~= 2 then return 0 end
                    if navigation.stack_top(stack).name ~= "alert" then return 0 end
                    return 1
                )lua"));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
                CHECK(p_clicks->clickCount() == 1U);
            }

            SUBCASE("with no interrupt page on screen the walk raises a timeout")
            {
                // A control error and never a "no": nothing that ran out of time
                // learned anything about the screen, so the run gets the same
                // host-minted Tier B error every other automation failure gets.
                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(2, 0, 0)},
                        .projectRoot = directory.path(),
                    }
                );
                REQUIRE(built.session.has_value());
                TaskContext context{
                    *std::move(built.session),
                    *built.recorder,
                    TaskContextConfig{.projectRoot = directory.path()},
                };

                auto const result = runModel(context, built, graphScript(R"lua(
                    local stack = navigation.stack_new{ graph = graph, max_depth = 4 }
                    navigation.stack_reset(stack, base)
                    local ok, err = ctx:try(function()
                        return observe.walk_edge(ctx, stack, open_detail, {
                            consecutive = 1, timeout_ms = 0, interval_ms = 0,
                        })
                    end)
                    if ok ~= false then return 0 end
                    if type(err) ~= 'userdata' then return 0 end
                    if err.kind ~= uf.errors.timeout then return 0 end

                    -- The belief was never updated, and the wait left no cycle
                    -- open behind it.
                    if navigation.stack_depth(stack) ~= 1 then return 0 end
                    local ticket = ctx:cycle_open()
                    ctx:cycle_close(ticket)
                    return 1
                )lua"));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
                CHECK_FALSE(context.hasOpenCycle());
            }
        }

        TEST_CASE("A destination set settles on one page rather than on any of them")
        {
            // `to` is a set because an outcome can be uncertain, and the streak
            // has to be about ONE of them: seeing the base page once and the
            // result page once is two observations of a screen in motion, not two
            // agreeing observations of anything.
            //
            // Count the streak per walk instead of per page and the wait below
            // returns on the second frame with whichever page it happened to see
            // last, so the arrival name goes red.
            auto const directory = TemporaryDirectory{"uf-model-destination-set"};
            seedTemplates(directory.path());
            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {
                        pixels(2, 0, 0),
                        pixels(3, 0, 0),
                        pixels(2, 0, 0),
                        pixels(2, 0, 0),
                    },
                    .projectRoot = directory.path(),
                }
            );
            REQUIRE(built.session.has_value());
            auto* const p_frames = built.frames;
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runModel(context, built, graphScript(R"lua(
                local either = navigation.Edge.new{
                    from = detail,
                    to = { result, base },
                    via = "spontaneous",
                    kind = "navigate",
                }
                local wider = navigation.Graph.new{
                    pages = { base, detail, zoom, alert, result },
                    edges = { either },
                }
                local stack = navigation.stack_new{ graph = wider, max_depth = 4 }
                navigation.stack_reset(stack, base)

                local outcome = observe.walk_edge(ctx, stack, either, {
                    consecutive = 2, timeout_ms = 2000, interval_ms = 0,
                })
                if outcome.page.name ~= "base" then return 0 end

                -- The set is sorted, so the file and this list agree whatever
                -- order the author wrote them in.
                if either.to[1].name ~= "base" then return 0 end
                if either.to[2].name ~= "result" then return 0 end
                if navigation.Edge.leads_to(either, result) ~= true then return 0 end
                if navigation.Edge.leads_to(either, zoom) ~= false then return 0 end
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
            CHECK(p_frames->captureCount() == 4U);
        }

        // The canonical project file, in exactly the form project.encode writes.
        //
        // It carries three things a naive writer would destroy: a key inside
        // [[element]] this schema version does not know, a [element.extra]
        // subtable that belongs to the project rather than to this layer, and a
        // whole [[gadget]] section kind nothing here understands.
        constexpr std::string_view k_canonicalProject =
            "schema = \"umbraflow-project/l2-v1\"\n"
            "\n"
            "[[element]]\n"
            "name = \"anchor\"\n"
            "capabilities = [\"identify\", \"interact\"]\n"
            "rect = [0, 0, 3, 1]\n"
            "\n"
            "[[appearance]]\n"
            "element = \"anchor\"\n"
            "name = \"on_dark\"\n"
            "source = \"gray2.png\"\n"
            "threshold = 10000\n"
            "\n"
            "[[appearance]]\n"
            "element = \"anchor\"\n"
            "name = \"on_light\"\n"
            "source = \"gray5.png\"\n"
            "threshold = 10000\n"
            "[appearance.extra]\n"
            "mine = 1\n"
            "\n"
            "[[element]]\n"
            "name = \"slot\"\n"
            "capabilities = [\"interact\", \"read\"]\n"
            "rect = [0, 0, 3, 1]\n"
            "expected_text = \"battle\"\n"
            "future_field = 7\n"
            "\n"
            "[element.extra]\n"
            "my_grid_stride = 5\n"
            "\n"
            "[[page]]\n"
            "name = \"battle\"\n"
            "\n"
            "[[reference]]\n"
            "page = \"battle\"\n"
            "element = \"anchor\"\n"
            "holding = \"owned\"\n"
            "exercised = [\"identify\", \"interact\"]\n"
            "identify = \"required\"\n"
            "pinned_appearance = \"on_dark\"\n"
            "\n"
            "[[reference]]\n"
            "page = \"battle\"\n"
            "element = \"slot\"\n"
            "holding = \"owned\"\n"
            "exercised = [\"interact\", \"read\"]\n"
            "\n"
            "[[gadget]]\n"
            "name = \"not mine\"\n"
            "count = 3\n";

        // The same model, written the way a hand edit leaves it: elements out of
        // order, one capability list out of order, and the unknown section in the
        // middle instead of at the end.
        //
        // It is what the case below actually starts from, so a save that never
        // ran would leave these bytes on disk and the comparison against the
        // canonical form would fail. Starting from the canonical form instead
        // would let a no-op save pass.
        constexpr std::string_view k_unsortedProject =
            "schema = \"umbraflow-project/l2-v1\"\n"
            "\n"
            "[[element]]\n"
            "name = \"slot\"\n"
            "capabilities = [\"read\", \"interact\"]\n"
            "rect = [0, 0, 3, 1]\n"
            "expected_text = \"battle\"\n"
            "future_field = 7\n"
            "\n"
            "[element.extra]\n"
            "my_grid_stride = 5\n"
            "\n"
            "[[gadget]]\n"
            "name = \"not mine\"\n"
            "count = 3\n"
            "\n"
            "[[element]]\n"
            "name = \"anchor\"\n"
            "capabilities = [\"interact\", \"identify\"]\n"
            "rect = [0, 0, 3, 1]\n"
            "\n"
            "[[appearance]]\n"
            "element = \"anchor\"\n"
            "name = \"on_dark\"\n"
            "source = \"gray2.png\"\n"
            "threshold = 10000\n"
            "\n"
            "[[appearance]]\n"
            "element = \"anchor\"\n"
            "name = \"on_light\"\n"
            "source = \"gray5.png\"\n"
            "threshold = 10000\n"
            "\n"
            "[appearance.extra]\n"
            "mine = 1\n"
            "\n"
            "[[page]]\n"
            "name = \"battle\"\n"
            "\n"
            "[[reference]]\n"
            "page = \"battle\"\n"
            "element = \"anchor\"\n"
            "holding = \"owned\"\n"
            "exercised = [\"identify\", \"interact\"]\n"
            "identify = \"required\"\n"
            "pinned_appearance = \"on_dark\"\n"
            "\n"
            "[[reference]]\n"
            "page = \"battle\"\n"
            "element = \"slot\"\n"
            "holding = \"owned\"\n"
            "exercised = [\"interact\", \"read\"]\n";

        TEST_CASE("A project file round trips byte for byte and keeps what it does not know")
        {
            // The load-bearing persistence case. A build that dropped an unknown
            // key would not fail -- it would silently delete a newer build's
            // data, which is the worst failure a file format has, so the proof
            // has to be on the bytes rather than on the model.
            //
            // Remove the residual list from Element.new (or stop writing it in
            // project.encode) and the rewritten file loses `future_field = 7`, so
            // the comparison below goes red. Merge [element.extra] into the
            // element's own fields and the isolation checks go red instead.
            auto const directory = TemporaryDirectory{"uf-model-project-roundtrip"};
            seedTemplates(directory.path());
            auto const modelPath = directory.path() / "page-model.toml";
            REQUIRE(k_unsortedProject != k_canonicalProject);
            writeFile(modelPath, std::as_bytes(std::span{k_unsortedProject}));

            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(2, 5, 0)},
                    .projectRoot = directory.path(),
                }
            );
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runModel(context, built, R"lua(
                local built = project.load_project(ctx)
                if built.schema ~= project.schema then return 0 end

                local slot = built.element_by_name.slot
                if slot == nil then return 0 end
                if slot.expected_text ~= "battle" then return 0 end

                -- A project's own field lives under extra and NOWHERE else.
                if slot.extra.my_grid_stride ~= 5 then return 0 end
                if rawget(slot, "my_grid_stride") ~= nil then return 0 end

                -- An unknown key of this layer's own section is carried verbatim
                -- and is not mistaken for a project field.
                if #slot.residual ~= 1 then return 0 end
                if slot.residual[1] ~= "future_field = 7" then return 0 end
                if slot.extra.future_field ~= nil then return 0 end

                -- An appearance has no extra of its own, so a subtable written
                -- under one is carried as raw lines rather than parsed into a
                -- field nothing would write back.
                local light = built.element_by_name.anchor.appearances[2]
                if #light.residual ~= 2 then return 0 end
                if light.residual[1] ~= "[appearance.extra]" then return 0 end
                if light.residual[2] ~= "mine = 1" then return 0 end

                -- The model still describes the same page.
                local battle = built.page_by_name.battle
                if battle == nil then return 0 end
                if #battle.references ~= 2 then return 0 end
                if battle.references[1].pinned_appearance ~= "on_dark" then return 0 end
                if battle.references[1].element.appearances[2].name ~= "on_light" then
                    return 0
                end

                project.save_project(ctx, built)

                -- Loading what was just written and writing it again is the
                -- fixpoint half: the first save normalises a hand-edited file,
                -- and the second must change nothing at all.
                project.save_project(ctx, project.load_project(ctx))
                return 1
            )lua");
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));

            CHECK(readFileText(modelPath) == std::string{k_canonicalProject});
        }

        // The graph half of the file format, canonical. Three edges, both page
        // flags, one key inside [[edge]] this schema version does not know, and
        // an [edge.extra] subtable that belongs to the project rather than to
        // this layer.
        //
        // The pop edge is the shape worth looking at twice: it carries no `to`
        // line at all, because where a pop lands is the run's page stack rather
        // than anything a file can state.
        constexpr std::string_view k_canonicalGraphProject =
            "schema = \"umbraflow-project/l2-v1\"\n"
            "\n"
            "[[element]]\n"
            "name = \"mark_base\"\n"
            "capabilities = [\"identify\", \"interact\"]\n"
            "rect = [0, 0, 3, 1]\n"
            "\n"
            "[[appearance]]\n"
            "element = \"mark_base\"\n"
            "name = \"lit\"\n"
            "source = \"gray2.png\"\n"
            "threshold = 10000\n"
            "\n"
            "[[element]]\n"
            "name = \"mark_over\"\n"
            "capabilities = [\"identify\", \"interact\"]\n"
            "rect = [0, 0, 3, 1]\n"
            "\n"
            "[[appearance]]\n"
            "element = \"mark_over\"\n"
            "name = \"lit\"\n"
            "source = \"gray20.png\"\n"
            "threshold = 10000\n"
            "\n"
            "[[page]]\n"
            "name = \"alert\"\n"
            "overlay = true\n"
            "interrupt = true\n"
            "\n"
            "[[reference]]\n"
            "page = \"alert\"\n"
            "element = \"mark_base\"\n"
            "holding = \"referenced\"\n"
            "exercised = [\"identify\"]\n"
            "identify = \"required\"\n"
            "\n"
            "[[page]]\n"
            "name = \"base\"\n"
            "\n"
            "[[reference]]\n"
            "page = \"base\"\n"
            "element = \"mark_base\"\n"
            "holding = \"owned\"\n"
            "exercised = [\"identify\", \"interact\"]\n"
            "identify = \"required\"\n"
            "\n"
            "[[page]]\n"
            "name = \"detail\"\n"
            "overlay = true\n"
            "\n"
            "[[reference]]\n"
            "page = \"detail\"\n"
            "element = \"mark_over\"\n"
            "holding = \"owned\"\n"
            "exercised = [\"identify\", \"interact\"]\n"
            "identify = \"required\"\n"
            "\n"
            "[[edge]]\n"
            "from = \"base\"\n"
            "to = [\"detail\"]\n"
            "via = \"click\"\n"
            "via_element = \"mark_base\"\n"
            "kind = \"push\"\n"
            "future_edge_field = 9\n"
            "\n"
            "[edge.extra]\n"
            "my_edge_weight = 3\n"
            "\n"
            "[[edge]]\n"
            "from = \"detail\"\n"
            "via = \"click\"\n"
            "via_element = \"mark_over\"\n"
            "kind = \"pop\"\n"
            "\n"
            "[[edge]]\n"
            "from = \"detail\"\n"
            "to = [\"base\"]\n"
            "via = \"key\"\n"
            "via_key = \"Z\"\n"
            "kind = \"navigate\"\n";

        // The same model as a hand edit leaves it: pages out of order, edges
        // written before the pages they name, and the three edges in the reverse
        // of the order a save puts them in. A save that never ran would leave
        // these bytes on disk and the comparison against the canonical form
        // would fail.
        constexpr std::string_view k_unsortedGraphProject =
            "schema = \"umbraflow-project/l2-v1\"\n"
            "\n"
            "[[edge]]\n"
            "from = \"detail\"\n"
            "to = [\"base\"]\n"
            "via = \"key\"\n"
            "via_key = \"Z\"\n"
            "kind = \"navigate\"\n"
            "\n"
            "[[edge]]\n"
            "from = \"detail\"\n"
            "via = \"click\"\n"
            "via_element = \"mark_over\"\n"
            "kind = \"pop\"\n"
            "\n"
            "[[edge]]\n"
            "from = \"base\"\n"
            "to = [\"detail\"]\n"
            "via = \"click\"\n"
            "via_element = \"mark_base\"\n"
            "kind = \"push\"\n"
            "future_edge_field = 9\n"
            "\n"
            "[edge.extra]\n"
            "my_edge_weight = 3\n"
            "\n"
            "[[page]]\n"
            "name = \"detail\"\n"
            "overlay = true\n"
            "\n"
            "[[reference]]\n"
            "page = \"detail\"\n"
            "element = \"mark_over\"\n"
            "holding = \"owned\"\n"
            "exercised = [\"identify\", \"interact\"]\n"
            "identify = \"required\"\n"
            "\n"
            "[[page]]\n"
            "name = \"base\"\n"
            "\n"
            "[[reference]]\n"
            "page = \"base\"\n"
            "element = \"mark_base\"\n"
            "holding = \"owned\"\n"
            "exercised = [\"interact\", \"identify\"]\n"
            "identify = \"required\"\n"
            "\n"
            "[[page]]\n"
            "name = \"alert\"\n"
            "interrupt = true\n"
            "overlay = true\n"
            "\n"
            "[[reference]]\n"
            "page = \"alert\"\n"
            "element = \"mark_base\"\n"
            "holding = \"referenced\"\n"
            "exercised = [\"identify\"]\n"
            "identify = \"required\"\n"
            "\n"
            "[[element]]\n"
            "name = \"mark_over\"\n"
            "capabilities = [\"identify\", \"interact\"]\n"
            "rect = [0, 0, 3, 1]\n"
            "\n"
            "[[appearance]]\n"
            "element = \"mark_over\"\n"
            "name = \"lit\"\n"
            "source = \"gray20.png\"\n"
            "threshold = 10000\n"
            "\n"
            "[[element]]\n"
            "name = \"mark_base\"\n"
            "capabilities = [\"identify\", \"interact\"]\n"
            "rect = [0, 0, 3, 1]\n"
            "\n"
            "[[appearance]]\n"
            "element = \"mark_base\"\n"
            "name = \"lit\"\n"
            "source = \"gray2.png\"\n"
            "threshold = 10000\n";

        TEST_CASE("A project file carries the page graph and round trips it too")
        {
            // The persistence half of the graph ruling. Edges are DATA, so they
            // have to survive a load and a save the same way an element does --
            // including the parts this schema version does not understand.
            //
            // Stop writing [[edge]] sections and the rewritten file loses the
            // whole graph. Stop writing the page flags and `detail` comes back as
            // an ordinary page, which makes the push edge that leads to it
            // unbuildable on the next load. Drop the edge residual and
            // `future_edge_field = 9` is silently deleted. Each of those turns
            // the byte comparison below red.
            auto const directory = TemporaryDirectory{"uf-model-graph-roundtrip"};
            seedTemplates(directory.path());
            auto const modelPath = directory.path() / "page-model.toml";
            REQUIRE(k_unsortedGraphProject != k_canonicalGraphProject);
            writeFile(modelPath, std::as_bytes(std::span{k_unsortedGraphProject}));

            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(2, 20, 0)},
                    .projectRoot = directory.path(),
                }
            );
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runModel(context, built, R"lua(
                local loaded = project.load_project(ctx)
                local graph = loaded.graph
                if graph == nil then return 0 end
                if #graph.edges ~= 3 then return 0 end

                -- The flags came back as flags rather than as unknown keys.
                if loaded.page_by_name.base.overlay ~= false then return 0 end
                if loaded.page_by_name.base.interrupt ~= false then return 0 end
                if loaded.page_by_name.detail.overlay ~= true then return 0 end
                if loaded.page_by_name.detail.interrupt ~= false then return 0 end
                if loaded.page_by_name.alert.interrupt ~= true then return 0 end

                -- An interrupt page is found by its flag and by nothing else:
                -- no edge in this file leads to it.
                if #graph.interrupts ~= 1 then return 0 end
                if graph.interrupts[1].name ~= "alert" then return 0 end

                local outgoing =
                    navigation.Graph.edges_from(graph, loaded.page_by_name.base)
                if #outgoing ~= 1 then return 0 end
                local push = outgoing[1]
                if push.kind ~= "push" then return 0 end
                if push.via ~= "click" then return 0 end
                if push.via_element.name ~= "mark_base" then return 0 end
                if push.via_reference == nil then return 0 end
                if push.via_reference.exercised.interact ~= true then return 0 end
                if #push.to ~= 1 then return 0 end
                if push.to[1].name ~= "detail" then return 0 end

                -- A project's own field lives under extra and NOWHERE else, and
                -- a key of this layer's own section that this build does not
                -- know is carried verbatim rather than mistaken for one.
                if push.extra.my_edge_weight ~= 3 then return 0 end
                if rawget(push, "my_edge_weight") ~= nil then return 0 end
                if #push.residual ~= 1 then return 0 end
                if push.residual[1] ~= "future_edge_field = 9" then return 0 end
                if push.extra.future_edge_field ~= nil then return 0 end

                -- The pop declares no destination at all.
                local fromDetail =
                    navigation.Graph.edges_from(graph, loaded.page_by_name.detail)
                if #fromDetail ~= 2 then return 0 end
                local pop = nil
                for _, edge in fromDetail do
                    if edge.kind == "pop" then pop = edge end
                end
                if pop == nil then return 0 end
                if pop.to ~= nil then return 0 end
                if pop.via_element.name ~= "mark_over" then return 0 end

                project.save_project(ctx, loaded)

                -- Loading what was just written and writing it again is the
                -- fixpoint half: the first save normalises a hand-edited file,
                -- and the second must change nothing at all.
                project.save_project(ctx, project.load_project(ctx))
                return 1
            )lua");
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));

            CHECK(readFileText(modelPath) == std::string{k_canonicalGraphProject});
        }

        TEST_CASE("A project file whose edge names something it never declared is refused")
        {
            auto const directory = TemporaryDirectory{"uf-model-edge-dangling"};
            seedTemplates(directory.path());
            constexpr std::string_view dangling =
                "schema = \"umbraflow-project/l2-v1\"\n"
                "\n"
                "[[element]]\n"
                "name = \"mark_base\"\n"
                "capabilities = [\"identify\", \"interact\"]\n"
                "rect = [0, 0, 3, 1]\n"
                "\n"
                "[[appearance]]\n"
                "element = \"mark_base\"\n"
                "name = \"lit\"\n"
                "source = \"gray2.png\"\n"
                "threshold = 10000\n"
                "\n"
                "[[page]]\n"
                "name = \"base\"\n"
                "\n"
                "[[reference]]\n"
                "page = \"base\"\n"
                "element = \"mark_base\"\n"
                "holding = \"owned\"\n"
                "exercised = [\"identify\", \"interact\"]\n"
                "identify = \"required\"\n"
                "\n"
                "[[edge]]\n"
                "from = \"base\"\n"
                "to = [\"nowhere\"]\n"
                "via = \"click\"\n"
                "via_element = \"mark_base\"\n"
                "kind = \"navigate\"\n";
            writeFile(
                directory.path() / "page-model.toml",
                std::as_bytes(std::span{dangling})
            );

            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(2, 0, 0)},
                    .projectRoot = directory.path(),
                }
            );
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runModel(context, built, R"lua(
                local ok, err = pcall(function()
                    return project.load_project(ctx)
                end)
                if ok then return 0 end
                if string.find(err, "names page 'nowhere'", 1, true) == nil then
                    return 0
                end
                -- The line number is what makes it fixable, and it is the [[edge]]
                -- section's own rather than the file's first.
                if string.find(err, "line 24", 1, true) == nil then return 0 end
                return 1
            )lua");
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("A project file this build cannot read is refused by name")
        {
            auto const directory = TemporaryDirectory{"uf-model-project-schema"};
            seedTemplates(directory.path());
            constexpr std::string_view future =
                "schema = \"umbraflow-project/l2-v9\"\n";
            writeFile(
                directory.path() / "page-model.toml",
                std::as_bytes(std::span{future})
            );

            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(2, 5, 0)},
                    .projectRoot = directory.path(),
                }
            );
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runModel(context, built, R"lua(
                local ok, err = pcall(function()
                    return project.load_project(ctx)
                end)
                if ok then return 0 end
                if string.find(err, "umbraflow-project/l2-v9", 1, true) == nil then
                    return 0
                end
                return 1
            )lua");
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("A project file cannot give one element two homes")
        {
            auto const directory = TemporaryDirectory{"uf-model-project-owner"};
            seedTemplates(directory.path());
            constexpr std::string_view twoHomes =
                "schema = \"umbraflow-project/l2-v1\"\n"
                "\n"
                "[[element]]\n"
                "name = \"anchor\"\n"
                "capabilities = [\"identify\"]\n"
                "rect = [0, 0, 3, 1]\n"
                "\n"
                "[[appearance]]\n"
                "element = \"anchor\"\n"
                "name = \"on_dark\"\n"
                "source = \"gray2.png\"\n"
                "threshold = 10000\n"
                "\n"
                "[[page]]\n"
                "name = \"first\"\n"
                "\n"
                "[[reference]]\n"
                "page = \"first\"\n"
                "element = \"anchor\"\n"
                "holding = \"owned\"\n"
                "exercised = [\"identify\"]\n"
                "identify = \"required\"\n"
                "\n"
                "[[page]]\n"
                "name = \"second\"\n"
                "\n"
                "[[reference]]\n"
                "page = \"second\"\n"
                "element = \"anchor\"\n"
                "holding = \"owned\"\n"
                "exercised = [\"identify\"]\n"
                "identify = \"required\"\n";
            writeFile(
                directory.path() / "page-model.toml",
                std::as_bytes(std::span{twoHomes})
            );

            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(2, 5, 0)},
                    .projectRoot = directory.path(),
                }
            );
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runModel(context, built, R"lua(
                local ok, err = pcall(function()
                    return project.load_project(ctx)
                end)
                if ok then return 0 end
                if string.find(err, "an element has one home", 1, true) == nil then
                    return 0
                end
                return 1
            )lua");
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("The three model modules are in the framework bundle")
        {
            // The modules ship with the binary rather than with a project, which
            // is the whole of what makes them trusted. Nothing else in this file
            // would notice a module that failed to be embedded, because the
            // config the tests boot names them by hand.
            auto const entries = frameworkBundleEntries();
            for (auto const& expected : {"model", "observe", "project"})
            {
                auto found = false;
                for (auto const& entry : entries)
                {
                    if (entry.name == std::string_view{expected})
                    {
                        found = true;
                    }
                }
                INFO("framework bundle carries ", expected);
                CHECK(found);
            }
        }
    }
}
