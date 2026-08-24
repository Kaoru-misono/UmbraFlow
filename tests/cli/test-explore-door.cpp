// The one production admission door, entered from the annotation side.
//
// tests/cli/test-observe.cpp already covers that a ProductLifecycle composes at
// all, so what is only covered here is that `explore` reaches the SAME door and
// that an exploration session admitted through it is judged by the Operator's
// policy artifact -- which, when the Operator wrote none, is the deny-all
// artifact.
//
// That is the guarantee the four-step annotation order exists for: under
// deny-all an exploration session may look at the screen and may not touch it,
// and the refusal names the grant the Operator did not make
// (docs/decisions/2026-08-24-the-annotation-policy-is-the-operators.md).
//
// The recorded seam is modules/conformance's own, exactly as test-observe uses
// it: an ObservationFrameSource over the capture the exemplar project published,
// declaring TargetWorld::Recorded.

#include <cli/args.hpp>
#include <cli/explore.hpp>

#include <conformance/observation-fixture.hpp>

#include <operator/agent-profile.hpp>
#include <operator/controller.hpp>
#include <operator/ledger.hpp>
#include <operator/tool-invocation.hpp>

#include <service/product-lifecycle.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/ids.hpp>
#include <domain/key.hpp>
#include <domain/space.hpp>

#include <engine/ports.hpp>

#include <json/value.hpp>

#include <ocr/engine.hpp>

#include <task/exploration-session.hpp>
#include <task/task-host.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::cli
{
    namespace
    {
        // The geometry the exemplar project's model declares, which is what a
        // live target would have to present for the engine to accept a capture
        // at all.
        constexpr auto k_recordedWidth  = uint32{3};
        constexpr auto k_recordedHeight = uint32{1};
        constexpr auto k_recordedDpi    = uint32{96};

        constexpr auto k_observeTool = std::string_view{
            "framework.screen.observe"
        };
        constexpr auto k_deliverInputTool = std::string_view{
            "framework.input.deliver"
        };

        // The exact sentence the Operator's admission refuses a Privileged
        // surface with. Written out rather than matched on a substring: what
        // this case is about is that the refusal NAMES the missing grant and the
        // Tool it was missing for, and a case asking only whether an injection
        // failed would pass on a malformed argument.
        constexpr auto k_privilegedRefusal = std::string_view{
            "Operator policy grants no Privileged surface to tool "
            "framework.input.deliver"
        };

        // Counts every verb the engine was asked to post, through a counter the
        // case owns: EngineSession destroys the sink inside the object under
        // test, so a sink that answered for itself would be dangling exactly
        // where the assertion needs it.
        class CountingActionSink final : public engine::IActionSink
        {
            std::shared_ptr<uint32> m_delivered;

        public:
            explicit CountingActionSink(std::shared_ptr<uint32> delivered) noexcept
                : m_delivered{std::move(delivered)}
            {
            }

            [[nodiscard]]
            auto click(Point<ClientSpace>, ObservationLease const&) -> Status override
            {
                ++*m_delivered;
                return ok();
            }

            [[nodiscard]] auto pressKey(KeyName, TargetGeneration) -> Status override
            {
                ++*m_delivered;
                return ok();
            }

            [[nodiscard]] auto scroll(int32, ObservationLease const&) -> Status override
            {
                ++*m_delivered;
                return ok();
            }

            [[nodiscard]]
            auto engageHold(Point<ClientSpace>, ObservationLease const&)
                -> Status override
            {
                ++*m_delivered;
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
                ++*m_delivered;
                return ok();
            }

            [[nodiscard]]
            auto movePointer(
                Point<ClientSpace>,
                ObservationLease const&
            ) -> Status override
            {
                ++*m_delivered;
                return ok();
            }

            [[nodiscard]] auto releaseHeldInputs() -> Status override
            {
                return ok();
            }

            [[nodiscard]] auto targetWorld() const noexcept -> TargetWorld override
            {
                return TargetWorld::Recorded;
            }
        };

        // A bound OCR adapter that answers nothing. It stands for an adapter
        // that is present: a null one is a different case, and not this one.
        class SilentReader final : public ocr::IOcrEngine
        {
        public:
            [[nodiscard]] auto identity() const noexcept -> std::string_view override
            {
                return "test-silent-reader";
            }

            [[nodiscard]]
            auto read(BgraImage const&, ocr::ReadSpec const&)
                -> Result<ocr::Readout> override
            {
                return ocr::Readout{};
            }
        };

        // The exemplar project copied out of the repository, its RuntimeArtifact
        // installed into an Operator production root beside it, and NO policy
        // artifact written into that root.
        //
        // The absence is the fixture. An Operator that has written no
        // policy-artifact.json is what a developer's machine looks like the
        // first time they annotate, and the deny-all resolution that follows is
        // what this file is about.
        class ExploreDoorWorld final
        {
            std::filesystem::path  m_root{};
            std::filesystem::path  m_project{};
            std::filesystem::path  m_runtime{};
            std::vector<std::byte> m_probe{};

        public:
            ExploreDoorWorld()
            {
                m_root = (
                    std::filesystem::temp_directory_path()
                    / std::filesystem::path{
                        "uf-explore-door-"
                            + std::to_string(std::random_device{}()),
                    }
                );
                m_project = m_root / "project";
                m_runtime = m_root / "production";
                std::filesystem::remove_all(m_root);
                std::filesystem::create_directories(m_root);
                std::filesystem::copy(
                    std::filesystem::path{UF_STAGED_UMBRAFLOW_PROJECT},
                    m_project,
                    std::filesystem::copy_options::recursive
                );

                auto stream = std::ifstream{
                    m_project / "runtime" / "probe-frame.png",
                    std::ios::binary,
                };
                REQUIRE(stream.good());
                auto const text = std::string{
                    std::istreambuf_iterator<char>{stream},
                    std::istreambuf_iterator<char>{},
                };
                m_probe.reserve(text.size());
                for (auto const value : text)
                {
                    m_probe.emplace_back(
                        static_cast<std::byte>(static_cast<unsigned char>(value))
                    );
                }

                auto const release = operator_runtime::conformance::observationRelease(
                    m_root / "handoff",
                    m_project / "runtime" / "artifact"
                );

                // Scoped, so the SQLite handle this fixture opened is closed
                // before the lifecycle opens the same root.
                auto store = operator_runtime::OperatorCoordinator::open(m_runtime);
                REQUIRE(store.has_value());
                auto const installed = store->installRuntimeArtifact(
                    operator_runtime::RuntimeArtifactInstallRequest{
                        .handoffRoot                 = release.handoffRoot,
                        .expectedReleaseManifestHash = release.releaseManifestHash,
                        .expectedInstalledGeneration = 0U,
                    }
                );
                auto const installedWhy = installed.has_value()
                    ? std::string{}
                    : std::string{installed.error().message()};
                REQUIRE_MESSAGE(installed.has_value(), installedWhy);
            }

            ExploreDoorWorld(ExploreDoorWorld const&)                    = delete;
            ExploreDoorWorld(ExploreDoorWorld&&)                         = delete;
            auto operator=(ExploreDoorWorld const&) -> ExploreDoorWorld& = delete;
            auto operator=(ExploreDoorWorld&&) -> ExploreDoorWorld&      = delete;

            ~ExploreDoorWorld()
            {
                auto discarded = std::error_code{};
                std::filesystem::remove_all(m_root, discarded);
            }

            [[nodiscard]] auto project() const -> std::filesystem::path const&
            {
                return m_project;
            }

            [[nodiscard]] auto runtime() const -> std::filesystem::path const&
            {
                return m_runtime;
            }

            [[nodiscard]] auto path(std::string_view name) const
                -> std::filesystem::path
            {
                return m_root / std::filesystem::path{name};
            }

            [[nodiscard]]
            auto ports(std::shared_ptr<uint32> delivered, std::string_view trace)
                const -> task::TaskRunConfig
            {
                auto const fingerprint = ProjectFingerprint::create(
                    k_recordedWidth,
                    k_recordedHeight,
                    k_recordedDpi,
                    k_recordedDpi
                );
                REQUIRE(fingerprint.has_value());
                return task::TaskRunConfig{
                    .frameSource = std::make_unique<
                        operator_runtime::conformance::ObservationFrameSource
                    >(
                        operator_runtime::conformance::observationFrame(
                            m_probe,
                            FrameId{4001}
                        )
                    ),
                    .actionSink = std::make_unique<CountingActionSink>(
                        std::move(delivered)
                    ),
                    .ocrEngine               = std::make_unique<SilentReader>(),
                    .liveFingerprint         = *fingerprint,
                    .maximumPixelComparisons = k_defaultPixelComparisonBudget,
                    .recognitionTimeout      = k_defaultRecognitionTimeout,
                    .tracePath               = path(trace),
                };
            }
        };

        [[nodiscard]]
        auto explorationCall(
            std::string_view toolName,
            std::string_view exactArgumentsJcs
        ) -> service::FrameworkToolCall
        {
            auto const identity = [](std::string_view material)
            {
                auto const hashed = sha256(std::as_bytes(std::span{material}));
                REQUIRE(hashed.has_value());
                return *hashed;
            };
            return service::FrameworkToolCall{
                .requestKey                  = "annotation-under-deny-all",
                .exactRootRequestPreimageJcs = R"({"objective":"annotate"})",
                .executionIdentity = operator_runtime::ToolExecutionIdentity{
                    .runIdentity              = identity("exploration-run"),
                    .frameworkReleaseIdentity = identity("framework-release"),
                    .toolRuntimeProtocolIdentity = identity(
                        "tool-runtime-protocol"
                    ),
                    .environmentIdentity = identity("annotation-environment"),
                },
                .toolName          = std::string{toolName},
                .exactArgumentsJcs = std::string{exactArgumentsJcs},
            };
        }
    }

    // The guarantee, stated as one case because the two halves are one rule:
    // deny-all is not "an exploration session cannot run", it is "it may see and
    // may not act".
    TEST_CASE(
        "an exploration session under deny-all observes the screen and is "
        "refused an input injection"
    )
    {
        auto const world     = ExploreDoorWorld{};
        auto const delivered = std::make_shared<uint32>();

        auto const scope = operator_runtime::ObservedInstanceWorldScope::run(
            "window-0",
            1
        );
        REQUIRE(scope.has_value());

        // Exactly the ProductStart cli::exploreProject builds, because there is
        // one door and this case is about what happens on the far side of it.
        auto lifecycle = service::ProductLifecycle::start(
            service::ProductStart{
                .projectDirectory          = world.project(),
                .runtimeDirectory          = world.runtime(),
                .authenticatedControllerId = "umbra-flow-explore",
                .controllerCapabilities    = {},
                .controlledTargetId        = "window-0",
                .kind                      = operator_runtime::ControllerKind::Human,
                .agentProfileJcs = std::string{
                    operator_runtime::k_unboundedAgentProfileJcs
                },
                .worldScope = *scope,
            }
        );
        auto const lifecycleWhy = lifecycle.has_value()
            ? std::string{}
            : std::string{lifecycle.error().message()};
        REQUIRE_MESSAGE(lifecycle.has_value(), lifecycleWhy);

        auto session = lifecycle->startExplorationSession(
            world.ports(delivered, "annotation-trace.jsonl"),
            std::stop_token{}
        );
        auto const sessionWhy = session.has_value()
            ? std::string{}
            : std::string{session.error().message()};
        REQUIRE_MESSAGE(session.has_value(), sessionWhy);

        auto& context = (*session)->context();

        // The 16 `explore_*` natives still reach the screen, over the engine
        // session the LIFECYCLE built rather than one this session opened for
        // itself. They become Tools in the step after this one; what this
        // asserts is that moving where the session comes from did not move how
        // they reach it.
        auto const cropped = (*session)->evaluate(
            R"lua(
                local blob = explore.cycle(function(cycle)
                    return cycle:crop(0, 0, 1, 1)
                end)
                local measured = explore.probe(blob, 0, 0, 1, 1)
                return type(blob) == "string" and #blob > 0
                    and measured.image_width == 1
                    and measured.image_height == 1
            )lua",
            "annotation-native-surface"
        );
        auto const croppedWhy = cropped.has_value()
            ? std::string{}
            : std::string{cropped.error().message()};
        REQUIRE_MESSAGE(cropped.has_value(), croppedWhy);
        CHECK(cropped->boolean() == std::optional<bool>{true});

        // Read-only screen observation carries no effect bounds, so deny-all
        // admits it. This is the half that would be lost if annotation answered
        // deny-all by refusing the session outright.
        auto const observed = lifecycle->invokeFrameworkTool(
            explorationCall(k_observeTool, "{}"),
            context
        );
        auto const observedWhy = observed.has_value()
            ? std::string{}
            : std::string{observed.error().message()};
        REQUIRE_MESSAGE(observed.has_value(), observedWhy);
        CHECK(observed->state == operator_runtime::ToolCallState::Confirmed);
        REQUIRE(observed->payload.has_value());
        auto const resolution = json::parse(observed->payload->bytes());
        REQUIRE(resolution.has_value());
        CHECK(resolution->find("state_resolution") != nullptr);

        // And the half the whole four-step order exists to repay: an injection
        // is refused at admission, by name, before the sink is reached.
        auto const injected = lifecycle->invokeFrameworkTool(
            explorationCall(
                k_deliverInputTool,
                R"({"action":"click","x":0,"y":0})"
            ),
            context
        );
        REQUIRE_FALSE(injected.has_value());
        CHECK(injected.error().message() == k_privilegedRefusal);
        CHECK(
            automationErrorKind(injected.error())
            == std::optional<AutomationErrorKind>{
                AutomationErrorKind::ActionRejected
            }
        );
        CHECK(*delivered == 0U);

        // Destroyed before the lifecycle closes, because the run this session
        // records into is the one the close ends.
        session->reset();
        CHECK(lifecycle->shutdown().has_value());
    }

    // The door itself. An Operator root holding no installed generation for this
    // project refuses the session rather than bootstrapping one -- which is what
    // separates going through the production door from opening the project's own
    // unsealed directory and calling it a session.
    TEST_CASE(
        "explore is refused an Operator root that installed nothing for this "
        "project"
    )
    {
        auto const world = ExploreDoorWorld{};

        auto const queue = world.path("queue.jsonl");
        {
            auto stream = std::ofstream{queue, std::ios::binary};
            REQUIRE(stream.good());
        }

        auto const args = ExploreArgs{
            .project      = world.project(),
            .windowHandle = 0,
            // An empty directory beside the real production root: an Operator
            // root this project was never released into.
            .runtime = world.path("unpublished"),
            .queue   = queue,
            .results = world.path("results.jsonl"),
            .trace   = world.path("refused-trace.jsonl"),
        };
        auto const paths = validateExploreIpcPaths(args);
        auto const pathsWhy = paths.has_value()
            ? std::string{}
            : std::string{paths.error().message()};
        REQUIRE_MESSAGE(paths.has_value(), pathsWhy);

        auto const delivered = std::make_shared<uint32>();
        auto const refused   = exploreProject(
            args,
            *paths,
            world.ports(delivered, "refused-trace.jsonl"),
            std::stop_token{}
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(
            std::string{refused.error().message()}.contains(
                "No active RuntimeArtifact is compatible with the required root"
            )
        );
    }
}
