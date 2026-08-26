#include "../support/runtime-v4-fixture.hpp"

#include <task/exploration-session.hpp>
#include <task/framework-bundle.hpp>
#include <task/host-delivery.hpp>
#include <task/runtime-model-file.hpp>
#include <task/task-context.hpp>
#include <task/task-host.hpp>
#include <task/ui-observation.hpp>

#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <engine/session.hpp>

#include <ocr/engine.hpp>

#include <script/engine.hpp>

#include <trace/file-sink.hpp>
#include <trace/recorder.hpp>

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace uf::task
{
    namespace
    {
        [[nodiscard]]
        auto runText(
            TaskHost& host,
            GenerationId generation,
            RuntimeContext& runtime,
            std::string_view source
        ) -> std::string
        {
            auto value = TaskHostTestAccess::run(
                host,
                generation,
                runtime.context(),
                source
            );
            REQUIRE(value.has_value());
            REQUIRE(value->text() != nullptr);
            return *value->text();
        }

        [[nodiscard]]
        auto pixelRect(uint32 x, uint32 y, uint32 width, uint32 height) -> PixelRect
        {
            auto const value = PixelRect::create(x, y, width, height);
            REQUIRE(value.has_value());
            return *value;
        }

        // An OCR engine that answers with what the case scripted, and records
        // what it was asked. It is not a stub standing in for a real engine: the
        // property under test is what the resolver does with a reading, and a
        // real engine would make the text a fact about a PNG rather than about
        // this file, without making any of these assertions stronger. Its line
        // bounds deliberately differ from the requested rect so SingleLine's
        // declared-placement contract is exercised at the engine boundary.
        class ScriptedReader final : public ocr::IOcrEngine
        {
            std::string m_text;
            uint32      m_confidenceBp;
            uint32      m_calls{};
            PixelRect   m_lastRect{pixelRect(0, 0, 1, 1)};
            PixelRect   m_reportedBounds{pixelRect(0, 0, 1, 1)};

        public:
            ScriptedReader(std::string text, uint32 confidenceBp)
                : m_text{std::move(text)}
                , m_confidenceBp{confidenceBp}
            {
            }

            [[nodiscard]] auto identity() const noexcept -> std::string_view override
            {
                return "scripted-reader";
            }

            [[nodiscard]]
            auto read(BgraImage const&, ocr::ReadSpec const& spec)
                -> Result<ocr::Readout> override
            {
                ++m_calls;
                if (spec.rect.has_value())
                {
                    m_lastRect = *spec.rect;
                }
                return ocr::Readout{
                    .lines = {
                        ocr::TextLine{
                            .text         = m_text,
                            .bounds       = m_reportedBounds,
                            .confidenceBp = m_confidenceBp,
                        },
                    },
                };
            }

            [[nodiscard]] auto calls() const noexcept -> uint32 { return m_calls; }

            [[nodiscard]] auto lastRect() const noexcept -> PixelRect
            {
                return m_lastRect;
            }
        };

        // A Reader that looked and found nothing. It is the only way to reach
        // the third reading outcome, because EngineSession::readText reports "no
        // text here" as an empty line list and never as an empty string: a line
        // whose text is "" is still a line the recogniser produced.
        class SilentReader final : public ocr::IOcrEngine
        {
            uint32 m_calls{};

        public:
            [[nodiscard]] auto identity() const noexcept -> std::string_view override
            {
                return "silent-reader";
            }

            [[nodiscard]]
            auto read(BgraImage const&, ocr::ReadSpec const&)
                -> Result<ocr::Readout> override
            {
                ++m_calls;
                return ocr::Readout{};
            }

            [[nodiscard]] auto calls() const noexcept -> uint32 { return m_calls; }
        };

        // What a block Reader's engine answers: several lines, each located
        // somewhere else in the IMAGE. It reports bounds of its own rather than
        // the rect it was asked about, which is what a detector does and what
        // makes a reported rectangle a measurement rather than a copy of the
        // question.
        class ScriptedBlockReader final : public ocr::IOcrEngine
        {
            std::vector<ocr::TextLine>     m_lines;
            std::optional<ocr::TextLayout> m_lastLayout{};

        public:
            explicit ScriptedBlockReader(std::vector<ocr::TextLine> lines)
                : m_lines{std::move(lines)}
            {
            }

            [[nodiscard]] auto identity() const noexcept -> std::string_view override
            {
                return "scripted-block-reader";
            }

            [[nodiscard]]
            auto read(BgraImage const&, ocr::ReadSpec const& spec)
                -> Result<ocr::Readout> override
            {
                m_lastLayout = spec.layout;
                return ocr::Readout{.lines = m_lines};
            }

            [[nodiscard]]
            auto lastLayout() const noexcept -> std::optional<ocr::TextLayout>
            {
                return m_lastLayout;
            }
        };

        // One rectangle has two different answers because layout selects a
        // different OCR pipeline. Serving either answer to the other question
        // makes the memoisation key observably incomplete.
        class LayoutSensitiveReader final : public ocr::IOcrEngine
        {
            uint32 m_calls{};

        public:
            [[nodiscard]] auto identity() const noexcept -> std::string_view override
            {
                return "layout-sensitive-reader";
            }

            [[nodiscard]]
            auto read(BgraImage const&, ocr::ReadSpec const& spec)
                -> Result<ocr::Readout> override
            {
                ++m_calls;
                if (spec.layout == ocr::TextLayout::SingleLine)
                {
                    return ocr::Readout{
                        .lines = {
                            ocr::TextLine{
                                .text   = "single answer",
                                .bounds = pixelRect(0, 0, 1, 1),
                                .confidenceBp = 9'100,
                            },
                        },
                    };
                }
                return ocr::Readout{
                    .lines = {
                        ocr::TextLine{
                            .text   = "block first",
                            .bounds = pixelRect(0, 0, 1, 1),
                            .confidenceBp = 9'200,
                        },
                        ocr::TextLine{
                            .text   = "block second",
                            .bounds = pixelRect(2, 0, 1, 1),
                            .confidenceBp = 9'300,
                        },
                    },
                };
            }

            [[nodiscard]] auto calls() const noexcept -> uint32 { return m_calls; }
        };

        // The fixture's world plus one Readout over the middle pixel, and a
        // Binding over that same pixel whose detector is the confirm mark. The
        // Binding is here to be turned OFF: the middle pixel decides whether it
        // is present while the anchor still resolves the Surface, which is what
        // lets "a Readout is gated by its Surface and by nothing else" be
        // exercised without also unresolving the state.
        [[nodiscard]]
        auto readingRuntimeModel(std::string_view titleLayout = "single_line")
            -> std::string
        {
            return R"toml(schema_version = 4
base_resolution = [3, 1]
base_dpi = [96, 96]

[[ui_target]]
id = "screen-marker"
kind = "region"

[[ui_target]]
id = "title"
kind = "region"

[[locator]]
id = "screen-anchor"
kind = "template"
asset_path = "assets/anchor.png"
threshold = 1

[[locator]]
id = "confirm-mark"
kind = "template"
asset_path = "assets/confirm.png"
threshold = 1

[[binding]]
id = "screen.anchor"
surface = "screen"
ui_target = "screen-marker"
placement = { kind = "fixed", rect = [0, 0, 1, 1] }
variants = [{ name = "primary", detector = { all = [{ kind = "locator_present", locator = "screen-anchor" }], any = [], none = [] } }]
actions = []

[[binding]]
id = "title.primary"
surface = "screen"
ui_target = "title"
placement = { kind = "fixed", rect = [1, 0, 1, 1] }
variants = [{ name = "primary", detector = { all = [{ kind = "locator_present", locator = "confirm-mark" }], any = [], none = [] } }]
actions = []

[[readout]]
id = "title"
surface = "screen"
rect = [1, 0, 1, 1]
layout = ")toml"
                + std::string{titleLayout}
                + R"toml("
confidence_floor = 0.5
normalization = "collapse_whitespace"

[[surface]]
id = "screen"
covers = []
identity = ["screen.anchor"]
)toml";
        }

        // T-009's falsifier. A detector is positive evidence that a visual
        // element is present; text_equals asserts what text a rectangle holds,
        // which is evidence about what a Surface shows and never a Surface's
        // identity -- its only legal home is a Collection filter, over members a
        // locator has already found. This shape, a Binding detector naming
        // text_equals, must fail to parse and refuse the whole artifact.
        //
        // The detector line is the whole difference from the activating control:
        // swapping it for the locator_present over the declared title.mark (same
        // body, same asset, same manifest) must activate, so the refusal is about
        // the predicate kind and not about anything else in the model.
        [[nodiscard]] auto textIdentityRuntimeModel(std::string_view detector)
            -> std::string
        {
            return std::string{R"toml(schema_version = 4
base_resolution = [3, 1]
base_dpi = [96, 96]

[[ui_target]]
id = "title"
kind = "region"

[[locator]]
id = "title.mark"
kind = "template"
asset_path = "assets/title.png"
threshold = 1

[[binding]]
id = "title.identity"
surface = "screen"
ui_target = "title"
placement = { kind = "fixed", rect = [0, 0, 1, 1] }
variants = [{ name = "default", detector = { all = [)toml"}
                + std::string{detector}
                + R"toml(], any = [], none = [] } }]
actions = []

[[surface]]
id = "screen"
covers = []
identity = ["title.identity"]
)toml";
        }

        [[nodiscard]] auto collectionRuntimeModel() -> std::string
        {
            auto result = runtimeModel();
            auto constexpr baseResolution = std::string_view{"base_resolution = [3, 1]"};
            auto const baseResolutionAt = result.find(baseResolution);
            REQUIRE(baseResolutionAt != std::string::npos);
            result.replace(baseResolutionAt, baseResolution.size(), "base_resolution = [5, 1]");
            result += R"toml(
[[collection]]
id = "options"
surface = "screen"
placement = { kind = "detected", search_rect = [0, 0, 5, 1], confidence_floor = 0.5, order = "left_to_right", slots = { origin = 2, pitch = 2, extent = 5, tolerance = 0, maximum_slots = 4 } }
)toml";
            return result;
        }

        // One rectangle read twice, under the two layouts. A cycle memoises per
        // read, and the layout is part of the question, so the same pixels
        // under two layouts are two Host reads rather than one cached answer.
        [[nodiscard]] auto mixedLayoutReadingRuntimeModel() -> std::string
        {
            return readingRuntimeModel() + R"toml(
[[readout]]
id = "title.block"
surface = "screen"
rect = [1, 0, 1, 1]
layout = "block"
confidence_floor = 0.5
normalization = "collapse_whitespace"
)toml";
        }

        // The reading world with a SECOND Readout over the third pixel. Two
        // rectangles are what a read budget can be exhausted against, and
        // `title` stays declared first, which is the order stack_readouts walks
        // and therefore which of the two spends the one read a budget of 1 buys.
        [[nodiscard]] auto budgetedReadingRuntimeModel() -> std::string
        {
            return readingRuntimeModel() + R"toml(
[[readout]]
id = "subtitle"
surface = "screen"
rect = [2, 0, 1, 1]
layout = "single_line"
confidence_floor = 0.5
normalization = "collapse_whitespace"
)toml";
        }

        // The fixture's world with one Binding that grants a keystroke and no
        // click. Its placement carries no action_point, which is the whole
        // difference the kind makes to a model, and its detector is the confirm
        // mark so the middle pixel decides whether that Binding is present
        // while the anchor still resolves the Surface.
        [[nodiscard]] auto keyRuntimeModel() -> std::string
        {
            return R"toml(schema_version = 4
base_resolution = [3, 1]
base_dpi = [96, 96]

[[ui_target]]
id = "screen-marker"
kind = "region"

[[ui_target]]
id = "commit"
kind = "control"

[[locator]]
id = "screen-anchor"
kind = "template"
asset_path = "assets/anchor.png"
threshold = 1

[[locator]]
id = "confirm-mark"
kind = "template"
asset_path = "assets/confirm.png"
threshold = 1

[[binding]]
id = "screen.anchor"
surface = "screen"
ui_target = "screen-marker"
placement = { kind = "fixed", rect = [0, 0, 1, 1] }
variants = [{ name = "primary", detector = { all = [{ kind = "locator_present", locator = "screen-anchor" }], any = [], none = [] } }]
actions = []

[[binding]]
id = "commit.primary"
surface = "screen"
ui_target = "commit"
placement = { kind = "fixed", rect = [1, 0, 1, 1] }
variants = [{ name = "primary", detector = { all = [{ kind = "locator_present", locator = "confirm-mark" }], any = [], none = [] } }]
actions = [{ id = "activate", kind = "key", key = "E", proof_locator = "confirm-mark" }]

[[surface]]
id = "screen"
covers = []
identity = ["screen.anchor"]
)toml";
        }

        // The fixture's world with its one AIMED action replaced by another.
        // Every aimed kind authorizes against the same measured placement and
        // the same proof, so the declaration is the only thing that varies
        // between the cases below and a difference at the sink is a difference
        // the kind made.
        [[nodiscard]]
        auto runtimeModelGranting(std::string_view action) -> std::string
        {
            auto result = runtimeModel();
            auto constexpr click = std::string_view{
                R"({ id = "activate", kind = "click", proof_locator = "confirm-mark" })"
            };
            auto const at = result.find(click);
            REQUIRE(at != std::string::npos);
            result.replace(at, click.size(), action);
            return result;
        }

        // The same substitution over the world whose Binding carries no
        // action_point, which is the only world an UNAIMED kind can be
        // declared in: the model refuses a point nothing aims at.
        [[nodiscard]]
        auto keyRuntimeModelGranting(std::string_view action) -> std::string
        {
            auto result = keyRuntimeModel();
            auto constexpr key = std::string_view{
                R"({ id = "activate", kind = "key", key = "E", proof_locator = "confirm-mark" })"
            };
            auto const at = result.find(key);
            REQUIRE(at != std::string::npos);
            result.replace(at, key.size(), action);
            return result;
        }

        [[nodiscard]] auto dragRuntimeModel() -> std::string
        {
            return runtimeModelGranting(
                R"({ id = "activate", kind = "drag", offset = [1, 0], duration_ms = 600, proof_locator = "confirm-mark" })"
            );
        }

        auto const k_runtimeKeyAction = UiActionUnderTest{
            .surface  = "screen",
            .uiTarget = "commit",
            .action   = "activate",
        };

        [[nodiscard]]
        auto loadedKeyRuntime(TaskHost& host, TemporaryDirectory const& directory)
            -> GenerationId
        {
            auto const rootHash = publish(
                directory.path(),
                keyRuntimeModel(),
                runtimeAssets()
            );
            auto const generation = TaskHostTestAccess::activate(
                host,
                directory.path(),
                rootHash
            );
            REQUIRE(generation.has_value());
            return *generation;
        }

        [[nodiscard]]
        auto loadedRuntime(
            TaskHost& host,
            TemporaryDirectory const& directory,
            std::string_view model
        ) -> GenerationId
        {
            auto const rootHash = publish(
                directory.path(),
                std::string{model},
                runtimeAssets()
            );
            auto const generation = TaskHostTestAccess::activate(
                host,
                directory.path(),
                rootHash
            );
            REQUIRE(generation.has_value());
            return *generation;
        }

        [[nodiscard]]
        auto loadedDragRuntime(TaskHost& host, TemporaryDirectory const& directory)
            -> GenerationId
        {
            return loadedRuntime(host, directory, dragRuntimeModel());
        }

        // A Tool Runtime that records the name of every call and answers each
        // one confirmed with an empty result.
        //
        // It stands where service::ProductLifecycle's own seam stands, and a
        // case using it is asking one question only: DID THE VERB GO THROUGH
        // THE SEAM. What the Operator then does with the call -- admission, the
        // policy, the ledger row -- is asserted against a real Operator root in
        // tests/cli/test-explore-door.cpp, because none of it is a property of
        // the Host.
        [[nodiscard]]
        auto recordingToolRuntime(std::shared_ptr<std::vector<std::string>> issued)
            -> ToolRuntimeBinder
        {
            return [issued = std::move(issued)](TaskContext&)
                -> script::ToolRuntimeInvoke
            {
                return [issued](
                           std::string_view toolName,
                           json::Value const&
                       ) -> Result<json::Value>
                {
                    issued->emplace_back(toolName);
                    return json::Value::ofObject({
                        {"call_identity",
                         json::Value::ofString(std::string(64U, '0'))},
                        {"delivery", json::Value::ofString("confirmed")},
                        {"ok", json::Value::ofBoolean(true)},
                        {"result", json::Value::ofObject({})},
                    });
                };
            };
        }

        [[nodiscard]]
        auto explorationCatalogResource()
            -> script::PureDataProgram::Resource
        {
            constexpr auto tools = std::string_view{
                R"([{"description":"Click at one observed coordinate","input_schema":{"additional_properties":false,"required":["screenshot_sha256","x","y"],"type":"object"},"name":"framework.input.click","tool_version":"1"},{"description":"Observe one retained screenshot","input_schema":{"additional_properties":false,"required":["screenshot_sha256"],"type":"object"},"name":"framework.screen.observe","tool_version":"1"},{"description":"Read lines from one retained screenshot","input_schema":{"additional_properties":false,"required":["height","screenshot_sha256","width","x","y"],"type":"object"},"name":"framework.screen.read_lines","tool_version":"1"}])"
            };
            auto digest = sha256(std::as_bytes(std::span{tools}));
            REQUIRE(digest.has_value());
            return script::PureDataProgram::Resource{
                .kind = script::PureDataProgram::ResourceKind::Json,
                .name = std::string{scopedToolCatalogResourceName()},
                .bytes = R"({"catalog_hash":")" + digest->hex()
                    + R"(","tools":)" + std::string{tools} + "}",
            };
        }

        // What service::ProductLifecycle::startExplorationSession composes in
        // production, composed here instead: the recorder, the engine session
        // and the Tool Runtime are the ledgered caller's to build, so a test
        // that wants an exploration session builds them too rather than
        // reaching a door the Host no longer has.
        [[nodiscard]]
        auto startExploration(
            TaskHost& host,
            GenerationId generation,
            Frame value,
            std::filesystem::path const& projectRoot,
            std::filesystem::path tracePath,
            std::shared_ptr<std::vector<std::string>> issued
                = std::make_shared<std::vector<std::string>>()
        ) -> Result<std::unique_ptr<ExplorationSession>>
        {
            auto sink = trace::FileTraceSink::createNew(tracePath);
            REQUIRE(sink.has_value());
            auto const manifestHash = sha256(
                std::as_bytes(std::span{std::string_view{"exploration-fixture"}})
            );
            REQUIRE(manifestHash.has_value());
            auto opened = trace::TraceRecorder::create(
                std::move(*sink),
                trace::TraceStreamSpec{
                    .sessionId           = "session-exploration-fixture",
                    .sessionManifestHash = *manifestHash,
                    .producer            = "annotation",
                }
            );
            REQUIRE(opened.has_value());
            auto recorder = std::make_unique<trace::TraceRecorder>(
                std::move(*opened)
            );
            auto session = engine::EngineSession::create(
                std::make_unique<FrameSource>(std::move(value)),
                std::make_unique<ActionSink>(),
                *recorder,
                engine::EngineSessionConfig{
                    .liveFingerprint    = fingerprint(),
                    .projectFingerprint = fingerprint(),
                    .maximumPixelComparisons = 1'000,
                    .recognitionTimeout      = std::chrono::seconds{1},
                },
                nullptr
            );
            REQUIRE(session.has_value());
            return host.startExplorationSession(
                generation,
                std::move(recorder),
                *std::move(session),
                ExplorationSessionSpec{
                    .projectId           = "exploration-fixture",
                    .projectRoot         = projectRoot,
                    .tracePath           = std::move(tracePath),
                    .bindToolRuntime     = recordingToolRuntime(std::move(issued)),
                    .toolCatalogResource = explorationCatalogResource(),
                }
            );
        }

        // The fixture's model with `geometry` standing in for the two lines it
        // declares, and its body reused byte for byte. Every rectangle in that
        // body sits within 2x1, so any extent at or above the fixture's own
        // accepts the same bindings and the declared geometry is the only thing
        // that varies between the models below.
        [[nodiscard]] auto modelDeclaring(std::string_view geometry) -> std::string
        {
            auto const fixture = runtimeModel();
            auto const body    = fixture.find("\n\n[[ui_target]]");
            REQUIRE(body != std::string::npos);
            return "schema_version = 4\n" + std::string{geometry}
                + fixture.substr(body);
        }

        [[nodiscard]]
        auto activateDeclaring(
            TaskHost& host,
            TemporaryDirectory const& directory,
            std::string_view geometry
        ) -> Result<GenerationId>
        {
            auto const model = modelDeclaring(geometry);
            auto const rootHash = publish(directory.path(), model, runtimeAssets());
            return TaskHostTestAccess::activate(host, directory.path(), rootHash);
        }

        [[nodiscard]]
        auto bindingDeclaring(
            TaskHost& host,
            TemporaryDirectory const& directory,
            std::string_view geometry
        ) -> RuntimeModelBinding
        {
            auto const generation = activateDeclaring(host, directory, geometry);
            REQUIRE(generation.has_value());
            auto binding = host.runtimeModelBinding(*generation);
            REQUIRE(binding.has_value());
            return *std::move(binding);
        }
    }

    TEST_CASE("contract-runtime-u01")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedRuntime(host, directory);
        auto status = host.queryTask(generation);
        REQUIRE(status.has_value());
        CHECK(status->runtimeModelBound);

        auto modelBytes = host.runtimeModelBytes(generation);
        REQUIRE(modelBytes.has_value());
        CHECK(*modelBytes == bytes(runtimeModel()));

        auto const invalidDirectory = TemporaryDirectory{};
        auto const invalidModel = std::string{"schema_version = 4\n"};
        auto const invalidRoot = publish(invalidDirectory.path(), invalidModel, {});
        auto invalidHost = TaskHost{};
        CHECK_FALSE(
            TaskHostTestAccess::activate(
                invalidHost,
                invalidDirectory.path(),
                invalidRoot
            ).has_value()
        );
    }

    // The seam between the trusted parser's own RuntimeModel generation and the
    // one the artifact states. loadRuntimeArtifact has already held the artifact
    // against k_runtimeModelFormat by the time finalize runs, so this refusal
    // fires only when modules/task/runtime/model.luau reads a different
    // generation than modules/task/source/task/runtime-model-file.hpp expects --
    // a build made of two halves, which nothing published can produce and which
    // therefore has to be reached deliberately.
    //
    // It is worth reaching: it is the whole of what replaced the surface gate's
    // model.schema_hash rule. That rule compared a Luau literal to a file; this
    // reddens every activation in the suite when the two halves disagree.
    TEST_CASE("a parser reading another RuntimeModel generation cannot finalize")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedRuntime(host, directory);

        constexpr auto k_foreign = uint64{99U};
        auto const refused = TaskHostTestAccess::finalizeWithParserFormat(
            host,
            generation,
            k_foreign
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains(
            std::format("parser reads RuntimeModel format {}", k_foreign)
        ));
        CHECK(refused.error().message().contains(
            std::format("artifact states format {}", k_runtimeModelFormat)
        ));

        // The positive control: the generation this activation already
        // finalized is still bound, so the refusal above is about the number
        // and not about a Host that refuses every second finalize.
        auto const status = host.queryTask(generation);
        REQUIRE(status.has_value());
        CHECK(status->runtimeModelBound);
    }

    TEST_CASE("contract-runtime-u02")
    {
        auto const missingDirectory = TemporaryDirectory{};
        auto assets = runtimeAssets();
        assets.pop_back();
        auto const missingRoot = publish(missingDirectory.path(), runtimeModel(), assets);
        auto missingHost = TaskHost{};
        CHECK_FALSE(
            TaskHostTestAccess::activate(
                missingHost,
                missingDirectory.path(),
                missingRoot
            ).has_value()
        );

        auto const extraDirectory = TemporaryDirectory{};
        assets = runtimeAssets();
        assets.emplace_back(
            ArtifactFile{.path = "assets/unreferenced.png", .bytes = templatePng(9)}
        );
        auto const extraRoot = publish(extraDirectory.path(), runtimeModel(), assets);
        auto extraHost = TaskHost{};
        CHECK_FALSE(TaskHostTestAccess::activate(
            extraHost,
            extraDirectory.path(),
            extraRoot
        ).has_value());
    }

    TEST_CASE("contract-runtime-u03")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedRuntime(host, directory);
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{11}),
            1'000
        };
        CHECK(
            runText(
                host,
                generation,
                runtime,
                R"lua(
                    local cycle = observe.open(project.load_project())
                    local state = cycle:resolve_state()
                    local binding = cycle:resolve_binding(state, "confirm")
                    cycle:close()
                    return state.kind .. ":" .. state.ordered_surface_stack[1]
                        .. ":" .. binding.kind .. ":" .. binding.binding
                )lua"
            )
            == "resolved_state:screen:resolved_binding:confirm.primary"
        );
    }

    TEST_CASE("contract-runtime-u04")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedRuntime(host, directory);
        auto runtime = RuntimeContext{
            frame({std::byte{0}, std::byte{k_actionGray}, std::byte{0}}, FrameId{12}),
            1'000
        };
        CHECK(
            runText(
                host,
                generation,
                runtime,
                R"lua(
                    local cycle = observe.open(project.load_project())
                    local state = cycle:resolve_state()
                    cycle:close()
                    return state.kind .. ":" .. state.reason
                )lua"
            )
            == "unknown_state:no_scene_candidate"
        );
    }

    TEST_CASE("contract-runtime-u05")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedRuntime(host, directory);
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{13}),
            0
        };
        CHECK(
            runText(
                host,
                generation,
                runtime,
                R"lua(
                    local cycle = observe.open(project.load_project())
                    local state = cycle:resolve_state()
                    cycle:close()
                    return state.kind .. ":" .. state.reason
                )lua"
            )
            == "unknown_state:unknown_scene_competitor"
        );
        CHECK(runtime.actions().clicks() == 0U);
    }

    TEST_CASE("T-004 T10 visible unmatched content emits a diagnostic")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedRuntime(host, directory);
        auto runtime = RuntimeContext{
            frame({std::byte{42}, std::byte{42}, std::byte{42}}, FrameId{54}),
            1'000
        };

        auto const observed = observeOnce(host, generation, runtime.context());
        REQUIRE(observed.has_value());
        CHECK_MESSAGE(
            observed->canonicalJcs()
            == R"({"diagnostic":"visible_content_matched_nothing",)"
               R"("kind":"unknown_state","reason":"no_scene_candidate"})",
            "T-004 T10 visible unmatched content must emit its diagnostic"
        );
        CHECK(
            observed->canonicalJcs().find(
                R"("diagnostic":"visible_content_matched_nothing")"
            )
            != std::string::npos
        );
    }

    TEST_CASE("contract-runtime-u06")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedRuntime(host, directory);
        auto const fence = controlFence(7);
        REQUIRE(TaskHostTestAccess::adoptControlFence(host, fence).has_value());
        auto const authority = dispatchAuthority(fence, generation);
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{14}),
            1'000
        };
        auto minted = TaskHostTestAccess::run(
            host,
            generation,
            runtime.context(),
            authorizeActionSource(k_runtimeUiAction)
        );
        REQUIRE(minted.has_value());
        CHECK(minted->number() == std::optional<double>{1.0});
        CHECK(runtime.actions().clicks() == 0U);
        CHECK(TaskHostTestAccess::pendingSemanticHash(host) != hash(runtimeModel()));

        auto const receipt = TaskHostTestAccess::pendingReceipt(host, k_runtimeUiAction);
        auto const delivered = TaskHostTestAccess::deliver(
            host,
            authority,
            receipt,
            runtime.context()
        );
        REQUIRE(delivered.has_value());
        CHECK(delivered->outcome() == DeliveryOutcome::Delivered);
        CHECK(delivered->reason().empty());
        CHECK(runtime.actions().clicks() == 1U);

        // The reservation comes back untouched, which is what lets the ledger
        // recognise its own row instead of taking the Host's word for it.
        CHECK(delivered->authority().dispatchSequence == authority.dispatchSequence);
        CHECK(delivered->authority().leaseId == authority.leaseId);
        CHECK(delivered->authority().frozenPlanHash == authority.frozenPlanHash);

        // A second presentation is an ERROR rather than a report: nothing was
        // consumed, so there is no fact for the ledger to record.
        CHECK_FALSE(
            TaskHostTestAccess::deliver(
                host,
                authority,
                receipt,
                runtime.context()
            ).has_value()
        );
        CHECK(runtime.actions().clicks() == 1U);

        // A Receipt authorizes one delivery against the cycle it was minted in,
        // so presenting a fresh one with a different context posts no click: no
        // other context holds that cycle. That is what lets the Host stop
        // remembering a pointer to the context that minted it. The refusal is a
        // NotDelivered report rather than an error, because the Receipt is gone.
        auto const reminted = TaskHostTestAccess::run(
            host,
            generation,
            runtime.context(),
            authorizeActionSource(k_runtimeUiAction)
        );
        REQUIRE(reminted.has_value());
        auto other = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{19}),
            1'000
        };

        // The other context must be holding a cycle of its own, or the refusal
        // proves only that it has none -- which would still hold with the
        // generation stamp removed. With both open, the stamp is what separates
        // them.
        auto const otherCycle = TaskHostTestAccess::run(
            host,
            generation,
            other.context(),
            "return observe.open(project.load_project()) ~= nil"
        );
        REQUIRE(otherCycle.has_value());
        CHECK(otherCycle->boolean() == std::optional<bool>{true});
        auto const refused = TaskHostTestAccess::deliver(
            host,
            authority,
            TaskHostTestAccess::pendingReceipt(host, k_runtimeUiAction),
            other.context()
        );
        REQUIRE(refused.has_value());
        CHECK(refused->outcome() == DeliveryOutcome::NotDelivered);
        CHECK_FALSE(refused->reason().empty());
        CHECK(refused->receiptId() != delivered->receiptId());
        CHECK(other.actions().clicks() == 0U);
        CHECK(runtime.actions().clicks() == 1U);
    }

    // One friend, and it is TaskHost. TaskHostTestAccess reaches the Host's
    // privates and can therefore call deliver, but it cannot assemble what
    // deliver returns -- without which every case above would be asserting on a
    // value the test itself could have written.
    static_assert(!std::is_default_constructible_v<HostDeliveryReport>);
    static_assert(
        !std::is_constructible_v<
            HostDeliveryReport,
            DispatchAuthority,
            DeliveryOutcome,
            std::string,
            uint64,
            std::optional<engine::ActReceipt>
        >
    );
    static_assert(std::is_copy_constructible_v<HostDeliveryReport>);

    // The key path end to end: a model that grants a keystroke and no click, a
    // Receipt minted for it, and the KeyName that arrives at the last port
    // before the platform adapter -- the same value ControllerActionSink hands
    // to MapVirtualKeyW. Naming the key is the point. A case that only counted
    // one delivery would stay green if every action resolved to the same key,
    // and a production path that cannot spell "E" cannot end a turn.
    TEST_CASE("TaskHost::deliver posts the key its Receipt authorized")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedKeyRuntime(host, directory);
        auto const fence = controlFence(7);
        REQUIRE(TaskHostTestAccess::adoptControlFence(host, fence).has_value());
        auto const authority = dispatchAuthority(
            fence,
            generation,
            k_runtimeKeyAction.uiTarget
        );
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{41}),
            1'000
        };
        auto const minted = TaskHostTestAccess::run(
            host,
            generation,
            runtime.context(),
            authorizeActionSource(k_runtimeKeyAction)
        );
        REQUIRE(minted.has_value());
        CHECK(runtime.actions().keys() == 0U);

        auto const receipt = TaskHostTestAccess::pendingReceipt(
            host,
            k_runtimeKeyAction
        );
        auto const delivered = TaskHostTestAccess::deliver(
            host,
            authority,
            receipt,
            runtime.context()
        );
        REQUIRE(delivered.has_value());
        CHECK(delivered->outcome() == DeliveryOutcome::Delivered);
        CHECK(delivered->reason().empty());

        // A keystroke names no coordinate, so nothing on this path posts one.
        // Measured at the port rather than on the report, which is where the
        // claim belongs: a dispatch that took the click branch would arrive as
        // a click here whatever the report said about itself.
        CHECK(runtime.actions().clicks() == 0U);
        CHECK(runtime.actions().keys() == 1U);

        auto const posted = runtime.actions().lastKey();
        REQUIRE(posted.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): REQUIRE above proved engagement.
        CHECK(posted->value() == "E");
    }

    // The three actions the declared vocabulary gained, each end to end: a
    // model that grants exactly one of them, the Receipt minted for it, and the
    // verb that arrives at the last port before the platform adapter.
    //
    // One case rather than three, because the setup is identical and only the
    // declaration moves -- which is also the property being asserted. Each arm
    // checks the OTHER verbs stayed at zero, so a dispatch that fell through to
    // the click branch is red here whatever the report says about itself.
    //
    // These are production deliveries. Before this vocabulary existed a project
    // could declare none of them, so a method proven during annotation -- a hold
    // that opens a detail view a click never shows -- was undeliverable; that is
    // what these arms are here to keep true.
    TEST_CASE("the three actions the vocabulary gained deliver as declared")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const fence = controlFence(7);
        REQUIRE(TaskHostTestAccess::adoptControlFence(host, fence).has_value());

        SUBCASE("a declared hold posts its measured point for its declared span")
        {
            auto const generation = loadedRuntime(
                host,
                directory,
                runtimeModelGranting(
                    R"({ id = "activate", kind = "hold", hold_ms = 350, proof_locator = "confirm-mark" })"
                )
            );
            auto const authority = dispatchAuthority(fence, generation);
            auto runtime = RuntimeContext{
                frame(
                    {std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}},
                    FrameId{71}
                ),
                1'000
            };
            REQUIRE(
                TaskHostTestAccess::run(
                    host,
                    generation,
                    runtime.context(),
                    authorizeActionSource(k_runtimeUiAction)
                ).has_value()
            );
            auto const delivered = TaskHostTestAccess::deliver(
                host,
                authority,
                TaskHostTestAccess::pendingReceipt(host, k_runtimeUiAction),
                runtime.context()
            );
            REQUIRE(delivered.has_value());
            CHECK(delivered->outcome() == DeliveryOutcome::Delivered);
            CHECK(runtime.actions().holds() == 1U);
            CHECK(runtime.actions().clicks() == 0U);
            REQUIRE(runtime.actions().lastHoldPoint().has_value());
            REQUIRE(runtime.actions().lastHeldFor().has_value());
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): REQUIRE above proved engagement.
            CHECK(runtime.actions().lastHoldPoint()->x() == doctest::Approx(1.0));

            // The declared 350 ms is proved by how long the BUTTON WAS DOWN,
            // not by a number handed to the sink: nothing tells the sink a
            // duration any more, so a chain that dropped the declaration would
            // press and release with nothing in between. Delete the settle in
            // task::TaskContext::holdFor and this goes red.
            CHECK(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    *runtime.actions().lastHeldFor()
                ).count()
                >= 350
            );
        }

        SUBCASE("a declared move posts its measured point and presses nothing")
        {
            auto const generation = loadedRuntime(
                host,
                directory,
                runtimeModelGranting(
                    R"({ id = "activate", kind = "move", proof_locator = "confirm-mark" })"
                )
            );
            auto const authority = dispatchAuthority(fence, generation);
            auto runtime = RuntimeContext{
                frame(
                    {std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}},
                    FrameId{72}
                ),
                1'000
            };
            REQUIRE(
                TaskHostTestAccess::run(
                    host,
                    generation,
                    runtime.context(),
                    authorizeActionSource(k_runtimeUiAction)
                ).has_value()
            );
            auto const delivered = TaskHostTestAccess::deliver(
                host,
                authority,
                TaskHostTestAccess::pendingReceipt(host, k_runtimeUiAction),
                runtime.context()
            );
            REQUIRE(delivered.has_value());
            CHECK(delivered->outcome() == DeliveryOutcome::Delivered);
            CHECK(runtime.actions().moves() == 1U);
            CHECK(runtime.actions().clicks() == 0U);
            CHECK(runtime.actions().holds() == 0U);
            REQUIRE(runtime.actions().lastMovePoint().has_value());
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): REQUIRE above proved engagement.
            CHECK(runtime.actions().lastMovePoint()->x() == doctest::Approx(1.0));
        }

        SUBCASE("a declared scroll posts its declared detents and no coordinate")
        {
            auto const generation = loadedRuntime(
                host,
                directory,
                keyRuntimeModelGranting(
                    R"({ id = "activate", kind = "scroll", notches = -3, proof_locator = "confirm-mark" })"
                )
            );
            auto const authority = dispatchAuthority(
                fence,
                generation,
                k_runtimeKeyAction.uiTarget
            );
            auto runtime = RuntimeContext{
                frame(
                    {std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}},
                    FrameId{73}
                ),
                1'000
            };
            REQUIRE(
                TaskHostTestAccess::run(
                    host,
                    generation,
                    runtime.context(),
                    authorizeActionSource(k_runtimeKeyAction)
                ).has_value()
            );
            auto const delivered = TaskHostTestAccess::deliver(
                host,
                authority,
                TaskHostTestAccess::pendingReceipt(host, k_runtimeKeyAction),
                runtime.context()
            );
            REQUIRE(delivered.has_value());
            CHECK(delivered->outcome() == DeliveryOutcome::Delivered);
            CHECK(runtime.actions().scrolls() == 1U);
            CHECK(runtime.actions().clicks() == 0U);
            CHECK(runtime.actions().keys() == 0U);

            // The sign travels. A chain that dropped it would scroll the wrong
            // way and every count above would stay green.
            CHECK(runtime.actions().lastNotches() == std::optional<int32>{-3});
        }
    }

    TEST_CASE("T-006 Runtime drag is one authorized delivery with declared offset and duration")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedDragRuntime(host, directory);
        auto const fence = controlFence(7);
        REQUIRE(TaskHostTestAccess::adoptControlFence(host, fence).has_value());
        auto const authority = dispatchAuthority(fence, generation);
        auto runtime = RuntimeContext{
            frame(
                {std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}},
                FrameId{61}
            ),
            1'000
        };
        auto const minted = TaskHostTestAccess::run(
            host,
            generation,
            runtime.context(),
            authorizeActionSource(k_runtimeUiAction)
        );
        REQUIRE(minted.has_value());
        CHECK(runtime.actions().drags() == 0U);

        auto const delivered = TaskHostTestAccess::deliver(
            host,
            authority,
            TaskHostTestAccess::pendingReceipt(host, k_runtimeUiAction),
            runtime.context()
        );
        REQUIRE(delivered.has_value());
        CHECK(delivered->outcome() == DeliveryOutcome::Delivered);
        CHECK_MESSAGE(
            runtime.actions().drags() == 1U,
            "T-006 atomic drag must reach the action sink as exactly one authorized delivery"
        );
        REQUIRE(runtime.actions().lastDragStart().has_value());
        REQUIRE(runtime.actions().lastDragEnd().has_value());
        REQUIRE(runtime.actions().lastDragTravel().has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): REQUIRE above proved engagement.
        CHECK(runtime.actions().lastDragStart()->x() == doctest::Approx(1.0));
        CHECK_MESSAGE(
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): REQUIRE above proved engagement.
            runtime.actions().lastDragEnd()->x() == doctest::Approx(2.0),
            "T-006 drag endpoint must equal its measured start plus declared offset"
        );
        CHECK(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                *runtime.actions().lastDragTravel()
            ).count()
            == 600
        );
    }

    // Every key-path error is TransportUnknown, for the reason the click path's
    // is: EngineSession::pressKey fails before the sink, at the sink, and after
    // the press has already gone down -- controller::keyPress reports that last
    // case as one Err, and the engine's own teardown putting the key back up
    // does not make the press unhappen. NotDelivered would claim an absence
    // this path cannot prove, and only NotDelivered unlocks a Rejected
    // disposition downstream.
    //
    // The case above is its positive control: the same model, the same Receipt
    // and the same authority deliver, so the refusal here comes from the sink
    // rather than from anything else in the chain.
    TEST_CASE("TaskHost::deliver reports a refused keystroke as TransportUnknown")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedKeyRuntime(host, directory);
        auto const fence = controlFence(7);
        REQUIRE(TaskHostTestAccess::adoptControlFence(host, fence).has_value());
        auto const authority = dispatchAuthority(
            fence,
            generation,
            k_runtimeKeyAction.uiTarget
        );
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{42}),
            1'000
        };
        auto const minted = TaskHostTestAccess::run(
            host,
            generation,
            runtime.context(),
            authorizeActionSource(k_runtimeKeyAction)
        );
        REQUIRE(minted.has_value());

        runtime.actions().refuseKeys();
        auto const report = TaskHostTestAccess::deliver(
            host,
            authority,
            TaskHostTestAccess::pendingReceipt(host, k_runtimeKeyAction),
            runtime.context()
        );
        REQUIRE(report.has_value());
        CHECK(report->outcome() == DeliveryOutcome::TransportUnknown);
        CHECK(
            report->reason().find("reached the engine and did not complete")
            != std::string_view::npos
        );
        CHECK(runtime.actions().keys() == 0U);
    }

    // A model naming a key nothing can press is refused where a model is
    // checked, rather than at the moment an action is taken. KeyName is the one
    // definition of which names exist, and "e" is outside it because the set is
    // spelled in uppercase and stays injective in both directions.
    TEST_CASE("TaskHost activation refuses a model naming an unresolvable key")
    {
        auto const directory = TemporaryDirectory{};
        auto host  = TaskHost{};
        auto model = keyRuntimeModel();

        auto const at = model.find(R"(key = "E")");
        REQUIRE(at != std::string::npos);
        model.replace(at, std::string_view{R"(key = "E")"}.size(), R"(key = "e")");

        auto const rootHash = publish(directory.path(), model, runtimeAssets());
        auto const refused = TaskHostTestAccess::activate(
            host,
            directory.path(),
            rootHash
        );
        REQUIRE_FALSE(refused.has_value());
        // The phrase is unique to KeyName's own refusal in the whole tree, so a
        // refusal from anywhere else in activation would not satisfy it.
        CHECK(
            std::string{refused.error().message()}
                .find("spelled in uppercase throughout")
            != std::string::npos
        );

        // The positive control: the same publication with the accepted spelling
        // activates, so the refusal above is the key name and not the model.
        auto const accepted = TemporaryDirectory{};
        auto acceptedHost = TaskHost{};
        CHECK(
            TaskHostTestAccess::activate(
                acceptedHost,
                accepted.path(),
                publish(accepted.path(), keyRuntimeModel(), runtimeAssets())
            ).has_value()
        );
    }

    TEST_CASE("TaskHost::deliver refuses an authority the adopted fence does not name")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedRuntime(host, directory);
        auto const fence = controlFence(7);
        REQUIRE(TaskHostTestAccess::adoptControlFence(host, fence).has_value());
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{26}),
            1'000
        };
        REQUIRE(
            TaskHostTestAccess::run(
                host,
                generation,
                runtime.context(),
                authorizeActionSource(k_runtimeUiAction)
            ).has_value()
        );
        auto const receipt = TaskHostTestAccess::pendingReceipt(host, k_runtimeUiAction);
        auto const authority = dispatchAuthority(fence, generation);

        // One forgery per checked field: a Host that compared only three of the
        // four would still refuse a value that broke all four at once.
        auto const forgeries = std::vector<DispatchAuthority>{
            dispatchAuthority(controlFenceOn("another-target", 7), generation),
            dispatchAuthority(
                ControlFence{
                    .controlledTargetId = fence.controlledTargetId,
                    .sessionEpoch       = fence.sessionEpoch + 1,
                    .fencingToken       = fence.fencingToken,
                },
                generation
            ),
            dispatchAuthority(controlFence(fence.fencingToken + 1), generation),
            dispatchAuthority(fence, GenerationId{generation.value() + 1}),
        };
        for (auto const& forged : forgeries)
        {
            auto const rejected = TaskHostTestAccess::deliver(
                host,
                forged,
                receipt,
                runtime.context()
            );
            CHECK_FALSE(rejected.has_value());
        }
        CHECK(runtime.actions().clicks() == 0U);

        // An Err has to mean the Receipt was never spent, or "refused" would be
        // indistinguishable from "consumed and lost".
        auto const delivered = TaskHostTestAccess::deliver(
            host,
            authority,
            receipt,
            runtime.context()
        );
        REQUIRE(delivered.has_value());
        CHECK(delivered->outcome() == DeliveryOutcome::Delivered);
        CHECK(runtime.actions().clicks() == 1U);
    }

    TEST_CASE("A Host with no adopted control fence can mint no Receipt")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedRuntime(host, directory);
        auto unfenced = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{27}),
            1'000
        };
        CHECK_FALSE(
            TaskHostTestAccess::run(
                host,
                generation,
                unfenced.context(),
                authorizeActionSource(k_runtimeUiAction)
            ).has_value()
        );
        CHECK(unfenced.actions().clicks() == 0U);

        // The positive control: the same artifact and the same chunk mint as
        // soon as a fence exists, so the refusal above is the fence and not the
        // world the chunk observed. It needs its own Host because a Runtime
        // generation caches its template handles against the context that
        // measured them, so a second context on one Host resolves nothing --
        // measured, and independent of anything W4 changed.
        auto const fencedDirectory = TemporaryDirectory{};
        auto fencedHost = TaskHost{};
        auto const fencedGeneration = loadedRuntime(fencedHost, fencedDirectory);
        REQUIRE(
            TaskHostTestAccess::adoptControlFence(
                fencedHost,
                controlFence(7)
            ).has_value()
        );
        auto fenced = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{28}),
            1'000
        };
        CHECK(
            TaskHostTestAccess::run(
                fencedHost,
                fencedGeneration,
                fenced.context(),
                authorizeActionSource(k_runtimeUiAction)
            ).has_value()
        );
    }

    TEST_CASE("TaskHost::adoptControlFence is strictly monotone and binds one target")
    {
        auto host = TaskHost{};
        CHECK_FALSE(
            TaskHostTestAccess::adoptControlFence(
                host,
                controlFenceOn("", 1)
            ).has_value()
        );
        CHECK(TaskHostTestAccess::fence(host).fencingToken == 0U);

        REQUIRE(
            TaskHostTestAccess::adoptControlFence(host, controlFence(7)).has_value()
        );
        CHECK(TaskHostTestAccess::fence(host).fencingToken == 7U);
        CHECK(
            TaskHostTestAccess::fence(host).controlledTargetId == k_controlledTarget
        );

        // At or below is refused, so a lease a takeover already superseded
        // cannot re-arm the Host it fenced out.
        CHECK_FALSE(
            TaskHostTestAccess::adoptControlFence(host, controlFence(7)).has_value()
        );
        CHECK_FALSE(
            TaskHostTestAccess::adoptControlFence(host, controlFence(6)).has_value()
        );
        CHECK(TaskHostTestAccess::fence(host).fencingToken == 7U);

        CHECK_FALSE(
            TaskHostTestAccess::adoptControlFence(
                host,
                controlFenceOn("another-target", 8)
            ).has_value()
        );
        CHECK(
            TaskHostTestAccess::fence(host).controlledTargetId == k_controlledTarget
        );

        REQUIRE(
            TaskHostTestAccess::adoptControlFence(host, controlFence(8)).has_value()
        );
        CHECK(TaskHostTestAccess::fence(host).fencingToken == 8U);
    }

    TEST_CASE("A takeover fence turns an outstanding Receipt into proof of absence")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedRuntime(host, directory);
        REQUIRE(
            TaskHostTestAccess::adoptControlFence(host, controlFence(7)).has_value()
        );
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{29}),
            1'000
        };
        REQUIRE(
            TaskHostTestAccess::run(
                host,
                generation,
                runtime.context(),
                authorizeActionSource(k_runtimeUiAction)
            ).has_value()
        );
        auto const receipt = TaskHostTestAccess::pendingReceipt(host, k_runtimeUiAction);

        auto const seized = controlFence(8);
        REQUIRE(TaskHostTestAccess::adoptControlFence(host, seized).has_value());

        // The Receipt is still consumed, and the report is what makes the
        // in-flight case recordable: an Err here would leave the ledger unable
        // to state that this dispatch posted nothing.
        auto const report = TaskHostTestAccess::deliver(
            host,
            dispatchAuthority(seized, generation),
            receipt,
            runtime.context()
        );
        REQUIRE(report.has_value());
        CHECK(report->outcome() == DeliveryOutcome::NotDelivered);
        CHECK_FALSE(report->reason().empty());
        CHECK(runtime.actions().clicks() == 0U);
        CHECK_FALSE(
            TaskHostTestAccess::deliver(
                host,
                dispatchAuthority(seized, generation),
                receipt,
                runtime.context()
            ).has_value()
        );
    }

    TEST_CASE("A refused click is transport unknown and never proof of absence")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedRuntime(host, directory);
        auto const fence = controlFence(7);
        REQUIRE(TaskHostTestAccess::adoptControlFence(host, fence).has_value());
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{30}),
            1'000
        };
        REQUIRE(
            TaskHostTestAccess::run(
                host,
                generation,
                runtime.context(),
                authorizeActionSource(k_runtimeUiAction)
            ).has_value()
        );

        runtime.actions().refuseClicks();
        auto const report = TaskHostTestAccess::deliver(
            host,
            dispatchAuthority(fence, generation),
            TaskHostTestAccess::pendingReceipt(host, k_runtimeUiAction),
            runtime.context()
        );
        REQUIRE(report.has_value());

        // Nothing reached the target here, and the Host still may not say so:
        // clickPoint returns one Err whether it failed before the sink, at it,
        // or after the click had already landed. Only NotDelivered proves
        // absence, so a click-path failure must never spell it.
        CHECK(report->outcome() == DeliveryOutcome::TransportUnknown);
        CHECK_FALSE(report->reason().empty());
        CHECK(runtime.actions().clicks() == 0U);
    }

    // U-07, re-anchored: the boundary between a project's own directory and a
    // deployable artifact is the SEAL, not a phase.
    //
    // What this case no longer asserts, because
    // docs/decisions/2026-08-24-there-is-no-annotation-phase.md deleted it: that
    // a Host generation has a kind, that a production one cannot start an
    // exploration front end, and that a project-directory one cannot read its
    // own model. All three were the compile-time authoring/production split,
    // which is a limit the framework invented rather than one it was given.
    //
    // What is left is real and is here: a hash nothing sealed cannot become a
    // RuntimeModelBinding, and the refusal names the hash and the record that
    // is missing.
    TEST_CASE("contract-runtime-u07")
    {
        auto const sealedDirectory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const sealedGeneration = loadedRuntime(host, sealedDirectory);

        // A project directory, with the same model in the place a project's own
        // declaration names. Nothing has sealed these bytes: the hash is
        // derived from what is on disk at this instant.
        auto const projectDirectory = TemporaryDirectory{};
        write(projectDirectory.path() / "annotation-screenshot.png", "authoring pixels");
        auto const unsealedArtifact =
            projectDirectory.path() / "runtime" / "artifact";
        auto const unsealedHash =
            publish(unsealedArtifact, runtimeModel(), runtimeAssets());
        auto const unsealed = host.openUnsealedProject(
            projectDirectory.path(),
            unsealedArtifact
        );
        REQUIRE(unsealed.has_value());

        // Both read the model they carry. There is no phase gate on bytes: a
        // session editing a model reads it exactly as a session running one
        // does, and what it may DO with it is its Tool closure's answer.
        auto const sealedBytes = host.runtimeModelBytes(sealedGeneration);
        REQUIRE(sealedBytes.has_value());
        auto const unsealedBytes = host.runtimeModelBytes(*unsealed);
        REQUIRE(unsealedBytes.has_value());
        CHECK(*sealedBytes == *unsealedBytes);

        // The seal, and the only difference between the two. Removing the
        // sealedAt() test in TaskHost::runtimeModelBinding reds this.
        auto const bound = host.runtimeModelBinding(sealedGeneration);
        REQUIRE(bound.has_value());

        auto const refused = host.runtimeModelBinding(*unsealed);
        REQUIRE_FALSE(refused.has_value());
        auto const message = std::string{refused.error().message()};
        INFO("refusal: ", message);
        CHECK(message.contains(unsealedHash.hex()));
        CHECK(message.contains("no closing record"));
        CHECK(
            automationErrorKind(refused.error())
            == std::optional<AutomationErrorKind>{
                AutomationErrorKind::UnsupportedCapability
            }
        );

        // The other half of the seal, at the door a binding is INSTALLED
        // through rather than read through. The trusted parser's own seam is
        // reachable here on purpose (see TaskHostTestAccess), so this is not a
        // check nothing can fail: a generation whose artifact declares no asset
        // satisfies every closure test finalize makes, and the seal is the one
        // thing left that refuses it. Removing the sealedAt() test in
        // TaskHost::Generation::installBinding reds this.
        auto const genesisDirectory = TemporaryDirectory{};
        auto const genesisArtifact =
            genesisDirectory.path() / "runtime" / "artifact";
        auto const genesisHash = publish(
            genesisArtifact,
            std::string{k_genesisRuntimeModelToml},
            {}
        );
        auto const genesis = host.openUnsealedProject(
            genesisDirectory.path(),
            genesisArtifact
        );
        REQUIRE(genesis.has_value());
        auto const finalized = TaskHostTestAccess::finalizeWithParserFormat(
            host,
            *genesis,
            k_runtimeModelFormat
        );
        REQUIRE_FALSE(finalized.has_value());
        auto const installRefusal = std::string{finalized.error().message()};
        INFO("install refusal: ", installRefusal);
        CHECK(installRefusal.contains(genesisHash.hex()));
        CHECK(installRefusal.contains("no closing record"));

        // And an exploration front end opens over the unsealed generation, on
        // the ordinary path and with no kind consulted. A second one over the
        // same generation is refused, because two front ends cannot drive one
        // generation's ledger and trace at once.
        auto const issued = std::make_shared<std::vector<std::string>>();
        auto session = startExploration(
            host,
            *unsealed,
            frame({std::byte{0}, std::byte{0}, std::byte{0}}, FrameId{17}),
            projectDirectory.path(),
            projectDirectory.path() / "authoring-trace.jsonl",
            issued
        );
        REQUIRE(session.has_value());
        CHECK_FALSE(
            startExploration(
                host,
                *unsealed,
                frame({std::byte{0}, std::byte{0}, std::byte{0}}, FrameId{18}),
                projectDirectory.path(),
                projectDirectory.path() / "second-trace.jsonl"
            ).has_value()
        );

        // EVERY VERB AN EXPLORATION CHUNK WRITES IS A TOOL CALL, including the
        // measurement over an explicit screenshot digest. The Host reaches no
        // engine on their behalf and holds no private verb they could reach it
        // through: what a chunk does is decided where the call is admitted, by
        // the Operator that admitted this session
        // (docs/decisions/2026-08-24-there-is-no-annotation-phase.md).
        auto authoringSurface = (*session)->evaluate(
            R"lua(
                local screen = require("@umbraflow/screen")
                local tools = require("@umbraflow/tools")
                local hash = string.rep("0", 64)
                screen.observe(hash)
                tools.call("framework.screen.read_lines", {
                    screenshot_sha256 = hash,
                    x = 0,
                    y = 0,
                    width = 1,
                    height = 1,
                })
                tools.call("framework.input.click", {
                    screenshot_sha256 = hash,
                    x = 0,
                    y = 0,
                })
                return "issued"
            )lua",
            "authoring-boundary"
        );
        REQUIRE(authoringSurface.has_value());
        REQUIRE(authoringSurface->text() != nullptr);
        CHECK(*authoringSurface->text() == std::string{"issued"});
        CHECK(
            *issued
            == std::vector<std::string>{
                "framework.screen.observe",
                "framework.screen.read_lines",
                "framework.input.click",
            }
        );
    }

    TEST_CASE("contract-runtime-u08")
    {
        auto const directory = TemporaryDirectory{};
        auto const rootHash = publish(directory.path(), runtimeModel(), runtimeAssets());
        write(directory.path() / "annotation-screenshot.png", "pixels must stay offline");

        CHECK_FALSE(loadRuntimeArtifact(directory.path(), rootHash).has_value());
        auto host = TaskHost{};
        CHECK_FALSE(TaskHostTestAccess::activate(
            host,
            directory.path(),
            rootHash
        ).has_value());
    }

    // resolve_state stamps every resolved state with an id drawn from a
    // module-level counter, and the trusted VM outlives one observation, so two
    // readings of ONE unchanged screen carry two different ids. Comparing the
    // two canonical documents is therefore what proves the id is outside them:
    // a single observation's bytes could not tell the difference.
    TEST_CASE("TaskHost::observe keeps the resolver's counter out of the document")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const rootHash = publish(
            directory.path(),
            runtimeModel(),
            runtimeAssets()
        );
        auto const generation = TaskHostTestAccess::activate(
            host,
            directory.path(),
            rootHash
        );
        REQUIRE(generation.has_value());
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{21}),
            1'000
        };

        // One friend, and it is the Host. A caller holding every field still
        // cannot assemble a snapshot, so there is no second producer to
        // disagree with the one the resolver fed.
        CHECK_FALSE(
            (std::is_constructible_v<
                UiObservationSnapshot,
                std::string,
                GenerationId,
                TargetGeneration,
                ContentHash,
                ContentHash,
                std::string
            >)
        );
        CHECK(std::is_copy_constructible_v<UiObservationSnapshot>);

        auto const first = observeOnce(host, *generation, runtime.context());
        REQUIRE(first.has_value());
        auto const second = observeOnce(host, *generation, runtime.context());
        REQUIRE(second.has_value());

        CHECK(first->canonicalJcs() == second->canonicalJcs());
        CHECK(first->stateResolutionHash() == second->stateResolutionHash());
        CHECK(first->observationId() != second->observationId());

        // readings is present and empty because this model declares no Readout.
        // The one resolved action is named with all three identifiers and its
        // kind; semantic Tools later spend this exact observation authority.
        CHECK(
            first->canonicalJcs()
            == R"({"kind":"resolved_state","ordered_surface_stack":["screen"],)"
               R"("readings":[],"ui_actions":[{"action":"activate",)"
               R"("binding":"confirm.primary","kind":"click",)"
               R"("ui_target":"confirm"}]})"
        );
        CHECK(first->stateResolutionHash() == hash(first->canonicalJcs()));
        CHECK(first->generation() == *generation);
        CHECK(first->targetGeneration() == TargetGeneration::fromValue(3));
        CHECK(first->artifactRootHash() == rootHash);
        CHECK(first->semanticHash() != rootHash);
        CHECK(first->semanticHash() != hash(runtimeModel()));

        // The chunk leaves its cycle open for the Host to read the capture off;
        // the Host closing it again is what lets a second observation open one.
        CHECK_FALSE(runtime.context().hasOpenCycle());
    }

    TEST_CASE("TaskHost::observe canonicalizes an unresolved state and its reason")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedRuntime(host, directory);
        auto runtime = RuntimeContext{
            frame({std::byte{0}, std::byte{k_actionGray}, std::byte{0}}, FrameId{22}),
            1'000
        };
        auto const unresolved = observeOnce(host, generation, runtime.context());
        REQUIRE(unresolved.has_value());
        CHECK(
            unresolved->canonicalJcs()
            == R"({"diagnostic":"visible_content_matched_nothing",)"
               R"("kind":"unknown_state","reason":"no_scene_candidate"})"
        );
        CHECK(unresolved->stateResolutionHash() == hash(unresolved->canonicalJcs()));

        auto const resolvedDirectory = TemporaryDirectory{};
        auto resolvedHost = TaskHost{};
        auto const resolvedGeneration = loadedRuntime(resolvedHost, resolvedDirectory);
        auto resolvedRuntime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{23}),
            1'000
        };
        auto const resolved = observeOnce(resolvedHost, resolvedGeneration,
            resolvedRuntime.context()
        );
        REQUIRE(resolved.has_value());

        // Two worlds, two documents, two digests: the hash follows the bytes
        // rather than the occasion that produced them.
        CHECK(unresolved->canonicalJcs() != resolved->canonicalJcs());
        CHECK(unresolved->stateResolutionHash() != resolved->stateResolutionHash());
    }

    TEST_CASE("TaskHost::observe canonicalizes an ambiguous state through its conflict")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const rootHash = publish(
            directory.path(),
            twoSceneRuntimeModel(),
            runtimeAssets()
        );
        auto const generation = TaskHostTestAccess::activate(
            host,
            directory.path(),
            rootHash
        );
        REQUIRE(generation.has_value());
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{24}),
            1'000
        };
        auto const ambiguous = observeOnce(host, *generation, runtime.context());
        REQUIRE(ambiguous.has_value());
        CHECK(
            ambiguous->canonicalJcs()
            == R"({"kind":"ambiguous_state","reason":"multiple_scenes"})"
        );
    }

    // The whole reading path, from the trusted OCR boundary to the bytes a
    // plugin is handed. Nothing below asserts against a string this file also
    // produced: the text comes out of the scripted engine, the normalization
    // out of the RuntimeModel, and the document out of the resolver.
    TEST_CASE("TaskHost::observe reports what a Readout on the resolved stack read")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const rootHash = publish(
            directory.path(),
            readingRuntimeModel(),
            runtimeAssets()
        );
        auto const generation = TaskHostTestAccess::activate(
            host,
            directory.path(),
            rootHash
        );
        REQUIRE(generation.has_value());

        auto reader = std::make_unique<ScriptedReader>("  Wandering   Merchant \n", 9'100);
        auto* const p_reader = reader.get();
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{31}),
            1'000,
            std::move(reader)
        };
        auto const observed = observeOnce(host, *generation, runtime.context());
        REQUIRE(observed.has_value());
        auto const& canonical = observed->canonicalJcs();

        CHECK_MESSAGE(
            canonical.find(R"("kind":"read","lines":[)") != std::string::npos,
            "Read outcomes must carry a lines list rather than top-level text"
        );
        CHECK_MESSAGE(
            canonical.find(R"("rect":[1,0,1,1])") != std::string::npos,
            "SingleLine readings must use the declared Readout rectangle"
        );

        // The reading is named by the Readout's own id, carries one line of the
        // NORMALISED text that Readout declared collapse_whitespace for with the
        // rectangle that line was read in, and is inside the document whose
        // sha256 is the state resolution hash.
        CHECK(
            observed->canonicalJcs()
            == R"({"kind":"resolved_state","ordered_surface_stack":["screen"],)"
               R"("readings":[{"id":"title","kind":"read",)"
               R"("lines":[{"rect":[1,0,1,1],"text":"Wandering Merchant"}]}]})"
        );
        CHECK(observed->stateResolutionHash() == hash(observed->canonicalJcs()));

        // The engine was pointed at the Readout's own rectangle and not at the
        // Surface, so a document that named the right Readout over the wrong
        // pixels would fail here rather than read as a correct answer.
        REQUIRE(p_reader->calls() == 1U);
        CHECK(p_reader->lastRect().x() == 1);
        CHECK(p_reader->lastRect().y() == 0);
        CHECK(p_reader->lastRect().width() == 1);
        CHECK(p_reader->lastRect().height() == 1);

        // Nothing the Reader knew beyond the text and that rectangle travels:
        // not the score that cleared the floor, not the variant that matched,
        // and not the pre-normalisation string.
        CHECK(observed->canonicalJcs().find("confidence") == std::string::npos);
        CHECK(observed->canonicalJcs().find("0.91") == std::string::npos);
        CHECK(observed->canonicalJcs().find("primary") == std::string::npos);
        CHECK(observed->canonicalJcs().find("title.primary") == std::string::npos);
        CHECK(observed->canonicalJcs().find("Wandering   Merchant") == std::string::npos);

        // Two captures of one unchanged screen are one decision. A score or a
        // capture identity inside the reading would break this even though the
        // world did not move.
        auto const again = observeOnce(host, *generation, runtime.context());
        REQUIRE(again.has_value());
        CHECK(again->canonicalJcs() == observed->canonicalJcs());
        CHECK(again->stateResolutionHash() == observed->stateResolutionHash());
    }

    // What a read's layout buys, end to end. A Readout declaring block is read
    // under Block, and every line the detector found travels with the rectangle
    // it found it at -- which is the capability a single_line read cannot
    // express, because it asserts there is only ever one.
    TEST_CASE("TaskHost::observe reports every line a block Readout read")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const rootHash = publish(
            directory.path(),
            readingRuntimeModel("block"),
            runtimeAssets()
        );
        auto const generation = TaskHostTestAccess::activate(
            host,
            directory.path(),
            rootHash
        );
        REQUIRE(generation.has_value());

        auto reader = std::make_unique<ScriptedBlockReader>(
            std::vector<ocr::TextLine>{
                ocr::TextLine{
                    .text   = "The road is blocked",
                    .bounds = pixelRect(0, 0, 1, 1),
                    .confidenceBp = 9'100,
                },
                ocr::TextLine{
                    .text   = "by a stranger",
                    .bounds = pixelRect(1, 0, 1, 1),
                    .confidenceBp = 9'200,
                },
                ocr::TextLine{
                    .text   = "who will not move",
                    .bounds = pixelRect(2, 0, 1, 1),
                    .confidenceBp = 9'300,
                },
            }
        );
        auto* const p_reader = reader.get();
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{51}),
            1'000,
            std::move(reader)
        };
        auto const observed = observeOnce(host, *generation, runtime.context());
        REQUIRE(observed.has_value());

        // The model's declared layout is what the Host ran, not a default the
        // engine chose: a Readout saying block and a Host reading one line would
        // report the first line of three and call the region read.
        CHECK(p_reader->lastLayout() == ocr::TextLayout::Block);
        CHECK(
            observed->canonicalJcs()
            == R"({"kind":"resolved_state","ordered_surface_stack":["screen"],)"
               R"("readings":[{"id":"title","kind":"read","lines":[)"
               R"({"rect":[0,0,1,1],"text":"The road is blocked"},)"
               R"({"rect":[1,0,1,1],"text":"by a stranger"},)"
               R"({"rect":[2,0,1,1],"text":"who will not move"}]}]})"
        );

        // Each line's own rectangle, and not the Readout's: a reading that
        // copied the question would put [1,0,1,1] on all three.
        CHECK(observed->stateResolutionHash() == hash(observed->canonicalJcs()));
    }

    // T-002 / T07-equivalent. One declaration is resolved against one, two and
    // three detected items. The engine returns right-to-left order for the
    // multi-item cases, so the exact result also proves that adapter enumeration
    // order is not item identity. A final unchanged three-item capture returns
    // left-to-right and must produce the same indices and rectangles.
    TEST_CASE("A detected collection reports completeness stable slot index and exact rectangle")
    {
        auto const resolve = [](std::vector<ocr::TextLine> lines, FrameId frameId)
        {
            auto const directory = TemporaryDirectory{};
            auto host = TaskHost{};
            auto const rootHash = publish(
                directory.path(),
                collectionRuntimeModel(),
                runtimeAssets()
            );
            auto const generation = TaskHostTestAccess::activate(
                host,
                directory.path(),
                rootHash
            );
            REQUIRE(generation.has_value());
            auto runtime = RuntimeContext{
                frame(
                    {
                        std::byte{k_anchorGray},
                        std::byte{k_actionGray},
                        std::byte{0},
                        std::byte{0},
                        std::byte{0},
                    },
                    frameId
                ),
                1'000,
                std::make_unique<ScriptedBlockReader>(std::move(lines))
            };
            return runText(
                host,
                *generation,
                runtime,
                R"lua(
                    local cycle = observe.open(project.load_project())
                    local state = cycle:resolve_state()
                    local collection, reason = cycle:resolve_collection(state, "options")
                    cycle:close()
                    if collection == nil then error(reason) end
                    return jcs.encode(collection)
                )lua"
            );
        };

        auto const one = resolve(
            {
                ocr::TextLine{
                    .text   = "only",
                    .bounds = pixelRect(2, 0, 1, 1),
                    .confidenceBp = 9'100,
                },
            },
            FrameId{45}
        );
        CHECK_MESSAGE(
            one
                == R"({"completeness":"complete","count":1,"items":[{"index":0,"readings":[],"rect":[2,0,1,1]}]})",
            "T-002 one-item Host result must report count, index and exact rectangle"
        );

        auto const two = resolve(
            {
                ocr::TextLine{
                    .text   = "right",
                    .bounds = pixelRect(2, 0, 1, 1),
                    .confidenceBp = 9'100,
                },
                ocr::TextLine{
                    .text   = "left",
                    .bounds = pixelRect(0, 0, 1, 1),
                    .confidenceBp = 9'100,
                },
            },
            FrameId{46}
        );
        CHECK_MESSAGE(
            two
                == R"({"completeness":"partial","count":2,"items":[{"index":0,"readings":[],"rect":[0,0,1,1]},{"index":1,"readings":[],"rect":[2,0,1,1]}]})",
            "collection completeness must report partial when a layout slot may be missing"
        );

        auto const expectedThree = std::string{
            R"({"completeness":"complete","count":3,"items":[{"index":0,"readings":[],"rect":[0,0,1,1]},{"index":1,"readings":[],"rect":[2,0,1,1]},{"index":2,"readings":[],"rect":[4,0,1,1]}]})"
        };
        auto const threeReversed = resolve(
            {
                ocr::TextLine{
                    .text   = "right",
                    .bounds = pixelRect(4, 0, 1, 1),
                    .confidenceBp = 9'100,
                },
                ocr::TextLine{
                    .text   = "middle",
                    .bounds = pixelRect(2, 0, 1, 1),
                    .confidenceBp = 9'100,
                },
                ocr::TextLine{
                    .text   = "left",
                    .bounds = pixelRect(0, 0, 1, 1),
                    .confidenceBp = 9'100,
                },
            },
            FrameId{47}
        );
        CHECK_MESSAGE(
            threeReversed == expectedThree,
            "collection completeness must report complete for all slots in any detector order"
        );

        auto const threeForward = resolve(
            {
                ocr::TextLine{
                    .text   = "left",
                    .bounds = pixelRect(0, 0, 1, 1),
                    .confidenceBp = 9'100,
                },
                ocr::TextLine{
                    .text   = "middle",
                    .bounds = pixelRect(2, 0, 1, 1),
                    .confidenceBp = 9'100,
                },
                ocr::TextLine{
                    .text   = "right",
                    .bounds = pixelRect(4, 0, 1, 1),
                    .confidenceBp = 9'100,
                },
            },
            FrameId{48}
        );
        CHECK_MESSAGE(
            threeForward == expectedThree,
            "collection completeness must report complete when every layout slot is measured"
        );

        auto const unknown = resolve(
            {
                ocr::TextLine{
                    .text   = "first",
                    .bounds = pixelRect(1, 0, 1, 1),
                    .confidenceBp = 9'100,
                },
                ocr::TextLine{
                    .text   = "second",
                    .bounds = pixelRect(2, 0, 1, 1),
                    .confidenceBp = 9'100,
                },
            },
            FrameId{49}
        );
        CHECK_MESSAGE(
            unknown == R"({"completeness":"unknown","count":2})",
            "collection completeness must report unknown without items when no layout fits"
        );
    }

    TEST_CASE("TaskHost::observe floors a block reading as one set of lines")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const rootHash = publish(
            directory.path(),
            readingRuntimeModel("block"),
            runtimeAssets()
        );
        auto const generation = TaskHostTestAccess::activate(
            host,
            directory.path(),
            rootHash
        );
        REQUIRE(generation.has_value());

        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{52}),
            1'000,
            std::make_unique<ScriptedBlockReader>(
                std::vector<ocr::TextLine>{
                    ocr::TextLine{
                        .text   = "high first",
                        .bounds = pixelRect(0, 0, 1, 1),
                        .confidenceBp = 9'100,
                    },
                    ocr::TextLine{
                        .text   = "low middle",
                        .bounds = pixelRect(1, 0, 1, 1),
                        .confidenceBp = 1'000,
                    },
                    ocr::TextLine{
                        .text   = "high last",
                        .bounds = pixelRect(2, 0, 1, 1),
                        .confidenceBp = 9'300,
                    },
                }
            )
        };
        auto const observed = observeOnce(host, *generation, runtime.context());
        REQUIRE(observed.has_value());
        auto const& canonical = observed->canonicalJcs();

        CHECK_MESSAGE(
            canonical.find(R"("reason":"low_confidence")") != std::string::npos,
            "A block reading must become low_confidence when any line is below confidence_floor"
        );
        CHECK_MESSAGE(
            canonical.find(R"("kind":"read")") == std::string::npos,
            "A below-floor block line must not be dropped to salvage a partial read"
        );
        CHECK(
            canonical
            == R"({"kind":"resolved_state","ordered_surface_stack":["screen"],)"
               R"("readings":[{"id":"title","kind":"unknown",)"
               R"("reason":"low_confidence"}]})"
        );
    }

    TEST_CASE("TaskHost::observe memoises one rectangle separately per layout")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const rootHash = publish(
            directory.path(),
            mixedLayoutReadingRuntimeModel(),
            runtimeAssets()
        );
        auto const generation = TaskHostTestAccess::activate(
            host,
            directory.path(),
            rootHash
        );
        REQUIRE(generation.has_value());

        auto reader = std::make_unique<LayoutSensitiveReader>();
        auto* const p_reader = reader.get();
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{53}),
            1'000,
            std::move(reader)
        };
        auto const observed = observeOnce(host, *generation, runtime.context());
        REQUIRE(observed.has_value());

        CHECK_MESSAGE(
            p_reader->calls() == 2U,
            "Read memoisation must distinguish the same rectangle by layout"
        );
        CHECK(observed->canonicalJcs().find("single answer") != std::string::npos);
        CHECK(observed->canonicalJcs().find("block first") != std::string::npos);
        CHECK(observed->canonicalJcs().find("block second") != std::string::npos);
    }

    // The falsifier for the decision basis. state_resolution_hash is one member
    // of DecisionBasis and it is a digest over this whole document, so a reading
    // that changed while the document's hash did not would be a plugin input
    // outside everything a replay is checked against.
    TEST_CASE("TaskHost::observe moves the state resolution hash when a reading moves")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const rootHash = publish(
            directory.path(),
            readingRuntimeModel(),
            runtimeAssets()
        );
        auto const generation = TaskHostTestAccess::activate(
            host,
            directory.path(),
            rootHash
        );
        REQUIRE(generation.has_value());

        auto const pixels = std::vector<std::byte>{
            std::byte{k_anchorGray},
            std::byte{k_actionGray},
            std::byte{0},
        };
        auto firstRuntime = RuntimeContext{
            frame(pixels, FrameId{32}),
            1'000,
            std::make_unique<ScriptedReader>("Wandering Merchant", 9'100)
        };
        auto const first = observeOnce(host, *generation, firstRuntime.context());
        REQUIRE(first.has_value());

        // A second Host, because observe.luau caches template handles per
        // RuntimeModel and a second TaskContext on one generation resolves
        // nothing -- the reason host-delivery-fixture.hpp already states.
        auto const secondDirectory = TemporaryDirectory{};
        auto secondHost = TaskHost{};
        auto const secondRoot = publish(
            secondDirectory.path(),
            readingRuntimeModel(),
            runtimeAssets()
        );
        auto const secondGeneration = TaskHostTestAccess::activate(
            secondHost,
            secondDirectory.path(),
            secondRoot
        );
        REQUIRE(secondGeneration.has_value());
        auto secondRuntime = RuntimeContext{
            frame(pixels, FrameId{33}),
            1'000,
            std::make_unique<ScriptedReader>("Abandoned Shrine", 9'100)
        };
        auto const second = observeOnce(secondHost, *secondGeneration,
            secondRuntime.context()
        );
        REQUIRE(second.has_value());

        // Identical pixels, identical model, identical surface stack: the ONLY
        // difference between the two worlds is what the Reader read.
        CHECK(first->canonicalJcs() != second->canonicalJcs());
        CHECK(first->stateResolutionHash() != second->stateResolutionHash());
        CHECK(second->canonicalJcs().find("Abandoned Shrine") != std::string::npos);
        CHECK(
            second->canonicalJcs().find(R"("ordered_surface_stack":["screen"])")
            != std::string::npos
        );
    }

    // T-009's direct falsifier. A detector is positive evidence that a visual
    // element is present, and text_equals asserts what text a rectangle holds:
    // evidence about a Surface, never a Surface's identity. A Binding detector
    // naming text_equals must therefore fail to parse and refuse the whole
    // artifact. The acceptance half -- the same predicate as a Collection's
    // filter, acting on members a locator has already found -- is exercised by
    // test-runtime-model-v4.luau, whose valid model carries
    // predicate = { kind = "text_equals", ... } on a Collection and compiles.
    TEST_CASE("T-009 a Binding detector naming text_equals cannot activate")
    {
        auto constexpr textPredicate =
            R"({ kind = "text_equals", read = "title", value = "Settings" })";
        auto constexpr locatorPredicate =
            R"({ kind = "locator_present", locator = "title.mark" })";
        auto const titleAsset = std::vector<ArtifactFile>{
            ArtifactFile{.path = "assets/title.png", .bytes = templatePng(k_anchorGray)},
        };

        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const rootHash = publish(
            directory.path(),
            textIdentityRuntimeModel(textPredicate),
            titleAsset
        );
        auto const generation = TaskHostTestAccess::activate(
            host,
            directory.path(),
            rootHash
        );
        REQUIRE_FALSE(generation.has_value());

        // The control shares the model's body, asset and manifest: only the
        // detector line differs, and it must activate. If this stopped
        // activating the refusal above would be about the world rather than
        // about the predicate kind, and the falsifier would be green by
        // accident.
        auto const controlDirectory = TemporaryDirectory{};
        auto controlHost = TaskHost{};
        auto const controlRoot = publish(
            controlDirectory.path(),
            textIdentityRuntimeModel(locatorPredicate),
            titleAsset
        );
        auto const control = TaskHostTestAccess::activate(
            controlHost,
            controlDirectory.path(),
            controlRoot
        );
        REQUIRE(control.has_value());
    }

    // T-004 / T05. Three read boundary cases through the real EngineSession
    // boundary, on one model whose Surface identity is locator-only: absent, a
    // read carrying the raw text, and unknown carrying low_confidence. The
    // Surface resolves in every case -- text never decides identity -- and the
    // three outcomes reach the plugin distinguishable, each case's assertions
    // going red if its outcome is reported as either of the other two.
    TEST_CASE("T-004 T05 read outcomes stay distinguishable under a resolved Surface")
    {
        struct Case final
        {
            std::string_view name{};
            std::string_view text{};
            uint32           confidenceBp{};
            bool             silent{};
            std::string_view expectedReading{};
        };

        auto constexpr cases = std::array{
            Case{
                .name   = "absent",
                .silent = true,
                .expectedReading = R"({"id":"title","kind":"absent"})",
            },
            Case{
                .name = "read",
                .text = "Settings",
                .confidenceBp  = 9'000,
                .expectedReading =
                    R"({"id":"title","kind":"read",)"
                    R"("lines":[{"rect":[1,0,1,1],"text":"Settings"}]})",
            },
            Case{
                .name = "unknown",
                .text = "Settings",
                .confidenceBp  = 1'000,
                .expectedReading =
                    R"({"id":"title","kind":"unknown",)"
                    R"("reason":"low_confidence"})",
            },
        };

        for (auto const& testCase : cases)
        {
            CAPTURE(testCase.name);
            auto const directory = TemporaryDirectory{};
            auto host = TaskHost{};
            auto const rootHash = publish(
                directory.path(),
                readingRuntimeModel(),
                runtimeAssets()
            );
            auto const generation = TaskHostTestAccess::activate(
                host,
                directory.path(),
                rootHash
            );
            REQUIRE(generation.has_value());

            // The concrete type stays reachable across the type-erasing move so
            // the engine's call counter can be asserted through it, as the
            // dedicated Reader tests in this file do.
            auto reader = std::unique_ptr<ocr::IOcrEngine>{};
            SilentReader* p_silent = nullptr;
            ScriptedReader* p_scripted = nullptr;
            if (testCase.silent)
            {
                auto concrete = std::make_unique<SilentReader>();
                p_silent      = concrete.get();
                reader        = std::move(concrete);
            }
            else
            {
                auto concrete = std::make_unique<ScriptedReader>(
                    std::string{testCase.text},
                    testCase.confidenceBp
                );
                p_scripted = concrete.get();
                reader     = std::move(concrete);
            }
            auto runtime = RuntimeContext{
                frame(
                    {std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}},
                    FrameId{55}
                ),
                1'000,
                std::move(reader)
            };
            auto const observed = observeOnce(host, *generation, runtime.context());
            REQUIRE(observed.has_value());

            // The Surface resolves whatever the read reported: the identity is
            // the locator's judgement, so the reading can move without the
            // decision basis' first member moving with it.
            auto const expected = std::string{
                R"({"kind":"resolved_state","ordered_surface_stack":["screen"],)"
                R"("readings":[)"}
                + std::string{testCase.expectedReading} + "]}";
            CHECK_MESSAGE(
                observed->canonicalJcs() == expected,
                "T-004 T05 each read outcome must reach the plugin as itself"
            );
            CHECK(observed->stateResolutionHash() == hash(observed->canonicalJcs()));

            // The outcome is what crossed the Host boundary, not a default the
            // Host painted in.
            REQUIRE(
                (testCase.silent ? p_silent->calls() : p_scripted->calls()) == 1U
            );

            if (testCase.silent)
            {
                // Absent is not unknown: no reason attaches to a rectangle that
                // was read and held nothing. A mutation merging absence into
                // the unknown path would carry low_confidence here and go red.
                CHECK(observed->canonicalJcs().find(R"("reason")") == std::string::npos);
            }
            else if (testCase.confidenceBp >= 5'000)
            {
                // A read travels with the raw text: the engine's answer, not a
                // summary of it. A mutation withholding the text -- or
                // normalizing it past recognition -- goes red.
                CHECK(observed->canonicalJcs().find("Settings") != std::string::npos);
            }
            else
            {
                // Below the floor is the declaration's judgement, not a text: the
                // reason travels and the rejected text -- and the score that
                // rejected it -- stay behind. A mutation reporting the
                // below-floor answer as a read, or carrying the score, goes red.
                CHECK(
                    observed->canonicalJcs().find(R"("reason":"low_confidence")")
                    != std::string::npos
                );
                CHECK(observed->canonicalJcs().find("Settings") == std::string::npos);
                CHECK(observed->canonicalJcs().find(R"("confidence")") == std::string::npos);
            }
        }
    }

    // Below the floor is not a text, and it is not silence either. The plugin is
    // told the read could not decide and why, out of a closed vocabulary, so
    // it can tell that case apart from a Reader that found nothing and need not
    // fail closed on one blurry capture. What still never travels is the score:
    // the floor is the trusted Reader's judgement, a caller handed the rejected
    // text could re-take it, and a float in a hashed document would make two
    // captures of one unchanged screen two decisions.
    TEST_CASE("TaskHost::observe reports an unreadable reading with its reason")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const rootHash = publish(
            directory.path(),
            readingRuntimeModel(),
            runtimeAssets()
        );
        auto const generation = TaskHostTestAccess::activate(
            host,
            directory.path(),
            rootHash
        );
        REQUIRE(generation.has_value());

        // The model floors the `title` Readout at 0.5 and this clears 0.1, so the
        // text is one the engine itself produced and the floor is what refuses it.
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{34}),
            1'000,
            std::make_unique<ScriptedReader>("Wandering Merchant", 1'000)
        };
        auto const observed = observeOnce(host, *generation, runtime.context());
        REQUIRE(observed.has_value());
        CHECK(
            observed->canonicalJcs()
            == R"({"kind":"resolved_state","ordered_surface_stack":["screen"],)"
               R"("readings":[{"id":"title","kind":"unknown",)"
               R"("reason":"low_confidence"}]})"
        );

        // The reason is the whole of what the failure adds, so it is asserted on
        // its own as well: an equality that stopped carrying it would otherwise
        // be repaired by rewriting the expected document, and this line names
        // the property instead of the bytes.
        CHECK(
            observed->canonicalJcs().find(R"("reason":"low_confidence")")
            != std::string::npos
        );

        // The rejected text and the score that rejected it stay behind. `0.1`
        // is what the ScriptedReader reported; a document carrying either would
        // hand the caller the judgement the floor already made.
        CHECK(observed->canonicalJcs().find("Wandering") == std::string::npos);
        CHECK(observed->canonicalJcs().find(R"("lines")") == std::string::npos);
        CHECK(observed->canonicalJcs().find(R"("confidence")") == std::string::npos);
        CHECK(observed->canonicalJcs().find("0.1") == std::string::npos);

        // Two captures of one unchanged screen stay one decision even when the
        // reading failed, which is the property a score inside the reason would
        // have broken.
        auto const again = observeOnce(host, *generation, runtime.context());
        REQUIRE(again.has_value());
        CHECK(again->stateResolutionHash() == observed->stateResolutionHash());
    }

    // A read that found no text at all is a third answer and says so. Without
    // this the reason above could be read as "any failure is unknown", and the
    // distinction the whole shape exists for -- nothing is written here, versus
    // this could not be read -- would be untested.
    TEST_CASE("TaskHost::observe reports an empty reading as absent")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const rootHash = publish(
            directory.path(),
            readingRuntimeModel(),
            runtimeAssets()
        );
        auto const generation = TaskHostTestAccess::activate(
            host,
            directory.path(),
            rootHash
        );
        REQUIRE(generation.has_value());

        // The rectangle was read and held nothing, which no confidence floor is
        // consulted about: there is no score because there is no line.
        auto reader = std::make_unique<SilentReader>();
        auto* const p_reader = reader.get();
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{36}),
            1'000,
            std::move(reader)
        };
        auto const observed = observeOnce(host, *generation, runtime.context());
        REQUIRE(observed.has_value());
        CHECK(p_reader->calls() == 1U);
        CHECK(
            observed->canonicalJcs()
            == R"({"kind":"resolved_state","ordered_surface_stack":["screen"],)"
               R"("readings":[{"id":"title","kind":"absent"}]})"
        );

        // Absent is not unknown: it carries no reason, because there is nothing
        // undecided about a rectangle that was read and held no text.
        CHECK(observed->canonicalJcs().find(R"("reason")") == std::string::npos);
    }

    // A Host that stopped on its own read budget did not look, and must not say
    // a locator did. Both readings travel in one document: title spends the
    // cycle's single read and comes back with the Reader's text, subtitle is
    // refused before the recogniser is reached, and the budget is the only thing
    // that differs between them.
    TEST_CASE("TaskHost::observe names a spent read budget as the reason it stopped")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const rootHash = publish(
            directory.path(),
            budgetedReadingRuntimeModel(),
            runtimeAssets()
        );
        auto const generation = TaskHostTestAccess::activate(
            host,
            directory.path(),
            rootHash
        );
        REQUIRE(generation.has_value());

        auto reader = std::make_unique<ScriptedReader>("Wandering Merchant", 9'100);
        auto* const p_reader = reader.get();
        auto runtime = RuntimeContext{
            frame(
                {
                    std::byte{k_anchorGray},
                    std::byte{k_actionGray},
                    std::byte{k_actionGray},
                },
                FrameId{43}
            ),
            1'000,
            std::move(reader),
            1
        };
        auto const observed = observeOnce(host, *generation, runtime.context());
        REQUIRE(observed.has_value());
        CHECK(
            observed->canonicalJcs()
            == R"({"kind":"resolved_state","ordered_surface_stack":["screen"],)"
               R"("readings":[{"id":"subtitle","kind":"unknown",)"
               R"("reason":"budget_exhausted"},)"
               R"({"id":"title","kind":"read",)"
               R"("lines":[{"rect":[1,0,1,1],"text":"Wandering Merchant"}]}]})"
        );

        // The reason on its own, because it is the whole of what this case
        // claims: an equality that stopped carrying it would be repaired by
        // rewriting the expected bytes rather than by fixing anything.
        CHECK(
            observed->canonicalJcs().find(R"("reason":"budget_exhausted")")
            != std::string::npos
        );

        // The two answers this refusal must never wear. locator_failed says a
        // locator looked and did not find; not_measured says nobody asked. The
        // Host asked, was told to stop, and both spellings would send a consumer
        // that fails closed to the wrong conclusion about the subtitle pixels.
        CHECK(observed->canonicalJcs().find("locator_failed") == std::string::npos);
        CHECK(observed->canonicalJcs().find("not_measured") == std::string::npos);

        // The budget is charged before the engine, so the refused read cost no
        // inference. One call is the whole of what this cycle paid.
        CHECK(p_reader->calls() == 1U);
    }

    // The control for the case above, and what makes its refusal mean anything:
    // the same model, the same frame and the same Reader, with the budget raised
    // by one, read BOTH rectangles. Without it the subtitle refusal is equally
    // consistent with a second Binding this world cannot measure at all.
    TEST_CASE("TaskHost::observe reads both rectangles once the budget covers them")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const rootHash = publish(
            directory.path(),
            budgetedReadingRuntimeModel(),
            runtimeAssets()
        );
        auto const generation = TaskHostTestAccess::activate(
            host,
            directory.path(),
            rootHash
        );
        REQUIRE(generation.has_value());

        auto reader = std::make_unique<ScriptedReader>("Wandering Merchant", 9'100);
        auto* const p_reader = reader.get();
        auto runtime = RuntimeContext{
            frame(
                {
                    std::byte{k_anchorGray},
                    std::byte{k_actionGray},
                    std::byte{k_actionGray},
                },
                FrameId{44}
            ),
            1'000,
            std::move(reader),
            2
        };
        auto const observed = observeOnce(host, *generation, runtime.context());
        REQUIRE(observed.has_value());
        CHECK(
            observed->canonicalJcs()
            == R"({"kind":"resolved_state","ordered_surface_stack":["screen"],)"
               R"("readings":[{"id":"subtitle","kind":"read",)"
               R"("lines":[{"rect":[2,0,1,1],"text":"Wandering Merchant"}]},)"
               R"({"id":"title","kind":"read",)"
               R"("lines":[{"rect":[1,0,1,1],"text":"Wandering Merchant"}]}]})"
        );
        CHECK(observed->canonicalJcs().find(R"("reason")") == std::string::npos);
        CHECK(p_reader->calls() == 2U);
    }

    // R2's falsifier at the Host boundary. A Readout's Surface is the WHOLE of
    // its gate: this world turns OFF the Binding that sits on the very same
    // pixels, and the Readout is still read and still reported. The rule this
    // replaces -- a reading needs a present Binding beside it -- is exactly what
    // made a readout region undeclarable, because the digits printed in one
    // change every frame and no template can be measured on them. Reinstating
    // that rule empties the readings list here and reds this case.
    TEST_CASE("TaskHost::observe reads a Readout with no Binding present on it")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const rootHash = publish(
            directory.path(),
            readingRuntimeModel(),
            runtimeAssets()
        );
        auto const generation = TaskHostTestAccess::activate(
            host,
            directory.path(),
            rootHash
        );
        REQUIRE(generation.has_value());

        // The middle pixel no longer matches title.primary's locator, so the
        // Surface still resolves off its anchor and that Binding does not.
        auto reader = std::make_unique<ScriptedReader>("Wandering Merchant", 9'100);
        auto* const p_reader = reader.get();
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{0}, std::byte{0}}, FrameId{35}),
            1'000,
            std::move(reader)
        };
        auto const observed = observeOnce(host, *generation, runtime.context());
        REQUIRE(observed.has_value());
        CHECK(
            observed->canonicalJcs()
            == R"({"kind":"resolved_state","ordered_surface_stack":["screen"],)"
               R"("readings":[{"id":"title","kind":"read",)"
               R"("lines":[{"rect":[1,0,1,1],"text":"Wandering Merchant"}]}]})"
        );

        // The read was spent, on the Readout's own rectangle, with no Binding
        // measured present anywhere near it.
        CHECK(p_reader->calls() == 1U);
    }

    // Three models differing only in the geometry they declare. The first pins
    // each of the four components separately, so a fingerprint that transposed
    // an extent into a DPI passes none of them; the second changes only the
    // extent and the third only the DPI, so a constant, or a value carrying
    // half of what the model states, cannot satisfy all three.
    TEST_CASE("RuntimeModelBinding publishes the geometry its model declares")
    {
        auto const declaredDirectory = TemporaryDirectory{};
        auto declaredHost = TaskHost{};
        auto const declared = bindingDeclaring(
            declaredHost,
            declaredDirectory,
            "base_resolution = [1920, 1080]\nbase_dpi = [96, 120]\n"
        );
        CHECK(declared.fingerprint().width() == 1920U);
        CHECK(declared.fingerprint().height() == 1080U);
        CHECK(declared.fingerprint().dpiX() == 96U);
        CHECK(declared.fingerprint().dpiY() == 120U);

        // The geometry travels beside the three vocabularies and not inside
        // them: a fourth name here would be the Operator gaining a way to ask
        // what a name means, which is what DeclaredRuntimeUi exists to refuse.
        CHECK(declared.declaredUi().surfaces == std::vector<std::string>{"screen"});
        CHECK(
            declared.declaredUi().uiTargets
            == std::vector<std::string>{"confirm", "screen-marker"}
        );
        CHECK(declared.declaredUi().actions == std::vector<std::string>{"activate"});

        // Both extent components above 16 bits: a fingerprint that crossed the
        // native seam through a narrower integer arrives truncated rather than
        // merely different, and equality on the whole value would not say which.
        auto const wideDirectory = TemporaryDirectory{};
        auto wideHost = TaskHost{};
        auto const wide = bindingDeclaring(
            wideHost,
            wideDirectory,
            "base_resolution = [70000, 66000]\nbase_dpi = [96, 120]\n"
        );
        CHECK(wide.fingerprint().width() == 70000U);
        CHECK(wide.fingerprint().height() == 66000U);
        CHECK(wide.fingerprint() != declared.fingerprint());

        auto const dpiDirectory = TemporaryDirectory{};
        auto dpiHost = TaskHost{};
        auto const dpi = bindingDeclaring(
            dpiHost,
            dpiDirectory,
            "base_resolution = [1920, 1080]\nbase_dpi = [144, 144]\n"
        );
        CHECK(dpi.fingerprint().dpiX() == 144U);
        CHECK(dpi.fingerprint().dpiY() == 144U);
        CHECK(dpi.fingerprint() != declared.fingerprint());
    }

    // What a model may not leave out. Both halves are required and neither has
    // a default, so a model stating one and omitting the other is refused where
    // the omission is, and a zero component is not an extent. Each case reads
    // the whole refusal and not just the field it names: every rectangle in the
    // model is bounds-checked against base_resolution and says so, so a
    // substring stopping at the field name would go green off that second
    // refusal and prove nothing about this one.
    TEST_CASE("A RuntimeModel declaring one half of its geometry is refused")
    {
        auto const refusal = [](std::string_view geometry, std::string_view refused)
        {
            auto const directory = TemporaryDirectory{};
            auto host = TaskHost{};
            auto const activated = activateDeclaring(host, directory, geometry);
            REQUIRE_FALSE(activated.has_value());
            CHECK(activated.error().message().find(refused) != std::string::npos);
        };

        refusal("", "RuntimeModel.base_resolution must be a table");
        refusal(
            "base_resolution = [1920, 1080]\n",
            "RuntimeModel.base_dpi must be a table"
        );
        refusal(
            "base_dpi = [96, 120]\n",
            "RuntimeModel.base_resolution must be a table"
        );
        refusal(
            "base_resolution = [1920, 0]\nbase_dpi = [96, 120]\n",
            "RuntimeModel.base_resolution[2] must be positive"
        );
        refusal(
            "base_resolution = [1920, 1080]\nbase_dpi = [96, 0]\n",
            "RuntimeModel.base_dpi[2] must be positive"
        );
    }
}
