#include <task/exploration-session.hpp>
#include <task/page-model-file.hpp>
#include <task/task-context.hpp>
#include <task/task-host.hpp>

#include <core/error/result.hpp>
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

#include <trace/event.hpp>
#include <trace/recorder.hpp>
#include <trace/sink.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::task
{
    struct TaskHostTestAccess final
    {
        [[nodiscard]]
        static auto activate(
            TaskHost& host,
            std::filesystem::path const& artifactRoot,
            ContentHash const& expectedRootHash
        ) -> Result<GenerationId>
        {
            UF_TRY_VALUE(
                verified,
                uf::task::loadRuntimeArtifact(artifactRoot, expectedRootHash)
            );
            auto artifact = std::make_shared<RuntimeArtifactHandle const>(
                std::move(verified)
            );
            return host.activateRuntimeArtifact(
                InstalledRuntimeArtifact{std::move(artifact), 1U}
            );
        }

        [[nodiscard]]
        static auto run(
            TaskHost& host,
            GenerationId generation,
            TaskContext& context,
            std::string_view source
        ) -> Result<script::ScriptValue>
        {
            return host.runTrustedRuntime(
                generation,
                context,
                source,
                "runtime-v2-contract"
            );
        }

        [[nodiscard]] static auto pendingReceipt(TaskHost& host) -> TaskHost::Receipt
        {
            REQUIRE(host.m_receipts.size() == 1U);
            auto const& pending = host.m_receipts.front();
            CHECK(pending.intent.stateIdentity.starts_with("state-resolution-"));
            CHECK(pending.intent.surface == "screen");
            CHECK(pending.intent.uiTarget == "confirm");
            CHECK(pending.intent.binding == "confirm.primary");
            CHECK(pending.intent.variant == "primary");
            CHECK(pending.intent.action == "activate");
            CHECK(pending.intent.proofLocator == "confirm-mark");
            return TaskHost::Receipt{host.m_hostNonce, pending.ordinal};
        }

        [[nodiscard]] static auto pendingSemanticHash(TaskHost const& host) -> ContentHash
        {
            REQUIRE(host.m_receipts.size() == 1U);
            return host.m_receipts.front().semanticHash;
        }

        [[nodiscard]]
        static auto deliver(
            TaskHost& host,
            TaskHost::Receipt const& receipt,
            TaskContext& context
        ) -> Result<engine::ActReceipt>
        {
            return host.deliver(host.deliveryAuthority(), receipt, context);
        }
    };

    namespace
    {
        constexpr auto k_authorizeSource = std::string_view{R"lua(
            local cycle = observe.open(project.load_project())
            local state = cycle:resolve_state()
            local binding = cycle:resolve_binding(state, "confirm")
            local receipt, reason = cycle:authorize(binding, "activate")
            if receipt == nil then error(reason) end
            return 1
        )lua"};

        constexpr auto k_anchorGray = uint8{2};
        constexpr auto k_actionGray = uint8{5};

        class TemporaryDirectory final
        {
            std::filesystem::path m_path;

        public:
            TemporaryDirectory()
            {
                static auto sequence = std::atomic<uint64>{1};
                m_path = std::filesystem::temp_directory_path()
                    / std::format(
                        "umbraflow-runtime-v2-{}-{}",
                        std::chrono::steady_clock::now().time_since_epoch().count(),
                        sequence.fetch_add(1, std::memory_order_relaxed)
                    );
                REQUIRE(std::filesystem::create_directory(m_path));
            }

            ~TemporaryDirectory() noexcept
            {
                auto error = std::error_code{};
                static_cast<void>(std::filesystem::remove_all(m_path, error));
            }

            [[nodiscard]] auto path() const noexcept -> std::filesystem::path const&
            {
                return m_path;
            }
        };

        struct ArtifactFile final
        {
            std::string            path{};
            std::vector<std::byte> bytes{};
        };

        [[nodiscard]] auto bytes(std::string_view text) -> std::vector<std::byte>
        {
            auto const view = std::as_bytes(std::span{text.data(), text.size()});
            return {view.begin(), view.end()};
        }

        [[nodiscard]] auto hash(std::span<std::byte const> value) -> ContentHash
        {
            auto result = sha256(value);
            REQUIRE(result.has_value());
            return *result;
        }

        [[nodiscard]] auto hash(std::string_view value) -> ContentHash
        {
            return hash(std::as_bytes(std::span{value.data(), value.size()}));
        }

        auto write(
            std::filesystem::path const& path,
            std::span<std::byte const> value
        ) -> void
        {
            auto const parent = path.parent_path();
            auto const parentReady = std::filesystem::create_directories(parent)
                || std::filesystem::is_directory(parent);
            REQUIRE(parentReady);
            auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
            auto text   = std::string{};
            text.reserve(value.size());
            for (auto const byte : value)
            {
                text.push_back(
                    static_cast<char>(std::to_integer<unsigned char>(byte))
                );
            }
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            REQUIRE(stream.good());
        }

        auto write(std::filesystem::path const& path, std::string_view value) -> void
        {
            write(path, std::as_bytes(std::span{value.data(), value.size()}));
        }

        [[nodiscard]] auto templatePng(uint8 gray) -> std::vector<std::byte>
        {
            auto encoded = image::encodeRgbaPng(
                "runtime-v2-template.png",
                1,
                1,
                std::vector<std::byte>{
                    static_cast<std::byte>(gray),
                    static_cast<std::byte>(gray),
                    static_cast<std::byte>(gray),
                    std::byte{255},
                }
            );
            REQUIRE(encoded.has_value());
            return *std::move(encoded);
        }

        [[nodiscard]] auto runtimeModel() -> std::string
        {
            return R"toml(schema_version = 2
base_resolution = [3, 1]
base_dpi = [96, 96]

[[ui_target]]
id = "screen-marker"
kind = "region"

[[ui_target]]
id = "confirm"
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
variant = "primary"
placement = { kind = "fixed", rect = [0, 0, 1, 1] }
detector = { all = [{ kind = "locator_present", locator = "screen-anchor" }], any = [], none = [] }
actions = []

[[binding]]
id = "confirm.primary"
surface = "screen"
ui_target = "confirm"
variant = "primary"
placement = { kind = "fixed", rect = [1, 0, 1, 1], action_point = [1, 0] }
detector = { all = [{ kind = "locator_present", locator = "confirm-mark" }], any = [], none = [] }
actions = [{ id = "activate", kind = "click", proof_locator = "confirm-mark" }]

[[surface]]
id = "screen"
kind = "scene"
covers = []
identity = { all = ["screen.anchor"], any = [], none = [] }
)toml";
        }

        [[nodiscard]] auto runtimeAssets() -> std::vector<ArtifactFile>
        {
            return {
                ArtifactFile{.path = "assets/anchor.png", .bytes = templatePng(k_anchorGray)},
                ArtifactFile{.path = "assets/confirm.png", .bytes = templatePng(k_actionGray)},
            };
        }

        [[nodiscard]] auto manifestFile(ArtifactFile const& file) -> std::string
        {
            return std::format(
                "{{\"path\":\"{}\",\"sha256\":\"{}\",\"size\":{}}}",
                file.path,
                hash(file.bytes).hex(),
                file.bytes.size()
            );
        }

        [[nodiscard]]
        auto publish(
            std::filesystem::path const& root,
            std::string_view model,
            std::vector<ArtifactFile> assets
        ) -> ContentHash
        {
            std::ranges::sort(assets, {}, &ArtifactFile::path);
            write(root / k_runtimeModelFileName, model);
            auto rows = std::vector<std::string>{};
            rows.reserve(assets.size());
            for (auto const& asset : assets)
            {
                write(root / std::filesystem::path{asset.path}, asset.bytes);
                rows.emplace_back(manifestFile(asset));
            }

            auto assetJson = std::string{};
            for (auto index = std::size_t{0}; index < rows.size(); ++index)
            {
                if (index != 0U)
                {
                    assetJson.push_back(',');
                }
                assetJson += rows[index];
            }
            auto const modelFile = ArtifactFile{
                .path  = std::string{k_runtimeModelFileName},
                .bytes = bytes(model),
            };
            auto const manifest = std::format(
                "{{\"assets\":[{}],\"manifest_schema_hash\":\"{}\","
                "\"page_model\":{},\"runtime_model_schema_hash\":\"{}\"}}",
                assetJson,
                k_runtimeArtifactSchemaHash,
                manifestFile(modelFile),
                k_runtimeModelSchemaHash
            );
            write(root / k_runtimeArtifactManifestFileName, manifest);
            return hash(manifest);
        }

        [[nodiscard]] auto fingerprint() -> ProjectFingerprint
        {
            auto result = ProjectFingerprint::create(3, 1, 96, 96);
            REQUIRE(result.has_value());
            return *result;
        }

        [[nodiscard]] auto frame(std::vector<std::byte> pixels, FrameId id) -> Frame
        {
            auto transform = CoordinateTransform::create(
                Point<DesktopSpace>{0.0F, 0.0F},
                3.0F,
                1.0F,
                3,
                1
            );
            REQUIRE(transform.has_value());
            auto result = Frame::create(
                id,
                CaptureSessionId{7},
                TargetGeneration::fromValue(3),
                MonotonicInstant::now(),
                3,
                1,
                3,
                PixelFormat::Gray8,
                std::make_shared<FrameBuffer const>(std::move(pixels)),
                *transform
            );
            REQUIRE(result.has_value());
            return *std::move(result);
        }

        class FrameSource final : public engine::IFrameSource
        {
            Frame m_frame;

        public:
            explicit FrameSource(Frame value) noexcept : m_frame{std::move(value)} {}

            [[nodiscard]] auto capture(CaptureBudget const&) -> Result<Frame> override
            {
                return m_frame;
            }

            [[nodiscard]] auto validateTargetInstance() -> Status override
            {
                return ok();
            }
        };

        class ActionSink final : public engine::IActionSink
        {
            uint32 m_clicks{};

        public:
            [[nodiscard]]
            auto click(Point<ClientSpace>, ObservationLease const&) -> Status override
            {
                ++m_clicks;
                return ok();
            }

            [[nodiscard]] auto pressKey(KeyName, TargetGeneration) -> Status override
            {
                return ok();
            }

            [[nodiscard]] auto scroll(int32, ObservationLease const&) -> Status override
            {
                return ok();
            }

            [[nodiscard]]
            auto longPress(
                Point<ClientSpace>,
                MonotonicInstant::Duration,
                ObservationLease const&
            ) -> Status override
            {
                return ok();
            }

            [[nodiscard]]
            auto drag(
                Point<ClientSpace>,
                Point<ClientSpace>,
                MonotonicInstant::Duration,
                ObservationLease const&
            ) -> Status override
            {
                return ok();
            }

            [[nodiscard]]
            auto movePointer(Point<ClientSpace>, ObservationLease const&) -> Status override
            {
                return ok();
            }

            [[nodiscard]] auto clicks() const noexcept -> uint32 { return m_clicks; }
        };

        class TraceSink final : public trace::ITraceSink
        {
        public:
            [[nodiscard]] auto append(trace::TraceEvent const&) -> Status override
            {
                return ok();
            }
        };

        class RuntimeContext final
        {
            std::unique_ptr<trace::TraceRecorder> m_recorder{};
            ActionSink*                          m_pActions{};
            std::optional<TaskContext>           m_context{};

        public:
            RuntimeContext(Frame value, uint64 comparisonBudget)
            {
                auto recorder = trace::TraceRecorder::create(
                    std::make_unique<TraceSink>(),
                    trace::TraceStreamSpec{
                        .sessionId           = "runtime-v2-contract",
                        .sessionManifestHash = hash("runtime-v2-contract-manifest"),
                        .producer            = "runtime-v2-test",
                    }
                );
                REQUIRE(recorder.has_value());
                m_recorder = std::make_unique<trace::TraceRecorder>(
                    *std::move(recorder)
                );

                auto actions = std::make_unique<ActionSink>();
                m_pActions   = actions.get();
                auto session = engine::EngineSession::create(
                    std::make_unique<FrameSource>(std::move(value)),
                    std::move(actions),
                    *m_recorder,
                    engine::EngineSessionConfig{
                        .liveFingerprint         = fingerprint(),
                        .projectFingerprint      = fingerprint(),
                        .maximumPixelComparisons = comparisonBudget,
                        .recognitionTimeout      = std::chrono::seconds{1},
                    }
                );
                REQUIRE(session.has_value());
                m_context.emplace(*std::move(session), *m_recorder);
            }

            [[nodiscard]] auto context() noexcept -> TaskContext& { return *m_context; }
            [[nodiscard]] auto actions() noexcept -> ActionSink& { return *m_pActions; }
        };

        [[nodiscard]] auto loadedRuntime(
            TaskHost& host,
            TemporaryDirectory const& directory
        ) -> GenerationId
        {
            auto const model = runtimeModel();
            auto const rootHash = publish(directory.path(), model, runtimeAssets());
            auto generation = TaskHostTestAccess::activate(
                host,
                directory.path(),
                rootHash
            );
            REQUIRE(generation.has_value());
            return *generation;
        }

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
        auto explorationConfig(Frame value, std::filesystem::path tracePath)
            -> TaskRunConfig
        {
            return TaskRunConfig{
                .frameSource     = std::make_unique<FrameSource>(std::move(value)),
                .actionSink      = std::make_unique<ActionSink>(),
                .liveFingerprint = fingerprint(),
                .maximumPixelComparisons = 1'000,
                .recognitionTimeout = std::chrono::seconds{1},
                .tracePath          = std::move(tracePath),
            };
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
        auto const invalidModel = std::string{"schema_version = 2\n"};
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

    TEST_CASE("contract-runtime-u06")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedRuntime(host, directory);
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{14}),
            1'000
        };
        auto minted = TaskHostTestAccess::run(
            host,
            generation,
            runtime.context(),
            k_authorizeSource
        );
        REQUIRE(minted.has_value());
        CHECK(minted->number() == std::optional<double>{1.0});
        CHECK(runtime.actions().clicks() == 0U);
        CHECK(TaskHostTestAccess::pendingSemanticHash(host) != hash(runtimeModel()));

        auto const receipt = TaskHostTestAccess::pendingReceipt(host);
        REQUIRE(TaskHostTestAccess::deliver(host, receipt, runtime.context()).has_value());
        CHECK(runtime.actions().clicks() == 1U);
        CHECK_FALSE(TaskHostTestAccess::deliver(host, receipt, runtime.context()).has_value());
        CHECK(runtime.actions().clicks() == 1U);

        // A Receipt authorizes one delivery against the cycle it was minted in,
        // so presenting a fresh one with a different context is refused before
        // any click: no other context holds that cycle. That is what lets the
        // Host stop remembering a pointer to the context that minted it.
        auto const reminted = TaskHostTestAccess::run(
            host,
            generation,
            runtime.context(),
            k_authorizeSource
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
        CHECK_FALSE(
            TaskHostTestAccess::deliver(
                host,
                TaskHostTestAccess::pendingReceipt(host),
                other.context()
            ).has_value()
        );
        CHECK(other.actions().clicks() == 0U);
        CHECK(runtime.actions().clicks() == 1U);
    }

    TEST_CASE("contract-runtime-u07")
    {
        auto const runtimeDirectory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const runtimeGeneration = loadedRuntime(host, runtimeDirectory);
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{15}),
            1'000
        };
        auto runtimeSurface = TaskHostTestAccess::run(
            host,
            runtimeGeneration,
            runtime.context(),
            "return project ~= nil and observe ~= nil and explore == nil "
            "and model == nil and resolution == nil and evidence == nil"
        );
        REQUIRE(runtimeSurface.has_value());
        CHECK(runtimeSurface->boolean() == std::optional<bool>{true});
        CHECK_FALSE(
            host.startExplorationSession(
                runtimeGeneration,
                explorationConfig(
                    frame({std::byte{0}, std::byte{0}, std::byte{0}}, FrameId{16}),
                    runtimeDirectory.path() / "forbidden-trace.jsonl"
                )
            ).has_value()
        );

        auto const authoringDirectory = TemporaryDirectory{};
        write(authoringDirectory.path() / "annotation-screenshot.png", "authoring pixels");
        auto annotation = host.openAnnotationProject(authoringDirectory.path());
        REQUIRE(annotation.has_value());
        CHECK_FALSE(host.runtimeModelBytes(*annotation).has_value());
        auto session = host.startExplorationSession(
            *annotation,
            explorationConfig(
                frame({std::byte{0}, std::byte{0}, std::byte{0}}, FrameId{17}),
                authoringDirectory.path() / "authoring-trace.jsonl"
            )
        );
        REQUIRE(session.has_value());
        auto authoringSurface = (*session)->evaluate(
            R"lua(
                local blob = explore.cycle(function(cycle)
                    return cycle:crop(0, 0, 1, 1)
                end)
                local measured = explore.probe(blob, 0, 0, 1, 1)
                return explore ~= nil and project == nil and observe == nil
                    and model == nil and resolution == nil and evidence == nil
                    and type(blob) == "string" and #blob > 0
                    and measured.image_width == 1 and measured.image_height == 1
            )lua",
            "authoring-boundary"
        );
        REQUIRE(authoringSurface.has_value());
        CHECK(authoringSurface->boolean() == std::optional<bool>{true});
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
}
