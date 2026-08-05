#include "binding-fixture.hpp"

#include <task/script-bindings.hpp>
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

#include <algorithm>
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
// the project file in project.luau. Everything here drives real Luau against a
// real session, because a pure-Luau harness would prove the tables are shaped
// right and nothing about what they do with pixels.
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
        // them back by. A model's appearances are project files, so seeding them is
        // what makes template_load reachable from a script at all.
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

        // A fake OCR adapter that answers one line, so the read verb has something
        // to compare against. It refuses Block exactly as the shipped adapter does,
        // so a host that quietly asked for Block fails here too.
        //
        // It can answer a DIFFERENT line each call, in the order the readouts were
        // given, repeating the last once they run out. That is what lets a case put
        // two screens in front of the falsification matrix and have the same region
        // read something different on each: the matrix opens one observation per
        // screen and reads once per claimed cell, so call order IS screen order.
        class FakeOcrEngine final : public ocr::IOcrEngine
        {
            std::vector<ocr::Readout> m_readouts;
            ocr::Readout              m_block{};
            std::size_t               m_next{0};
            bool                      m_answersBlock{false};

        public:
            explicit FakeOcrEngine(ocr::Readout readout)
                : m_readouts{std::move(readout)}
            {
            }

            explicit FakeOcrEngine(std::vector<ocr::Readout> readouts)
                : m_readouts{std::move(readouts)}
            {
                REQUIRE(!m_readouts.empty());
            }

            // The block-reading fake. Supplying a `block` readout is what makes
            // this engine claim to have a detector at all; the single-line sequence
            // is untouched, so a case can use both layouts on one engine.
            FakeOcrEngine(ocr::Readout readout, ocr::Readout block)
                : m_readouts{std::move(readout)}
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
            auto read(BgraImage const& /*image*/, ocr::ReadSpec const& spec)
                -> Result<ocr::Readout> override
            {
                if (spec.layout == ocr::TextLayout::Block)
                {
                    if (!m_answersBlock)
                    {
                        return fail(
                            AutomationErrorKind::UnsupportedCapability,
                            "this adapter does not run the detection model"
                        );
                    }
                    return m_block;
                }
                auto const index = std::min(m_next, m_readouts.size() - 1U);
                ++m_next;
                return m_readouts[index];
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
            CountingActionSink*                   clicks;
            FakeFrameSource*                      frames;

            // Every case records rather than discards: the framework's semantic
            // events pass the host's validation state machine on the way here,
            // so a run that reaches this sink is a run whose sequence the host
            // accepted, and a case that wants to read the sequence can.
            RecordingTraceSink*                   traces;
        };

        [[nodiscard]]
        auto buildHarness(HarnessSpec spec) -> Harness
        {
            auto const fingerprint = fixtureFingerprint();
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
            auto traceSink         = std::make_unique<RecordingTraceSink>();
            auto* const p_traces   = traceSink.get();
            auto recorder          = std::make_unique<trace::TraceRecorder>(
                std::move(traceSink),
                k_fixtureRunId,
                k_fixtureGenerationId,
                trace::FrontEnd::Task
            );
            auto session = engine::EngineSession::create(
                std::move(frameSource),
                std::move(actionSink),
                *recorder,
                engine::EngineSessionConfig{
                    .liveFingerprint         = fingerprint,
                    .projectFingerprint      = fingerprint,
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
                .clicks   = p_clicks,
                .frames   = p_frames,
                .traces   = p_traces,
            };
        }

        // The VM a layer-three script would boot, plus the framework modules the
        // script-owned page model added, published under their own names. They are
        // published HERE rather than in frameworkProjectGlobals() because wiring
        // them into every task VM is a host decision that belongs with whoever
        // lands the layer-three loader.
        //
        // `mint` is deliberately NOT among them: it is the spec-checking toolkit
        // the constructors share, so publishing it would put a second, unchecked
        // way to shape a model in front of a project script for no gain.
        [[nodiscard]]
        auto modelVmConfig(TaskContext& context) -> script::EngineConfig
        {
            auto config = taskVmConfig(context);
            config.frameworkProjectGlobals.emplace_back("model");
            config.frameworkProjectGlobals.emplace_back("navigation");
            config.frameworkProjectGlobals.emplace_back("observe");
            config.frameworkProjectGlobals.emplace_back("project");
            config.frameworkProjectGlobals.emplace_back("hits");
            return config;
        }

        [[nodiscard]]
        auto runModel(TaskContext& context, Harness& /*harness*/, std::string_view source)
            -> Result<double>
        {
            auto engine = script::Engine::create(modelVmConfig(context));
            REQUIRE(engine.has_value());
            return engine->runNumber(source, "script-owned-model");
        }

        // Frames, by what the model built on them sees. The fixture frame is three
        // grey pixels: gray 2 at x = 0 and gray 5 at x = 1 on the resolving frame,
        // so a one-by-one template of either grey is an element that hits at a
        // known pixel and misses on a frame that does not carry it.
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

        // A constructor refusal, expressed as the smallest body that proves it: the
        // call raises, and the sentence it raised names the thing that is wrong.
        // Returning 1 only on both counts is what keeps a refusal for the WRONG
        // reason from passing. The fragment goes in a long-bracket literal because
        // the sentences under test quote the field names they are about. It is the
        // BODY rather than the whole script, because a refusal list whose subject
        // needs its own fixture puts that fixture between the prelude and this.
        [[nodiscard]]
        auto refusalBody(std::string_view body, std::string_view fragment)
            -> std::string
        {
            return std::string{"local ok, err = pcall(function()\n"}
                + std::string{body}
                + "\nend)\n"
                  "if ok then return 0 end\n"
                  "if type(err) ~= 'string' then return 0 end\n"
                  "if string.find(err, [==["
                + std::string{fragment}
                + "]==], 1, true) == nil then return 0 end\n"
                  "return 1\n";
        }

        [[nodiscard]]
        auto refusalScript(std::string_view body, std::string_view fragment)
            -> std::string
        {
            return script(refusalBody(body, fragment));
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
                    .label = "identify with neither pixels nor a way to read",
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
                    .label = "a read floor outside basis points",
                    .body  = R"lua(
                        return model.Element.new{
                            name = "title",
                            capabilities = { "read" },
                            rect = { x = 0, y = 0, width = 3, height = 1 },
                            read_floor = 10001,
                        }
                    )lua",
                    .fragment = "read_floor must be a whole number",
                },
                Refusal{
                    .label = "a read floor on an element nothing may read",
                    .body  = R"lua(
                        return model.Element.new{
                            name = "title",
                            capabilities = { "interact" },
                            rect = { x = 0, y = 0, width = 3, height = 1 },
                            read_floor = 9000,
                        }
                    )lua",
                    .fragment = "has a read_floor but does not declare",
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
                Refusal{
                    .label = "an extra table that refers back to itself",
                    .body  = R"lua(
                        local mine = {}
                        mine.mine = mine
                        return model.Element.new{
                            name = "back",
                            capabilities = { "interact" },
                            rect = { x = 0, y = 0, width = 3, height = 1 },
                            extra = mine,
                        }
                    )lua",
                    .fragment = "nests more than 8 tables deep",
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
            -- The shared title box: no template of its own, and no text of its
            -- own either, because what it reads is the page's business.
            local title = model.Element.new{
                name = "title",
                capabilities = { "identify", "read" },
                rect = { x = 0, y = 0, width = 3, height = 1 },
            }
            -- One confirm button drawn once and placed by whoever uses it: no
            -- rectangle of its own, so a row that references it has to say where
            -- it sits on that page.
            local confirm = model.Element.new{
                name = "confirm",
                capabilities = { "interact" },
                appearances = {
                    {
                        name = "only",
                        source = "gray5.png",
                        template = template("gray5.png"),
                        threshold = 10000,
                    },
                },
            }
            -- The same, verified by text rather than by pixels, so a refusal
            -- about identifying with it fails on the rectangle rather than on
            -- having nothing to read.
            local floating = model.Element.new{
                name = "floating",
                capabilities = { "identify", "read" },
                expected_text = "confirm",
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
                    // The name is emitted as a trace scope label the instant
                    // this page resolves, and the host refuses a label over 64
                    // bytes -- from inside observe.resolve_page, which nothing
                    // pcalls. Twenty-two CJK characters are 66 bytes, so a name
                    // no author would call long is already over it. Drop the
                    // check in Page.new and this page is built, then kills the
                    // run on the first frame it is actually on.
                    .label = "a page name too long to write to the trace",
                    .body  = R"lua(
                        return model.Page.new{
                            name = "限時活動戰鬥員配置確認畫面標題列的長名稱測試",
                            references = { anchorRow({}) },
                        }
                    )lua",
                    .fragment = "is 66 bytes, over the host's 64-byte ceiling",
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
                    .label = "expected text on a row whose element nothing may read",
                    .body  = R"lua(
                        return model.Page.new{
                            name = "battle",
                            references = { anchorRow({ expected_text = "battle" }) },
                        }
                    )lua",
                    .fragment = "carries expected_text but element 'anchor'",
                },
                Refusal{
                    .label = "expected text that says nothing",
                    .body  = R"lua(
                        return model.Page.new{
                            name = "battle",
                            references = {
                                {
                                    element = title,
                                    holding = "owned",
                                    exercised = { "identify" },
                                    identify = "required",
                                    expected_text = "",
                                },
                            },
                        }
                    )lua",
                    .fragment = "expected_text must be the text this page's copy",
                },
                Refusal{
                    .label = "identifying by a region nobody said what reads",
                    .body  = R"lua(
                        return model.Page.new{
                            name = "battle",
                            references = {
                                {
                                    element = title,
                                    holding = "owned",
                                    exercised = { "identify" },
                                    identify = "required",
                                },
                            },
                        }
                    )lua",
                    .fragment = "no appearances and no expected text here",
                },
                Refusal{
                    .label = "a row placing an element that draws no rectangle nowhere",
                    .body  = R"lua(
                        return model.Page.new{
                            name = "battle",
                            references = {
                                anchorRow({}),
                                {
                                    element = confirm,
                                    holding = "owned",
                                    exercised = { "interact" },
                                },
                            },
                        }
                    )lua",
                    .fragment = "and says no rect_override",
                },
                Refusal{
                    .label = "identifying by an element that draws no rectangle",
                    .body  = R"lua(
                        return model.Page.new{
                            name = "battle",
                            references = {
                                {
                                    element = floating,
                                    holding = "owned",
                                    exercised = { "identify" },
                                    identify = "required",
                                },
                            },
                        }
                    )lua",
                    .fragment = "which draws no rectangle of its own; an identify",
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

        TEST_CASE("An element's extra is a snapshot every level down")
        {
            // Stop mint.frozen_extra descending into a nested table and the two
            // writes below succeed, so this goes red.
            auto const directory = TemporaryDirectory{"uf-model-extra-snapshot"};
            seedTemplates(directory.path());
            auto built = refusalHarness(directory.path());
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runModel(context, built, script(R"lua(
                local mine = { tags = { "boss" }, limits = { retries = 2 } }
                local element = model.Element.new{
                    name = "back",
                    capabilities = { "interact" },
                    rect = { x = 0, y = 0, width = 3, height = 1 },
                    extra = mine,
                }

                -- The table the author kept is still theirs to write, and what
                -- they write there is not what the element holds.
                mine.tags[2] = "added after minting"
                mine.limits.retries = 9
                if #element.extra.tags ~= 1 then return 0 end
                if element.extra.limits.retries ~= 2 then return 0 end

                if not table.isfrozen(element.extra) then return 0 end
                if not table.isfrozen(element.extra.tags) then return 0 end
                if not table.isfrozen(element.extra.limits) then return 0 end

                -- And the snapshot refuses the write at every level rather than
                -- only at the one the caller happened to reach first.
                if pcall(function() element.extra.tags[1] = "rewritten" end) then
                    return 0
                end
                if pcall(function() element.extra.limits.retries = 9 end) then
                    return 0
                end
                if pcall(function() element.extra.limits.added = 1 end) then
                    return 0
                end
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("Page.new accepts an element layer three derived from ours")
        {
            // The inheritance ruling, made observable: layer three extends a
            // layer-two element with setmetatable and __index, and the page that
            // references the derived table is built rather than refused. Tighten
            // the check to plain identity and this goes red.
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

        TEST_CASE("An element that draws no rectangle is placed by each row that uses it")
        {
            // `rect` has only ever meant where to LOOK, and some shapes have no
            // answer of their own: one confirm button drawn once sits somewhere
            // different on every screen that shows it, so carrying the rectangle it
            // was cut from made the model state a fact nothing could contradict.
            //
            // Two pages place the same pixels at two rectangles here, and the
            // find answers where each row says. Delete the rect_override branch
            // in observe.luau's local searchRect and the chunk raises on the
            // very first find -- `confirm` draws no rectangle, so what is left
            // is the branch that refuses. That is why the case asserts both a
            // nil find at [0, 0] and a hit at [1, 0]: only the two together show
            // the ROW deciding where to look rather than the element.
            auto const directory = TemporaryDirectory{"uf-model-placed-element"};
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
                local anchor = model.Element.new{
                    name = "anchor",
                    capabilities = { "identify" },
                    rect = { x = 0, y = 0, width = 3, height = 1 },
                    appearances = {
                        {
                            name = "only",
                            source = "gray2.png",
                            template = template("gray2.png"),
                            threshold = 10000,
                        },
                    },
                }
                local confirm = model.Element.new{
                    name = "confirm",
                    capabilities = { "interact" },
                    appearances = {
                        {
                            name = "only",
                            source = "gray5.png",
                            template = template("gray5.png"),
                            threshold = 10000,
                        },
                    },
                }
                if confirm.rect ~= nil then return 0 end

                local function pageAt(name, holding, rect)
                    return model.Page.new{
                        name = name,
                        references = {
                            {
                                element = anchor,
                                holding = "referenced",
                                exercised = { "identify" },
                                identify = "required",
                            },
                            {
                                element = confirm,
                                holding = holding,
                                exercised = { "interact" },
                                rect_override = rect,
                            },
                        },
                    }
                end
                local here = pageAt(
                    "here",
                    "owned",
                    { x = 1, y = 0, width = 1, height = 1 }
                )
                local elsewhere = pageAt(
                    "elsewhere",
                    "referenced",
                    { x = 0, y = 0, width = 1, height = 1 }
                )

                local ticket = ctx:cycle_open()

                -- The page that says these pixels sit at x = 0 is looking at the
                -- wrong pixel, and says so rather than finding the button two
                -- columns over. One element, two rows, two answers.
                if observe.resolve_page(ctx, ticket, elsewhere) == nil then
                    return 0
                end
                if observe.find(ctx, ticket, elsewhere, confirm) ~= nil then
                    return 0
                end

                local receipt = observe.resolve_page(ctx, ticket, here)
                if receipt == nil then return 0 end
                local hit = observe.find(ctx, ticket, here, confirm)
                if hit == nil then return 0 end
                if hit.positioned_by ~= "pixels" then return 0 end
                if hit.x ~= 1 then return 0 end
                observe.click(ctx, ticket, receipt, hit)
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
            CHECK(p_clicks->clickCount() == 1U);

            // The click named the element it was authorised against, and named it
            // BEFORE the engine reported the delivery. Nothing else in the stream
            // can: the page model is this layer's, so engine.action_delivered
            // carries a frame and a client point and nothing that ties either to
            // a model -- which is what leaves a replay able to attribute a
            // keystroke to an edge and not a click.
            auto const& events = built.traces->events();
            auto clicked   = std::optional<std::size_t>{};
            auto delivered = std::optional<std::size_t>{};
            for (auto index = std::size_t{0}; index < events.size(); ++index)
            {
                auto const& event = events[index].event();
                if (event.kind == trace::TraceEventKind::FrameworkElementClicked)
                {
                    REQUIRE(event.framework.has_value());
                    CHECK(event.framework->label == "confirm");
                    clicked = index;
                }
                if (event.kind == trace::TraceEventKind::EngineActionDelivered)
                {
                    delivered = index;
                }
            }
            REQUIRE(clicked.has_value());
            REQUIRE(delivered.has_value());
            CHECK(*clicked < *delivered);
        }

        TEST_CASE("Reading an unplaced element needs the row that places it")
        {
            // The read verbs take a BARE element as well as a page's row, so they
            // are the one door a shape with no rectangle can reach without the row
            // that gives it one. `Page.new` guards the other door; the alternative
            // here is indexing a nil inside cycle_read and reporting a host failure.
            auto const directory = TemporaryDirectory{"uf-model-unplaced-read"};
            seedTemplates(directory.path());
            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(2, 5, 0)},
                    .ocrEngine   = std::make_unique<FakeOcrEngine>(
                        oneLineReadout("confirm", 9'600)
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

            auto const result = runModel(context, built, script(R"lua(
                local confirm = model.Element.new{
                    name = "confirm",
                    capabilities = { "interact", "read" },
                }
                local anchor = model.Element.new{
                    name = "anchor",
                    capabilities = { "identify" },
                    rect = { x = 0, y = 0, width = 3, height = 1 },
                    appearances = {
                        {
                            name = "only",
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
                            element = anchor,
                            holding = "owned",
                            exercised = { "identify" },
                            identify = "required",
                        },
                        {
                            element = confirm,
                            holding = "owned",
                            exercised = { "interact", "read" },
                            rect_override = { x = 0, y = 0, width = 3, height = 1 },
                        },
                    },
                }

                local ticket = ctx:cycle_open()
                local ok, err = pcall(function()
                    return observe.read_element(
                        ctx, ticket, confirm, observe.empty_is_absence
                    )
                end)
                if ok then return 0 end
                if type(err) ~= "string" then return 0 end
                if string.find(err, "draws no rectangle of its own", 1, true) == nil then
                    return 0
                end

                -- And the same element read through the row that places it comes
                -- back, so the refusal is about the missing rectangle rather than
                -- about the element.
                local row = model.Page.reference_for(page, confirm)
                local reading = observe.read_element(
                    ctx, ticket, row, observe.empty_is_absence
                )
                if reading == nil then return 0 end
                if reading.text ~= "confirm" then return 0 end
                return 1
            )lua"));
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
                    local receipt, why = observe.resolve_page(ctx, ticket, battle)
                    ctx:cycle_close(ticket)
                    if receipt == nil then return 0 end
                    if receipt.page ~= battle then return 0 end
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
                    local receipt, why = observe.resolve_page(ctx, ticket, battle)
                    ctx:cycle_close(ticket)
                    if receipt ~= nil then return 0 end
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
                // The overlay case in miniature: the page is only this page while
                // the grey-5 element is absent. Delete the forbidden branch and the
                // page resolves on a frame that carries it, so this goes red.
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
                    local receipt, why = observe.resolve_page(ctx, ticket, plain)
                    ctx:cycle_close(ticket)
                    if receipt ~= nil then return 0 end
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
            // The identify sweep runs BEFORE the page is known, so it cannot consult
            // a page's pin. The page below pins on_dark, the frame carries only the
            // on_light pixels, and the page must still resolve. Honour the pin
            // inside resolve_page and this goes red -- which is the whole reason the
            // pin is documented as interact-only.
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
                if observe.resolve_page(ctx, ticket, battle) == nil then return 0 end

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
            // The fail-closed rule, one layer up. cycle_match raises when a budget
            // stopped the search, and no verb here may turn that into "the element
            // is not on screen": a page that reported itself absent because a number
            // ran out would make the model's answer a function of a configuration
            // value. Wrap the cycle_match call in observe.foldAppearances with a
            // pcall and this case goes red -- resolve_page starts returning false.
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
                // "Declaration order only breaks ties", as a fact. On a [2, 5, 0]
                // frame the grey-3 template is declared first and DOES clear its
                // own threshold, at x = 0 with a score of one; the grey-5 template
                // scores zero at x = 1. Under "first past the threshold wins" the
                // hit would name wide_first and a click would land on the wrong
                // pixel with nothing downstream noticing.
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
                local receipt = observe.resolve_page(ctx, ticket, battle)
                if receipt == nil then return 0 end
                local hit = observe.find(ctx, ticket, battle, slot)
                if hit == nil then return 0 end
                if hit.positioned_by ~= "page" then return 0 end
                if hit.page ~= "battle" then return 0 end
                if hit.match ~= nil then return 0 end
                if hit.click_x ~= 1 or hit.click_y ~= 0 then return 0 end
                observe.click(ctx, ticket, receipt, hit)
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
            CHECK(p_clicks->clickCount() == 1U);
        }

        TEST_CASE("observe.long_press presses a page-positioned element")
        {
            // The run-mode half of the long-press capability, and the only place it
            // can be shown: a run VM publishes no `explore`, so the one way to prove
            // its private surface really binds cycle_long_press is to deliver one
            // through the trusted framework. Move the installPrimitive call for
            // cycle_long_press inside buildPrivateSurface's Exploration branch and
            // this goes red, because `native.cycle_long_press` is then nil on a run
            // VM.
            auto const directory = TemporaryDirectory{"uf-model-long-press"};
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
                local receipt = observe.resolve_page(ctx, ticket, battle)
                if receipt == nil then return 0 end
                local hit = observe.find(ctx, ticket, battle, slot)
                if hit == nil then return 0 end
                observe.long_press(ctx, ticket, receipt, hit, 400)
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));

            // The hold the script named arrived, and it arrived as a press rather
            // than as a click: two claims a sloppier wiring would break separately.
            REQUIRE(p_clicks->longPresses().size() == 1U);
            CHECK(
                p_clicks->longPresses().front().hold
                == MonotonicInstant::Duration{std::chrono::milliseconds{400}}
            );
            CHECK(p_clicks->clickCount() == 0U);
        }

        TEST_CASE("observe.long_press demands the same authorisation a click does")
        {
            // A long press delivers pointer input at a hit exactly as a click does,
            // so it goes through the same door: the same receipt for this ticket,
            // the same page, the same hit from this frame, the same interact edge.
            // Swap the requireAuthorisedHit call in observe.long_press for nothing
            // at all and the first two subcases go red while observe.click stays
            // green. The third is behind that door rather than part of it: the hold
            // is this verb's own argument, checked after the authorisation passes.
            auto const directory = TemporaryDirectory{"uf-model-long-press-auth"};
            seedTemplates(directory.path());

            struct PressCase final
            {
                std::string_view label;
                std::string_view body;
                std::string_view fragment;
            };

            auto const cases = std::vector<PressCase>{
                PressCase{
                    .label = "no receipt at all",
                    .body  = R"lua(
                        local ticket = ctx:cycle_open()
                        local hit = observe.find(ctx, ticket, battle, slot)
                        if hit == nil then return 0 end
                        local ok, err = pcall(function()
                            observe.long_press(ctx, ticket, nil, hit, 400)
                        end)
                        ctx:cycle_close(ticket)
                        if ok then return 0 end
                        return report(err)
                    )lua",
                    .fragment = "needs the receipt observe.resolve_page returned",
                },
                PressCase{
                    .label = "a hit located on an earlier cycle",
                    .body  = R"lua(
                        local first = ctx:cycle_open()
                        local stale = observe.find(ctx, first, battle, slot)
                        ctx:cycle_close(first)
                        if stale == nil then return 0 end

                        local second = ctx:cycle_open()
                        local receipt = observe.resolve_page(ctx, second, battle)
                        if receipt == nil then return 0 end
                        local ok, err = pcall(function()
                            observe.long_press(ctx, second, receipt, stale, 400)
                        end)
                        ctx:cycle_close(second)
                        if ok then return 0 end
                        return report(err)
                    )lua",
                    .fragment = "hit located on another observation cycle",
                },
                PressCase{
                    .label = "a hold nobody named",
                    .body  = R"lua(
                        local ticket = ctx:cycle_open()
                        local receipt = observe.resolve_page(ctx, ticket, battle)
                        if receipt == nil then return 0 end
                        local hit = observe.find(ctx, ticket, battle, slot)
                        if hit == nil then return 0 end
                        local ok, err = pcall(function()
                            observe.long_press(ctx, ticket, receipt, hit)
                        end)
                        ctx:cycle_close(ticket)
                        if ok then return 0 end
                        return report(err)
                    )lua",
                    .fragment = "it has no default",
                },
            };

            for (auto const& subcase : cases)
            {
                CAPTURE(subcase.label);

                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(2, 5, 0), pixels(2, 5, 0)},
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

                auto source = std::string{
                    "local function report(err)\n"
                    "    if type(err) ~= 'string' then return 0 end\n"
                    "    if string.find(err, [==["
                };
                source += subcase.fragment;
                source += "]==], 1, true) == nil then return 0 end\n"
                          "    return 1\n"
                          "end\n";
                source += subcase.body;

                auto const result = runModel(context, built, battleScript(source));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
                CHECK(p_clicks->longPresses().empty());
                CHECK(p_clicks->clickCount() == 0U);
            }
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
                local receipt = observe.resolve_page(ctx, ticket, battle)
                if receipt == nil then return 0 end
                local hit = observe.find(ctx, ticket, battle, anchor)
                if hit == nil then return 0 end
                if hit.appearance ~= "on_dark" then return 0 end
                if hit.x ~= 0 then return 0 end
                observe.click(ctx, ticket, receipt, hit)
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
            CHECK(p_clicks->clickCount() == 1U);
        }

        TEST_CASE("Finding is allowed without interact and clicking is not")
        {
            // The enforcement that moved out of C++ with the model. A state element
            // the page only identifies must stay queryable -- reading which
            // appearance hit is how a script knows what state the game is in --
            // while the click is refused HERE, by the trusted framework. Drop the
            // interact test in observe.click and the click is delivered, so the
            // counter below goes to one and this case goes red.
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
                local receipt = observe.resolve_page(ctx, ticket, page)
                if receipt == nil then return 0 end
                local hit = observe.find(ctx, ticket, page, state)
                if hit == nil then return 0 end
                if hit.appearance ~= "auto_on" then return 0 end
                if hit.interact ~= false then return 0 end

                local ok, err = pcall(function()
                    observe.click(ctx, ticket, receipt, hit)
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
                local receipt = observe.resolve_page(ctx, ticket, battle)
                if receipt == nil then return 0 end
                local ok, err = pcall(function()
                    observe.click(ctx, ticket, receipt, {
                        element = slot,
                        page = "battle",
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

        // The same-frame authorisation rule, which the move of the page model into
        // Luau dissolved and the receipt restores. `resolve_page` returning a
        // boolean carries no trace of the ticket that produced it, so nothing would
        // stop a script from resolving on one frame and clicking on the next. Each
        // subcase below is a way of arriving at `observe.click` without that
        // evidence, and each one must refuse with no click delivered. Delete the
        // corresponding check in `observe.click` and exactly one of them goes red.
        TEST_CASE("clicking requires this cycle's own proof of the page")
        {
            auto const directory = TemporaryDirectory{"uf-model-receipt"};
            seedTemplates(directory.path());

            struct ReceiptCase final
            {
                std::string_view label;
                std::string_view body;
                std::string_view fragment;
            };

            auto const cases = std::vector<ReceiptCase>{
                ReceiptCase{
                    .label = "a receipt no resolution minted",
                    .body  = R"lua(
                        local ticket = ctx:cycle_open()
                        local hit = observe.find(ctx, ticket, battle, anchor)
                        if hit == nil then return 0 end
                        local forged = { page = battle, ticket = ticket }
                        local ok, err = pcall(function()
                            observe.click(ctx, ticket, forged, hit)
                        end)
                        ctx:cycle_close(ticket)
                        if ok then return 0 end
                        return report(err)
                    )lua",
                    .fragment = "needs the receipt observe.resolve_page returned",
                },
                ReceiptCase{
                    .label = "no receipt at all",
                    .body  = R"lua(
                        local ticket = ctx:cycle_open()
                        local hit = observe.find(ctx, ticket, battle, anchor)
                        if hit == nil then return 0 end
                        local ok, err = pcall(function()
                            observe.click(ctx, ticket, nil, hit)
                        end)
                        ctx:cycle_close(ticket)
                        if ok then return 0 end
                        return report(err)
                    )lua",
                    .fragment = "needs the receipt observe.resolve_page returned",
                },
                ReceiptCase{
                    .label = "a receipt minted on an earlier cycle",
                    .body  = R"lua(
                        local first = ctx:cycle_open()
                        local stale = observe.resolve_page(ctx, first, battle)
                        ctx:cycle_close(first)
                        if stale == nil then return 0 end

                        local second = ctx:cycle_open()
                        local hit = observe.find(ctx, second, battle, anchor)
                        if hit == nil then return 0 end
                        local ok, err = pcall(function()
                            observe.click(ctx, second, stale, hit)
                        end)
                        ctx:cycle_close(second)
                        if ok then return 0 end
                        return report(err)
                    )lua",
                    .fragment = "minted on another observation cycle",
                },
                ReceiptCase{
                    .label = "a receipt for another page that also resolved",
                    .body  = R"lua(
                        local elsewhere = model.Page.new{
                            name = "elsewhere",
                            references = {
                                {
                                    element = anchor,
                                    holding = "referenced",
                                    exercised = { "identify" },
                                    identify = "required",
                                },
                            },
                        }
                        local ticket = ctx:cycle_open()
                        local other = observe.resolve_page(ctx, ticket, elsewhere)
                        if other == nil then return 0 end
                        local hit = observe.find(ctx, ticket, battle, anchor)
                        if hit == nil then return 0 end
                        local ok, err = pcall(function()
                            observe.click(ctx, ticket, other, hit)
                        end)
                        ctx:cycle_close(ticket)
                        if ok then return 0 end
                        return report(err)
                    )lua",
                    // "interactive" rather than "clickable": the refusal is written
                    // once and serves observe.click and observe.long_press alike,
                    // so its wording names acting on the element rather than one
                    // way of doing so.
                    .fragment = "interactive on the page that was recognised",
                },
            };

            for (auto const& subcase : cases)
            {
                CAPTURE(subcase.label);

                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(2, 5, 0), pixels(2, 5, 0)},
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

                // The refusal has to be the RIGHT refusal: a case that failed for
                // an unrelated reason would pass while proving nothing about the
                // receipt.
                auto source = std::string{
                    "local function report(err)\n"
                    "    if type(err) ~= 'string' then return 0 end\n"
                    "    if string.find(err, [==["
                };
                source += subcase.fragment;
                source += "]==], 1, true) == nil then return 0 end\n"
                          "    return 1\n"
                          "end\n";
                source += subcase.body;

                auto const result = runModel(context, built, battleScript(source));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
                CHECK(p_clicks->clickCount() == 0U);
            }
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
                        ctx, ticket, slot, observe.empty_is_absence,
                        observe.exact_text
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
                // The point of passing the rule in: the reading is the same and the
                // verdict is not, because "what counts as a match" is this layer's
                // policy. cycle_read takes no expected text for exactly that reason.
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
                        ctx, ticket, slot, observe.empty_is_absence,
                        observe.exact_text
                    )
                    local loose = observe.read_element(
                        ctx, ticket, slot, observe.empty_is_absence,
                        function(actual, expected)
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
                        return observe.read_element(
                            ctx, ticket, slot, observe.empty_is_absence, nil
                        )
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
                            ctx, ticket, anchor, observe.empty_is_absence,
                            observe.exact_text
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

        // The model the identify-by-text cases are written against. `title` is the
        // shared box every page in the target prints its own name in: one
        // rectangle, no template, and nothing said here about what it reads,
        // because that is different on every page. `anchor` keeps its template, so
        // a case can put a forbidden text clause on a page that still has a
        // required clause of the older kind.
        constexpr std::string_view k_titleModel = R"lua(
            local title = model.Element.new{
                name = "title",
                capabilities = { "identify", "read" },
                rect = { x = 0, y = 0, width = 3, height = 1 },
            }
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
            local function titleRow(overrides)
                local row = {
                    element = title,
                    holding = "referenced",
                    exercised = { "identify" },
                    identify = "required",
                }
                for key, value in overrides do
                    row[key] = value
                end
                return row
            end
            local function anchorRow()
                return {
                    element = anchor,
                    holding = "owned",
                    exercised = { "identify" },
                    identify = "required",
                }
            end
        )lua";

        [[nodiscard]]
        auto titleScript(std::string_view body) -> std::string
        {
            return script(std::string{k_titleModel} + std::string{body});
        }

        TEST_CASE("A page is identified by the text its own title box reads")
        {
            auto const directory = TemporaryDirectory{"uf-model-identify-text"};
            seedTemplates(directory.path());

            SUBCASE("the page whose name is in the box resolves")
            {
                // The reading arrives padded because a single-line read of a drawn
                // rectangle does. Drop the trim from observe.exact_text and this
                // goes red -- which is why the trim is in the comparison rather
                // than in the project file's data.
                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(2, 5, 0)},
                        .ocrEngine   = std::make_unique<FakeOcrEngine>(
                            oneLineReadout("  battle  ", 9'000)
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

                auto const result = runModel(context, built, titleScript(R"lua(
                    local page = model.Page.new{
                        name = "sortie",
                        references = { titleRow({ expected_text = "battle" }) },
                    }
                    local ticket = ctx:cycle_open()
                    local receipt, why = observe.resolve_page(ctx, ticket, page)
                    ctx:cycle_close(ticket)
                    if receipt == nil then return 0 end
                    if why ~= nil then return 0 end
                    if receipt.page ~= page then return 0 end
                    return 1
                )lua"));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("a longer name that merely starts with it is another page")
            {
                // The case the comparison rule exists for. Every screen prints its
                // name in this box and one name is a prefix of another; make the
                // comparison a `string.find` and this page resolves on the wrong
                // screen with nothing downstream noticing.
                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(2, 5, 0)},
                        .ocrEngine   = std::make_unique<FakeOcrEngine>(
                            oneLineReadout("battle log", 9'000)
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

                auto const result = runModel(context, built, titleScript(R"lua(
                    local page = model.Page.new{
                        name = "sortie",
                        references = { titleRow({ expected_text = "battle" }) },
                    }
                    local ticket = ctx:cycle_open()
                    local receipt, why = observe.resolve_page(ctx, ticket, page)
                    ctx:cycle_close(ticket)
                    if receipt ~= nil then return 0 end
                    if string.find(why, [[requires element 'title']], 1, true) == nil then
                        return 0
                    end
                    if string.find(why, [[to read "battle"]], 1, true) == nil then
                        return 0
                    end
                    if string.find(why, [[the region read "battle log"]], 1, true) == nil then
                        return 0
                    end
                    return 1
                )lua"));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("a reading the engine was unsure of is not evidence either way")
            {
                // Reading fails OPEN: the engine returns plausible text for any
                // rectangle, so confidence is the only thing separating a reading
                // from a guess. Below the floor the clause is NOT satisfied and the
                // text is not "found" either -- which is why the forbidden page
                // below resolves on the same frame. Delete the floor check and the
                // required page resolves on a guess; make a low reading a match and
                // the forbidden page stops resolving. Either way a half goes red.
                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(2, 5, 0)},
                        .ocrEngine   = std::make_unique<FakeOcrEngine>(
                            oneLineReadout("battle", 7'000)
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

                auto const result = runModel(context, built, titleScript(R"lua(
                    local required = model.Page.new{
                        name = "sortie",
                        references = { titleRow({ expected_text = "battle" }) },
                    }
                    local elsewhere = model.Page.new{
                        name = "elsewhere",
                        references = {
                            anchorRow(),
                            titleRow({
                                identify = "forbidden",
                                expected_text = "battle",
                            }),
                        },
                    }
                    local ticket = ctx:cycle_open()
                    local receipt, why = observe.resolve_page(ctx, ticket, required)
                    local other = observe.resolve_page(ctx, ticket, elsewhere)
                    ctx:cycle_close(ticket)
                    if receipt ~= nil then return 0 end
                    if string.find(why, "only 7000 basis points", 1, true) == nil then
                        return 0
                    end
                    if string.find(why, "under the 8000", 1, true) == nil then
                        return 0
                    end
                    if other == nil then return 0 end
                    return 1
                )lua"));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("an element that states its own floor is judged at that one")
            {
                // 9000 clears the framework's 8000 and not this element's 9500, so
                // the same reading that resolves a page above refuses one here.
                // Read the constant instead of the element's field and this goes red.
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

                auto const result = runModel(context, built, titleScript(R"lua(
                    local strict = model.Element.new{
                        name = "strict_title",
                        capabilities = { "identify", "read" },
                        rect = { x = 0, y = 0, width = 3, height = 1 },
                        read_floor = 9500,
                    }
                    if model.Element.read_floor(strict) ~= 9500 then return 0 end
                    if model.Element.read_floor(title) ~= 8000 then return 0 end

                    local page = model.Page.new{
                        name = "sortie",
                        references = {
                            {
                                element = strict,
                                holding = "owned",
                                exercised = { "identify" },
                                identify = "required",
                                expected_text = "battle",
                            },
                        },
                    }
                    local ticket = ctx:cycle_open()
                    local receipt, why = observe.resolve_page(ctx, ticket, page)
                    ctx:cycle_close(ticket)
                    if receipt ~= nil then return 0 end
                    if string.find(why, "under the 9500", 1, true) == nil then
                        return 0
                    end
                    return 1
                )lua"));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("a forbidden clause fires on the text it forbids and not otherwise")
            {
                // Both polarities on one frame and one reading. Implement only
                // the required half and the first page below resolves; implement
                // only the forbidden half and the second one does not.
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

                auto const result = runModel(context, built, titleScript(R"lua(
                    local notBattle = model.Page.new{
                        name = "not_battle",
                        references = {
                            anchorRow(),
                            titleRow({
                                identify = "forbidden",
                                expected_text = "battle",
                            }),
                        },
                    }
                    local notMenu = model.Page.new{
                        name = "not_menu",
                        references = {
                            anchorRow(),
                            titleRow({
                                identify = "forbidden",
                                expected_text = "menu",
                            }),
                        },
                    }
                    local ticket = ctx:cycle_open()
                    local refused, why = observe.resolve_page(ctx, ticket, notBattle)
                    local allowed = observe.resolve_page(ctx, ticket, notMenu)
                    ctx:cycle_close(ticket)
                    if refused ~= nil then return 0 end
                    if string.find(why, [[forbids element 'title']], 1, true) == nil then
                        return 0
                    end
                    if string.find(why, [[from reading "battle"]], 1, true) == nil then
                        return 0
                    end
                    if allowed == nil then return 0 end
                    return 1
                )lua"));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }
        }

        TEST_CASE("The row's expected text outranks the element's")
        {
            // The precedence, in both directions on one frame: the page that states
            // the text resolves, and the page that leaves it to the element is
            // judged by the element's. Read the element's field first and the first
            // page fails; forget the fallback and the second stops explaining itself.
            auto const directory = TemporaryDirectory{"uf-model-text-precedence"};
            seedTemplates(directory.path());
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

            auto const result = runModel(context, built, script(R"lua(
                local heading = model.Element.new{
                    name = "heading",
                    capabilities = { "identify", "read" },
                    rect = { x = 0, y = 0, width = 3, height = 1 },
                    expected_text = "menu",
                }
                local function row(overrides)
                    local entry = {
                        element = heading,
                        holding = "referenced",
                        exercised = { "identify" },
                        identify = "required",
                    }
                    for key, value in overrides do
                        entry[key] = value
                    end
                    return entry
                end

                local stated = model.Page.new{
                    name = "sortie",
                    references = { row({ expected_text = "battle" }) },
                }
                local inherited = model.Page.new{
                    name = "home",
                    references = { row({}) },
                }
                if model.Reference.expected_text(stated.references[1]) ~= "battle" then
                    return 0
                end
                if model.Reference.expected_text(inherited.references[1]) ~= "menu" then
                    return 0
                end

                local ticket = ctx:cycle_open()
                local here = observe.resolve_page(ctx, ticket, stated)
                local there, why = observe.resolve_page(ctx, ticket, inherited)
                ctx:cycle_close(ticket)
                if here == nil then return 0 end
                if there ~= nil then return 0 end
                if string.find(why, [[to read "menu"]], 1, true) == nil then
                    return 0
                end
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("An element with a template identifies by pixels whatever its row reads")
        {
            // The older path, unchanged. This page's row says the region reads
            // "battle" and the engine reads something else entirely, and the page
            // still resolves -- an element that HAS pixels identifies by them, and
            // a row's text is a fact about a reading rather than a second identity
            // clause. Let the text path run whenever a row carries expected text
            // and this goes red immediately.
            auto const directory = TemporaryDirectory{"uf-model-pixels-win"};
            seedTemplates(directory.path());
            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(2, 5, 0)},
                    .ocrEngine   = std::make_unique<FakeOcrEngine>(
                        oneLineReadout("not the title", 9'000)
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

            auto const result = runModel(context, built, script(R"lua(
                local anchor = model.Element.new{
                    name = "anchor",
                    capabilities = { "identify", "read" },
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
                local page = model.Page.new{
                    name = "battle",
                    references = {
                        {
                            element = anchor,
                            holding = "owned",
                            exercised = { "identify", "read" },
                            identify = "required",
                            expected_text = "battle",
                        },
                    },
                }

                local ticket = ctx:cycle_open()
                local receipt = observe.resolve_page(ctx, ticket, page)
                if receipt == nil then return 0 end

                -- And the same row's text is exactly what a READ of it compares
                -- against, which is the other half of why it is allowed here.
                local reading = observe.read_element(
                    ctx, ticket, page.references[1], observe.empty_is_absence,
                    observe.exact_text
                )
                ctx:cycle_close(ticket)
                if reading.expected ~= "battle" then return 0 end
                if reading.matches ~= false then return 0 end
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("read_element reads what the page's own row says about the element")
        {
            // One element, two answers, and both are right: on its own it reads its
            // own rectangle against its own expected text, and through a page's row
            // it reads what THAT page refined -- text and rectangle both. Ignore the
            // row's expected text and the first pair goes red; read the element's
            // rectangle instead of the row's and the width checks do.
            auto const directory = TemporaryDirectory{"uf-model-read-row"};
            seedTemplates(directory.path());
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
                local slot = model.Element.new{
                    name = "slot",
                    capabilities = { "interact", "read" },
                    rect = { x = 0, y = 0, width = 3, height = 1 },
                    expected_text = "menu",
                }
                local page = model.Page.new{
                    name = "battle",
                    references = {
                        {
                            element = anchor,
                            holding = "owned",
                            exercised = { "identify" },
                            identify = "required",
                        },
                        {
                            element = slot,
                            holding = "owned",
                            exercised = { "read" },
                            expected_text = "battle",
                            rect_override = { x = 1, y = 0, width = 2, height = 1 },
                        },
                    },
                }

                local row = model.Page.reference_for(page, slot)
                local ticket = ctx:cycle_open()
                local viaRow = observe.read_element(
                    ctx, ticket, row, observe.empty_is_absence, observe.exact_text
                )
                local viaElement = observe.read_element(
                    ctx, ticket, slot, observe.empty_is_absence, observe.exact_text
                )
                ctx:cycle_close(ticket)

                if viaRow == nil or viaElement == nil then return 0 end
                if viaRow.expected ~= "battle" then return 0 end
                if viaRow.matches ~= true then return 0 end
                if viaRow.x ~= 1 or viaRow.width ~= 2 then return 0 end
                if viaElement.expected ~= "menu" then return 0 end
                if viaElement.matches ~= false then return 0 end
                if viaElement.x ~= 0 or viaElement.width ~= 3 then return 0 end
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        // Three lines wherever the block-reading fake is asked for one, each at its
        // own place in the fixture frame's only row: what a scrolling grid gives a
        // run, text the project file never named at positions the frame alone knows.
        [[nodiscard]]
        auto rosterReadout() -> ocr::Readout
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
                    .confidenceBp = 9'500,
                }
            );
            readout.lines.emplace_back(
                ocr::TextLine{
                    .text   = "shop",
                    .bounds = test::pixelRect(2, 0, 1, 1),
                    .confidenceBp = 8'000,
                }
            );
            return readout;
        }

        TEST_CASE("read_lines locates the text a page never named and clicks it")
        {
            // The whole shape of the feature in one script: resolve the page, read
            // the annotated region, pick the line by what it says, and click THAT
            // line -- through the same observe.click a template hit goes through,
            // on the same ticket, with the receipt that ticket minted.
            auto const directory = TemporaryDirectory{"uf-model-read-lines"};
            seedTemplates(directory.path());
            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(2, 5, 0)},
                    .ocrEngine   = std::make_unique<FakeOcrEngine>(
                        oneLineReadout("battle", 9'000),
                        rosterReadout()
                    ),
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
                local receipt = observe.resolve_page(ctx, ticket, battle)
                if receipt == nil then return 0 end

                local row = model.Page.reference_for(battle, slot)
                local lines = observe.read_lines(
                    ctx, ticket, row, observe.empty_is_unknown
                )
                if #lines ~= 3 then return 0 end

                -- Every line carries its own place, its own text and its own
                -- confidence, and says the frame is what positioned it.
                if lines[1].text ~= "battle" then return 0 end
                if lines[2].x ~= 1 or lines[2].width ~= 1 then return 0 end
                if lines[2].confidence ~= 9500 then return 0 end
                if lines[2].positioned_by ~= "text" then return 0 end
                if lines[2].page ~= "battle" then return 0 end
                if lines[2].interact ~= true then return 0 end
                if lines[2].click_x ~= 1 or lines[2].click_y ~= 0 then return 0 end

                local wanted = hits.matching(lines, "rest", observe.exact_text)
                if wanted == nil then return 0 end
                if wanted ~= lines[2] then return 0 end

                observe.click(ctx, ticket, receipt, wanted)
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
            CHECK(p_clicks->clickCount() == 1U);
        }

        TEST_CASE("A line located on one frame cannot authorise a click on another")
        {
            // The rule the feature turns on. A line's position is read off one
            // frame and is true of that frame alone -- there is no match handle for
            // C++ to re-check, because the click reaches the host as a bare
            // coordinate. Remove the ticket comparison in observe.click and the
            // click below is delivered at where the text used to be.
            auto const directory = TemporaryDirectory{"uf-model-read-lines-stale"};
            seedTemplates(directory.path());
            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(2, 5, 0), pixels(2, 5, 0)},
                    .ocrEngine   = std::make_unique<FakeOcrEngine>(
                        oneLineReadout("battle", 9'000),
                        rosterReadout()
                    ),
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
                local first = ctx:cycle_open()
                local row = model.Page.reference_for(battle, slot)
                local stale = observe.read_lines(
                    ctx, first, row, observe.empty_is_unknown
                )[2]
                ctx:cycle_close(first)

                -- A fresh frame, freshly resolved, so the receipt is beyond
                -- reproach and the line is the only stale thing in the call.
                local second = ctx:cycle_open()
                local receipt = observe.resolve_page(ctx, second, battle)
                if receipt == nil then return 0 end

                local ok, err = pcall(function()
                    observe.click(ctx, second, receipt, stale)
                end)
                if ok then return 0 end
                if string.find(err, "located on another observation", 1, true) == nil then
                    return 0
                end
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
            CHECK(p_clicks->clickCount() == 0U);
        }

        TEST_CASE("A read region that does not exercise interact is looked at only")
        {
            // The capability model is unchanged by the ruling that a located
            // line may be clicked: `read` is what lets the region be read, and
            // `interact` on the page's own row is still what lets anything in it
            // be clicked. Add interact to the row below and the refusal goes.
            auto const directory = TemporaryDirectory{"uf-model-read-lines-nointeract"};
            seedTemplates(directory.path());
            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(2, 5, 0)},
                    .ocrEngine   = std::make_unique<FakeOcrEngine>(
                        oneLineReadout("battle", 9'000),
                        rosterReadout()
                    ),
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
                local list = model.Element.new{
                    name = "list",
                    capabilities = { "read" },
                    rect = { x = 0, y = 0, width = 3, height = 1 },
                }
                local listing = model.Page.new{
                    name = "listing",
                    references = {
                        {
                            element = anchor,
                            holding = "owned",
                            exercised = { "identify" },
                            identify = "required",
                        },
                        {
                            element = list,
                            holding = "owned",
                            exercised = { "read" },
                        },
                    },
                }

                local ticket = ctx:cycle_open()
                local receipt = observe.resolve_page(ctx, ticket, listing)
                if receipt == nil then return 0 end

                local row = model.Page.reference_for(listing, list)
                local lines = observe.read_lines(
                    ctx, ticket, row, observe.empty_is_unknown
                )
                if #lines ~= 3 then return 0 end
                if lines[1].interact ~= false then return 0 end

                local ok, err = pcall(function()
                    observe.click(ctx, ticket, receipt, lines[1])
                end)
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

        TEST_CASE("hits.offset aims beside the line that authorised the click")
        {
            // The card a name sits in. The offset is the caller's number and the
            // derived hit keeps the line's own rectangle, its page and its
            // cycle, so every check in observe.click still applies to it.
            auto const directory = TemporaryDirectory{"uf-model-offset-hit"};
            seedTemplates(directory.path());
            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(2, 5, 0)},
                    .ocrEngine   = std::make_unique<FakeOcrEngine>(
                        oneLineReadout("battle", 9'000),
                        rosterReadout()
                    ),
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
                local receipt = observe.resolve_page(ctx, ticket, battle)
                if receipt == nil then return 0 end

                local row = model.Page.reference_for(battle, slot)
                local lines = observe.read_lines(
                    ctx, ticket, row, observe.empty_is_unknown
                )
                local name = hits.matching(lines, "battle", observe.exact_text)
                if name == nil then return 0 end

                local card = hits.offset(name, 2, 0)
                if card.click_x ~= name.click_x + 2 then return 0 end
                if card.click_y ~= name.click_y then return 0 end
                -- The rectangle is still the evidence's: nothing here located a
                -- card, so nothing here claims a rectangle for one.
                if card.x ~= name.x or card.width ~= name.width then return 0 end
                if card.text ~= "battle" then return 0 end

                -- A table nobody minted is refused, and so is an offset that is
                -- not a whole number of pixels.
                local forged = { click_x = 0, click_y = 0 }
                if pcall(function() return hits.offset(forged, 1, 1) end) then
                    return 0
                end
                if pcall(function() return hits.offset(name, 0.5, 0) end) then
                    return 0
                end

                observe.click(ctx, ticket, receipt, card)
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
            CHECK(p_clicks->clickCount() == 1U);
        }

        TEST_CASE("An offset hit made on an earlier frame is refused like the line was")
        {
            // The derived hit inherits its source's cycle, so offsetting is not
            // a way to launder a stale line into a fresh click. Mint the derived
            // hit against no ticket and the refusal below goes.
            auto const directory = TemporaryDirectory{"uf-model-offset-stale"};
            seedTemplates(directory.path());
            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(2, 5, 0), pixels(2, 5, 0)},
                    .ocrEngine   = std::make_unique<FakeOcrEngine>(
                        oneLineReadout("battle", 9'000),
                        rosterReadout()
                    ),
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
                local first = ctx:cycle_open()
                local row = model.Page.reference_for(battle, slot)
                local stale = hits.offset(
                    observe.read_lines(ctx, first, row, observe.empty_is_unknown)[1],
                    2,
                    0
                )
                ctx:cycle_close(first)

                local second = ctx:cycle_open()
                local receipt = observe.resolve_page(ctx, second, battle)
                if receipt == nil then return 0 end

                local ok, err = pcall(function()
                    observe.click(ctx, second, receipt, stale)
                end)
                if ok then return 0 end
                if string.find(err, "located on another observation", 1, true) == nil then
                    return 0
                end
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
            CHECK(p_clicks->clickCount() == 0U);
        }

        TEST_CASE("A region that read nothing is not a region holding nothing")
        {
            // The failure this argument exists for. The engine finds no line at all
            // in the annotated region -- which is what a reward card's name band
            // gives a run that reads it before the card has finished drawing, and
            // equally what a badge gives on a screen that genuinely carries none.
            // The bytes are identical; only the caller knows which of the two it
            // would act on. Delete the raise in observe.read_lines and the first
            // subcase gets its empty list back, so it goes red; raise regardless of
            // the policy and the second one does.
            auto const directory = TemporaryDirectory{"uf-model-empty-region"};
            seedTemplates(directory.path());

            SUBCASE("a caller that came for content is told it learned nothing")
            {
                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(2, 5, 0)},
                        // An empty block readout: the detector located nothing.
                        .ocrEngine   = std::make_unique<FakeOcrEngine>(
                            oneLineReadout("battle", 9'000),
                            ocr::Readout{}
                        ),
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
                    local receipt = observe.resolve_page(ctx, ticket, battle)
                    if receipt == nil then return 0 end

                    local row = model.Page.reference_for(battle, slot)
                    local ok, err = ctx:try(function()
                        return observe.read_lines(
                            ctx, ticket, row, observe.empty_is_unknown
                        )
                    end)
                    if ok ~= false then return 0 end

                    -- A host-minted control error and not a value: there is no
                    -- list to walk, so the choosing loop that turned a blank
                    -- into "none of these" never runs.
                    if type(err) ~= 'userdata' then return 0 end
                    if err.kind ~= uf.errors.recognition_incomplete then return 0 end
                    if err.retryable ~= true then return 0 end
                    if string.find(err.message, "found no text at all", 1, true) == nil then
                        return 0
                    end
                    if string.find(err.message, "has not been drawn", 1, true) == nil then
                        return 0
                    end
                    return 1
                )lua"));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
                CHECK(p_clicks->clickCount() == 0U);
            }

            SUBCASE("a caller asking whether the region is clear gets its answer")
            {
                // The other direction, and the reason a verb that simply raised
                // on empty would be wrong: some callers are asking precisely
                // whether anything is there.
                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(2, 5, 0)},
                        .ocrEngine   = std::make_unique<FakeOcrEngine>(
                            oneLineReadout("battle", 9'000),
                            ocr::Readout{}
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
                    local row = model.Page.reference_for(battle, slot)
                    local lines = observe.read_lines(
                        ctx, ticket, row, observe.empty_is_absence
                    )
                    ctx:cycle_close(ticket)
                    if type(lines) ~= "table" then return 0 end
                    if #lines ~= 0 then return 0 end
                    return 1
                )lua"));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }
        }

        TEST_CASE("A forbidden clause is still satisfied by a region reading nothing")
        {
            // The identify sweep asks the emptiness question on every frame and must
            // keep getting the emptiness answer: a page whose signature is "this box
            // does NOT read 'battle'" resolves on a screen where the box reads
            // nothing at all. That path reads the region itself rather than through
            // read_element, so this is what says the policy argument did not leak
            // into it. Make an empty read raise inside resolve_page and this goes red.
            auto const directory = TemporaryDirectory{"uf-model-empty-forbidden"};
            seedTemplates(directory.path());
            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(2, 5, 0)},
                    .ocrEngine   = std::make_unique<FakeOcrEngine>(ocr::Readout{}),
                    .projectRoot = directory.path(),
                }
            );
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runModel(context, built, titleScript(R"lua(
                local notBattle = model.Page.new{
                    name = "not_battle",
                    references = {
                        anchorRow(),
                        titleRow({
                            identify = "forbidden",
                            expected_text = "battle",
                        }),
                    },
                }
                local isBattle = model.Page.new{
                    name = "is_battle",
                    references = { titleRow({ expected_text = "battle" }) },
                }

                local ticket = ctx:cycle_open()
                local allowed = observe.resolve_page(ctx, ticket, notBattle)
                local refused, why = observe.resolve_page(ctx, ticket, isBattle)
                ctx:cycle_close(ticket)

                if allowed == nil then return 0 end
                if refused ~= nil then return 0 end
                if string.find(why, "the region read nothing at all", 1, true) == nil then
                    return 0
                end
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("read_element's empty reading is the caller's to interpret too")
        {
            // The same hole one rectangle down. A nil reading is quieter than an
            // empty list and conflates the same two states: the caller writes
            // `reading == nil or not reading.matches` and a region nobody has drawn
            // yet reads as "this is not that text". Delete the raise in
            // observe.read_element and the first subcase goes red; raise regardless
            // of the policy and the second one does.
            auto const directory = TemporaryDirectory{"uf-model-empty-reading"};
            seedTemplates(directory.path());

            SUBCASE("empty_is_unknown refuses to answer at all")
            {
                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(2, 5, 0)},
                        .ocrEngine   = std::make_unique<FakeOcrEngine>(ocr::Readout{}),
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
                    local ok, err = ctx:try(function()
                        return observe.read_element(
                            ctx, ticket, slot, observe.empty_is_unknown,
                            observe.exact_text
                        )
                    end)
                    if ok ~= false then return 0 end
                    if type(err) ~= 'userdata' then return 0 end
                    if err.kind ~= uf.errors.recognition_incomplete then return 0 end
                    if string.find(err.message, "observe.read_element", 1, true) == nil then
                        return 0
                    end
                    return 1
                )lua"));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("empty_is_absence hands the nil reading back")
            {
                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(2, 5, 0)},
                        .ocrEngine   = std::make_unique<FakeOcrEngine>(ocr::Readout{}),
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
                        ctx, ticket, slot, observe.empty_is_absence,
                        observe.exact_text
                    )
                    ctx:cycle_close(ticket)
                    if reading ~= nil then return 0 end
                    return 1
                )lua"));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }
        }

        TEST_CASE("Neither read verb has a default for what an empty region means")
        {
            // The policy has no default and cannot be spelled wrong. Give either
            // argument a fallback -- or accept any string -- and the matching row
            // stops raising, so it goes red.
            auto const directory = TemporaryDirectory{"uf-model-empty-policy"};
            seedTemplates(directory.path());
            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(2, 5, 0)},
                    .ocrEngine   = std::make_unique<FakeOcrEngine>(
                        oneLineReadout("battle", 9'000),
                        rosterReadout()
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
                local row = model.Page.reference_for(battle, slot)

                local function refuses(body, fragment)
                    local ok, err = pcall(body)
                    if ok then return false end
                    if type(err) ~= "string" then return false end
                    return string.find(err, fragment, 1, true) ~= nil
                end

                -- read_lines with no policy at all.
                if not refuses(function()
                    return observe.read_lines(ctx, ticket, row)
                end, "has to be told what an empty region means") then return 0 end

                -- read_element with no policy at all.
                if not refuses(function()
                    return observe.read_element(ctx, ticket, slot)
                end, "has to be told what an empty region means") then return 0 end

                -- A string that is not one of the two published spellings picks
                -- no policy rather than the nearest one.
                if not refuses(function()
                    return observe.read_lines(ctx, ticket, row, "unknwon")
                end, "has to be told what an empty region means") then return 0 end

                -- The comparison in the empty-region slot is named as the swap
                -- it is, because both arguments are policies.
                if not refuses(function()
                    return observe.read_element(ctx, ticket, slot, observe.exact_text)
                end, "takes the empty-region policy before the comparison") then
                    return 0
                end

                ctx:cycle_close(ticket)
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("wait_until needs K agreeing observations in a row")
        {
            // Four frames: hold, break, hold, hold. With K = 2 the streak the first
            // frame opened is broken by the second, so the wait ends on the FOURTH
            // capture and not the third. Drop the streak entirely and the run ends
            // after one capture, so the count below goes red; decrement the streak
            // instead of resetting it and the wait ends on the third, so it goes red
            // the other way.
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
            // already spent, and a host-minted Tier B timeout. Returning false here
            // would tell a script the page is not up when nothing waited long enough
            // to know. Replace the raise with `return false` and this goes red; the
            // run still terminates, which is what keeps proving it red cheap.
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

        // Five pages over five one-by-one greys, the smallest model the 2026-08-01
        // graph ruling can be exercised against: a base screen, an overlay over it,
        // a second overlay over that, a popup that can appear over anything, and a
        // screen the game can jump to on its own. Each page is recognised by one
        // grey, so a three-pixel frame decides exactly which are on screen.
        // `pixels(2, 20, 0)` carries the base marker AND the overlay marker at once,
        // which is not an accident: an overlay COVERS the page underneath rather
        // than replacing it, and that page is still recognisable and still clickable
        // while it is up (docs/plans/2026-07-31-script-owned-page-model.md 1).
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

            SUBCASE("a click edge refuses to walk off a page this frame is not")
            {
                // Fail closed BEFORE the trigger, and on the FIRST question rather
                // than the second. A click is authorised by the page the frame in
                // front of the run resolved, so walking away from a page whose own
                // marker is absent is refused for that reason and never for the
                // missing click target.
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
                    if string.find(err, "this frame is not that page", 1, true) == nil then
                        return 0
                    end
                    if navigation.stack_depth(stack) ~= 1 then return 0 end
                    return 1
                )lua"));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
                CHECK(p_clicks->clickCount() == 0U);
            }

            SUBCASE("a click edge whose element is not on screen refuses to walk")
            {
                // The second question, reached only once the first is answered: the
                // page IS this page and the thing to click is not on it. The model
                // here identifies its from page by one mark and clicks another,
                // which the shared graph above cannot express, since every page
                // there is identified by the element it clicks.
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

                auto const result = runModel(context, built, script(R"lua(
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
                    local sign   = marker("sign", "gray2.png")
                    local button = marker("button", "gray5.png")

                    local here = model.Page.new{
                        name = "here",
                        references = {
                            {
                                element = sign,
                                holding = "owned",
                                exercised = { "identify" },
                                identify = "required",
                            },
                            {
                                element = button,
                                holding = "owned",
                                exercised = { "interact" },
                            },
                        },
                    }
                    local there = model.Page.new{
                        name = "there",
                        references = {
                            {
                                element = button,
                                holding = "referenced",
                                exercised = { "identify" },
                                identify = "required",
                            },
                        },
                    }
                    local press = navigation.Edge.new{
                        from = here,
                        to = { there },
                        via = "click",
                        via_element = button,
                        kind = "navigate",
                    }
                    local graph = navigation.Graph.new{
                        pages = { here, there },
                        edges = { press },
                    }

                    local stack = navigation.stack_new{ graph = graph, max_depth = 4 }
                    navigation.stack_reset(stack, here)
                    local ok, err = pcall(function()
                        return observe.walk_edge(ctx, stack, press, {
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
            // The first touchstone the ruling named: open an overlay, open a second
            // over it, close that one, and land back where the stack said you were.
            // The destination of each pop is read off the stack at walk time and is
            // never written in the model, so replacing the pop branch with the
            // edge's own `to` cannot even compile a destination, and dropping the
            // entry the pop removes lands the run one page too shallow.
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
            // The ruling's own instance, made mechanical: a run standing in a battle
            // with the card detail open, when the battle ends and the game jumps to
            // the result screen on its own. The overlay AND the page it covered are
            // gone together, so belief has to be thrown away rather than popped
            // once. Make the arrival always push and the stack ends three deep; make
            // it always pop and it ends two deep with the wrong base. Both go red.
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
            // A runaway push is a script bug, so the cap raises a plain Luau error
            // rather than a Tier B automation failure -- nothing about the target
            // went wrong. The ORDER is the other half: move the check to the moment
            // the belief is applied and the keystroke is delivered first, so the
            // target moves before the run refuses to reason about where it is. The
            // key count below is what goes red.
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
                // reached by no inbound edge at all. Delete the interrupt sweep and
                // this walk raises a timeout instead, which is the difference the
                // flag buys: a sentence naming what is actually on screen rather
                // than one naming what is not. The walk does not dismiss it --
                // closing a popup is a policy projects answer differently -- but it
                // does record that an overlay went onto the stack.
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
            // `to` is a set because an outcome can be uncertain, and the streak has
            // to be about ONE of them: seeing the base page once and the result page
            // once is two observations of a screen in motion, not two agreeing
            // observations of anything. Count the streak per walk instead of per
            // page and the wait returns on the second frame with whichever page it
            // saw last, so the arrival name goes red.
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

        // The canonical project file, in exactly the form project.encode writes. It
        // carries three things a naive writer would destroy: a key inside
        // [[element]] this schema version does not know, a [element.extra] subtable
        // that belongs to the project rather than to this layer, and a whole
        // [[gadget]] section kind nothing here understands.
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
            "read_floor = 9500\n"
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
            "expected_text = \"sortie\"\n"
            "\n"
            "[[gadget]]\n"
            "name = \"not mine\"\n"
            "count = 3\n";

        // The same model, written the way a hand edit leaves it: elements out of
        // order, one capability list out of order, and the unknown section in the
        // middle instead of at the end. It is what the case below actually starts
        // from, so a save that never ran would leave these bytes on disk; starting
        // from the canonical form instead would let a no-op save pass.
        constexpr std::string_view k_unsortedProject =
            "schema = \"umbraflow-project/l2-v1\"\n"
            "\n"
            "[[element]]\n"
            "name = \"slot\"\n"
            "capabilities = [\"read\", \"interact\"]\n"
            "rect = [0, 0, 3, 1]\n"
            "read_floor = 9500\n"
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
            "expected_text = \"sortie\"\n"
            "exercised = [\"interact\", \"read\"]\n";

        TEST_CASE("A project file round trips byte for byte and keeps what it does not know")
        {
            // The load-bearing persistence case. A build that dropped an unknown key
            // would not fail -- it would silently delete a newer build's data, which
            // is the worst failure a file format has, so the proof has to be on the
            // bytes rather than on the model. Remove the residual list from
            // Element.new (or stop writing it in project.encode) and the rewritten
            // file loses `future_field = 7`; merge [element.extra] into the element's
            // own fields and the isolation checks go red instead.
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

                -- The floor an element states survives the file, and the one it
                -- does not state is answered by the framework rather than by a
                -- nil a caller would compare a confidence against.
                if slot.read_floor ~= 9500 then return 0 end
                if model.Element.read_floor(slot) ~= 9500 then return 0 end
                local anchorElement = built.element_by_name.anchor
                if anchorElement.read_floor ~= nil then return 0 end
                if model.Element.read_floor(anchorElement) ~= 8000 then return 0 end

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

                -- What one page says its copy of a shared region reads is a row
                -- field, and it outranks the element's own.
                if battle.references[2].expected_text ~= "sortie" then return 0 end
                if model.Reference.expected_text(battle.references[2]) ~= "sortie" then
                    return 0
                end
                if model.Reference.expected_text(battle.references[1]) ~= nil then
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

        TEST_CASE("A project file refuses the extra field it would have to drop")
        {
            // Let renderValue write any table as an array again and the map
            // below encodes to `limits = []` with its keys gone instead of
            // raising, so this goes red.
            auto const directory = TemporaryDirectory{"uf-model-extra-encode"};
            seedTemplates(directory.path());
            auto built = refusalHarness(directory.path());
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runModel(context, built, script(R"lua(
                local function modelWith(extra)
                    return {
                        elements = {
                            model.Element.new{
                                name = "slot",
                                capabilities = { "interact" },
                                rect = { x = 0, y = 0, width = 3, height = 1 },
                                extra = extra,
                            },
                        },
                        pages = {},
                    }
                end

                -- A list under extra has a line this format can write, and it
                -- comes back the same list.
                local written = project.encode(modelWith({ tags = { "boss", "elite" } }))
                if string.find(written, '\ntags = ["boss", "elite"]\n', 1, true) == nil then
                    return 0
                end
                local section = project.parse(written).sections[1]
                if section.kind ~= "element" then return 0 end
                if #section.extra.tags ~= 2 then return 0 end
                if section.extra.tags[1] ~= "boss" then return 0 end
                if section.extra.tags[2] ~= "elite" then return 0 end

                -- A map has none, so the save stops and names the key rather
                -- than writing a file the keys are missing from.
                local ok, err = pcall(function()
                    return project.encode(modelWith({ limits = { retries = 2 } }))
                end)
                if ok then return 0 end
                if string.find(tostring(err), 'cannot carry "limits"', 1, true) == nil then
                    return 0
                end
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        // The graph half of the file format, canonical. Three edges, both page
        // flags, one key inside [[edge]] this schema version does not know, and an
        // [edge.extra] subtable that belongs to the project rather than to this
        // layer. The pop edge carries no `to` line at all, because where a pop lands
        // is the run's page stack rather than anything a file can state.
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

        // The same model as a hand edit leaves it: pages out of order, edges written
        // before the pages they name, and the three edges in the reverse of the
        // order a save puts them in. A save that never ran would leave these bytes
        // on disk and the comparison against the canonical form would fail.
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
            // The persistence half of the graph ruling. Edges are DATA, so they have
            // to survive a load and a save the same way an element does, including
            // the parts this schema version does not understand. Stop writing
            // [[edge]] sections and the file loses the whole graph; stop writing the
            // page flags and `detail` comes back an ordinary page, which makes the
            // push edge to it unbuildable on the next load; drop the edge residual
            // and `future_edge_field = 9` is silently deleted. Each turns the byte
            // comparison below red.
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

        TEST_CASE("A project file whose screen names a page it never declared is refused")
        {
            // One of the two doors. `oracle.Screen.new` is handed a screen's own two
            // facts and can see no pages, so the name a screen declares is checked
            // by whoever CAN see them -- here, while the line the author wrote is
            // still known; the other door is `scribe.add_screen`, in
            // tests/task/test-annotation-routines.cpp. What gets through an
            // unguarded door is a declaration `recognition` has no page to resolve,
            // so the screen's own claim cannot be measured at all.
            auto const directory = TemporaryDirectory{"uf-model-screen-dangling"};
            seedTemplates(directory.path());
            constexpr std::string_view dangling =
                "schema = \"umbraflow-project/l2-v1\"\n"
                "\n"
                "[[element]]\n"
                "name = \"mark_base\"\n"
                "capabilities = [\"identify\"]\n"
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
                "exercised = [\"identify\"]\n"
                "identify = \"required\"\n"
                "\n"
                "[[screen]]\n"
                "name = \"capture\"\n"
                "hash = \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"\n"
                "page = \"nowhere\"\n";
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
                if string.find(err, "says it is page 'nowhere'", 1, true) == nil then
                    return 0
                end
                -- The [[screen]] section's own line, which is what makes it
                -- fixable without reading the whole file.
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

        TEST_CASE("A project file whose appearance names no element it declares is refused")
        {
            // The bucket nobody owns: neither loop reads it and it is not raw
            // residual either, so without `requireOwners` the load succeeds with
            // an element that has no appearances and the next save deletes the
            // [[appearance]] block without a word.
            auto const directory =
                TemporaryDirectory{"uf-model-project-orphan-appearance"};
            seedTemplates(directory.path());
            constexpr std::string_view orphan =
                "schema = \"umbraflow-project/l2-v1\"\n"
                "\n"
                "[[element]]\n"
                "name = \"slot\"\n"
                "capabilities = [\"identify\", \"interact\"]\n"
                "rect = [0, 0, 3, 1]\n"
                "\n"
                "[[appearance]]\n"
                "element = \"slot\"\n"
                "name = \"lit\"\n"
                "source = \"gray2.png\"\n"
                "threshold = 10000\n"
                "\n"
                "[[appearance]]\n"
                "element = \"slot_typo\"\n"
                "name = \"dim\"\n"
                "source = \"gray5.png\"\n"
                "threshold = 10000\n"
                "\n"
                "[[page]]\n"
                "name = \"home\"\n"
                "\n"
                "[[reference]]\n"
                "page = \"home\"\n"
                "element = \"slot\"\n"
                "holding = \"owned\"\n"
                "exercised = [\"identify\", \"interact\"]\n"
                "identify = \"required\"\n";
            writeFile(
                directory.path() / "page-model.toml",
                std::as_bytes(std::span{orphan})
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
                local needle =
                    "names element 'slot_typo', which this project file does not declare"
                if string.find(err, needle, 1, true) == nil then return 0 end
                -- The orphan block's own line rather than the file's first, which
                -- is what makes it fixable.
                if string.find(err, "line 14", 1, true) == nil then return 0 end
                return 1
            )lua");
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("A project file whose reference names no page it declares is refused")
        {
            // The same bucket rule on the other side of the file: a misspelled
            // page name silently drops the row that authorises a click on it.
            auto const directory =
                TemporaryDirectory{"uf-model-project-orphan-reference"};
            seedTemplates(directory.path());
            constexpr std::string_view orphan =
                "schema = \"umbraflow-project/l2-v1\"\n"
                "\n"
                "[[element]]\n"
                "name = \"anchor\"\n"
                "capabilities = [\"identify\"]\n"
                "rect = [0, 0, 3, 1]\n"
                "\n"
                "[[appearance]]\n"
                "element = \"anchor\"\n"
                "name = \"lit\"\n"
                "source = \"gray2.png\"\n"
                "threshold = 10000\n"
                "\n"
                "[[page]]\n"
                "name = \"home\"\n"
                "\n"
                "[[reference]]\n"
                "page = \"home\"\n"
                "element = \"anchor\"\n"
                "holding = \"owned\"\n"
                "exercised = [\"identify\"]\n"
                "identify = \"required\"\n"
                "\n"
                "[[reference]]\n"
                "page = \"hom\"\n"
                "element = \"anchor\"\n"
                "holding = \"referenced\"\n"
                "exercised = [\"identify\"]\n"
                "identify = \"required\"\n";
            writeFile(
                directory.path() / "page-model.toml",
                std::as_bytes(std::span{orphan})
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
                local needle =
                    "names page 'hom', which this project file does not declare"
                if string.find(err, needle, 1, true) == nil then return 0 end
                if string.find(err, "line 24", 1, true) == nil then return 0 end
                return 1
            )lua");
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("A sub-table inside an unknown section stays in that section's block")
        {
            // Parse and write, with no host between them: the lines belong to the
            // block they were written under, and a rewrite that put them in the
            // preamble instead would hoist them above every header in the file.
            auto const directory =
                TemporaryDirectory{"uf-model-unknown-section-subtable"};
            auto built = refusalHarness(directory.path());
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runModel(context, built, R"lua(
                local text = 'schema = "umbraflow-project/l2-v1"\n'
                    .. '\n[[gadget]]\nname = "x"\n'
                    .. '\n[gadget.extra]\ntone = "dark"\n'
                local document = project.parse(text)
                if #document.preamble ~= 0 then return 0 end
                if #document.blocks ~= 1 then return 0 end

                local block = document.blocks[1]
                if #block ~= 4 then return 0 end
                if block[1] ~= "[[gadget]]" then return 0 end
                if block[2] ~= 'name = "x"' then return 0 end
                if block[3] ~= "[gadget.extra]" then return 0 end
                if block[4] ~= 'tone = "dark"' then return 0 end

                -- The byte half: the sub-table still follows its own header at
                -- the end of the file rather than sitting under the schema line.
                local written = project.encode({
                    elements = {},
                    pages    = {},
                    residual = {
                        preamble = document.preamble,
                        sections = document.blocks,
                    },
                })
                local sequence =
                    '[[gadget]]\nname = "x"\n[gadget.extra]\ntone = "dark"\n'
                if string.find(written, sequence, 1, true) == nil then return 0 end
                return 1
            )lua");
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        // ------------- the falsification matrix over a cell no template answers

        // Two screen files, written under the content hashes the matrix derives
        // their paths from, handed back in the order the walk will visit them. The
        // sort is the contract: `regress.check` opens one observation per screen in
        // CONTENT-HASH order and each claimed text cell spends one read of it, so
        // the Nth readout the fake engine answers with belongs to the Nth hash here.
        // A case that assumed declaration order instead would pass or fail according
        // to which grey happened to hash lower.
        [[nodiscard]]
        auto seedScreens(std::filesystem::path const& root)
            -> std::vector<std::string>
        {
            auto error = std::error_code{};
            std::filesystem::create_directories(
                root / "assets" / "screens",
                error
            );

            auto hashes = std::vector<std::string>{};
            for (auto const gray : {uint8{20}, uint8{40}})
            {
                auto const screen = encodedTemplate(gray);
                auto const hex    = screen.hash.hex();
                writeFile(
                    root / "assets" / "screens" / (hex + ".png"),
                    std::span<std::byte const>{screen.pngBytes}
                );
                hashes.emplace_back(hex);
            }
            std::ranges::sort(hashes);
            return hashes;
        }

        // One shared title box with no pixels of its own, two screens, and a
        // `matrix` helper that walks whatever claims a case hands it. The model is
        // assembled in memory rather than loaded from a file, because what is under
        // test is the walk and the judging; the file is round-tripped by its own
        // case below.
        constexpr std::string_view k_matrixPreludeHead = R"lua(
            local title = model.Element.new{
                name = "title",
                capabilities = { "identify", "read" },
                rect = { x = 0, y = 0, width = 3, height = 1 },
            }
            local hashOne = ")lua";

        constexpr std::string_view k_matrixPreludeMiddle = R"lua("
            local hashTwo = ")lua";

        constexpr std::string_view k_matrixPreludeTail = R"lua("
            local first  = oracle.Screen.new{ name = "battle", hash = hashOne }
            local second = oracle.Screen.new{ name = "battle_log", hash = hashTwo }
            -- No pages, and the walk is asked for the list rather than left to
            -- assume one. Every element these cases measure is read rather than
            -- matched, and the two rules that consult the pages judge a set of
            -- appearances.
            local function matrix(expectations, elements)
                return regress.check(ctx, {
                    elements = elements or { title },
                    pages    = {},
                    claims   = oracle.Claims.new{
                        screens      = { first, second },
                        expectations = expectations,
                    },
                })
            end

            -- The same walk over screens that say which PAGE they are of, which
            -- is the only thing in the model that can say two pictures are two
            -- views of one page. `page_by_name` is what a loaded model carries
            -- and what `recognition` resolves a declaration through.
            local function pageMatrix(expectations, screens, pages)
                local byName = {}
                for _, page in pages do byName[page.name] = page end
                return regress.check(ctx, {
                    elements     = { title },
                    pages        = pages,
                    page_by_name = byName,
                    claims       = oracle.Claims.new{
                        screens      = screens,
                        expectations = expectations,
                    },
                })
            end

            -- A page whose whole signature is the shared title box reading one
            -- name: the shape the real project has, and the one a template per
            -- page would have to be cut and thresholded for.
            local function titledPage(name, reads)
                return model.Page.new{
                    name = name,
                    references = {
                        {
                            element = title,
                            holding = "referenced",
                            exercised = { "identify" },
                            identify = "required",
                            expected_text = reads,
                        },
                    },
                }
            end

            local function screenOf(name, hash, page)
                return oracle.Screen.new{
                    name = name,
                    hash = hash,
                    page = page,
                }
            end

            local function claimsText(screen, text)
                return oracle.Expectation.new{
                    screen = screen,
                    element = title,
                    text = text,
                    state = "match",
                }
            end
        )lua";

        [[nodiscard]]
        auto matrixScript(
            std::vector<std::string> const& hashes,
            std::string_view body
        ) -> std::string
        {
            REQUIRE(hashes.size() == 2U);
            auto source = std::string{k_matrixPreludeHead};
            source += hashes[0];
            source += k_matrixPreludeMiddle;
            source += hashes[1];
            source += k_matrixPreludeTail;
            source += body;
            return source;
        }

        // One matrix case: the readings the engine answers with, in screen
        // order, and the script that claims something about them.
        [[nodiscard]]
        auto runMatrix(
            std::filesystem::path const& root,
            std::vector<std::string> const& hashes,
            std::vector<ocr::Readout> readouts,
            std::string_view body
        ) -> Result<double>
        {
            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(2, 5, 0), pixels(2, 5, 0)},
                    .ocrEngine   = std::make_unique<FakeOcrEngine>(
                        std::move(readouts)
                    ),
                    .projectRoot = root,
                }
            );
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = root},
            };
            return runModel(context, built, matrixScript(hashes, body));
        }

        TEST_CASE("The matrix judges a cell by what its region reads")
        {
            // The half of the model the matrix used to be silent about. An element
            // with no templates identifies by the text its rectangle reads, and
            // until this existed `Expectation.new` refused to claim one at all -- so
            // every page identified that way was outside the one guard in this
            // repository that has ever caught anything real. Confidence is what
            // makes it measurable: a reading is a match when it says the claimed
            // text AND clears the element's read floor, which is exactly a score
            // against a threshold.
            auto const directory = TemporaryDirectory{"uf-model-matrix-text"};
            seedTemplates(directory.path());
            auto const hashes = seedScreens(directory.path());

            SUBCASE("a region reading its claimed text is a match and the row shows it")
            {
                auto const result = runMatrix(
                    directory.path(),
                    hashes,
                    {
                        oneLineReadout("  battle  ", 9'000),
                        oneLineReadout("battle log", 9'000),
                    },
                    R"lua(
                    local verdict = matrix({
                        oracle.Expectation.new{
                            screen = first,
                            element = title,
                            text = "battle",
                            state = "match",
                        },
                        oracle.Expectation.new{
                            screen = second,
                            element = title,
                            text = "battle",
                            state = "absent",
                        },
                    })
                    if not verdict.accepted then return 0 end
                    if #verdict.findings ~= 0 then return 0 end
                    if #verdict.cells ~= 2 then return 0 end

                    -- The reading arrives padded, because a single-line read of a
                    -- drawn rectangle does. The trim lives in observe.exact_text,
                    -- which this reaches rather than reimplements.
                    local hit = verdict.cells[1]
                    if hit.screen ~= "battle" then return 0 end
                    if hit.subject ~= "element" then return 0 end
                    if hit.outcome ~= "hit" then return 0 end
                    if hit.verdict ~= "expected" then return 0 end
                    if hit.expected_text ~= "battle" then return 0 end
                    if hit.text ~= "  battle  " then return 0 end
                    if hit.confidence ~= 9000 then return 0 end
                    -- A text row carries no score and no rectangle: there was no
                    -- template and nothing landed anywhere.
                    if hit.score ~= nil then return 0 end
                    if hit.appearance ~= nil then return 0 end
                    if hit.x ~= nil then return 0 end

                    -- "battle" is a prefix of "battle log", and the screen that
                    -- reads the longer name is the screen the file says this text
                    -- is absent from. Make the comparison a `find` and this cell
                    -- becomes a misfire.
                    local elsewhere = verdict.cells[2]
                    if elsewhere.screen ~= "battle_log" then return 0 end
                    if elsewhere.outcome ~= "miss" then return 0 end
                    if elsewhere.verdict ~= "expected" then return 0 end
                    if elsewhere.text ~= "battle log" then return 0 end
                    if elsewhere.confidence ~= 9000 then return 0 end

                    -- EVERY TEXT CELL REPORTS ITS MARGIN, the way a template cell
                    -- reports its score, so a reader sees how close it was rather
                    -- than only which way it went.
                    local rendered = regress.render(verdict)
                    if string.find(rendered, '"confidence":9000', 1, true) == nil then
                        return 0
                    end
                    if string.find(rendered, '"expected_text":"battle"', 1, true) == nil then
                        return 0
                    end
                    if string.find(rendered, '"text":"battle log"', 1, true) == nil then
                        return 0
                    end
                    return 1
                )lua"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("a region reading something else where the file says match is a hole")
            {
                auto const result = runMatrix(
                    directory.path(),
                    hashes,
                    {
                        oneLineReadout("battle log", 9'000),
                        oneLineReadout("menu", 9'000),
                    },
                    R"lua(
                    local verdict = matrix({
                        oracle.Expectation.new{
                            screen = first,
                            element = title,
                            text = "battle",
                            state = "match",
                        },
                    })
                    if verdict.accepted then return 0 end
                    if #verdict.findings ~= 1 then return 0 end

                    local finding = verdict.findings[1]
                    if finding.kind ~= "hole" then return 0 end
                    if finding.screen ~= "battle" then return 0 end
                    if finding.element ~= "title" then return 0 end
                    if string.find(finding.detail, [[reads "battle"]], 1, true) == nil then
                        return 0
                    end
                    if string.find(finding.detail, [[it read "battle log"]], 1, true) == nil then
                        return 0
                    end

                    -- The unclaimed screen is measured by nothing at all, so its
                    -- reading never happened and its row says as much.
                    if verdict.cells[2].outcome ~= "no_pixels" then return 0 end
                    if verdict.cells[2].text ~= nil then return 0 end
                    return 1
                )lua"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("a reading under the floor is an absence and never a match")
            {
                // The rule that makes confidence a score and not decoration, both
                // halves of it on ONE reading. The engine spells the claimed word
                // and is not sure enough to be believed, so the screen claiming a
                // match has a hole and the screen claiming an absence is satisfied
                // -- a reading nobody is sure of is evidence of neither. Drop
                // observe.confident from reading.measure and the first becomes a
                // match and the second a misfire; both assertions below go red, in
                // opposite directions.
                auto const result = runMatrix(
                    directory.path(),
                    hashes,
                    {
                        oneLineReadout("battle", 7'000),
                        oneLineReadout("battle", 7'000),
                    },
                    R"lua(
                    local verdict = matrix({
                        oracle.Expectation.new{
                            screen = first,
                            element = title,
                            text = "battle",
                            state = "match",
                        },
                        oracle.Expectation.new{
                            screen = second,
                            element = title,
                            text = "battle",
                            state = "absent",
                        },
                    })
                    if verdict.accepted then return 0 end
                    if #verdict.findings ~= 1 then return 0 end
                    if verdict.findings[1].kind ~= "hole" then return 0 end
                    if verdict.findings[1].screen ~= "battle" then return 0 end

                    -- The margin is in the sentence, because a cell that read the
                    -- right words and still missed looks like a defect in the
                    -- matrix until the number is in front of the reader.
                    local why = verdict.findings[1].detail
                    if string.find(why, "at 7000 basis points", 1, true) == nil then
                        return 0
                    end

                    local guess = verdict.cells[1]
                    if guess.outcome ~= "miss" then return 0 end
                    if guess.text ~= "battle" then return 0 end
                    if guess.confidence ~= 7000 then return 0 end

                    -- The same guess, on the screen that claims the absence.
                    if verdict.cells[2].verdict ~= "expected" then return 0 end
                    if verdict.cells[2].outcome ~= "miss" then return 0 end
                    return 1
                )lua"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("two screens claiming one region reads one text cannot be told apart")
            {
                // The text analogue of "no two appearances of one element may hit
                // one screen". EVERY recorded expectation below holds: the region
                // reads "battle" on both screens and the file says match on both.
                // Only a rule that looks at the claims themselves can see that this
                // element separates nothing, and that a page signature resting on it
                // resolves on whichever screen is in front.
                auto const result = runMatrix(
                    directory.path(),
                    hashes,
                    {
                        oneLineReadout("battle", 9'000),
                        oneLineReadout("battle", 9'000),
                    },
                    R"lua(
                    local verdict = matrix({
                        oracle.Expectation.new{
                            screen = first,
                            element = title,
                            text = "battle",
                            state = "match",
                        },
                        oracle.Expectation.new{
                            screen = second,
                            element = title,
                            text = "battle",
                            state = "match",
                        },
                    })
                    -- Both cells are satisfied, which is exactly what makes the
                    -- rule the only thing that can reject this model.
                    if verdict.cells[1].verdict ~= "expected" then return 0 end
                    if verdict.cells[2].verdict ~= "expected" then return 0 end

                    if verdict.accepted then return 0 end
                    if #verdict.findings ~= 1 then return 0 end
                    local finding = verdict.findings[1]
                    if finding.kind ~= "ambiguous_text" then return 0 end
                    if finding.element ~= "title" then return 0 end
                    if finding.screen ~= "battle" then return 0 end
                    if finding.rival ~= "battle_log" then return 0 end
                    if finding.appearance ~= nil then return 0 end
                    local phrase = "cannot tell those two apart"
                    if string.find(finding.detail, phrase, 1, true) == nil then
                        return 0
                    end
                    return 1
                )lua"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("one region reading two different names on two screens is accepted")
            {
                // The control for the rule above. Same element, same two screens,
                // same two matches -- and the texts differ, which is the point of
                // one title box serving every page. Without this the rule could be
                // rejecting every text element in every project and the case above
                // would still pass.
                auto const result = runMatrix(
                    directory.path(),
                    hashes,
                    {
                        oneLineReadout("battle", 9'000),
                        oneLineReadout("battle log", 9'000),
                    },
                    R"lua(
                    local verdict = matrix({
                        oracle.Expectation.new{
                            screen = first,
                            element = title,
                            text = "battle",
                            state = "match",
                        },
                        oracle.Expectation.new{
                            screen = second,
                            element = title,
                            text = "battle log",
                            state = "match",
                        },
                    })
                    if not verdict.accepted then return 0 end
                    if #verdict.findings ~= 0 then return 0 end
                    if verdict.matches ~= 2 then return 0 end
                    return 1
                )lua"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }
        }

        TEST_CASE("A screen says which page it is and both rules read the word")
        {
            // The false positive this field exists for. A real project captured ONE
            // page at two scroll positions -- a scrolling grid, its anchors
            // deliberately outside the scrolling area -- and both captures
            // legitimately read the same name in the same title box, so
            // `reading.confusions` reported the property being verified as two
            // defects. The rule was right and had no way to know the two screens
            // were one page, because a screen carried a name and a hash and nothing
            // else.
            //
            // The readouts come in the order the walk spends them, and the walk
            // measures a screen's own declaration BEFORE its cells: a screen that
            // declares a page and claims a text costs two readings of the same box,
            // and one that declares nothing costs one.
            auto const directory = TemporaryDirectory{"uf-model-matrix-page"};
            seedTemplates(directory.path());
            auto const hashes = seedScreens(directory.path());

            SUBCASE("two screens of one page are not a confusion")
            {
                auto const result = runMatrix(
                    directory.path(),
                    hashes,
                    {
                        oneLineReadout("roster", 9'000),
                        oneLineReadout("roster", 9'000),
                        oneLineReadout("roster", 9'000),
                        oneLineReadout("roster", 9'000),
                    },
                    R"lua(
                    local roster = titledPage("roster", "roster")
                    local top    = screenOf("battle", hashOne, "roster")
                    local down   = screenOf("battle_log", hashTwo, "roster")
                    local verdict = pageMatrix(
                        { claimsText(top, "roster"), claimsText(down, "roster") },
                        { top, down },
                        { roster }
                    )

                    -- Every cell holds, exactly as it did when this was reported
                    -- as two defects.
                    if verdict.cells[1].verdict ~= "expected" then return 0 end
                    if verdict.cells[2].verdict ~= "expected" then return 0 end
                    if verdict.matches ~= 2 then return 0 end

                    -- And the model is accepted: the repeated text is one page
                    -- read twice, and the declaration that says so is itself
                    -- paid for -- the page resolved on both screens.
                    if not verdict.accepted then return 0 end
                    if #verdict.findings ~= 0 then return 0 end
                    return 1
                )lua"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("two screens of two pages still are")
            {
                // The control, and the model that really is broken: two pages
                // resting on one word. Both resolve, both cells hold, and the
                // only thing that can reject it is the rule that reads the
                // claims -- unchanged, because these two screens are two pages.
                auto const result = runMatrix(
                    directory.path(),
                    hashes,
                    {
                        oneLineReadout("roster", 9'000),
                        oneLineReadout("roster", 9'000),
                        oneLineReadout("roster", 9'000),
                        oneLineReadout("roster", 9'000),
                    },
                    R"lua(
                    local roster = titledPage("roster", "roster")
                    local squad  = titledPage("squad", "roster")
                    local top    = screenOf("battle", hashOne, "roster")
                    local down   = screenOf("battle_log", hashTwo, "squad")
                    local verdict = pageMatrix(
                        { claimsText(top, "roster"), claimsText(down, "roster") },
                        { top, down },
                        { roster, squad }
                    )
                    if verdict.accepted then return 0 end
                    if #verdict.findings ~= 1 then return 0 end

                    local finding = verdict.findings[1]
                    if finding.kind ~= "ambiguous_text" then return 0 end
                    if finding.screen ~= "battle" then return 0 end
                    if finding.rival ~= "battle_log" then return 0 end
                    local phrase = "does not declare those two to be one page"
                    if string.find(finding.detail, phrase, 1, true) == nil then
                        return 0
                    end
                    return 1
                )lua"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("a screen that declares nothing is never assumed to be another")
            {
                // The third case and the ruling on it. One screen says which page it
                // is and the other says nothing, so nothing in the file says they
                // are one page -- and silence is not a fact. The rule fires, and the
                // way to clear it is to write down something true, which the matrix
                // then measures. Three readouts and not four: the undeclared screen
                // has no page to resolve, so it spends only its cell's read.
                auto const result = runMatrix(
                    directory.path(),
                    hashes,
                    {
                        oneLineReadout("roster", 9'000),
                        oneLineReadout("roster", 9'000),
                        oneLineReadout("roster", 9'000),
                    },
                    R"lua(
                    local roster  = titledPage("roster", "roster")
                    local top     = screenOf("battle", hashOne, "roster")
                    local unknown = screenOf("battle_log", hashTwo)
                    local verdict = pageMatrix(
                        {
                            claimsText(top, "roster"),
                            claimsText(unknown, "roster"),
                        },
                        { top, unknown },
                        { roster }
                    )
                    if unknown.page ~= nil then return 0 end
                    if verdict.accepted then return 0 end
                    if #verdict.findings ~= 1 then return 0 end
                    if verdict.findings[1].kind ~= "ambiguous_text" then
                        return 0
                    end
                    return 1
                )lua"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("a declared page that does not resolve is its own finding")
            {
                // A declaration is a claim, and this is the only cell of the matrix
                // that measures a whole page signature rather than one element: the
                // page says its title box reads "roster" and the box reads something
                // else, so the model holds a page that resolves on nothing. Nothing
                // is CLAIMED here, so the one reading spent on the first screen is
                // the resolution's own -- which is what makes the finding below
                // attributable to the declaration alone.
                auto const result = runMatrix(
                    directory.path(),
                    hashes,
                    {
                        oneLineReadout("battle log", 9'000),
                    },
                    R"lua(
                    local roster = titledPage("roster", "roster")
                    local top    = screenOf("battle", hashOne, "roster")
                    local down   = screenOf("battle_log", hashTwo)
                    local verdict = pageMatrix({}, { top, down }, { roster })
                    if verdict.accepted then return 0 end
                    if #verdict.findings ~= 1 then return 0 end

                    local finding = verdict.findings[1]
                    if finding.kind ~= "unresolved_page" then return 0 end
                    if finding.screen ~= "battle" then return 0 end
                    if finding.page ~= "roster" then return 0 end
                    -- Not a cell: this one is about a signature, so it names no
                    -- element and no appearance.
                    if finding.element ~= nil then return 0 end
                    if finding.appearance ~= nil then return 0 end

                    -- The sentence names the screen, the page, and the reason
                    -- observe.resolve_page gave -- which is the clause a run
                    -- would have failed on, not a second sentence about it.
                    local detail = finding.detail
                    if string.find(detail, "screen 'battle'", 1, true) == nil then
                        return 0
                    end
                    if string.find(detail, "is page 'roster'", 1, true) == nil then
                        return 0
                    end
                    local why = [[requires element 'title' to read "roster"]]
                    if string.find(detail, why, 1, true) == nil then return 0 end
                    if string.find(detail, "battle log", 1, true) == nil then
                        return 0
                    end

                    -- The page a finding is about is a field of its own, so a
                    -- reader filtering the report by element never picks it up.
                    local rendered = regress.render(verdict)
                    if string.find(rendered, '"page":"roster"', 1, true) == nil then
                        return 0
                    end
                    return 1
                )lua"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("a page that is not a name is refused where it was written")
            {
                // The shape check `Screen.new` CAN make, beside the one it cannot:
                // an empty string is not a page name any project declares, and it
                // would otherwise sit in the file as a declaration nothing resolves.
                auto const result = runMatrix(
                    directory.path(),
                    hashes,
                    {
                        oneLineReadout("roster", 9'000),
                    },
                    R"lua(
                    local ok, err = pcall(function()
                        return screenOf("battle", hashOne, "")
                    end)
                    if ok then return 0 end
                    local phrase = "page must be the NAME of a page"
                    if string.find(tostring(err), phrase, 1, true) == nil then
                        return 0
                    end

                    -- A screen that says nothing is not malformed: it is a
                    -- picture nobody has annotated yet, which is what an agent
                    -- captures first.
                    if screenOf("battle", hashOne).page ~= nil then return 0 end
                    return 1
                )lua"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("a page the model does not hold is a raise and not a finding")
            {
                // A construction error filed under evidence about the target would
                // be a finding no measurement produced. Both doors into a model
                // refuse this already, so reaching the walk with one means the
                // model was assembled past them.
                auto const result = runMatrix(
                    directory.path(),
                    hashes,
                    {
                        oneLineReadout("roster", 9'000),
                    },
                    R"lua(
                    local roster = titledPage("roster", "roster")
                    local top    = screenOf("battle", hashOne, "ghost")
                    local down   = screenOf("battle_log", hashTwo)
                    local ok, err = pcall(function()
                        return pageMatrix({}, { top, down }, { roster })
                    end)
                    if ok then return 0 end
                    local phrase = "which this model does not declare"
                    if string.find(tostring(err), phrase, 1, true) == nil then
                        return 0
                    end
                    return 1
                )lua"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }
        }

        TEST_CASE("A declared page and a claim over one region cost that region one read")
        {
            // Both halves are spent on ONE observation -- the frames come from a
            // directory served one file per capture, so a re-opened cycle would be
            // the next screen's pixels -- and both ask the same rectangle of the
            // same frame: once for the element's own cell, once for the page the
            // screen says it is, whose identify row is that same element. The host
            // answers such a question once per frame (task::CycleAnswers), so a
            // screen costs its DISTINCT regions and not its rows.
            auto const directory = TemporaryDirectory{"uf-model-page-budget"};
            seedTemplates(directory.path());
            auto const hashes = seedScreens(directory.path());

            // One screen that both declares a page and claims a text about the
            // element that page identifies by, so the two reads are of one region
            // on one observation.
            constexpr std::string_view walk = R"lua(
                local function walk()
                    local roster = titledPage("roster", "roster")
                    local top    = screenOf("battle", hashOne, "roster")
                    local down   = screenOf("battle_log", hashTwo)
                    return pageMatrix(
                        { claimsText(top, "roster") },
                        { top, down },
                        { roster }
                    )
                end
            )lua";

            SUBCASE("a cycle that may read nothing measures nothing")
            {
                // The positive control for the subcase below: with no read
                // allowed at all the walk stops on the budget, so a budget of one
                // passing there means one read really happened rather than none.
                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(2, 5, 0), pixels(2, 5, 0)},
                        .ocrEngine   = std::make_unique<FakeOcrEngine>(
                            oneLineReadout("roster", 9'000)
                        ),
                        .projectRoot = directory.path(),
                    }
                );
                REQUIRE(built.session.has_value());
                TaskContext context{
                    *std::move(built.session),
                    *built.recorder,
                    TaskContextConfig{
                        .projectRoot          = directory.path(),
                        .maximumReadsPerCycle = 0,
                    },
                };

                auto const result = runModel(
                    context,
                    built,
                    matrixScript(
                        hashes,
                        std::string{walk} + R"lua(
                        local ok, err = pcall(walk)
                        if ok then return 0 end
                        local spent = "spent its budget of"
                        if string.find(tostring(err), spent, 1, true) == nil then
                            return 0
                        end
                        return 1
                    )lua"
                    )
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("one read per screen covers the cell and the page alike")
            {
                auto built = buildHarness(
                    HarnessSpec{
                        .framePixels = {pixels(2, 5, 0), pixels(2, 5, 0)},
                        .ocrEngine   = std::make_unique<FakeOcrEngine>(
                            oneLineReadout("roster", 9'000)
                        ),
                        .projectRoot = directory.path(),
                    }
                );
                REQUIRE(built.session.has_value());
                TaskContext context{
                    *std::move(built.session),
                    *built.recorder,
                    TaskContextConfig{
                        .projectRoot          = directory.path(),
                        .maximumReadsPerCycle = 1,
                    },
                };

                // One rectangle, asked for by the cell, by the screen's own page
                // declaration and by the sweep that offers every page to every
                // screen. Remove the memoisation in TaskContext::cycleRead and the
                // second of those raises on the budget, so this goes red.
                auto const result = runModel(
                    context,
                    built,
                    matrixScript(
                        hashes,
                        std::string{walk} + R"lua(
                        local verdict = walk()
                        if not verdict.accepted then return 0 end
                        if #verdict.findings ~= 0 then return 0 end
                        return 1
                    )lua"
                    )
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }
        }

        TEST_CASE("A text cell nobody claimed is measured by nothing at all")
        {
            // A reading needs something to be read AGAINST, and only a claim says
            // what that is on THIS screen -- so an unclaimed text cell is not a cell
            // the matrix declined to measure, it is a cell with no question in it.
            // The falsification pressure the template half gets from unclaimed
            // cells, this half gets from the two-screens rule instead. The session
            // has NO OCR engine, which turns "it read anyway" from an invisible cost
            // into a red case: cycle_read raises on a host with nothing to read
            // through, so a walk that read an unclaimed cell could not finish.
            auto const directory = TemporaryDirectory{"uf-model-matrix-unclaimed"};
            seedTemplates(directory.path());
            auto const hashes = seedScreens(directory.path());

            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(2, 5, 0), pixels(2, 5, 0)},
                    .projectRoot = directory.path(),
                }
            );
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runModel(context, built, matrixScript(hashes, R"lua(
                local verdict = matrix({})
                if not verdict.accepted then return 0 end
                if #verdict.cells ~= 2 then return 0 end
                if verdict.unclaimed ~= 2 then return 0 end
                for _, cell in verdict.cells do
                    if cell.outcome ~= "no_pixels" then return 0 end
                    if cell.expected ~= "unclaimed" then return 0 end
                    if cell.verdict ~= "unclaimed" then return 0 end
                    if cell.expected_text ~= nil then return 0 end
                    if cell.text ~= nil then return 0 end
                    if cell.confidence ~= nil then return 0 end
                end
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("The matrix measures a shape with no rectangle where each claim says")
        {
            // The cell a shape with no authored region enters the matrix by. A
            // minimap cell is matched at coordinates the script works out for each
            // frame, because the map pans -- so the only falsifiable form of the
            // fact is "on THESE stored pixels, this shape matches HERE and stays
            // away from THERE". That is two claims about one element on one screen,
            // which `Claims.new` used to refuse outright. The session has NO OCR
            // engine, so a walk that took a reading here could not finish: this half
            // is pixels and nothing else.
            auto const directory = TemporaryDirectory{"uf-model-matrix-placed"};
            seedTemplates(directory.path());
            auto const hashes = seedScreens(directory.path());

            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(2, 5, 0), pixels(2, 5, 0)},
                    .projectRoot = directory.path(),
                }
            );
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runModel(context, built, matrixScript(hashes, R"lua(
                local drifting = model.Element.new{
                    name = "drifting",
                    capabilities = { "identify" },
                    appearances = {
                        {
                            name = "cell",
                            source = "gray5.png",
                            template = ctx:template_load(
                                ctx:project_read("gray5.png")
                            ),
                            threshold = 10000,
                        },
                    },
                }
                -- The frame carries grey 5 at x = 1 and grey 2 at x = 0, so the
                -- two claims below are a hit and a miss of the SAME template on
                -- the SAME screen, and only the rectangle separates them.
                local verdict = matrix({
                    oracle.Expectation.new{
                        screen = first,
                        element = drifting,
                        rect = { x = 1, y = 0, width = 1, height = 1 },
                        state = "match",
                    },
                    oracle.Expectation.new{
                        screen = first,
                        element = drifting,
                        rect = { x = 0, y = 0, width = 1, height = 1 },
                        state = "absent",
                    },
                }, { drifting })
                if not verdict.accepted then return 0 end
                if #verdict.findings ~= 0 then return 0 end

                -- Two rows for one element on one screen, plus the row for the
                -- screen no claim places it on.
                if #verdict.cells ~= 3 then return 0 end

                local found = verdict.cells[1]
                if found.screen ~= "battle" then return 0 end
                if found.outcome ~= "hit" then return 0 end
                if found.verdict ~= "expected" then return 0 end
                if found.appearance ~= "cell" then return 0 end
                if found.search_x ~= 1 then return 0 end
                if found.search_width ~= 1 then return 0 end

                local away = verdict.cells[2]
                if away.screen ~= "battle" then return 0 end
                if away.outcome ~= "miss" then return 0 end
                if away.verdict ~= "expected" then return 0 end
                if away.search_x ~= 0 then return 0 end

                -- The screen nobody placed it on: no region to search, so
                -- nothing was measured rather than a miss nobody looked for.
                local absent = verdict.cells[3]
                if absent.screen ~= "battle_log" then return 0 end
                if absent.outcome ~= "no_pixels" then return 0 end
                if absent.expected ~= "unclaimed" then return 0 end
                if absent.search_x ~= nil then return 0 end

                -- The searched rectangle is on the rendered row, because two
                -- rows naming one element on one screen are otherwise two lines
                -- a reader cannot tell apart.
                local rendered = regress.render(verdict)
                if string.find(rendered, '"search_x":1', 1, true) == nil then
                    return 0
                end
                if string.find(rendered, '"search_x":0', 1, true) == nil then
                    return 0
                end
                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("One region reads one text on two screens only at one rectangle")
        {
            // The text half of the same argument. `reading.confusions` catches one
            // region claimed to read one text on two screens, because then that
            // region is not what separates them -- and a page signature resting on
            // it resolves on whichever screen is in front of the run. A shape drawn
            // once and placed many times breaks the premise: nine confirm buttons
            // are ONE element and nine regions, each reading "confirm", and
            // reporting that as nine models that cannot tell nine screens apart is a
            // rule an author switches off. So the rule is keyed by the region.
            auto const directory = TemporaryDirectory{"uf-model-placed-confusion"};
            seedTemplates(directory.path());
            auto const hashes = seedScreens(directory.path());

            constexpr std::string_view k_stamp = R"lua(
                local stamp = model.Element.new{
                    name = "stamp",
                    capabilities = { "read" },
                }
                local function reads(screen, x, text, state)
                    return oracle.Expectation.new{
                        screen = screen,
                        element = stamp,
                        rect = { x = x, y = 0, width = 1, height = 1 },
                        text = text,
                        state = state,
                    }
                end
            )lua";

            SUBCASE("two screens claiming one rectangle reads one text cannot be told apart")
            {
                auto const result = runMatrix(
                    directory.path(),
                    hashes,
                    {
                        oneLineReadout("confirm", 9'600),
                        oneLineReadout("confirm", 9'600),
                    },
                    std::string{k_stamp} + R"lua(
                    local verdict = matrix({
                        reads(first, 0, "confirm", "match"),
                        reads(second, 0, "confirm", "match"),
                    }, { stamp })
                    if verdict.accepted then return 0 end
                    if #verdict.findings ~= 1 then return 0 end
                    local finding = verdict.findings[1]
                    if finding.kind ~= "ambiguous_text" then return 0 end
                    if finding.element ~= "stamp" then return 0 end
                    if string.find(finding.detail, "at [0,0,1,1]", 1, true) == nil then
                        return 0
                    end
                    return 1
                )lua"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("two rectangles reading one text are two buttons and not one region")
            {
                auto const result = runMatrix(
                    directory.path(),
                    hashes,
                    {
                        oneLineReadout("confirm", 9'600),
                        oneLineReadout("confirm", 9'600),
                    },
                    std::string{k_stamp} + R"lua(
                    local verdict = matrix({
                        reads(first, 0, "confirm", "match"),
                        reads(second, 1, "confirm", "match"),
                    }, { stamp })
                    -- Both cells hold, exactly as in the subcase above, and the
                    -- difference is entirely in what the claims say about WHERE.
                    if verdict.cells[1].verdict ~= "expected" then return 0 end
                    if verdict.cells[2].verdict ~= "expected" then return 0 end
                    if not verdict.accepted then return 0 end
                    if #verdict.findings ~= 0 then return 0 end
                    return 1
                )lua"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }
        }

        // ------------- the falsification matrix over a set of appearances

        // Two one-by-one templates a frame of grey four puts one unit from an
        // exact match, so both clear a 9000 threshold and neither is four times
        // the other: the smallest model that is ambiguous and thinly separated
        // at once. Every subcase below builds this same element and differs only
        // in what the pages do with it.
        constexpr std::string_view k_appearanceHead = R"lua(
            local function template(name)
                return ctx:template_load(ctx:project_read(name))
            end
            local function twoLooks(capabilities)
                return model.Element.new{
                    name = "arrow",
                    capabilities = capabilities,
                    rect = { x = 0, y = 0, width = 3, height = 1 },
                    appearances = {
                        {
                            name = "lit",
                            source = "gray3.png",
                            template = template("gray3.png"),
                            threshold = 9000,
                        },
                        {
                            name = "dim",
                            source = "gray5.png",
                            template = template("gray5.png"),
                            threshold = 9000,
                        },
                    },
                }
            end

            -- Page.new refuses a page with no required identify reference, so a
            -- page that only clicks the arrow still needs something to be
            -- anchored by. One appearance, so it never judges a set of its own.
            local anchor = model.Element.new{
                name = "anchor",
                capabilities = { "identify" },
                rect = { x = 0, y = 0, width = 3, height = 1 },
                appearances = {
                    {
                        name = "corner",
                        source = "gray2.png",
                        template = template("gray2.png"),
                        threshold = 10000,
                    },
                },
            }
            local function anchoredPage(name, arrowRow)
                return model.Page.new{
                    name = name,
                    references = {
                        {
                            element = anchor,
                            holding = "owned",
                            exercised = { "identify" },
                            identify = "required",
                        },
                        arrowRow,
                    },
                }
            end
            local hashOne = ")lua";

        constexpr std::string_view k_appearanceTail = R"lua("
            local first  = oracle.Screen.new{ name = "one", hash = hashOne }
            local second = oracle.Screen.new{ name = "two", hash = hashTwo }
            local function matrix(elements, pages)
                local byName = {}
                for _, page in pages do byName[page.name] = page end
                return regress.check(ctx, {
                    elements     = elements,
                    pages        = pages,
                    page_by_name = byName,
                    claims       = oracle.Claims.new{
                        screens      = { first, second },
                        expectations = {},
                    },
                })
            end
            local function counted(verdict, kind)
                local total = 0
                for _, finding in verdict.findings do
                    if finding.kind == kind then total += 1 end
                end
                return total
            end
            -- Both appearances answering on both screens is the premise every
            -- subcase rests on, asserted rather than assumed: a case that
            -- measured nothing would be green for the wrong reason.
            local function bothAnswered(verdict)
                local hit = 0
                for _, cell in verdict.cells do
                    if cell.subject == "appearance" and cell.outcome == "hit" then
                        hit += 1
                    end
                end
                return hit == 4
            end
        )lua";

        [[nodiscard]]
        auto appearanceScript(
            std::vector<std::string> const& hashes,
            std::string_view                body
        ) -> std::string
        {
            REQUIRE(hashes.size() == 2U);
            auto source = std::string{k_appearanceHead};
            source += hashes[0];
            source += k_matrixPreludeMiddle;
            source += hashes[1];
            source += k_appearanceTail;
            source += body;
            return source;
        }

        [[nodiscard]]
        auto runAppearanceMatrix(
            std::filesystem::path const&    root,
            std::vector<std::string> const& hashes,
            std::string_view                body
        ) -> Result<double>
        {
            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = {pixels(4, 4, 4), pixels(4, 4, 4)},
                    .projectRoot = root,
                }
            );
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = root},
            };
            return runModel(context, built, appearanceScript(hashes, body));
        }

        // ------------- the co-resolution sweep and the anchor subset lattice

        // Two marks a frame either carries or does not, and pages built from
        // them. The sweep's whole question is which OTHER pages resolve on a
        // screen, so the smallest model that can answer it is two pages over one
        // shared mark.
        //
        // Frame one carries both marks, frame two only the first, and the greys
        // are exact matches against 10000 so nothing here depends on how close a
        // near miss is.
        constexpr std::string_view k_sweepHead = R"lua(
            local function template(name)
                return ctx:template_load(ctx:project_read(name))
            end
            local function mark(name, x, source)
                return model.Element.new{
                    name = name,
                    capabilities = { "identify" },
                    rect = { x = x, y = 0, width = 1, height = 1 },
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
            local markA = mark("mark_a", 0, "gray2.png")
            local markB = mark("mark_b", 1, "gray5.png")
            local function page(name, rows)
                return model.Page.new{ name = name, references = rows }
            end
            local function row(element, polarity)
                return {
                    element = element,
                    holding = "owned",
                    exercised = { "identify" },
                    identify = polarity,
                }
            end
            local hashOne = ")lua";

        constexpr std::string_view k_sweepTail = R"lua("
            local first  = oracle.Screen.new{ name = "both", hash = hashOne }
            local second = oracle.Screen.new{ name = "one_only", hash = hashTwo }
            local function screenNaming(name, hash, page)
                if page == nil then
                    return oracle.Screen.new{ name = name, hash = hash }
                end
                return oracle.Screen.new{ name = name, hash = hash, page = page }
            end
            -- The elements are a parameter because a clause is keyed on WHAT it
            -- checks as well as on which element: the two marks above are
            -- checked by their pixels, and only an element with none carries a
            -- per-page text for that key to differ on.
            local function sweepModel(
                elements,
                pages,
                declaredFirst,
                declaredSecond,
                options
            )
                local byName = {}
                for _, entry in pages do byName[entry.name] = entry end
                return regress.check(ctx, {
                    elements     = elements,
                    pages        = pages,
                    page_by_name = byName,
                    claims       = oracle.Claims.new{
                        screens      = {
                            screenNaming("both", hashOne, declaredFirst),
                            screenNaming("one_only", hashTwo, declaredSecond),
                        },
                        expectations = {},
                    },
                }, options)
            end
            local function sweep(pages, declaredFirst, declaredSecond, options)
                return sweepModel(
                    { markA, markB },
                    pages,
                    declaredFirst,
                    declaredSecond,
                    options
                )
            end
            local function resolvedOn(verdict, screen)
                local names = {}
                for _, resolution in verdict.resolutions do
                    if resolution.screen == screen then
                        table.insert(names, resolution.page)
                    end
                end
                table.sort(names)
                return table.concat(names, ",")
            end
            local function coverageOf(verdict, name)
                for _, entry in verdict.coverage do
                    if entry.page == name then return entry end
                end
                return nil
            end
            -- The same walk with the model's graph attached. Separate from
            -- `sweepModel` rather than a sixth parameter on it: every case above
            -- hands over a model with no graph, and that is the control the
            -- linkage rows are measured against.
            local function sweepLinked(pages, edges, options)
                local byName = {}
                for _, entry in pages do byName[entry.name] = entry end
                return regress.check(ctx, {
                    elements     = { markA, markB },
                    pages        = pages,
                    page_by_name = byName,
                    graph        =
                        navigation.Graph.new{ pages = pages, edges = edges },
                    claims       = oracle.Claims.new{
                        screens      = {
                            screenNaming("both", hashOne),
                            screenNaming("one_only", hashTwo),
                        },
                        expectations = {},
                    },
                }, options)
            end
            local function linkageOf(verdict, name)
                for _, entry in verdict.linkage do
                    if entry.page == name then return entry end
                end
                return nil
            end
        )lua";

        // `walks` is how many times the body calls `sweep`: each one opens an
        // observation per screen, and the source repeats its LAST frame once
        // exhausted -- so a body that walks twice on two frames would measure the
        // second screen's pixels twice and be green for the wrong reason.
        [[nodiscard]]
        auto runSweep(
            std::filesystem::path const&    root,
            std::vector<std::string> const& hashes,
            std::string_view                body,
            std::size_t                     walks = 1U
        ) -> Result<double>
        {
            REQUIRE(hashes.size() == 2U);
            auto source = std::string{k_sweepHead};
            source += hashes[0];
            source += k_matrixPreludeMiddle;
            source += hashes[1];
            source += k_sweepTail;
            source += body;

            auto framePixels = std::vector<std::vector<std::byte>>{};
            for (auto walk = std::size_t{0}; walk < walks; ++walk)
            {
                framePixels.emplace_back(pixels(2, 5, 9));
                framePixels.emplace_back(pixels(2, 9, 9));
            }

            auto built = buildHarness(
                HarnessSpec{
                    .framePixels = std::move(framePixels),
                    .projectRoot = root,
                }
            );
            REQUIRE(built.session.has_value());
            TaskContext context{
                *std::move(built.session),
                *built.recorder,
                TaskContextConfig{.projectRoot = root},
            };
            return runModel(context, built, source);
        }

        TEST_CASE("The matrix reports which pages resolve on a screen besides its own")
        {
            // The measurement the dispatcher's order rests on, and which nothing
            // measured before: a page whose clauses another page also satisfies
            // resolves on that page's screens too, so a loop offering the looser
            // one first can never reach the tighter one. It is REPORTED and never
            // judged -- an overlay screen resolves the page underneath it and the
            // model holds no relation saying which page that is
            // (docs/plans/2026-08-01-three-layers-and-agent-operator.md,
            // "Recognition asks one direction only").
            auto const directory = TemporaryDirectory{"uf-model-sweep"};
            seedTemplates(directory.path());
            auto const hashes = seedScreens(directory.path());

            SUBCASE("a looser page resolves wherever the tighter one does")
            {
                auto const result = runSweep(
                    directory.path(),
                    hashes,
                    R"lua(
                    local verdict = sweep({
                        page("loose", { row(markA, "required") }),
                        page("tight", {
                            row(markA, "required"),
                            row(markB, "required"),
                        }),
                    })

                    -- The premise, asserted rather than assumed: frame one carries
                    -- both marks and frame two only the first. A case measuring
                    -- nothing would be green for the wrong reason.
                    if resolvedOn(verdict, "both") ~= "loose,tight" then return 0 end
                    if resolvedOn(verdict, "one_only") ~= "loose" then return 0 end
                    if #verdict.resolutions ~= 3 then return 0 end

                    -- No screen said which page it is, so nothing here is a
                    -- finding: co-resolution moves the exit code nowhere.
                    if not verdict.accepted then return 0 end
                    if #verdict.findings ~= 0 then return 0 end
                    for _, resolution in verdict.resolutions do
                        if resolution.declared then return 0 end
                    end

                    -- The lattice half, decided from the clauses alone: `loose` is
                    -- contained in `tight` and never the other way about.
                    if #verdict.subsets ~= 1 then return 0 end
                    local subset = verdict.subsets[1]
                    if subset.general ~= "loose" then return 0 end
                    if subset.specific ~= "tight" then return 0 end
                    if subset.extra ~= 1 then return 0 end
                    if subset.extra_forbidden ~= 0 then return 0 end

                    -- And the inventory, which counts the screens a page resolved
                    -- on against the screens that name it.
                    if coverageOf(verdict, "loose").resolved_on ~= 2 then return 0 end
                    if coverageOf(verdict, "tight").resolved_on ~= 1 then return 0 end
                    if coverageOf(verdict, "loose").declared_screens ~= 0 then
                        return 0
                    end

                    -- All three products reach the REPORT, under the line kinds
                    -- a reader filters by. `umbra-flow check` prints exactly
                    -- what `regress.groups` hands it and nothing else, so a
                    -- block missing from there is measured and unreadable --
                    -- which every assertion above this one would still allow.
                    local rendered = regress.render(verdict)
                    local resolutionLine = '{"check":"resolution","screen":'
                        .. '"both","page":"tight","declared":false}'
                    local subsetLine = '{"check":"anchor_subset","general":'
                        .. '"loose","specific":"tight","extra":1,'
                        .. '"extra_forbidden":0}'
                    local coverageLine = '{"check":"page_coverage","page":'
                        .. '"tight","resolved_on":1,"declared_screens":0}'
                    if string.find(rendered, resolutionLine, 1, true) == nil then
                        return 0
                    end
                    if string.find(rendered, subsetLine, 1, true) == nil then
                        return 0
                    end
                    if string.find(rendered, coverageLine, 1, true) == nil then
                        return 0
                    end
                    return 1
                )lua"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("a forbidden clause keeps the tighter page off the looser one's screens")
            {
                auto const result = runSweep(
                    directory.path(),
                    hashes,
                    R"lua(
                    local verdict = sweep({
                        page("loose", { row(markA, "required") }),
                        page("guarded", {
                            row(markA, "required"),
                            row(markB, "forbidden"),
                        }),
                    })

                    -- The polarity is the whole difference: `guarded` stays away
                    -- from the frame carrying mark_b and resolves on the one that
                    -- does not, which is the opposite screen from the subcase
                    -- above.
                    if resolvedOn(verdict, "both") ~= "loose" then return 0 end
                    if resolvedOn(verdict, "one_only") ~= "guarded,loose" then
                        return 0
                    end

                    -- Containment still holds and still says the order: `loose`
                    -- resolves wherever `guarded` does. What the forbidden clause
                    -- buys is the OTHER direction, and the row says so separately
                    -- rather than calling the pair arbitrated.
                    if #verdict.subsets ~= 1 then return 0 end
                    local subset = verdict.subsets[1]
                    if subset.general ~= "loose" then return 0 end
                    if subset.specific ~= "guarded" then return 0 end
                    if subset.extra_forbidden ~= 1 then return 0 end
                    return 1
                )lua"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("the page a screen declares is read out of the sweep")
            {
                auto const result = runSweep(
                    directory.path(),
                    hashes,
                    R"lua(
                    -- The finding a screen's own declaration buys comes out of the
                    -- sweep when there is one, so a sweep that stopped covering
                    -- every declared page would take this finding with it rather
                    -- than fail on its own terms. The subcase below is the other
                    -- route to the same finding.
                    local function pages()
                        return {
                            page("loose", { row(markA, "required") }),
                            page("tight", {
                                row(markA, "required"),
                                row(markB, "required"),
                            }),
                        }
                    end

                    -- Declared on the screen that carries both marks: it resolves,
                    -- there is no finding, and exactly the declared row is marked.
                    local held = sweep(pages(), "tight", nil)
                    if #held.findings ~= 0 then return 0 end
                    local declared = 0
                    for _, resolution in held.resolutions do
                        if resolution.declared then
                            if resolution.page ~= "tight" then return 0 end
                            if resolution.screen ~= "both" then return 0 end
                            declared += 1
                        end
                    end
                    if declared ~= 1 then return 0 end

                    -- What `declared_screens` is FOR: `loose` and `tight` both
                    -- resolve somewhere, and only one of them was ever asked
                    -- for by name. The field separates a signature that does
                    -- not hold from a page nobody captured, so a count that
                    -- ignored the declarations would answer the same on every
                    -- page of every project.
                    if coverageOf(held, "tight").declared_screens ~= 1 then
                        return 0
                    end
                    if coverageOf(held, "loose").declared_screens ~= 0 then
                        return 0
                    end

                    -- The same declaration on the screen that lacks mark_b: the
                    -- page cannot resolve there, the finding names the clause that
                    -- failed, and no row claims it resolved.
                    local broken = sweep(pages(), nil, "tight")
                    if broken.accepted then return 0 end
                    if #broken.findings ~= 1 then return 0 end
                    local finding = broken.findings[1]
                    if finding.kind ~= "unresolved_page" then return 0 end
                    if finding.screen ~= "one_only" then return 0 end
                    if finding.page ~= "tight" then return 0 end
                    if string.find(finding.detail, "mark_b", 1, true) == nil then
                        return 0
                    end
                    for _, resolution in broken.resolutions do
                        if resolution.declared then return 0 end
                    end
                    return 1
                )lua",
                    2U
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("a run that does not sweep keeps every finding and reports no rows")
            {
                auto const result = runSweep(
                    directory.path(),
                    hashes,
                    R"lua(
                    -- What `sweep_pages = false` gives up and what it must not.
                    -- The co-resolution rows go, because nothing offered a page to
                    -- a screen that does not name it; the FINDINGS stay, because
                    -- the page a screen declares is still resolved -- through
                    -- `recognition.verify` rather than out of a sweep.
                    local function pages()
                        return {
                            page("loose", { row(markA, "required") }),
                            page("tight", {
                                row(markA, "required"),
                                row(markB, "required"),
                            }),
                        }
                    end
                    local quiet = { sweep_pages = false }

                    local held = sweep(pages(), "tight", nil, quiet)
                    if held.swept then return 0 end
                    if #held.findings ~= 0 then return 0 end

                    -- Empty and DECLARED empty. A reader that could not tell this
                    -- from "every page was offered and none resolved" would read a
                    -- green report over a question nobody asked.
                    if #held.resolutions ~= 0 then return 0 end
                    if #held.coverage ~= 0 then return 0 end

                    -- The file-only half costs no capture, so it is measured
                    -- either way.
                    if #held.subsets ~= 1 then return 0 end

                    -- The same declaration on the screen that lacks mark_b: same
                    -- finding, same clause named, with no sweep behind it.
                    local broken = sweep(pages(), nil, "tight", quiet)
                    if broken.accepted then return 0 end
                    if #broken.findings ~= 1 then return 0 end
                    local finding = broken.findings[1]
                    if finding.kind ~= "unresolved_page" then return 0 end
                    if finding.screen ~= "one_only" then return 0 end
                    if finding.page ~= "tight" then return 0 end
                    if string.find(finding.detail, "mark_b", 1, true) == nil then
                        return 0
                    end

                    -- And the control: the same model swept DOES report rows, so
                    -- the empties above are the option's doing and not an inert
                    -- fixture.
                    local full = sweep(pages(), "tight", nil)
                    if not full.swept then return 0 end
                    if #full.resolutions ~= 3 then return 0 end
                    if #full.coverage ~= 2 then return 0 end
                    return 1
                )lua",
                    3U
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("two pages with one clause set are one row under the first name")
            {
                auto const result = runSweep(
                    directory.path(),
                    hashes,
                    R"lua(
                    -- The degenerate end of the lattice: two pages no frame can
                    -- tell apart, which is a subset of each other both ways.
                    -- Every other pair differs by a clause, so nothing else
                    -- reaches the rule that keeps such a pair from being
                    -- reported twice. Declaration order is the reverse of the
                    -- answer, so a row that merely echoed the file would name
                    -- `zulu` as the general one.
                    local verdict = sweep({
                        page("zulu", { row(markA, "required") }),
                        page("alpha", { row(markA, "required") }),
                    })

                    -- The premise: one clause set means one answer on every
                    -- frame, which is what leaves the pair unarbitrable.
                    if resolvedOn(verdict, "both") ~= "alpha,zulu" then
                        return 0
                    end
                    if resolvedOn(verdict, "one_only") ~= "alpha,zulu" then
                        return 0
                    end

                    if #verdict.subsets ~= 1 then return 0 end
                    local pair = verdict.subsets[1]
                    if pair.general ~= "alpha" then return 0 end
                    if pair.specific ~= "zulu" then return 0 end
                    if pair.extra ~= 0 then return 0 end
                    if pair.extra_forbidden ~= 0 then return 0 end
                    return 1
                )lua"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("two pages reading one box for different words are no pair")
            {
                auto const result = runSweep(
                    directory.path(),
                    hashes,
                    R"lua(
                    -- Five pages of the reference project share one page_title
                    -- element and are told apart by nothing else. A clause keyed
                    -- on the element and the polarity alone would call those one
                    -- signature and report every pair of them as a containment
                    -- nothing can arbitrate.
                    local title = model.Element.new{
                        name = "title",
                        capabilities = { "identify", "read" },
                        rect = { x = 0, y = 0, width = 1, height = 1 },
                    }
                    local function reads(name, text)
                        return model.Page.new{
                            name = name,
                            references = {
                                {
                                    element = title,
                                    holding = "referenced",
                                    exercised = { "identify" },
                                    identify = "required",
                                    expected_text = text,
                                },
                            },
                        }
                    end
                    -- No page is offered to any screen: the lattice is decided
                    -- from the file, and resolving one of these would read a
                    -- region through an engine this harness binds none of.
                    local quiet = { sweep_pages = false }

                    local apart = sweepModel(
                        { title },
                        { reads("battle", "battle"), reads("menu", "menu") },
                        nil,
                        nil,
                        quiet
                    )
                    if #apart.subsets ~= 0 then return 0 end

                    -- The control: the same two pages expecting the same words
                    -- ARE one signature twice over and the report says so, so
                    -- the empty list above is the text's doing rather than a
                    -- fixture that can produce no row at all.
                    local alike = sweepModel(
                        { title },
                        { reads("battle", "battle"), reads("menu", "battle") },
                        nil,
                        nil,
                        quiet
                    )
                    if #alike.subsets ~= 1 then return 0 end
                    local pair = alike.subsets[1]
                    if pair.general ~= "battle" then return 0 end
                    if pair.specific ~= "menu" then return 0 end
                    if pair.extra ~= 0 then return 0 end
                    return 1
                )lua",
                    2U
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }
        }

        TEST_CASE("The matrix reports how many edges reach and leave each page")
        {
            // The graph half of the coverage question, and capture-free like the
            // subset lattice: a resolution row says a page can be told apart from
            // the others on some frame, and a linkage row says the file offers any
            // way to arrive at it. REPORTED, never judged -- a model draws its
            // edges long after it declares its pages, so pages with no inbound
            // edge are the normal state of an honest file.
            auto const directory = TemporaryDirectory{"uf-model-linkage"};
            seedTemplates(directory.path());
            auto const hashes = seedScreens(directory.path());

            SUBCASE("a page no edge names is counted, and a pop names none")
            {
                auto const result = runSweep(
                    directory.path(),
                    hashes,
                    R"lua(
                    local start  = page("start", { row(markA, "required") })
                    local middle = page("middle", { row(markB, "required") })
                    local cover  = model.Page.new{
                        name = "cover",
                        overlay = true,
                        references = {
                            row(markA, "required"),
                            row(markB, "required"),
                        },
                    }
                    -- Every page needs a required clause, so an unlinked one is
                    -- spelled as a signature no edge mentions rather than as a
                    -- page with nothing in it.
                    local loose  = page("loose", {
                        row(markB, "required"),
                        row(markA, "forbidden"),
                    })

                    -- Keys rather than clicks: a click edge needs the from page to
                    -- exercise interact on the element, and these marks are
                    -- identify-only. What is measured here is the edge's ends, and
                    -- a keystroke has the same two.
                    local verdict = sweepLinked(
                        { start, middle, cover, loose },
                        {
                            navigation.Edge.new{
                                from = start, to = { middle },
                                via = "key", via_key = "E", kind = "navigate",
                            },
                            navigation.Edge.new{
                                from = start, to = { cover },
                                via = "key", via_key = "C", kind = "push",
                            },
                            navigation.Edge.new{
                                from = cover,
                                via = "key", via_key = "B", kind = "pop",
                            },
                            -- Ends the turn in place. Walking it means already
                            -- standing on `middle`, so it is a way to leave and
                            -- not a way to arrive; counting it inbound would let
                            -- one such edge hide a page nothing else reaches.
                            navigation.Edge.new{
                                from = middle, to = { middle },
                                via = "key", via_key = "F", kind = "navigate",
                            },
                        }
                    )

                    if not verdict.linked then return 0 end
                    if #verdict.linkage ~= 4 then return 0 end

                    if linkageOf(verdict, "start").inbound ~= 0 then return 0 end
                    if linkageOf(verdict, "start").outbound ~= 2 then return 0 end
                    -- One inbound, from `start` alone: the self edge above adds
                    -- to the outbound side only.
                    if linkageOf(verdict, "middle").inbound ~= 1 then return 0 end
                    if linkageOf(verdict, "middle").outbound ~= 1 then return 0 end

                    -- The pop is the point of this subcase: it leaves `cover` and
                    -- arrives nowhere the file can name, so it counts once as
                    -- outbound and nowhere as inbound. Were a pop credited to the
                    -- page underneath, every overlay in a real model would invent
                    -- an inbound edge for whichever page happened to be declared
                    -- first.
                    if linkageOf(verdict, "cover").inbound ~= 1 then return 0 end
                    if linkageOf(verdict, "cover").outbound ~= 1 then return 0 end

                    if linkageOf(verdict, "loose").inbound ~= 0 then return 0 end
                    if linkageOf(verdict, "loose").outbound ~= 0 then return 0 end

                    -- Never judged: two pages here have no way in and the verdict
                    -- is still accepted.
                    if not verdict.accepted then return 0 end
                    if #verdict.findings ~= 0 then return 0 end

                    local rendered = regress.render(verdict)
                    local linkageLine = '{"check":"page_linkage","page":"cover",'
                        .. '"inbound":1,"outbound":1,"interrupt":false}'
                    if string.find(rendered, linkageLine, 1, true) == nil then
                        return 0
                    end
                    -- `start` and `loose`, and not `cover`, which a pop leaves
                    -- but nothing declares an arrival at.
                    local summary = '"pages_unlinked":2'
                    if string.find(rendered, summary, 1, true) == nil then
                        return 0
                    end
                    return 1
                )lua"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("an interrupt page is not counted among the unlinked")
            {
                auto const result = runSweep(
                    directory.path(),
                    hashes,
                    R"lua(
                    local start = page("start", { row(markA, "required") })
                    local alert = model.Page.new{
                        name = "alert",
                        overlay = true,
                        interrupt = true,
                        references = { row(markB, "required") },
                    }

                    local verdict = sweepLinked({ start, alert }, {})

                    -- Both pages have no inbound edge and only one is counted:
                    -- an interrupt page declares itself reachable from anywhere
                    -- once, instead of as an inbound edge from every page it can
                    -- cover, so an edge could never retire its zero.
                    if linkageOf(verdict, "start").inbound ~= 0 then return 0 end
                    if linkageOf(verdict, "alert").inbound ~= 0 then return 0 end
                    if linkageOf(verdict, "alert").interrupt ~= true then
                        return 0
                    end
                    if linkageOf(verdict, "start").interrupt ~= false then
                        return 0
                    end

                    local rendered = regress.render(verdict)
                    if string.find(rendered, '"pages_unlinked":1', 1, true) == nil
                    then
                        return 0
                    end
                    return 1
                )lua"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("the edges are counted on a check that sweeps no page")
            {
                auto const result = runSweep(
                    directory.path(),
                    hashes,
                    R"lua(
                    -- The product path this measurement must survive:
                    -- `umbra-flow check` without --sweep-pages offers no page to
                    -- any screen, and the edges are a fact about the file that
                    -- costs no capture either way.
                    local start = page("start", { row(markA, "required") })
                    local other = page("other", { row(markB, "required") })
                    local verdict = sweepLinked(
                        { start, other },
                        {
                            navigation.Edge.new{
                                from = start, to = { other },
                                via = "key", via_key = "E", kind = "navigate",
                            },
                        },
                        { sweep_pages = false }
                    )

                    -- The premise, asserted so this cannot pass by sweeping after
                    -- all: nothing was offered to any screen.
                    if verdict.swept then return 0 end
                    if #verdict.resolutions ~= 0 then return 0 end

                    if not verdict.linked then return 0 end
                    if #verdict.linkage ~= 2 then return 0 end
                    if linkageOf(verdict, "other").inbound ~= 1 then return 0 end

                    local rendered = regress.render(verdict)
                    if string.find(rendered, '"pages_unlinked":1', 1, true) == nil
                    then
                        return 0
                    end
                    -- And the co-resolution count is absent on the same line, so
                    -- the two halves are measured independently rather than one
                    -- of them carrying the other.
                    if string.find(rendered, "pages_unresolved", 1, true) ~= nil
                    then
                        return 0
                    end
                    return 1
                )lua"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("a model handed over without its graph reports no count")
            {
                auto const result = runSweep(
                    directory.path(),
                    hashes,
                    R"lua(
                    -- The distinction the whole row kind rests on. Every page here
                    -- has no inbound edge, and the honest report is silence rather
                    -- than the largest number this line can carry: nothing was
                    -- looked at.
                    local verdict = sweep({
                        page("start", { row(markA, "required") }),
                        page("other", { row(markB, "required") }),
                    })

                    if verdict.linked then return 0 end
                    if #verdict.linkage ~= 0 then return 0 end

                    local rendered = regress.render(verdict)
                    if string.find(rendered, "pages_unlinked", 1, true) ~= nil then
                        return 0
                    end
                    -- The positive control: the same summary DOES carry the count
                    -- once a graph is there, so the absence above is the missing
                    -- graph and not a key this report never writes.
                    local linked = regress.render(
                        sweepLinked({ page("only", { row(markA, "required") }) }, {})
                    )
                    if string.find(linked, '"pages_unlinked":1', 1, true) == nil then
                        return 0
                    end
                    return 1
                )lua",
                    2U
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }
        }

        TEST_CASE("A replayed run is judged against the edges the model draws")
        {
            // The trace library's half of falsifiability: the screen library
            // makes "recognise" and "permit" checkable and this makes "change"
            // checkable (docs/plans/2026-08-04-state-layer-and-policy-slots.md
            // 4.2). It costs no capture -- the run was recorded earlier and the
            // graph is on the file -- so every case here is one Luau call.
            auto const directory = TemporaryDirectory{"uf-model-replay"};
            seedTemplates(directory.path());
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

            auto const result = runModel(context, built, graphScript(R"lua(
                local model_only = { graph = graph }
                local function at(kind, seq, label)
                    return { kind = kind, seq = seq, label = label }
                end
                local function page(seq, name)
                    return at(replay.page_resolved, seq, name)
                end
                local function click(seq, element)
                    return at(replay.element_clicked, seq, element)
                end
                local function landed(seq)
                    return at(replay.action_delivered, seq, "")
                end
                local function key(seq, name)
                    return at(replay.key_delivered, seq, name)
                end
                local function only(verdict)
                    if #verdict.transitions ~= 1 then return nil end
                    return verdict.transitions[1]
                end

                -- A click edge, walked. The authorisation names the element and
                -- the delivery says it landed; the model draws base --mark_base->
                -- detail, so the move is explained.
                local walked = replay.check(model_only, {
                    page(1, "base"),
                    click(2, "mark_base"),
                    landed(3),
                    page(4, "detail"),
                })
                if not walked.accepted then return 0 end
                if only(walked) == nil then return 0 end
                if only(walked).verdict ~= "matched" then return 0 end
                if only(walked).element ~= "mark_base" then return 0 end
                if walked.resolutions ~= 2 then return 0 end

                -- A key edge, and the key is what tells it from the other edge
                -- leaving the same page.
                local pressed = replay.check(model_only, {
                    page(1, "detail"),
                    key(2, "Z"),
                    page(3, "zoom"),
                })
                if only(pressed).verdict ~= "matched" then return 0 end
                if only(pressed).key ~= "Z" then return 0 end

                -- A pop is matched WITHOUT checking where it landed: the file
                -- declares no destination for one, so agreeing about the
                -- destination would be agreeing about something nobody wrote.
                local popped = replay.check(model_only, {
                    page(1, "zoom"),
                    click(2, "mark_zoom"),
                    landed(3),
                    page(4, "detail"),
                })
                if popped.accepted ~= true then return 0 end
                if only(popped).verdict ~= "matched_pop" then return 0 end

                -- Nothing was delivered and the page changed anyway, which is
                -- exactly what a spontaneous edge declares.
                local drifted = replay.check(model_only, {
                    page(1, "detail"),
                    page(2, "result"),
                })
                if only(drifted).verdict ~= "matched" then return 0 end
                if only(drifted).trigger ~= "spontaneous" then return 0 end

                -- The self edge: base ends its turn on base. No page changed, so
                -- there is no move to explain and no row to report -- and the
                -- run is still accepted.
                local stayed = replay.check(model_only, {
                    page(1, "base"),
                    key(2, "E"),
                    page(3, "base"),
                })
                if #stayed.transitions ~= 0 then return 0 end
                if #stayed.findings ~= 0 then return 0 end
                if stayed.resolutions ~= 2 then return 0 end

                -- The finding this whole check exists for: the run went
                -- somewhere the model draws no way to.
                local unexplained = replay.check(model_only, {
                    page(1, "base"),
                    key(2, "Q"),
                    page(3, "result"),
                })
                if unexplained.accepted then return 0 end
                if #unexplained.findings ~= 1 then return 0 end
                local missing = unexplained.findings[1]
                if missing.kind ~= "no_edge" then return 0 end
                if missing.from ~= "base" or missing.to ~= "result" then
                    return 0
                end
                if string.find(missing.detail, "no way out", 1, true) == nil then
                    return 0
                end

                -- The other mistake, told apart from it: the model DOES draw
                -- this trigger leaving this page, and it lands elsewhere. Same
                -- kind, different sentence, because they are repaired
                -- differently.
                local elsewhere = replay.check(model_only, {
                    page(1, "base"),
                    click(2, "mark_base"),
                    landed(3),
                    page(4, "result"),
                })
                if elsewhere.accepted then return 0 end
                local wrong = elsewhere.findings[1]
                if string.find(wrong.detail, "somewhere else", 1, true) == nil then
                    return 0
                end

                -- A click nothing named -- a long press, or a run recorded
                -- before the line that names one. It is a row and NEVER a
                -- finding: a model is not contradicted by a move nobody can
                -- spell, and counting it as a missing edge would fill the report
                -- with edges that may well exist.
                local nameless = replay.check(model_only, {
                    page(1, "base"),
                    landed(2),
                    page(3, "result"),
                })
                if not nameless.accepted then return 0 end
                if #nameless.findings ~= 0 then return 0 end
                if only(nameless).verdict ~= "unattributable" then return 0 end
                if only(nameless).element ~= nil then return 0 end

                return 1
            )lua"));
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("An appearance set is judged where a page identifies by it")
        {
            // `ambiguous_appearances` and `thin_separation` both ask whether the
            // appearances of one element can be told apart, and the model's
            // consumer for that answer is a page reference exercising identify.
            // An element no page identifies by carries appearances so that
            // `observe.find` can locate it; the fold is asked for a place and
            // never for a name, so several of them answering costs the run
            // nothing and the two sentences describe a distinction no caller
            // makes. The measurement is unscoped -- every separation is still
            // recorded -- because a margin is evidence whoever reads it.
            auto const directory = TemporaryDirectory{"uf-model-appearance-set"};
            seedTemplates(directory.path());
            auto const hashes = seedScreens(directory.path());

            SUBCASE("a page identifying by the element gets both rules")
            {
                auto const result = runAppearanceMatrix(
                    directory.path(),
                    hashes,
                    R"lua(
                    local arrow = twoLooks({ "identify", "interact" })
                    local verdict = matrix({ arrow, anchor }, {
                        anchoredPage("node_map", {
                            element = arrow,
                            holding = "owned",
                            exercised = { "identify", "interact" },
                            identify = "required",
                        }),
                    })
                    if not bothAnswered(verdict) then return 0 end
                    if verdict.accepted then return 0 end
                    if #verdict.findings ~= 4 then return 0 end
                    if counted(verdict, "ambiguous_appearances") ~= 2 then
                        return 0
                    end
                    if counted(verdict, "thin_separation") ~= 2 then return 0 end

                    local finding = verdict.findings[1]
                    if finding.kind ~= "ambiguous_appearances" then return 0 end
                    if finding.element ~= "arrow" then return 0 end
                    if finding.appearance ~= "lit" then return 0 end
                    if finding.rival ~= "dim" then return 0 end
                    return 1
                )lua"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("an element only ever clicked is measured and not judged")
            {
                auto const result = runAppearanceMatrix(
                    directory.path(),
                    hashes,
                    R"lua(
                    local arrow = twoLooks({ "interact" })
                    local verdict = matrix({ arrow, anchor }, {
                        anchoredPage("node_map", {
                            element = arrow,
                            holding = "owned",
                            exercised = { "interact" },
                        }),
                    })

                    -- The same ambiguity as the subcase above, on the same
                    -- pixels: both appearances answered on both screens and
                    -- neither cleared the other by the factor.
                    if not bothAnswered(verdict) then return 0 end
                    if #verdict.separations ~= 2 then return 0 end
                    for _, separation in verdict.separations do
                        if separation.clears then return 0 end
                    end

                    if not verdict.accepted then return 0 end
                    if #verdict.findings ~= 0 then return 0 end
                    return 1
                )lua"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("identifying on one page holds on the pages that only click")
            {
                auto const result = runAppearanceMatrix(
                    directory.path(),
                    hashes,
                    R"lua(
                    -- The question is asked of the MODEL and not of a page. One
                    -- page anchors itself on the arrow and another only clicks
                    -- it; the consumer exists, so both rules hold on every
                    -- screen, and the walk never knows which page a screen is.
                    local arrow = twoLooks({ "identify", "interact" })
                    local verdict = matrix({ arrow, anchor }, {
                        anchoredPage("node_map", {
                            element = arrow,
                            holding = "owned",
                            exercised = { "interact" },
                        }),
                        anchoredPage("battle", {
                            element = arrow,
                            holding = "referenced",
                            exercised = { "identify" },
                            identify = "required",
                        }),
                    })
                    if not bothAnswered(verdict) then return 0 end
                    if verdict.accepted then return 0 end
                    if counted(verdict, "ambiguous_appearances") ~= 2 then
                        return 0
                    end
                    if counted(verdict, "thin_separation") ~= 2 then return 0 end
                    return 1
                )lua"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            SUBCASE("a model handed over without its pages is refused")
            {
                auto const result = runAppearanceMatrix(
                    directory.path(),
                    hashes,
                    R"lua(
                    -- Defaulting to an empty list here would answer "no page
                    -- identifies by anything" and report a quiet matrix, which
                    -- reads as a healthy model rather than as a missing field.
                    local arrow = twoLooks({ "interact" })
                    local ok, err = pcall(function()
                        return regress.check(ctx, {
                            elements = { arrow },
                            claims   = oracle.Claims.new{
                                screens      = { first, second },
                                expectations = {},
                            },
                        })
                    end)
                    if ok then return 0 end
                    if type(err) ~= "string" then return 0 end
                    if string.find(
                        err,
                        "needs the pages of the model it walks",
                        1,
                        true
                    ) == nil then
                        return 0
                    end
                    return 1
                )lua"
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }
        }

        // A screen and three elements that differ in exactly one thing each --
        // one verified by pixels, one by what it reads, one that may not be read
        // at all -- so every refusal row below differs from a legal claim in a
        // single field.
        constexpr std::string_view k_claimPrelude = R"lua(
            local screen = oracle.Screen.new{
                name = "battle",
                hash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                    .. "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            }
            local marked = model.Element.new{
                name = "marked",
                capabilities = { "identify" },
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
            local title = model.Element.new{
                name = "title",
                capabilities = { "identify", "read" },
                rect = { x = 0, y = 0, width = 3, height = 1 },
            }
            local mute = model.Element.new{
                name = "mute",
                capabilities = { "interact" },
                rect = { x = 0, y = 0, width = 3, height = 1 },
            }
            -- A minimap shape: real pixels, and no rectangle of its own, because
            -- the map pans and where a cell sits is a fact about the frame.
            local drifting = model.Element.new{
                name = "drifting",
                capabilities = { "identify" },
                appearances = {
                    {
                        name = "cell",
                        source = "gray2.png",
                        template = template("gray2.png"),
                        threshold = 10000,
                    },
                },
            }
        )lua";

        [[nodiscard]]
        auto claimRefusals() -> std::vector<Refusal>
        {
            return {
                Refusal{
                    .label = "a cell claiming both a template and a text",
                    .body  = R"lua(
                        return oracle.Expectation.new{
                            screen = screen,
                            element = marked,
                            appearance = "lit",
                            text = "battle",
                            state = "match",
                        }
                    )lua",
                    .fragment = "names both an appearance and a text",
                },
                Refusal{
                    .label = "a text claimed of an element that has pixels",
                    .body  = R"lua(
                        return oracle.Expectation.new{
                            screen = screen,
                            element = marked,
                            text = "battle",
                            state = "match",
                        }
                    )lua",
                    .fragment = "verifies itself by its template pixels",
                },
                Refusal{
                    .label = "an element with no pixels and no text claimed",
                    .body  = R"lua(
                        return oracle.Expectation.new{
                            screen = screen,
                            element = title,
                            state = "match",
                        }
                    )lua",
                    .fragment = "no measurement this claim could ever be read against",
                },
                Refusal{
                    .label = "an appearance named on an element that declares none",
                    .body  = R"lua(
                        return oracle.Expectation.new{
                            screen = screen,
                            element = title,
                            appearance = "lit",
                            state = "match",
                        }
                    )lua",
                    .fragment = "no measurement this claim could ever be read against",
                },
                Refusal{
                    .label = "a text claimed of an element nothing may read",
                    .body  = R"lua(
                        return oracle.Expectation.new{
                            screen = screen,
                            element = mute,
                            text = "battle",
                            state = "match",
                        }
                    )lua",
                    .fragment = "does not declare the read capability",
                },
                Refusal{
                    .label = "an empty text",
                    .body  = R"lua(
                        return oracle.Expectation.new{
                            screen = screen,
                            element = title,
                            text = "",
                            state = "match",
                        }
                    )lua",
                    .fragment = "text must be the text this element's region reads",
                },
                Refusal{
                    .label = "a shape with no rectangle claimed nowhere",
                    .body  = R"lua(
                        return oracle.Expectation.new{
                            screen = screen,
                            element = drifting,
                            state = "match",
                        }
                    )lua",
                    .fragment = "draws no rectangle of its own and is given none",
                },
                Refusal{
                    .label = "a rectangle claimed for an element that draws its own",
                    .body  = R"lua(
                        return oracle.Expectation.new{
                            screen = screen,
                            element = marked,
                            rect = { x = 0, y = 0, width = 1, height = 1 },
                            state = "match",
                        }
                    )lua",
                    .fragment = "which draws its own; a cell is measured at ONE region",
                },
                Refusal{
                    .label = "a claimed rectangle that is not a rectangle",
                    .body  = R"lua(
                        return oracle.Expectation.new{
                            screen = screen,
                            element = drifting,
                            rect = { x = 0, y = 0, width = 0, height = 1 },
                            state = "match",
                        }
                    )lua",
                    .fragment = "at least one pixel wide",
                },
            };
        }

        TEST_CASE("Expectation.new refuses every malformed claim by name")
        {
            auto const directory = TemporaryDirectory{"uf-model-claim-refusals"};
            seedTemplates(directory.path());
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

            for (auto const& refusal : claimRefusals())
            {
                CAPTURE(refusal.label);
                auto const result = runModel(
                    context,
                    built,
                    script(
                        std::string{k_claimPrelude}
                        + refusalBody(refusal.body, refusal.fragment)
                    )
                );
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }

            // The control every refusal list needs: the shape they each differ
            // from in one field is itself legal, so the rows above are failing on
            // the field they name rather than on the shape.
            auto const legal = runModel(
                context,
                built,
                script(std::string{k_claimPrelude} + R"lua(
                local claim = oracle.Expectation.new{
                    screen = screen,
                    element = title,
                    text = "battle",
                    state = "match",
                }
                if claim.text ~= "battle" then return 0 end
                if claim.appearance ~= nil then return 0 end

                local claims = oracle.Claims.new{
                    screens = { screen },
                    expectations = { claim },
                }
                if oracle.Claims.text_for(claims, "battle", "title") ~= "battle" then
                    return 0
                end
                if oracle.Claims.text_for(claims, "battle", "marked") ~= nil then
                    return 0
                end
                if not oracle.Claims.reads_text(claims) then return 0 end

                -- A project claiming nothing about any reading needs no engine,
                -- and says so, which is what the composition root asks before it
                -- refuses a check for a missing flag.
                local pixelsOnly = oracle.Claims.new{
                    screens = { screen },
                    expectations = {
                        oracle.Expectation.new{
                            screen = screen,
                            element = marked,
                            state = "match",
                        },
                    },
                }
                if oracle.Claims.reads_text(pixelsOnly) then return 0 end
                return 1
            )lua")
            );
            REQUIRE(legal.has_value());
            CHECK(*legal == doctest::Approx(1.0));
        }

        // The claims half of the file format, canonical: two screens, a cell
        // measured by a template, and two cells measured by what one shared
        // region reads.
        constexpr std::string_view k_canonicalClaimsProject =
            "schema = \"umbraflow-project/l2-v1\"\n"
            "\n"
            "[[element]]\n"
            "name = \"mark\"\n"
            "capabilities = [\"identify\"]\n"
            "rect = [0, 0, 3, 1]\n"
            "\n"
            "[[appearance]]\n"
            "element = \"mark\"\n"
            "name = \"lit\"\n"
            "source = \"gray2.png\"\n"
            "threshold = 10000\n"
            "\n"
            "[[element]]\n"
            "name = \"title\"\n"
            "capabilities = [\"identify\", \"read\"]\n"
            "rect = [0, 0, 3, 1]\n"
            "\n"
            "[[page]]\n"
            "name = \"battle\"\n"
            "\n"
            "[[reference]]\n"
            "page = \"battle\"\n"
            "element = \"mark\"\n"
            "holding = \"owned\"\n"
            "exercised = [\"identify\"]\n"
            "identify = \"required\"\n"
            "\n"
            "[[reference]]\n"
            "page = \"battle\"\n"
            "element = \"title\"\n"
            "holding = \"owned\"\n"
            "exercised = [\"identify\"]\n"
            "identify = \"required\"\n"
            "expected_text = \"battle\"\n"
            "\n"
            "[[screen]]\n"
            "name = \"log_screen\"\n"
            "hash = \"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"\n"
            "\n"
            "[[screen]]\n"
            "name = \"sortie_screen\"\n"
            "hash = \"cccccccccccccccccccccccccccccccc"
            "cccccccccccccccccccccccccccccccc\"\n"
            "page = \"battle\"\n"
            "\n"
            "[[expect]]\n"
            "screen = \"log_screen\"\n"
            "element = \"title\"\n"
            "text = \"battle log\"\n"
            "state = \"match\"\n"
            "\n"
            "[[expect]]\n"
            "screen = \"sortie_screen\"\n"
            "element = \"mark\"\n"
            "state = \"match\"\n"
            "\n"
            "[[expect]]\n"
            "screen = \"sortie_screen\"\n"
            "element = \"title\"\n"
            "text = \"battle\"\n"
            "state = \"match\"\n"
            "future_expect_field = 4\n"
            "\n"
            "[expect.extra]\n"
            "my_note = \"transcribed from the v4 grid\"\n";

        // The same claims as a hand edit leaves them: the screens after the
        // expectations that name them, the expectations in the reverse of the
        // order a save puts them in, and the text written after the state it
        // qualifies.
        constexpr std::string_view k_unsortedClaimsProject =
            "schema = \"umbraflow-project/l2-v1\"\n"
            "\n"
            "[[element]]\n"
            "name = \"mark\"\n"
            "capabilities = [\"identify\"]\n"
            "rect = [0, 0, 3, 1]\n"
            "\n"
            "[[appearance]]\n"
            "element = \"mark\"\n"
            "name = \"lit\"\n"
            "source = \"gray2.png\"\n"
            "threshold = 10000\n"
            "\n"
            "[[element]]\n"
            "name = \"title\"\n"
            "capabilities = [\"identify\", \"read\"]\n"
            "rect = [0, 0, 3, 1]\n"
            "\n"
            "[[page]]\n"
            "name = \"battle\"\n"
            "\n"
            "[[reference]]\n"
            "page = \"battle\"\n"
            "element = \"mark\"\n"
            "holding = \"owned\"\n"
            "exercised = [\"identify\"]\n"
            "identify = \"required\"\n"
            "\n"
            "[[reference]]\n"
            "page = \"battle\"\n"
            "element = \"title\"\n"
            "holding = \"owned\"\n"
            "exercised = [\"identify\"]\n"
            "expected_text = \"battle\"\n"
            "identify = \"required\"\n"
            "\n"
            "[[expect]]\n"
            "screen = \"sortie_screen\"\n"
            "element = \"title\"\n"
            "state = \"match\"\n"
            "text = \"battle\"\n"
            "future_expect_field = 4\n"
            "\n"
            "[expect.extra]\n"
            "my_note = \"transcribed from the v4 grid\"\n"
            "\n"
            "[[expect]]\n"
            "screen = \"sortie_screen\"\n"
            "element = \"mark\"\n"
            "state = \"match\"\n"
            "\n"
            "[[expect]]\n"
            "screen = \"log_screen\"\n"
            "element = \"title\"\n"
            "text = \"battle log\"\n"
            "state = \"match\"\n"
            "\n"
            "[[screen]]\n"
            "name = \"sortie_screen\"\n"
            "page = \"battle\"\n"
            "hash = \"cccccccccccccccccccccccccccccccc"
            "cccccccccccccccccccccccccccccccc\"\n"
            "\n"
            "[[screen]]\n"
            "name = \"log_screen\"\n"
            "hash = \"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"\n";

        TEST_CASE("A claim about what a region reads round trips byte for byte")
        {
            // The text a cell claims is DATA, and data that does not survive a save
            // is data a second session invents again. Stop writing the `text` line
            // and the file comes back as a claim about an element with no pixels and
            // no text, which `Expectation.new` refuses -- so the reload fails
            // outright and the byte comparison never happens.
            auto const directory = TemporaryDirectory{"uf-model-claims-roundtrip"};
            seedTemplates(directory.path());
            auto const modelPath = directory.path() / "page-model.toml";
            REQUIRE(k_unsortedClaimsProject != k_canonicalClaimsProject);
            writeFile(modelPath, std::as_bytes(std::span{k_unsortedClaimsProject}));

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
                local built  = project.load_project(ctx)
                local claims = built.claims
                if #claims.screens ~= 2 then return 0 end
                if #claims.expectations ~= 3 then return 0 end

                -- The text came back on the claim, and the claim about a template
                -- has none: one cell, one verification source.
                if oracle.Claims.text_for(claims, "sortie_screen", "title") ~= "battle" then
                    return 0
                end
                if oracle.Claims.text_for(claims, "log_screen", "title") ~= "battle log" then
                    return 0
                end
                if oracle.Claims.text_for(claims, "sortie_screen", "mark") ~= nil then
                    return 0
                end
                if not oracle.Claims.reads_text(claims) then return 0 end

                -- The screen came back knowing which page it is, and the screen
                -- that said nothing came back saying nothing. Stop writing the
                -- `page` line and the rule that reads it goes quiet on the next
                -- session over this same file -- silently, because every cell
                -- still holds.
                if claims.screen_by_name["sortie_screen"].page ~= "battle" then
                    return 0
                end
                if claims.screen_by_name["log_screen"].page ~= nil then return 0 end

                -- And the declaration is what the composition root asks about
                -- before it refuses a check for a missing engine: page `battle`
                -- is identified by what the title box reads. A screen NAMES that
                -- page, so the answer holds whether or not the walk sweeps.
                if not recognition.needs_engine(built, true) then return 0 end
                if not recognition.needs_engine(built, false) then return 0 end

                -- A project's own field lives under extra and an unknown key of
                -- this layer's own section is carried verbatim, on a claim as on
                -- everything else.
                local carried = nil
                for _, expectation in claims.expectations do
                    if expectation.text == "battle" then carried = expectation end
                end
                if carried == nil then return 0 end
                if carried.extra.my_note ~= "transcribed from the v4 grid" then
                    return 0
                end
                if #carried.residual ~= 1 then return 0 end
                if carried.residual[1] ~= "future_expect_field = 4" then return 0 end

                project.save_project(ctx, built)

                -- The fixpoint half: the first save normalises a hand edit, and
                -- the second must change nothing at all.
                project.save_project(ctx, project.load_project(ctx))
                return 1
            )lua");
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));

            CHECK(readFileText(modelPath) == std::string{k_canonicalClaimsProject});
        }

        // An element with no rectangle of its own, the row that places it, and
        // two claims about it on one screen: the whole of the new shape, in
        // canonical form.
        constexpr std::string_view k_canonicalPlacedProject =
            "schema = \"umbraflow-project/l2-v1\"\n"
            "\n"
            "[[element]]\n"
            "name = \"anchor\"\n"
            "capabilities = [\"identify\"]\n"
            "rect = [0, 0, 3, 1]\n"
            "\n"
            "[[appearance]]\n"
            "element = \"anchor\"\n"
            "name = \"lit\"\n"
            "source = \"gray2.png\"\n"
            "threshold = 10000\n"
            "\n"
            "[[element]]\n"
            "name = \"drifting\"\n"
            "capabilities = [\"interact\"]\n"
            "\n"
            "[[appearance]]\n"
            "element = \"drifting\"\n"
            "name = \"cell\"\n"
            "source = \"gray5.png\"\n"
            "threshold = 10000\n"
            "\n"
            "[[page]]\n"
            "name = \"battle\"\n"
            "\n"
            "[[reference]]\n"
            "page = \"battle\"\n"
            "element = \"anchor\"\n"
            "holding = \"owned\"\n"
            "exercised = [\"identify\"]\n"
            "identify = \"required\"\n"
            "\n"
            "[[reference]]\n"
            "page = \"battle\"\n"
            "element = \"drifting\"\n"
            "holding = \"owned\"\n"
            "exercised = [\"interact\"]\n"
            "rect_override = [1, 0, 1, 1]\n"
            "\n"
            "[[screen]]\n"
            "name = \"map\"\n"
            "hash = \"dddddddddddddddddddddddddddddddd"
            "dddddddddddddddddddddddddddddddd\"\n"
            "\n"
            "[[expect]]\n"
            "screen = \"map\"\n"
            "element = \"drifting\"\n"
            "rect = [0, 0, 1, 1]\n"
            "state = \"absent\"\n"
            "\n"
            "[[expect]]\n"
            "screen = \"map\"\n"
            "element = \"drifting\"\n"
            "rect = [1, 0, 1, 1]\n"
            "state = \"match\"\n";

        // The same model as a hand edit leaves it: the elements in the wrong
        // order, the fields inside a block shuffled, the two claims in the
        // reverse of the order a save puts them in, and the screen after them.
        constexpr std::string_view k_unsortedPlacedProject =
            "schema = \"umbraflow-project/l2-v1\"\n"
            "\n"
            "[[element]]\n"
            "name = \"drifting\"\n"
            "capabilities = [\"interact\"]\n"
            "\n"
            "[[element]]\n"
            "rect = [0, 0, 3, 1]\n"
            "name = \"anchor\"\n"
            "capabilities = [\"identify\"]\n"
            "\n"
            "[[appearance]]\n"
            "element = \"drifting\"\n"
            "threshold = 10000\n"
            "name = \"cell\"\n"
            "source = \"gray5.png\"\n"
            "\n"
            "[[appearance]]\n"
            "element = \"anchor\"\n"
            "threshold = 10000\n"
            "name = \"lit\"\n"
            "source = \"gray2.png\"\n"
            "\n"
            "[[page]]\n"
            "name = \"battle\"\n"
            "\n"
            "[[reference]]\n"
            "page = \"battle\"\n"
            "element = \"anchor\"\n"
            "holding = \"owned\"\n"
            "exercised = [\"identify\"]\n"
            "identify = \"required\"\n"
            "\n"
            "[[reference]]\n"
            "page = \"battle\"\n"
            "element = \"drifting\"\n"
            "rect_override = [1, 0, 1, 1]\n"
            "holding = \"owned\"\n"
            "exercised = [\"interact\"]\n"
            "\n"
            "[[expect]]\n"
            "screen = \"map\"\n"
            "element = \"drifting\"\n"
            "state = \"match\"\n"
            "rect = [1, 0, 1, 1]\n"
            "\n"
            "[[expect]]\n"
            "screen = \"map\"\n"
            "element = \"drifting\"\n"
            "state = \"absent\"\n"
            "rect = [0, 0, 1, 1]\n"
            "\n"
            "[[screen]]\n"
            "name = \"map\"\n"
            "hash = \"dddddddddddddddddddddddddddddddd"
            "dddddddddddddddddddddddddddddddd\"\n";

        TEST_CASE("A rectangle nobody drew round trips on the row and on the claim")
        {
            // Both new fields at once, because they are one fact split across two
            // sections: the element states no rectangle, the row says where it sits
            // on this page, and the claims say where it was measured on one screen.
            // Stop writing either line and the file comes back as a row that places
            // nothing and a claim about a shape with nowhere to look, which
            // `Page.new` and `Expectation.new` both refuse, so the reload fails
            // outright and the byte comparison never happens.
            auto const directory = TemporaryDirectory{"uf-model-placed-roundtrip"};
            seedTemplates(directory.path());
            auto const modelPath = directory.path() / "page-model.toml";
            REQUIRE(k_unsortedPlacedProject != k_canonicalPlacedProject);
            writeFile(modelPath, std::as_bytes(std::span{k_unsortedPlacedProject}));

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
                local built    = project.load_project(ctx)
                local drifting = built.element_by_name["drifting"]
                if drifting.rect ~= nil then return 0 end
                if built.element_by_name["anchor"].rect.width ~= 3 then return 0 end

                -- The row that places it, and the two rectangles it is measured
                -- at on the one screen this project holds.
                local row = model.Page.reference_for(
                    built.page_by_name["battle"],
                    drifting
                )
                if row.rect_override.x ~= 1 then return 0 end
                if row.rect_override.width ~= 1 then return 0 end

                local rects = oracle.Claims.rects_for(built.claims, "map", "drifting")
                if #rects ~= 2 then return 0 end

                local at = oracle.Claims.state_for
                if at(built.claims, "map", "drifting", nil,
                    { x = 1, y = 0, width = 1, height = 1 }) ~= "match" then
                    return 0
                end
                if at(built.claims, "map", "drifting", nil,
                    { x = 0, y = 0, width = 1, height = 1 }) ~= "absent" then
                    return 0
                end

                project.save_project(ctx, built)

                -- The fixpoint half: the first save normalises a hand edit, and
                -- the second must change nothing at all.
                project.save_project(ctx, project.load_project(ctx))
                return 1
            )lua");
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));

            CHECK(readFileText(modelPath) == std::string{k_canonicalPlacedProject});
        }

        // ------------- what a script is handed is a snapshot of the model

        // The predicate every row below is judged by: the write RAISES, and raises
        // for the value being readonly rather than for anything else. A new key and
        // an existing one are refused on one rule, so both are stated. `key` is read
        // before it is written, so a row naming a field its value never carried
        // cannot pass on a raise it did not mean.
        //
        // The graph model supplies the elements, pages, edges and graph; the screen,
        // the claims and the one cycle supply the rest.
        constexpr std::string_view k_snapshotPrelude = R"lua(
            local function writeRaises(value, key, written)
                local ok, err = pcall(function() value[key] = written end)
                if ok then return false end
                return type(err) == "string"
                    and string.find(
                        err,
                        "attempt to modify a readonly table",
                        1,
                        true
                    ) ~= nil
            end

            local function refusesEveryWrite(value, key)
                if type(value) ~= "table" then return false end
                if value[key] == nil then return false end
                if not writeRaises(value, "written_by_a_script", true) then
                    return false
                end
                return writeRaises(value, key, value[key])
            end

            -- An appearance that carries a colour key, a shape verified by what
            -- it reads, and a shape with no rectangle of its own: the three the
            -- graph model has no use for and the claims below are measured at.
            local keyed = model.Element.new{
                name = "keyed",
                capabilities = { "identify" },
                rect = { x = 0, y = 0, width = 3, height = 1 },
                appearances = {
                    {
                        name = "lit",
                        source = "gray2.png",
                        template = template("gray2.png"),
                        threshold = 10000,
                        key = {
                            red = 1,
                            green = 2,
                            blue = 3,
                            tolerance = 10,
                            removes = false,
                        },
                    },
                },
            }
            local caption = model.Element.new{
                name = "caption",
                capabilities = { "identify", "read" },
                rect = { x = 0, y = 0, width = 3, height = 1 },
            }
            local floating = model.Element.new{
                name = "floating",
                capabilities = { "identify", "read" },
            }

            local map = oracle.Screen.new{
                name = "map",
                hash = string.rep("d", 64),
            }
            local claims = oracle.Claims.new{
                screens = { map },
                expectations = {
                    oracle.Expectation.new{
                        screen = map,
                        element = mark_base,
                        appearance = "lit",
                        state = "match",
                    },
                    oracle.Expectation.new{
                        screen = map,
                        element = caption,
                        text = "base",
                        state = "match",
                    },
                    oracle.Expectation.new{
                        screen = map,
                        element = floating,
                        text = "confirm",
                        rect = { x = 0, y = 0, width = 1, height = 1 },
                        state = "match",
                    },
                },
            }

            local ticket  = ctx:cycle_open()
            local receipt = observe.resolve_page(ctx, ticket, base)
            local hit     = observe.find(ctx, ticket, base, mark_base)
            ctx:cycle_close(ticket)
            if receipt == nil or hit == nil then return 0 end
        )lua";

        // One value the framework hands a project script, and a key it carries. The
        // key is an EXPRESSION rather than a name, because the lists inside a page,
        // a graph and a claims table are keyed by index.
        struct Snapshot final
        {
            std::string_view label;
            std::string_view value;
            std::string_view key;
        };

        [[nodiscard]]
        auto snapshots() -> std::vector<Snapshot>
        {
            return {
                // Drop the freeze around `element` in model.luau's Element.new.
                Snapshot{
                    .label = "an element",
                    .value = "mark_base",
                    .key   = "'name'",
                },
                // Drop the freeze around `set` in model.luau's capabilitySet.
                Snapshot{
                    .label = "an element's capability set",
                    .value = "mark_base.capabilities",
                    .key   = "'identify'",
                },
                // Drop the freeze around `appearances` in model.luau's Element.new.
                Snapshot{
                    .label = "an element's appearance list",
                    .value = "mark_base.appearances",
                    .key   = "1",
                },
                // Drop the freeze around the appearance row in Element.new.
                Snapshot{
                    .label = "one appearance",
                    .value = "mark_base.appearances[1]",
                    .key   = "'name'",
                },
                // Drop the freeze in model.luau's frozenRect.
                Snapshot{
                    .label = "an element's rectangle",
                    .value = "mark_base.rect",
                    .key   = "'x'",
                },
                // Drop the freeze in model.luau's frozenColourKey.
                Snapshot{
                    .label = "an appearance's colour key",
                    .value = "keyed.appearances[1].key",
                    .key   = "'red'",
                },
                // Drop the freeze around `page` in model.luau's Page.new.
                Snapshot{
                    .label = "a page",
                    .value = "base",
                    .key   = "'name'",
                },
                // Drop the freeze around `references` in model.luau's Page.new.
                Snapshot{
                    .label = "a page's reference list",
                    .value = "base.references",
                    .key   = "1",
                },
                // Drop the freeze around `reference` in model.luau's Page.new.
                Snapshot{
                    .label = "a page's reference row",
                    .value = "base.references[1]",
                    .key   = "'element'",
                },
                // Drop the freeze in evidence.luau's mint_hit.
                Snapshot{
                    .label = "a hit",
                    .value = "hit",
                    .key   = "'element'",
                },
                // Drop the freeze in evidence.luau's mint_receipt.
                Snapshot{
                    .label = "a receipt",
                    .value = "receipt",
                    .key   = "'page'",
                },
                // Drop the freeze around `edge` in navigation.luau's Edge.new.
                Snapshot{
                    .label = "an edge",
                    .value = "open_detail",
                    .key   = "'from'",
                },
                // Drop the freeze around `destinations` in navigation's Edge.new.
                Snapshot{
                    .label = "an edge's destination set",
                    .value = "open_detail.to",
                    .key   = "1",
                },
                // Drop the freeze around `graph` in navigation.luau's Graph.new.
                Snapshot{
                    .label = "a graph",
                    .value = "graph",
                    .key   = "'pages'",
                },
                // Drop the freeze around `pages` in navigation.luau's Graph.new.
                Snapshot{
                    .label = "a graph's page list",
                    .value = "graph.pages",
                    .key   = "1",
                },
                // Drop the freeze around `pageByName` in navigation's Graph.new.
                Snapshot{
                    .label = "a graph's page index",
                    .value = "graph.page_by_name",
                    .key   = "'base'",
                },
                // Drop the freeze around `edges` in navigation.luau's Graph.new.
                Snapshot{
                    .label = "a graph's edge list",
                    .value = "graph.edges",
                    .key   = "1",
                },
                // Drop the freeze around `interrupts` in navigation's Graph.new.
                Snapshot{
                    .label = "a graph's interrupt list",
                    .value = "graph.interrupts",
                    .key   = "1",
                },
                // Drop the freeze around `screen` in oracle.luau's Screen.new.
                Snapshot{
                    .label = "a screen",
                    .value = "map",
                    .key   = "'name'",
                },
                // Drop the freeze around `expectation` in Expectation.new.
                Snapshot{
                    .label = "an expectation",
                    .value = "claims.expectations[1]",
                    .key   = "'screen'",
                },
                // Drop the freeze around `claims` in oracle.luau's Claims.new.
                Snapshot{
                    .label = "a claims table",
                    .value = "claims",
                    .key   = "'screens'",
                },
                // Drop the freeze around `screens` in oracle.luau's Claims.new.
                Snapshot{
                    .label = "a claims table's screen list",
                    .value = "claims.screens",
                    .key   = "1",
                },
                // Drop the freeze around `expectations` in oracle's Claims.new.
                Snapshot{
                    .label = "a claims table's expectation list",
                    .value = "claims.expectations",
                    .key   = "1",
                },
                // Drop the freeze around `screenByName` in oracle's Claims.new.
                Snapshot{
                    .label = "a claims table's screen index",
                    .value = "claims.screen_by_name",
                    .key   = "'map'",
                },
                // Drop the freeze around `stateByCell` in oracle's Claims.new.
                Snapshot{
                    .label = "a claims table's state index",
                    .value = "claims.state_by_cell",
                    .key   = "(next(claims.state_by_cell))",
                },
                // Drop the freeze around `textByCell` in oracle's Claims.new.
                Snapshot{
                    .label = "a claims table's text index",
                    .value = "claims.text_by_cell",
                    .key   = "(next(claims.text_by_cell))",
                },
                // Drop the freeze around `rectsBySubject` in oracle's Claims.new.
                Snapshot{
                    .label = "a claims table's rectangle index",
                    .value = "claims.rects_by_subject",
                    .key   = "(next(claims.rects_by_subject))",
                },
                // Drop the freeze over each `rects` list in oracle's Claims.new.
                Snapshot{
                    .label = "the rectangles claimed for one subject",
                    .value = "claims.rects_by_subject[next(claims.rects_by_subject)]",
                    .key   = "1",
                },
            };
        }

        TEST_CASE("Every value the framework hands a script refuses a write")
        {
            // The whole immutability contract, stated as the write it forbids
            // rather than as the flag that forbids it: `table.isfrozen` says a
            // value carries the bit, and only a refused write says a project script
            // cannot edit the model the framework goes on trusting.
            auto const directory = TemporaryDirectory{"uf-model-frozen-values"};
            seedTemplates(directory.path());

            for (auto const& snapshot : snapshots())
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

                auto body = std::string{k_snapshotPrelude};
                body += "if refusesEveryWrite(";
                body += snapshot.value;
                body += ", ";
                body += snapshot.key;
                body += ") then return 1 end\nreturn 0\n";

                INFO("a script cannot write to ", snapshot.label);
                auto const result = runModel(context, built, graphScript(body));
                REQUIRE(result.has_value());
                CHECK(*result == doctest::Approx(1.0));
            }
        }

        TEST_CASE("The three model modules are in the framework bundle")
        {
            // The modules ship with the binary rather than with a project, which is
            // the whole of what makes them trusted. Nothing else in this file would
            // notice a module that failed to be embedded, because the config the
            // tests boot names them by hand.
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
