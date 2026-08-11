#pragma once

#include <operator-contract/host-delivery-fixture.hpp>
#include <operator-contract/project-under-test.hpp>

#include <operator/ledger.hpp>
#include <operator/runtime-installation.hpp>

#include <task/host-delivery.hpp>
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
    // The two grays the probe frame below carries. Every supplied RuntimeModel
    // matches its locators against that frame, so a project authoring a model
    // for the suite authors its template assets from these: templatePng() turns
    // one into the one-pixel PNG a template locator matches. They are distinct
    // so that a frame carrying one and not the other resolves to a different
    // state.
    inline constexpr auto k_anchorGray = uint8{2};
    inline constexpr auto k_actionGray = uint8{5};

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

    // A release handoff the Operator's installer accepts, carrying the project's
    // own RuntimeArtifact rather than a placeholder or a model of the suite's:
    // the Host has to parse those bytes and match their locators before an
    // observation exists at all.
    struct ObservationRelease final
    {
        std::filesystem::path handoffRoot;
        ContentHash           releaseManifestHash;
        ContentHash           artifactRootHash;
    };

    [[nodiscard]]
    inline auto observationRelease(
        std::filesystem::path const& root,
        ProjectRuntimeArtifact const& artifact
    ) -> ObservationRelease
    {
        auto const handoff          = root / "release";
        auto const artifactRootHash = publishRuntimeArtifact(
            handoff / "runtime-artifact",
            artifact.model,
            artifact.assets
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

    // The world every supplied RuntimeModel is resolved against: three pixels
    // wide, one high, at 96 DPI. A model the suite is handed must declare
    // base_resolution = [3, 1] and base_dpi = [96, 96], because the session
    // refuses a project fingerprint the live one does not match.
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

    // The probe frame the suite captures. A supplied model reaches a resolved
    // state exactly when one of its scenes is satisfied by these three pixels,
    // which is what a project authors its locators and template assets against.
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
    //
    // Clicks are counted because "the Host posted nothing" is the only evidence
    // a NotDelivered report can be checked against, and a click can be refused
    // because a refused click is what a real sink cannot describe: the post may
    // have reached the target before the failure. EngineSession::clickPoint
    // reports one Err for every phase, so refusing here is the only way to reach
    // the outcome that deliberately under-claims.
    class ObservationActionSink final : public engine::IActionSink
    {
        uint32 m_clicks{};
        bool   m_refuseClicks{};

    public:
        [[nodiscard]] auto clicks() const noexcept -> uint32 { return m_clicks; }

        auto refuseClicks() noexcept -> void { m_refuseClicks = true; }

        [[nodiscard]]
        auto click(Point<ClientSpace>, ObservationLease const&) -> Status override
        {
            if (m_refuseClicks)
            {
                return fail(
                    AutomationErrorKind::TargetUnavailable,
                    "the observation action sink refused this click"
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
        ObservationActionSink*                m_pActions{};
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

            auto actions = std::make_unique<ObservationActionSink>();
            m_pActions   = actions.get();
            auto session = engine::EngineSession::create(
                std::make_unique<ObservationFrameSource>(std::move(value)),
                std::move(actions),
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

        [[nodiscard]] auto actions() noexcept -> ObservationActionSink&
        {
            return *m_pActions;
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

    [[nodiscard]]
    inline auto activateDeliveringGeneration(
        task::TaskHost& host,
        task::InstalledRuntimeArtifact installed
    ) -> GenerationId
    {
        auto generation = host.activateRuntimeArtifact(std::move(installed));
        REQUIRE(generation.has_value());
        return *generation;
    }

    // One Host that can act, held for as long as a case needs to dispatch.
    //
    // It is its own Host rather than the prepared store's observing one because
    // observe.luau's template cache is keyed by the RuntimeModel and outlives
    // the TaskContext that registered its handles: a second TaskContext on one
    // Host generation resolves nothing. A delivery therefore brings its own
    // Host, its own generation and its own frame -- which also keeps the frame
    // inside the engine's action-freshness bound, since it is captured when the
    // Host is built rather than whenever the case started.
    class DeliveringHost final
    {
        std::unique_ptr<task::TaskHost>     m_host;
        std::unique_ptr<ObservationRuntime> m_runtime;
        std::unique_ptr<ObservationRuntime> m_other{};
        GenerationId                        m_generation;

        // The action the project named. It is stored rather than passed to each
        // call because the chunk that mints a Receipt and the check that reads
        // one must name the same action across a whole dispatch.
        task::UiActionUnderTest m_action;

        auto mint() -> void
        {
            auto const minted = task::TaskHostTestAccess::run(
                *m_host,
                m_generation,
                m_runtime->context(),
                task::authorizeClickSource(m_action)
            );
            REQUIRE(minted.has_value());
        }

    public:
        DeliveringHost(
            task::InstalledRuntimeArtifact installed,
            task::ControlFence fence,
            task::UiActionUnderTest action
        )
            : m_host{std::make_unique<task::TaskHost>()}
            , m_runtime{
                  std::make_unique<ObservationRuntime>(
                      observationFrame(resolvedFramePixels(), FrameId{701})
                  )
              }
            , m_generation{
                  activateDeliveringGeneration(*m_host, std::move(installed))
              }
            , m_action{std::move(action)}
        {
            // Minting is refused until a ledger fence is adopted, so this is
            // where a Host stops being inert.
            REQUIRE(
                task::TaskHostTestAccess::adoptControlFence(
                    *m_host,
                    std::move(fence)
                ).has_value()
            );
        }

        DeliveringHost(DeliveringHost const&) = delete;
        DeliveringHost(DeliveringHost&&) = delete;
        auto operator=(DeliveringHost const&) -> DeliveringHost& = delete;
        auto operator=(DeliveringHost&&) -> DeliveringHost& = delete;
        ~DeliveringHost() = default;

        [[nodiscard]] auto generation() const noexcept -> GenerationId
        {
            return m_generation;
        }

        [[nodiscard]] auto clicks() const noexcept -> uint32
        {
            return m_runtime->actions().clicks();
        }

        auto refuseClicks() noexcept -> void
        {
            m_runtime->actions().refuseClicks();
        }

        auto adoptFence(task::ControlFence fence) -> void
        {
            REQUIRE(
                task::TaskHostTestAccess::adoptControlFence(
                    *m_host,
                    std::move(fence)
                ).has_value()
            );
        }

        // Mints one Receipt and presents it under `authority`. The Result is
        // returned rather than unwrapped: an Err means nothing was consumed, and
        // a case that could not see that difference could not tell a refusal
        // from a delivery nobody recorded.
        [[nodiscard]]
        auto deliver(task::DispatchAuthority authority)
            -> Result<task::HostDeliveryReport>
        {
            mint();
            auto const receipt = task::TaskHostTestAccess::pendingReceipt(
                *m_host,
                m_action
            );
            return task::TaskHostTestAccess::deliver(
                *m_host,
                std::move(authority),
                receipt,
                m_runtime->context()
            );
        }

        [[nodiscard]]
        auto deliverReport(task::DispatchAuthority authority)
            -> task::HostDeliveryReport
        {
            auto report = deliver(std::move(authority));
            REQUIRE(report.has_value());
            return *std::move(report);
        }

        // Mints one Receipt in this Host's cycle and presents it to a second
        // context holding a cycle of its own. The Receipt is consumed and no
        // click is posted, which is how a fixture reaches the one outcome that
        // proves an external effect absent.
        [[nodiscard]]
        auto deliverIntoAnotherCycle(task::DispatchAuthority authority)
            -> task::HostDeliveryReport
        {
            mint();
            if (!m_other)
            {
                m_other = std::make_unique<ObservationRuntime>(
                    observationFrame(resolvedFramePixels(), FrameId{702})
                );
                // The other context must hold a cycle of its own, or the refusal
                // proves only that it has none.
                auto const opened = task::TaskHostTestAccess::run(
                    *m_host,
                    m_generation,
                    m_other->context(),
                    "return observe.open(project.load_project()) ~= nil"
                );
                REQUIRE(opened.has_value());
            }
            auto const receipt = task::TaskHostTestAccess::pendingReceipt(
                *m_host,
                m_action
            );
            auto report = task::TaskHostTestAccess::deliver(
                *m_host,
                std::move(authority),
                receipt,
                m_other->context()
            );
            REQUIRE(report.has_value());
            REQUIRE(m_other->actions().clicks() == 0U);
            return *std::move(report);
        }
    };

    // A Host that can act under `lease`, activated from the artifact the store
    // already installed. Every prepared-store fixture wraps this, so there is
    // one spelling of "open the installed artifact again and adopt the fence".
    [[nodiscard]]
    inline auto deliveringHostFor(
        OperatorCoordinator& store,
        ControlLease const& lease,
        uint64 installedGeneration,
        ContentHash const& artifactRootHash,
        task::UiActionUnderTest const& action
    ) -> std::unique_ptr<DeliveringHost>
    {
        auto installed = store.openInstalledRuntimeArtifact(
            installedGeneration,
            artifactRootHash
        );
        REQUIRE(installed.has_value());
        return std::make_unique<DeliveringHost>(
            *std::move(installed),
            controlFence(lease),
            action
        );
    }
}
