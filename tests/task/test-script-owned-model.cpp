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
            for (auto const gray : {uint8{2}, uint8{3}, uint8{5}, uint8{20}})
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

        // The VM a layer-three script would boot, plus the three framework
        // modules this work order added, published under their own names.
        //
        // They are published HERE rather than in frameworkProjectGlobals()
        // because wiring them into every task VM is a host decision that belongs
        // with whoever lands the layer-three loader; the modules themselves are
        // in the bundle already, and a test that publishes them is exactly what
        // a project environment will do once that wiring exists.
        [[nodiscard]]
        auto modelVmConfig(CapabilitySurface const& surface, TaskContext& context)
            -> script::EngineConfig
        {
            auto config = taskVmConfig(surface, context);
            config.frameworkProjectGlobals.emplace_back("model");
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
