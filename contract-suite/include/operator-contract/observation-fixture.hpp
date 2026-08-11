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
#include <core/numeric/checked-arithmetic.hpp>
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

#include <image/pixels.hpp>
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

// A real RuntimeArtifact, a real EngineSession over the frame the supplying
// project captured, and the TaskHost that turns the two into one
// task::UiObservationSnapshot.
//
// Nothing here describes a world. The model, the geometry it was authored at
// and the capture that satisfies it all arrive in ProjectUnderTest, because a
// suite holding any one of the three would be asking whether ITS world resolves
// rather than whether the supplying project's does.
//
// It is shared rather than duplicated because createSnapshot composes an
// observation the Host minted, and UiObservationSnapshot's only friend is
// TaskHost: no test can assemble one, so every caller of createSnapshot -- this
// repository's unit tests and every consuming repository's run of the exported
// suite -- has to drive a Host through one real observation cycle.
namespace uf::operator_runtime::contract
{
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

    // The project's own capture, decoded into the Bgra8 shape a live one has.
    //
    // The extent is the PNG's rather than the fingerprint's, deliberately: a
    // capture is whatever the target produced, and it is EngineSession's
    // ensureCompatibleFrame -- not this fixture -- that decides whether it
    // describes the same pixels the model was authored in. Stamping the
    // fingerprint's extent onto a differently sized buffer would hide exactly
    // the disagreement that check exists to report.
    [[nodiscard]]
    inline auto observationFrame(
        ProjectProbeFrame const& probe,
        FrameId id
    ) -> Frame
    {
        auto decoded = image::decodePng(probe.png, "contract-probe-frame.png");
        REQUIRE(decoded.has_value());
        auto const width  = decoded->width;
        auto const height = decoded->height;

        auto pixels = image::rgba8ToBgra8(std::move(decoded->pixels));
        REQUIRE(pixels.has_value());

        auto transform = CoordinateTransform::create(
            Point<DesktopSpace>{0.0F, 0.0F},
            static_cast<float>(width),
            static_cast<float>(height),
            width,
            height
        );
        REQUIRE(transform.has_value());
        auto result = Frame::create(
            id,
            CaptureSessionId{7},
            TargetGeneration::fromValue(3),
            MonotonicInstant::now(),
            width,
            height,
            static_cast<std::size_t>(width) * 4U,
            PixelFormat::Bgra8,
            std::make_shared<FrameBuffer const>(*std::move(pixels)),
            *transform
        );
        REQUIRE(result.has_value());
        return *std::move(result);
    }

    // The comparison ceiling one observation runs under.
    //
    // One search costs the searched rectangle's candidate positions times the
    // template's pixels, and both are RuntimeModel fields C++ never reads, so
    // the suite cannot state a budget in comparisons that fits every project's
    // model. What it can state is the ceiling no search over a frame of this
    // extent can exceed -- at most one candidate position per frame pixel, at
    // most one template pixel per frame pixel -- which leaves the deadline as
    // the bound that actually stops a runaway search.
    [[nodiscard]]
    inline auto observationComparisonCeiling(Frame const& frame) -> uint64
    {
        auto const pixels = checkedMultiply(
            uint64{frame.width()},
            uint64{frame.height()}
        );
        REQUIRE(pixels.has_value());
        auto const ceiling = checkedMultiply(*pixels, *pixels);
        REQUIRE(ceiling.has_value());
        return *ceiling;
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
        ObservationRuntime(ProjectProbeFrame const& probe, FrameId frameId)
        {
            auto frame = observationFrame(probe, frameId);

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

            auto const comparisonCeiling = observationComparisonCeiling(frame);
            auto session = engine::EngineSession::create(
                std::make_unique<ObservationFrameSource>(std::move(frame)),
                std::move(actions),
                *m_recorder,
                engine::EngineSessionConfig{
                    // Both fingerprints are the project's own: the suite has no
                    // live target to measure, so the capture it was handed IS
                    // the live geometry. What ensureCompatibleFrame still
                    // decides here is whether that capture's extent matches the
                    // resolution the model declares, which is the disagreement a
                    // suite carrying a frame of its own could never produce.
                    .liveFingerprint    = probe.fingerprint,
                    .projectFingerprint = probe.fingerprint,

                    // Ten seconds rather than one: a project's frame may be a
                    // real client area, and the matcher converts all of it to
                    // gray once per search. The CTest timeout is what bounds a
                    // matcher that never finishes.
                    .maximumPixelComparisons = comparisonCeiling,
                    .recognitionTimeout      = std::chrono::seconds{10},
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
        ProjectProbeFrame const& probe,
        FrameId frameId
    ) -> ObservationHost
    {
        auto host       = std::make_unique<task::TaskHost>();
        auto generation = host->activateRuntimeArtifact(std::move(installed));
        REQUIRE(generation.has_value());
        return ObservationHost{
            .host       = std::move(host),
            .runtime    = std::make_unique<ObservationRuntime>(probe, frameId),
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

    // Fails the running case unless `snapshot` resolved `surface`.
    //
    // Every property the suite goes on to test is downstream of this: a plan is
    // frozen against a resolved state and a dispatch is delivered against a
    // binding of it. Without this check a probe frame the model does not
    // satisfy -- an extent the fingerprint disagrees with, a capture of the
    // wrong screen, an asset that no longer crops the same pixels -- reaches the
    // case as some later refusal about authority or delivery, which is the
    // diagnosis the supplying project cannot act on.
    inline auto requireResolvedSurface(
        task::UiObservationSnapshot const& snapshot,
        std::string_view surface
    ) -> void
    {
        auto const& resolution = snapshot.canonicalJcs();
        REQUIRE_MESSAGE(
            resolution.contains(R"("kind":"resolved_state")"),
            "the supplied probe frame resolved no surface: ",
            resolution
        );

        constexpr auto stackMember = std::string_view{
            R"("ordered_surface_stack":[)"
        };
        auto const opened = resolution.find(stackMember);
        REQUIRE(opened != std::string::npos);
        auto const from   = opened + stackMember.size();
        auto const closed = resolution.find(']', from);
        REQUIRE(closed != std::string::npos);

        auto const stack = std::string_view{resolution}.substr(
            from,
            closed - from
        );
        REQUIRE_MESSAGE(
            stack.contains(std::format(R"("{}")", surface)),
            "the supplied probe frame resolved another surface than the one "
            "this project's uiAction names: ",
            resolution
        );
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

        // The project's own capture, kept because deliverIntoAnotherCycle builds
        // a second runtime over it on demand. Owned rather than borrowed: a
        // DeliveringHost outlives the call that made it, so a view of the
        // caller's ProjectUnderTest would be a stored borrow with no contract.
        ProjectProbeFrame m_probe;

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
            task::UiActionUnderTest action,
            ProjectProbeFrame probe
        )
            : m_host{std::make_unique<task::TaskHost>()}
            , m_runtime{std::make_unique<ObservationRuntime>(probe, FrameId{701})}
            , m_generation{
                  activateDeliveringGeneration(*m_host, std::move(installed))
              }
            , m_action{std::move(action)}
            , m_probe{std::move(probe)}
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
                    m_probe,
                    FrameId{702}
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
        task::UiActionUnderTest const& action,
        ProjectProbeFrame const& probe
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
            action,
            probe
        );
    }
}
