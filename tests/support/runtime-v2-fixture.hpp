#pragma once

#include <operator-contract/host-delivery-fixture.hpp>

#include <task/host-delivery.hpp>
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
#include <type_traits>
#include <utility>
#include <vector>

// One Runtime v2 world: the RuntimeArtifact bytes and their asset closure, the
// EngineSession over a synthetic frame that the resolver matches them against,
// and the TaskHost that activates the two.
//
// It is shared rather than copied because a Host generation is reachable only
// through TaskHostTestAccess -- activateRuntimeArtifact, runTrustedRuntime and
// deliver are all private, and the friend declarations in task-host.hpp name
// exactly one harness type. A second copy would be a second spelling of the
// same artifact bytes, and the manifest hash is derived from those bytes, so
// the two spellings would silently pin different worlds.
//
// TaskHostTestAccess itself is not here: it is in
// operator-contract/host-delivery-fixture.hpp, because the Operator's contracts
// need the same privilege and uf::task may hold only one type of that name.
namespace uf::task
{
    inline constexpr auto k_anchorGray = uint8{2};
    inline constexpr auto k_actionGray = uint8{5};

    // Owns the directory it created: the destructor removes the whole tree.
    // Copying would leave two owners of one path, and the first destruction
    // would delete the tree the survivor still names -- silently, because
    // remove_all's second call is a successful no-op. Moving is deleted too
    // rather than written: every use here initializes from a prvalue, so
    // there is no move to elide and an unused move constructor would be a
    // second ownership rule nobody exercises.
    class TemporaryDirectory final
    {
        std::filesystem::path m_path{};

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

        TemporaryDirectory(TemporaryDirectory const&) = delete;
        TemporaryDirectory(TemporaryDirectory&&) = delete;
        auto operator=(TemporaryDirectory const&) -> TemporaryDirectory& = delete;
        auto operator=(TemporaryDirectory&&) -> TemporaryDirectory& = delete;

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

    // The ownership rule above, stated where it can fail: restoring either
    // implicit operation stops this header compiling.
    static_assert(!std::is_copy_constructible_v<TemporaryDirectory>);
    static_assert(!std::is_move_constructible_v<TemporaryDirectory>);

    struct ArtifactFile final
    {
        std::string            path{};
        std::vector<std::byte> bytes{};
    };

    [[nodiscard]] inline auto bytes(std::string_view text) -> std::vector<std::byte>
    {
        auto const view = std::as_bytes(std::span{text});
        return {view.begin(), view.end()};
    }

    [[nodiscard]] inline auto hash(std::span<std::byte const> value) -> ContentHash
    {
        auto result = sha256(value);
        REQUIRE(result.has_value());
        return *result;
    }

    [[nodiscard]] inline auto hash(std::string_view value) -> ContentHash
    {
        return hash(std::as_bytes(std::span{value}));
    }

    inline auto write(
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

    inline auto write(
        std::filesystem::path const& path,
        std::string_view value
    ) -> void
    {
        write(path, std::as_bytes(std::span{value}));
    }

    [[nodiscard]] inline auto templatePng(uint8 gray) -> std::vector<std::byte>
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

    [[nodiscard]] inline auto runtimeModel() -> std::string
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

    // The same model with a second scene the same frame also satisfies, so
    // resolve_state has two scene candidates and reports an ambiguous
    // state. It reuses the confirm mark rather than a third asset because
    // the asset closure is verified against the manifest.
    [[nodiscard]] inline auto twoSceneRuntimeModel() -> std::string
    {
        return runtimeModel() + R"toml(
[[binding]]
id = "panel.anchor"
surface = "panel"
ui_target = "screen-marker"
variant = "primary"
placement = { kind = "fixed", rect = [1, 0, 1, 1] }
detector = { all = [{ kind = "locator_present", locator = "confirm-mark" }], any = [], none = [] }
actions = []

[[surface]]
id = "panel"
kind = "scene"
covers = []
identity = { all = ["panel.anchor"], any = [], none = [] }
)toml";
    }

    [[nodiscard]] inline auto runtimeAssets() -> std::vector<ArtifactFile>
    {
        return {
            ArtifactFile{.path = "assets/anchor.png", .bytes = templatePng(k_anchorGray)},
            ArtifactFile{.path = "assets/confirm.png", .bytes = templatePng(k_actionGray)},
        };
    }

    [[nodiscard]] inline auto manifestFile(ArtifactFile const& file) -> std::string
    {
        return std::format(
            "{{\"path\":\"{}\",\"sha256\":\"{}\",\"size\":{}}}",
            file.path,
            hash(file.bytes).hex(),
            file.bytes.size()
        );
    }

    [[nodiscard]]
    inline auto publish(
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

    [[nodiscard]] inline auto fingerprint() -> ProjectFingerprint
    {
        auto result = ProjectFingerprint::create(3, 1, 96, 96);
        REQUIRE(result.has_value());
        return *result;
    }

    [[nodiscard]] inline auto frame(std::vector<std::byte> pixels, FrameId id) -> Frame
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
        bool   m_refuseClicks{};

    public:
        // A refused click is what a real sink cannot describe: the post may have
        // reached the target before the failure. EngineSession::clickPoint
        // reports one Err for every phase, so this is the only way a test can
        // reach the outcome that under-claims deliberately.
        [[nodiscard]]
        auto click(Point<ClientSpace>, ObservationLease const&) -> Status override
        {
            if (m_refuseClicks)
            {
                return fail(
                    AutomationErrorKind::TargetUnavailable,
                    "the action sink refused this click"
                );
            }
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

        auto refuseClicks() noexcept -> void { m_refuseClicks = true; }
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

    inline constexpr auto k_controlledTarget = std::string_view{"controlled-target"};
    inline constexpr auto k_controlSessionEpoch = uint64{4};

    [[nodiscard]]
    inline auto controlFenceOn(std::string_view target, uint64 fencingToken)
        -> ControlFence
    {
        return ControlFence{
            .controlledTargetId = std::string{target},
            .sessionEpoch       = k_controlSessionEpoch,
            .fencingToken       = fencingToken,
        };
    }

    [[nodiscard]] inline auto controlFence(uint64 fencingToken) -> ControlFence
    {
        return controlFenceOn(k_controlledTarget, fencingToken);
    }

    // A reservation the ledger would have written for `generation` under
    // `fence`. It is plain data on purpose: presenting a forged one can only
    // make the Host refuse, because acting still needs a Receipt the Host
    // itself minted.
    [[nodiscard]]
    inline auto dispatchAuthority(ControlFence const& fence, GenerationId generation)
        -> DispatchAuthority
    {
        return DispatchAuthority{
            .controlledTargetId  = fence.controlledTargetId,
            .leaseId             = "lease-1",
            .operationId         = "operation-1",
            .authorityDecisionId = "authority-decision-1",
            .frozenPlanHash      = hash("frozen-plan"),
            .runtimeGeneration   = generation,
            .targetGeneration    = TargetGeneration::fromValue(3),
            .sessionEpoch        = fence.sessionEpoch,
            .fencingToken        = fence.fencingToken,
            .dispatchSequence    = 1,
        };
    }

    [[nodiscard]] inline auto loadedRuntime(
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
}
