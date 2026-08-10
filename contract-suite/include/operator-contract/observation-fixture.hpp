#pragma once

#include <operator/ledger.hpp>
#include <operator/runtime-installation.hpp>

#include <task/page-model-file.hpp>
#include <task/task-context.hpp>
#include <task/task-host.hpp>
#include <task/ui-observation.hpp>

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

#include <trace/event.hpp>
#include <trace/recorder.hpp>
#include <trace/sink.hpp>

#include <doctest/doctest.h>

#include <algorithm>
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

// A real RuntimeArtifact, a real EngineSession over a synthetic frame, and the
// TaskHost that turns the two into one task::UiObservationSnapshot.
//
// It is shared rather than duplicated because createSnapshot composes an
// observation the Host minted, and UiObservationSnapshot's only friend is
// TaskHost: no test can assemble one, so every caller of createSnapshot -- this
// repository's unit tests and every consuming repository's run of the exported
// suite -- has to drive a Host through one real observation cycle.
namespace uf::operator_runtime::contract
{
    // The two grays the fixture model's locators match on. They are distinct so
    // that a frame carrying one and not the other resolves to a different state.
    inline constexpr auto k_anchorGray = uint8{2};
    inline constexpr auto k_actionGray = uint8{5};

    struct ArtifactFile final
    {
        std::string            path{};
        std::vector<std::byte> bytes{};
    };

    [[nodiscard]]
    inline auto observationHash(std::span<std::byte const> value) -> ContentHash
    {
        auto result = sha256(value);
        REQUIRE(result.has_value());
        return *result;
    }

    [[nodiscard]]
    inline auto observationHash(std::string_view value) -> ContentHash
    {
        return observationHash(
            std::as_bytes(std::span{value.data(), value.size()})
        );
    }

    [[nodiscard]]
    inline auto observationBytes(std::string_view text) -> std::vector<std::byte>
    {
        auto const view = std::as_bytes(std::span{text.data(), text.size()});
        return {view.begin(), view.end()};
    }

    inline auto writeArtifactFile(
        std::filesystem::path const& path,
        std::span<std::byte const> value
    ) -> void
    {
        auto const parent      = path.parent_path();
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

    inline auto writeArtifactFile(
        std::filesystem::path const& path,
        std::string_view value
    ) -> void
    {
        writeArtifactFile(
            path,
            std::as_bytes(std::span{value.data(), value.size()})
        );
    }

    [[nodiscard]]
    inline auto templatePng(uint8 gray) -> std::vector<std::byte>
    {
        auto encoded = image::encodeRgbaPng(
            "contract-observation-template.png",
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

    // One scene with one activatable binding. It is the smallest model whose
    // resolver output is a resolved state, which is what an Operator snapshot
    // has to be composed from.
    [[nodiscard]]
    inline auto observationRuntimeModel() -> std::string
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

    // The same model with a second scene the same frame also satisfies, so the
    // resolver reports an ambiguous state and the state_resolution_hash moves
    // without any Operator-held column moving with it.
    [[nodiscard]]
    inline auto ambiguousRuntimeModel() -> std::string
    {
        return observationRuntimeModel() + R"toml(
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

    [[nodiscard]]
    inline auto observationAssets() -> std::vector<ArtifactFile>
    {
        return {
            ArtifactFile{
                .path  = "assets/anchor.png",
                .bytes = templatePng(k_anchorGray),
            },
            ArtifactFile{
                .path  = "assets/confirm.png",
                .bytes = templatePng(k_actionGray),
            },
        };
    }

    [[nodiscard]]
    inline auto artifactManifestRow(ArtifactFile const& file) -> std::string
    {
        return std::format(
            R"({{"path":"{}","sha256":"{}","size":{}}})",
            file.path,
            observationHash(file.bytes).hex(),
            file.bytes.size()
        );
    }

    // Writes one RuntimeArtifact directory and returns its root hash, which is
    // the hash of the manifest naming every file in it.
    [[nodiscard]]
    inline auto publishRuntimeArtifact(
        std::filesystem::path const& root,
        std::string_view model,
        std::vector<ArtifactFile> assets
    ) -> ContentHash
    {
        std::ranges::sort(assets, {}, &ArtifactFile::path);
        writeArtifactFile(root / task::k_runtimeModelFileName, model);
        auto rows = std::vector<std::string>{};
        rows.reserve(assets.size());
        for (auto const& asset : assets)
        {
            writeArtifactFile(root / std::filesystem::path{asset.path}, asset.bytes);
            rows.emplace_back(artifactManifestRow(asset));
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
            .path  = std::string{task::k_runtimeModelFileName},
            .bytes = observationBytes(model),
        };
        auto const manifest = std::format(
            R"({{"assets":[{}],"manifest_schema_hash":"{}",)"
            R"("page_model":{},"runtime_model_schema_hash":"{}"}})",
            assetJson,
            task::k_runtimeArtifactSchemaHash,
            artifactManifestRow(modelFile),
            task::k_runtimeModelSchemaHash
        );
        writeArtifactFile(root / task::k_runtimeArtifactManifestFileName, manifest);
        return observationHash(manifest);
    }

    // A release handoff the Operator's installer accepts, carrying a real
    // RuntimeArtifact rather than a placeholder: the Host has to parse the model
    // and match its locators before an observation exists at all.
    struct ObservationRelease final
    {
        std::filesystem::path handoffRoot;
        ContentHash           releaseManifestHash;
        ContentHash           artifactRootHash;
    };

    [[nodiscard]]
    inline auto observationRelease(
        std::filesystem::path const& root,
        std::string_view model
    ) -> ObservationRelease
    {
        auto const handoff          = root / "release";
        auto const artifactRootHash = publishRuntimeArtifact(
            handoff / "runtime-artifact",
            model,
            observationAssets()
        );
        auto const releaseManifest = std::format(
            R"({{"annotation_workspace_schema_hash":"{}",)"
            R"("candidate_id":"candidate-1","candidate_revision":1,)"
            R"("generation":1,"predecessor_publication_id":null,)"
            R"("replay_gate_hash":"{}","runtime_artifact_root_hash":"{}",)"
            R"("workspace_sqlite_schema_hash":"{}"}})",
            detail::k_annotationWorkspaceSchemaHash,
            observationHash("replay-gate").hex(),
            artifactRootHash.hex(),
            detail::k_workspaceSqliteSchemaHash
        );
        writeArtifactFile(handoff / "release.manifest.json", releaseManifest);
        return ObservationRelease{
            .handoffRoot         = handoff,
            .releaseManifestHash = observationHash(releaseManifest),
            .artifactRootHash    = artifactRootHash,
        };
    }

    [[nodiscard]]
    inline auto observationFingerprint() -> ProjectFingerprint
    {
        auto result = ProjectFingerprint::create(3, 1, 96, 96);
        REQUIRE(result.has_value());
        return *result;
    }

    [[nodiscard]]
    inline auto observationFrame(
        std::vector<std::byte> pixels,
        FrameId id
    ) -> Frame
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

    // The frame every scene binding matches, so the resolver reaches a resolved
    // state.
    [[nodiscard]]
    inline auto resolvedFramePixels() -> std::vector<std::byte>
    {
        return {
            static_cast<std::byte>(k_anchorGray),
            static_cast<std::byte>(k_actionGray),
            std::byte{0},
        };
    }

    // The frame the scene anchor does not match, so the resolver reports an
    // unknown state and the observation's state_resolution_hash differs.
    [[nodiscard]]
    inline auto unresolvedFramePixels() -> std::vector<std::byte>
    {
        return {
            std::byte{0},
            static_cast<std::byte>(k_actionGray),
            std::byte{0},
        };
    }

    class ObservationFrameSource final : public engine::IFrameSource
    {
        Frame m_frame;

    public:
        explicit ObservationFrameSource(Frame value) noexcept
            : m_frame{std::move(value)}
        {
        }

        [[nodiscard]] auto capture(CaptureBudget const&) -> Result<Frame> override
        {
            return m_frame;
        }

        [[nodiscard]] auto validateTargetInstance() -> Status override
        {
            return ok();
        }
    };

    // Every verb succeeds and none of them touches anything: an observation
    // cycle never acts, and a sink that refused would make a snapshot failure
    // indistinguishable from an action failure.
    class ObservationActionSink final : public engine::IActionSink
    {
    public:
        [[nodiscard]]
        auto click(Point<ClientSpace>, ObservationLease const&) -> Status override
        {
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
    };

    class ObservationTraceSink final : public trace::ITraceSink
    {
    public:
        [[nodiscard]] auto append(trace::TraceEvent const&) -> Status override
        {
            return ok();
        }
    };

    // One EngineSession over one fixed frame, and the TaskContext that owns it.
    // It is a class rather than a struct of members because task::TaskContext is
    // neither copyable nor movable: the session and the recorder it borrows have
    // to be constructed in place and outlive it.
    class ObservationRuntime final
    {
        std::unique_ptr<trace::TraceRecorder> m_recorder{};
        std::optional<task::TaskContext>      m_context{};

    public:
        explicit ObservationRuntime(Frame value)
        {
            auto recorder = trace::TraceRecorder::create(
                std::make_unique<ObservationTraceSink>(),
                trace::TraceStreamSpec{
                    .sessionId           = "operator-contract-observation",
                    .sessionManifestHash = observationHash("observation-manifest"),
                    .producer            = "operator-contract",
                }
            );
            REQUIRE(recorder.has_value());
            m_recorder = std::make_unique<trace::TraceRecorder>(*std::move(recorder));

            auto session = engine::EngineSession::create(
                std::make_unique<ObservationFrameSource>(std::move(value)),
                std::make_unique<ObservationActionSink>(),
                *m_recorder,
                engine::EngineSessionConfig{
                    .liveFingerprint    = observationFingerprint(),
                    .projectFingerprint = observationFingerprint(),
                    .maximumPixelComparisons = 1'000U,
                    .recognitionTimeout      = std::chrono::seconds{1},
                }
            );
            REQUIRE(session.has_value());
            m_context.emplace(*std::move(session), *m_recorder);
        }

        ObservationRuntime(ObservationRuntime const&) = delete;
        ObservationRuntime(ObservationRuntime&&) = delete;
        auto operator=(ObservationRuntime const&) -> ObservationRuntime& = delete;
        auto operator=(ObservationRuntime&&) -> ObservationRuntime& = delete;
        ~ObservationRuntime() = default;

        [[nodiscard]] auto context() noexcept -> task::TaskContext&
        {
            return *m_context;
        }
    };

    // A TaskHost with one activated production generation, and the runtime whose
    // frames it observes. Held behind unique_ptr wherever a fixture is returned
    // by value: TaskHost is neither copyable nor movable on purpose.
    struct ObservationHost final
    {
        std::unique_ptr<task::TaskHost>     host;
        std::unique_ptr<ObservationRuntime> runtime;
        GenerationId                        generation;
    };

    [[nodiscard]]
    inline auto activateObservationHost(
        task::InstalledRuntimeArtifact installed,
        std::vector<std::byte> framePixels,
        FrameId frameId
    ) -> ObservationHost
    {
        auto host       = std::make_unique<task::TaskHost>();
        auto generation = host->activateRuntimeArtifact(std::move(installed));
        REQUIRE(generation.has_value());
        return ObservationHost{
            .host    = std::move(host),
            .runtime = std::make_unique<ObservationRuntime>(
                observationFrame(std::move(framePixels), frameId)
            ),
            .generation = *generation,
        };
    }

    [[nodiscard]]
    inline auto observeOnce(ObservationHost& observation) -> task::UiObservationSnapshot
    {
        auto result = observation.host->observe(
            observation.generation,
            observation.runtime->context()
        );
        REQUIRE(result.has_value());
        return *std::move(result);
    }
}
