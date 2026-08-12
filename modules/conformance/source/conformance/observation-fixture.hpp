#pragma once

#include "host-delivery-fixture.hpp"

#include <operator/ledger.hpp>
#include <operator/runtime-installation.hpp>

#include <task/host-delivery.hpp>
#include <task/runtime-model-file.hpp>
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

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// A real RuntimeArtifact, a real EngineSession over the frame the supplying
// project captured, and the TaskHost that turns the two into one
// task::UiObservationSnapshot.
//
// Nothing here describes a world. The model and the capture that satisfies it
// are both read out of the project directory, and the geometry the model was
// authored at is published by the RuntimeModelBinding the Host produced from
// that model -- so a suite holding any of the three would be asking whether ITS
// world resolves rather than whether the supplying project's does.
//
// It is shared rather than duplicated because createSnapshot composes an
// observation the Host minted, and UiObservationSnapshot's only friend is
// TaskHost: no test can assemble one, so every caller of createSnapshot -- this
// repository's unit tests and every consuming repository's run of the exported
// suite -- has to drive a Host through one real observation cycle.
namespace uf::operator_runtime::conformance
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
    inline auto readArtifactFile(
        std::filesystem::path const& path
    ) -> std::string
    {
        auto stream = std::ifstream{path, std::ios::binary};
        REQUIRE(stream.good());
        auto bytes = std::string{
            std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{},
        };
        REQUIRE_FALSE(stream.bad());
        return bytes;
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

    // Wraps the RuntimeArtifact a project already published in the handoff shape
    // the installer takes. Nothing here writes an artifact: the manifest naming
    // every file in it is the project's own, and its bytes are what the root
    // hash is of, so a suite that re-serialized one would install an artifact no
    // project ever published and pin sessions to a hash no project can restate.
    [[nodiscard]]
    inline auto observationRelease(
        std::filesystem::path const& root,
        std::filesystem::path const& artifactDirectory
    ) -> ObservationRelease
    {
        auto const handoff = root / "release";
        auto error         = std::error_code{};
        std::filesystem::create_directories(handoff, error);
        REQUIRE_FALSE(error);
        std::filesystem::copy(
            artifactDirectory,
            handoff / "runtime-artifact",
            std::filesystem::copy_options::recursive
                | std::filesystem::copy_options::overwrite_existing,
            error
        );
        REQUIRE_FALSE(error);

        auto const artifactRootHash = observationHash(readArtifactFile(
            artifactDirectory
            / std::filesystem::path{task::k_runtimeArtifactManifestFileName}
        ));
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

    // Fails the running case unless the capture a project supplied is the extent
    // its model declares.
    //
    // Both halves are the supplying project's own -- the PNG it handed over and
    // the base_resolution its model states, republished by the binding the Host
    // parsed that model into -- so this is the one place that can name the
    // disagreement in numbers the project can act on.
    //
    // It is not the only thing that refuses such a pair, and it is the only one
    // that says why. Measured 2026-08-11 by deleting this call and running a
    // project whose capture is one pixel wider than its model: the run stayed
    // red, at requireResolvedSurface, over
    // {"kind":"unknown_state","reason":"unknown_scene_competitor"} -- a
    // script-visible reason from a closed vocabulary that names no extent. A
    // project that met only that would be told its model resolved nothing and
    // left to work out why.
    inline auto requireProbeGeometry(
        std::span<std::byte const> probeFrame,
        ProjectFingerprint const& fingerprint
    ) -> void
    {
        auto const decoded = image::decodePng(probeFrame, "contract-probe-frame.png");
        REQUIRE(decoded.has_value());

        auto const extentMatches = (
            decoded->width == fingerprint.width()
            && decoded->height == fingerprint.height()
        );
        REQUIRE_MESSAGE(
            extentMatches,
            "the supplied probe frame is not the extent this project's model "
            "declares: capture ",
            decoded->width,
            "x",
            decoded->height,
            ", model ",
            fingerprint.width(),
            "x",
            fingerprint.height()
        );
    }

    // The project's own capture, decoded into the Bgra8 shape a live one has.
    //
    // The extent is the PNG's rather than the fingerprint's, deliberately: a
    // capture is whatever the target produced, and stamping the fingerprint's
    // extent onto a differently sized buffer would hide the disagreement
    // outright.
    //
    // It is EngineSession's ensureCompatibleFrame -- not this fixture -- that
    // decides whether the capture describes the same pixels the model was
    // authored in, which is what lets a case deliberately build a mismatched
    // world. requireProbeGeometry above is what a project SUPPLYING one meets
    // first.
    [[nodiscard]]
    inline auto observationFrame(
        std::span<std::byte const> probeFrame,
        FrameId id
    ) -> Frame
    {
        auto decoded = image::decodePng(probeFrame, "contract-probe-frame.png");
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
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): REQUIRE above proved engagement.
        auto const ceiling = checkedMultiply(*pixels, *pixels);
        REQUIRE(ceiling.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): REQUIRE above proved engagement.
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

        // One decoded PNG, returned for as long as this source exists. Nothing
        // it produces can change, so the interval between a capture and an
        // action over it measures how long the suite took and says nothing
        // about a target. Declaring that is what lets a case that resolves a
        // real model over real card rectangles reach its click in an
        // unoptimized build; a Live answer here would be a claim this source
        // cannot support.
        [[nodiscard]] auto targetWorld() const noexcept -> TargetWorld override
        {
            return TargetWorld::Recorded;
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

        // Counts and discards; no verb here reaches a window. It must agree
        // with ObservationFrameSource above or EngineSession::create refuses
        // the session, which is the check that keeps a recorded capture from
        // ever driving a sink that posts for real.
        [[nodiscard]] auto targetWorld() const noexcept -> TargetWorld override
        {
            return TargetWorld::Recorded;
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
        ObservationRuntime(
            std::span<std::byte const> probeFrame,
            ProjectFingerprint const& fingerprint,
            FrameId frameId
        )
        {
            auto frame = observationFrame(probeFrame, frameId);

            auto recorder = trace::TraceRecorder::create(
                std::make_unique<ObservationTraceSink>(),
                trace::TraceStreamSpec{
                    .sessionId           = "conformance-observation",
                    .sessionManifestHash = observationHash("observation-manifest"),
                    .producer            = "conformance",
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
                    .liveFingerprint    = fingerprint,
                    .projectFingerprint = fingerprint,

                    // Ten seconds rather than one: a project's frame may be a
                    // real client area, and the matcher converts all of it to
                    // gray once per search. The CTest timeout is what bounds a
                    // matcher that never finishes.
                    //
                    // It does not have to be reconciled with the action lease.
                    // Over a recorded target there is no lease deadline, so a
                    // search that runs to nine seconds still ends in a click
                    // rather than in a refusal nobody configured.
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
            // The constructor emplaces m_context after REQUIRE-ing every step
            // that feeds it, so engagement is a class invariant rather than a
            // fact on this path, and the check's dataflow does not cross a
            // constructor. `.value()` was measured in place of `*` here:
            // clang-tidy reports the same diagnostic for it, so rewriting the
            // access buys nothing.
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): a constructed ObservationRuntime holds a context.
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

    // The geometry the model of `generation` declares. It is read back through
    // the binding rather than carried alongside the capture because the model is
    // a Luau value: after the Q2 ruling the trusted parser publishes
    // base_resolution and base_dpi, and no document beside the model restates
    // them. See docs/archive/plans/2026-08-11-project-as-data.md 7.0 Q2.
    [[nodiscard]]
    inline auto declaredFingerprint(
        task::TaskHost& host,
        GenerationId generation
    ) -> ProjectFingerprint
    {
        auto const binding = host.runtimeModelBinding(generation);
        REQUIRE(binding.has_value());
        return binding->fingerprint();
    }

    [[nodiscard]]
    inline auto activateObservationHost(
        task::InstalledRuntimeArtifact installed,
        std::span<std::byte const> probeFrame,
        FrameId frameId
    ) -> ObservationHost
    {
        auto host       = std::make_unique<task::TaskHost>();
        auto generation = host->activateRuntimeArtifact(std::move(installed));
        REQUIRE(generation.has_value());

        // Here rather than before the install, because this is the first moment
        // the extent exists: the Host has just parsed the model, and the
        // ObservationRuntime built below is what needs the same fingerprint.
        // It is still ahead of every resolution, which is what keeps a mismatched
        // extent out of the causes requireResolvedSurface has to explain.
        auto const fingerprint = declaredFingerprint(*host, *generation);
        requireProbeGeometry(probeFrame, fingerprint);
        return ObservationHost{
            .host    = std::move(host),
            .runtime = std::make_unique<ObservationRuntime>(
                probeFrame,
                fingerprint,
                frameId
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

    // Fails the running case unless `snapshot` resolved `surface`.
    //
    // Every property the suite goes on to test is downstream of this: a plan is
    // frozen against a resolved state and a dispatch is delivered against a
    // binding of it. Without this check a probe frame the model does not satisfy
    // -- a capture of the wrong screen, an asset that no longer crops the same
    // pixels -- reaches the case as some later refusal about authority or
    // delivery, which is the diagnosis the supplying project cannot act on.
    //
    // What it prints is the resolution itself, which is all it has: the reasons
    // a resolver records are a closed script-visible vocabulary, and a locator
    // that ran and matched nothing is `absent` and carries no reason at all, so
    // nothing here can say which pixels differed.
    // The one cause that could be named precisely -- a capture whose extent is
    // not the model's -- is named by requireProbeGeometry above, which
    // activateObservationHost runs before any resolution, and is therefore not
    // among the causes that reach here.
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
    // Host, its own generation and its own frame.
    //
    // Freshness is not among the reasons. A frame built here is the same
    // recorded bytes a frame built in prepareStore is, and the engine leases
    // both without a deadline; a fixture that had to be built late to stay
    // inside a wall-clock bound would be one whose passing depended on how fast
    // the build it was compiled by happens to run.
    class DeliveringHost final
    {
        // Declared before m_runtime and in this order on purpose: the runtime
        // needs the geometry, and the geometry does not exist until this Host
        // has activated the artifact and parsed its model.
        std::unique_ptr<task::TaskHost>     m_host;
        GenerationId                        m_generation;
        ProjectFingerprint                  m_fingerprint;
        std::unique_ptr<ObservationRuntime> m_runtime;
        std::unique_ptr<ObservationRuntime> m_other{};

        // The action the project named. It is stored rather than passed to each
        // call because the chunk that mints a Receipt and the check that reads
        // one must name the same action across a whole dispatch.
        task::UiActionUnderTest m_action;

        // The project's own capture, kept because deliverIntoAnotherCycle builds
        // a second runtime over it on demand. Owned rather than borrowed: a
        // DeliveringHost outlives the call that made it, so a view of the
        // caller's loaded project would be a stored borrow with no contract.
        std::vector<std::byte> m_probe;

        auto mint() -> void
        {
            auto const minted = task::TaskHostTestAccess::run(
                *m_host,
                m_generation,
                m_runtime->context(),
                task::authorizeActionSource(m_action)
            );
            REQUIRE(minted.has_value());
        }

    public:
        DeliveringHost(
            task::InstalledRuntimeArtifact installed,
            task::ControlFence fence,
            task::UiActionUnderTest action,
            std::vector<std::byte> probeFrame
        )
            : m_host{std::make_unique<task::TaskHost>()}
            , m_generation{
                  activateDeliveringGeneration(*m_host, std::move(installed))
              }
            , m_fingerprint{declaredFingerprint(*m_host, m_generation)}
            , m_runtime{std::make_unique<ObservationRuntime>(
                  probeFrame,
                  m_fingerprint,
                  FrameId{701}
              )}
            , m_action{std::move(action)}
            , m_probe{std::move(probeFrame)}
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
                    m_fingerprint,
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
        std::vector<std::byte> probeFrame
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
            std::move(probeFrame)
        );
    }
}
