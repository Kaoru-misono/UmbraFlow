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
#include <conformance/operator-protocol.hpp>

#include <operator/agent-profile.hpp>
#include <operator/controller.hpp>
#include <operator/ledger.hpp>
#include <operator/policy.hpp>
#include <operator/tool-invocation.hpp>

#include <service/product-lifecycle.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
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

#include "../json/repository-path.hpp"

#include <doctest/doctest.h>

#include <sqlite3.h>

#include <algorithm>
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

        constexpr auto k_projectWriteTool = std::string_view{
            "framework.project.write"
        };
        constexpr auto k_readLinesTool = std::string_view{
            "framework.screen.read_lines"
        };
        constexpr auto k_probeTool = std::string_view{
            "framework.screen.probe"
        };

        // One durable Tool call row, joined to the admission that admitted it.
        // Six of the seven members are what "recorded in the ledger" means for
        // an annotator's input: which Tool ran, where it sat in the call tree,
        // how it ended, which session it is attributed to, and which policy
        // judged it.
        struct ToolCallRow final
        {
            std::string toolName{};
            std::string callIdentity{};
            std::string parentIdentity{};
            std::string state{};
            std::string sessionId{};
            std::string policyHash{};
            std::string canonicalArgs{};
        };

        struct HeldInputLog final
        {
            uint32 engaged{};
            uint32 released{};
            bool   held{};
            bool   observedWhileHeld{};
        };

        struct SqliteClose final
        {
            auto operator()(sqlite3* p_database) const noexcept -> void
            {
                static_cast<void>(sqlite3_close(p_database));
            }
        };

        struct SqliteFinalize final
        {
            auto operator()(sqlite3_stmt* p_statement) const noexcept -> void
            {
                static_cast<void>(sqlite3_finalize(p_statement));
            }
        };

        // What one read-only query answers over the Operator's own database, as
        // text. See ExploreDoorWorld::recordedToolCalls for why a case about
        // annotation has to read the store at all.
        [[nodiscard]]
        auto ledgerRows(
            std::filesystem::path const& databasePath,
            std::string_view query
        ) -> std::vector<std::vector<std::string>>
        {
            auto* p_openedDatabase = static_cast<sqlite3*>(nullptr);
            auto const opened      = sqlite3_open_v2(
                databasePath.string().c_str(),
                &p_openedDatabase,
                SQLITE_OPEN_READONLY,
                nullptr
            );
            auto database = std::unique_ptr<sqlite3, SqliteClose>{p_openedDatabase};
            REQUIRE(opened == SQLITE_OK);
            REQUIRE(database != nullptr);

            auto* p_preparedStatement = static_cast<sqlite3_stmt*>(nullptr);
            auto const prepared       = sqlite3_prepare_v2(
                database.get(),
                query.data(),
                static_cast<int>(query.size()),
                &p_preparedStatement,
                nullptr
            );
            auto statement = std::unique_ptr<sqlite3_stmt, SqliteFinalize>{
                p_preparedStatement,
            };
            REQUIRE(prepared == SQLITE_OK);
            REQUIRE(statement != nullptr);

            auto const columns = sqlite3_column_count(statement.get());
            auto rows = std::vector<std::vector<std::string>>{};
            auto step = sqlite3_step(statement.get());
            while (step == SQLITE_ROW)
            {
                auto row = std::vector<std::string>{};
                for (auto column = 0; column < columns; ++column)
                {
                    auto const* p_text = sqlite3_column_text(statement.get(), column);
                    REQUIRE(p_text != nullptr);
                    // SAFETY: SQLite answers with a pointer and a byte count,
                    // and sqlite3_column_bytes reports the length of the very
                    // column sqlite3_column_text just returned. A span is what
                    // names that pair without a raw pointer standing for a
                    // buffer.
                    UF_UNSAFE_BUFFER_BEGIN
                    auto const text = std::span{
                        p_text,
                        static_cast<std::size_t>(
                            sqlite3_column_bytes(statement.get(), column)
                        ),
                    };
                    UF_UNSAFE_BUFFER_END
                    row.emplace_back(text.begin(), text.end());
                }
                rows.emplace_back(std::move(row));
                step = sqlite3_step(statement.get());
            }
            REQUIRE(step == SQLITE_DONE);
            return rows;
        }

        // The published Operator protocol schema's hash, which is the one
        // ProductLifecycle pins into the SessionManifest a policy is verified
        // against. A policy artifact naming any other hash is refused.
        [[nodiscard]] auto operatorProtocolSchemaHash() -> ContentHash
        {
            constexpr auto k_operatorSchema = std::string_view{
                "schema/umbraflow-operator-v1.schema.json"
            };
            auto const root = json::repositoryRoot(k_operatorSchema);
            REQUIRE_FALSE(root.empty());
            auto stream = std::ifstream{root / k_operatorSchema, std::ios::binary};
            REQUIRE(stream.good());
            auto const bytes = std::string{
                std::istreambuf_iterator<char>{stream},
                std::istreambuf_iterator<char>{},
            };
            auto const hashed = sha256(
                std::as_bytes(std::span{std::string_view{bytes}})
            );
            REQUIRE(hashed.has_value());
            return *hashed;
        }

        // Counts every capture the engine was asked for, through a counter the
        // case owns, for CountingActionSink's reason.
        //
        // IT IS HOW "ONE FRAME" IS PROVED. An observation's body measures the
        // frame that observation is holding, so a body that read two rectangles
        // costs exactly ONE capture; a measuring Tool that captured for itself
        // would cost three, and would be answering about a screen nobody aimed
        // at (docs/decisions/2026-08-24-an-observation-frame-is-the-scope-of-its-call.md).
        class CountingFrameSource final : public engine::IFrameSource
        {
            Frame                         m_frame;
            std::shared_ptr<uint32>       m_captures;
            std::shared_ptr<HeldInputLog> m_held;

        public:
            CountingFrameSource(
                Frame frame,
                std::shared_ptr<uint32> captures,
                std::shared_ptr<HeldInputLog> held
            ) noexcept
                : m_frame{std::move(frame)}
                , m_captures{std::move(captures)}
                , m_held{std::move(held)}
            {
            }

            [[nodiscard]]
            auto capture(CaptureBudget const&) -> Result<Frame> override
            {
                ++*m_captures;
                if (m_held->held)
                {
                    m_held->observedWhileHeld = true;
                }
                return m_frame;
            }

            [[nodiscard]] auto validateTargetInstance() -> Status override
            {
                return ok();
            }

            [[nodiscard]] auto targetWorld() const noexcept -> TargetWorld override
            {
                return TargetWorld::Recorded;
            }
        };

        // Counts every verb the engine was asked to post, through a counter the
        // case owns: EngineSession destroys the sink inside the object under
        // test, so a sink that answered for itself would be dangling exactly
        // where the assertion needs it.
        class CountingActionSink final : public engine::IActionSink
        {
            std::shared_ptr<uint32>       m_delivered;
            std::shared_ptr<HeldInputLog> m_held;

        public:
            CountingActionSink(
                std::shared_ptr<uint32> delivered,
                std::shared_ptr<HeldInputLog> held
            ) noexcept
                : m_delivered{std::move(delivered)}
                , m_held{std::move(held)}
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
                ++m_held->engaged;
                m_held->held = true;
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
                ++m_held->released;
                m_held->held = false;
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
                    m_root / "source",
                    m_project / "runtime" / "artifact"
                );

                // Scoped, so the SQLite handle this fixture opened is closed
                // before the lifecycle opens the same root.
                auto store = operator_runtime::OperatorCoordinator::open(m_runtime);
                REQUIRE(store.has_value());
                auto const installed = store->installRuntimeArtifact(
                    operator_runtime::RuntimeArtifactInstallRequest{
                        .artifactDirectory           = release.artifactDirectory,
                        .artifactRootHash            = release.artifactRootHash,
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
            auto ports(
                std::shared_ptr<uint32> delivered,
                std::string_view trace,
                std::shared_ptr<uint32> captures = std::make_shared<uint32>(),
                std::shared_ptr<HeldInputLog> held =
                    std::make_shared<HeldInputLog>()
            ) const -> task::TaskRunConfig
            {
                auto const fingerprint = ProjectFingerprint::create(
                    k_recordedWidth,
                    k_recordedHeight,
                    k_recordedDpi,
                    k_recordedDpi
                );
                REQUIRE(fingerprint.has_value());
                return task::TaskRunConfig{
                    .frameSource = std::make_unique<CountingFrameSource>(
                        operator_runtime::conformance::observationFrame(
                            m_probe,
                            FrameId{4001}
                        ),
                        std::move(captures),
                        held
                    ),
                    .actionSink = std::make_unique<CountingActionSink>(
                        std::move(delivered),
                        std::move(held)
                    ),
                    .ocrEngine               = std::make_unique<SilentReader>(),
                    .liveFingerprint         = *fingerprint,
                    .maximumPixelComparisons = k_defaultPixelComparisonBudget,
                    .recognitionTimeout      = k_defaultRecognitionTimeout,
                    .tracePath               = path(trace),
                };
            }

            // The Operator's own file, written into the Operator's own root.
            //
            // THIS IS THE HAND-WRITTEN FILE THE CORRECTED ONBOARDING CLAIM NAMES
            // (docs/decisions/2026-08-24-the-annotation-policy-is-the-operators.md):
            // a developer annotates a brand-new project under deny-all as far as
            // looking at the screen, and the first annotation stroke or the
            // first injected input needs this. It grants the two Privileged
            // Tools by name and allows their effects, and it is written by the
            // Operator rather than scaffolded, because a permission the
            // requester signs for itself is a rubber stamp.
            auto authorizeAnnotation() const -> ContentHash
            {
                auto const policy =
                    operator_runtime::conformance::policyArtifactBytes(
                        operatorProtocolSchemaHash(),
                        std::vector<std::string>{
                            std::string{k_deliverInputTool},
                            std::string{k_projectWriteTool},
                        },
                        // The measuring Tools are granted here too, and only so
                        // that a case can watch one refuse for the RIGHT
                        // reason: outside a body they are refused twice over --
                        // the Privileged surface first, then the missing frame
                        // -- and a case that saw only the first would never
                        // reach the refusal it is about.
                        std::vector<std::string>{
                            std::string{k_deliverInputTool},
                            std::string{k_probeTool},
                            std::string{k_projectWriteTool},
                            std::string{k_readLinesTool},
                        }
                    );
                auto stream = std::ofstream{
                    m_runtime
                        / std::string{
                            operator_runtime::k_operatorPolicyArtifactFileName
                        },
                    std::ios::binary | std::ios::trunc,
                };
                REQUIRE(stream.good());
                stream << policy;
                REQUIRE(stream.good());
                auto const hashed = sha256(
                    std::as_bytes(std::span{std::string_view{policy}})
                );
                REQUIRE(hashed.has_value());
                return *hashed;
            }

            // Every Tool call the Operator recorded for this world, in the order
            // it recorded them, joined to the admission attempt that admitted
            // each one.
            //
            // It reads the Operator's storage because that is the subject: a
            // case that asked only whether an input landed would have passed
            // before this change, when an annotator's input reached the target
            // with no Receipt and no durable row at all. A coordinator holds the
            // file under PRAGMA locking_mode=EXCLUSIVE for its whole lifetime,
            // so this may only run once the lifecycle has been destroyed.
            [[nodiscard]] auto recordedToolCalls() const -> std::vector<ToolCallRow>
            {
                auto const rows = ledgerRows(
                    m_runtime / "operator-runtime.sqlite",
                    "SELECT position.tool_name, position.call_identity, "
                    "position.parent_call_identity, history.state, "
                    "attempt.session_id, attempt.policy_hash, "
                    "position.canonical_args "
                    "FROM tool_call_positions position "
                    "JOIN tool_call_history history "
                    "ON history.call_identity=position.call_identity "
                    "JOIN tool_admission_attempts attempt "
                    "ON attempt.call_identity=position.call_identity "
                    "AND attempt.attempt_number=history.active_admission_attempt "
                    "ORDER BY position.rowid"
                );
                auto calls = std::vector<ToolCallRow>{};
                for (auto const& row : rows)
                {
                    REQUIRE(row.size() == 7U);
                    calls.emplace_back(ToolCallRow{
                        .toolName      = row[0],
                        .callIdentity  = row[1],
                        .parentIdentity = row[2],
                        .state         = row[3],
                        .sessionId     = row[4],
                        .policyHash    = row[5],
                        .canonicalArgs = row[6],
                    });
                }
                return calls;
            }
        };

        [[nodiscard]]
        auto explorationCall(
            std::string_view toolName,
            std::string_view exactArgumentsJcs
        ) -> service::ToolRootCall
        {
            auto const identity = [](std::string_view material)
            {
                auto const hashed = sha256(std::as_bytes(std::span{material}));
                REQUIRE(hashed.has_value());
                return *hashed;
            };
            return service::ToolRootCall{
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

        // AN OBSERVATION WITH A BODY, UNDER DENY-ALL. The observation is
        // Semantic and read-only so deny-all admits it, and the measurement
        // inside its body is a CHILD call -- whose surface is judged against
        // what the observation declared it may delegate rather than against the
        // Operator's top-of-run grant. That is the whole of why a session with
        // no policy can still look at the screen
        // (docs/decisions/2026-08-24-policy-is-the-axis-and-observation-holds-a-frame.md
        // V4).
        auto const measured = (*session)->evaluate(
            R"lua(
                local screen = require("@umbraflow/screen")
                local tools = require("@umbraflow/tools")
                local resolved = screen.observe(function()
                    local answer = tools.call("framework.screen.probe", {
                        x = 0, y = 0, width = 1, height = 1,
                        colour_red = 0, colour_green = 0, colour_blue = 0,
                        tolerance = 12, removes = false,
                    })
                    local report = rawget(answer, "result")
                    if report.rect_pixels ~= 1 then
                        error("probe measured " .. tostring(report.rect_pixels))
                    end
                end)
                return tools.state(resolved) == "confirmed"
            )lua",
            "annotation-observation-body"
        );
        auto const measuredWhy = measured.has_value()
            ? std::string{}
            : std::string{measured.error().message()};
        REQUIRE_MESSAGE(measured.has_value(), measuredWhy);
        CHECK(measured->boolean() == std::optional<bool>{true});

        // A Project Tool is in the same pinned catalog and reaches the same
        // root admission door. The interactive transport does not select a
        // Framework-only catalog or a second dispatcher.
        auto const projectTool = (*session)->evaluate(
            R"lua(
                local tools = require("@umbraflow/tools")
                local answer = tools.call("fixture.alpha.observe-1", {
                    value = 1,
                })
                return rawget(rawget(answer, "result"), "outcome")
            )lua",
            "interactive-project-tool"
        );
        auto const projectToolWhy = projectTool.has_value()
            ? std::string{}
            : std::string{projectTool.error().message()};
        REQUIRE_MESSAGE(projectTool.has_value(), projectToolWhy);
        REQUIRE(projectTool->text() != nullptr);
        CHECK(*projectTool->text() == "observe-1");

        // The same session's input, refused by name from inside a chunk rather
        // than through the adapter below. The refusal reaches the chunk as the
        // Operator's own sentence, which is what an annotator has to read.
        auto const refused = (*session)->evaluate(
            R"lua(
                local tools = require("@umbraflow/tools")
                tools.call("framework.input.deliver", {
                    action = "click", x = 0, y = 0,
                })
                return "delivered"
            )lua",
            "annotation-input-under-deny-all"
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK_MESSAGE(
            std::string_view{refused.error().message()}.find(k_privilegedRefusal)
                != std::string_view::npos,
            "a chunk's input was not refused by the Operator's own sentence: ",
            refused.error().message()
        );

        // Read-only screen observation carries no effect bounds, so deny-all
        // admits it. This is the half that would be lost if annotation answered
        // deny-all by refusing the session outright.
        auto const observed = lifecycle->invokeTool(
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
        auto const injected = lifecycle->invokeTool(
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

    // THE DEBT THIS WHOLE FOUR-STEP ORDER EXISTS TO REPAY.
    //
    // Before this change an input issued while annotating went straight from a
    // private native to the engine: no Receipt, no admission, and NO LEDGER ROW
    // AT ALL. A case that only proved the input still lands would have passed
    // then and proves nothing now. What is asserted here is the row: which Tool
    // ran, which session it is attributed to, and which policy admitted it
    // (docs/decisions/2026-08-24-there-is-no-annotation-phase.md, step 4 of the
    // ordering in
    // docs/decisions/2026-08-24-policy-is-the-axis-and-observation-holds-a-frame.md).
    TEST_CASE(
        "an input issued while annotating is recorded in the ledger, with the "
        "Tool that issued it and the policy that admitted it"
    )
    {
        auto const world     = ExploreDoorWorld{};
        auto const policyHash = world.authorizeAnnotation();
        auto const delivered = std::make_shared<uint32>();

        auto const scope = operator_runtime::ObservedInstanceWorldScope::run(
            "window-0",
            1
        );
        REQUIRE(scope.has_value());

        auto sessionId = std::string{};
        {
            auto lifecycle = service::ProductLifecycle::start(
                service::ProductStart{
                    .projectDirectory          = world.project(),
                    .runtimeDirectory          = world.runtime(),
                    .authenticatedControllerId = "umbra-flow-explore",
                    // The capability the Operator's own rule requires. It is a
                    // grant like every other: the Operator wrote the rule and
                    // the session presents what the rule asks for.
                    .controllerCapabilities = {
                        std::string{
                            operator_runtime::conformance::k_operateCapability
                        },
                    },
                    .controlledTargetId = "window-0",
                    .kind               = operator_runtime::ControllerKind::Human,
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
            sessionId = lifecycle->identity().sessionId;

            auto session = lifecycle->startExplorationSession(
                world.ports(delivered, "granted-trace.jsonl"),
                std::stop_token{}
            );
            auto const sessionWhy = session.has_value()
                ? std::string{}
                : std::string{session.error().message()};
            REQUIRE_MESSAGE(session.has_value(), sessionWhy);

            auto const clicked = (*session)->evaluate(
                R"lua(
                    local tools = require("@umbraflow/tools")
                    local answer = tools.call("framework.input.deliver", {
                        action = "click", x = 0, y = 0,
                    })
                    return rawget(rawget(answer, "result"), "delivered") == true
                )lua",
                "annotation-input"
            );
            auto const clickedWhy = clicked.has_value()
                ? std::string{}
                : std::string{clicked.error().message()};
            REQUIRE_MESSAGE(clicked.has_value(), clickedWhy);
            CHECK(clicked->boolean() == std::optional<bool>{true});

            // The input landed. That half would have passed before this change
            // too, which is exactly why it is not the assertion.
            CHECK(*delivered == 1U);

            session->reset();
            CHECK(lifecycle->shutdown().has_value());
        }

        // The half that could not have passed before: the durable row.
        auto const recorded = world.recordedToolCalls();
        auto const input    = std::ranges::find(
            recorded,
            std::string{k_deliverInputTool},
            &ToolCallRow::toolName
        );
        REQUIRE_MESSAGE(
            input != recorded.end(),
            "the annotator's input left no Tool call row in the ledger"
        );
        CHECK(input->state == "confirmed");
        CHECK(input->sessionId == sessionId);
        CHECK(input->policyHash == policyHash.hex());
        CHECK(input->canonicalArgs == R"({"action":"click","x":0,"y":0})");
    }

    // The three properties the frame ruling asks of a body, on one run.
    TEST_CASE(
        "an observation's body measures one frame and its measurements are "
        "recorded as its children"
    )
    {
        auto const world     = ExploreDoorWorld{};
        auto const delivered = std::make_shared<uint32>();
        auto const captures  = std::make_shared<uint32>();

        // The measuring Tools are granted the Privileged surface here, and only
        // so that the stray call below refuses for the right reason. Outside a
        // body a measurement is refused twice over -- the surface first, the
        // missing frame second -- and a case that saw only the first would pass
        // without the frame rule existing at all.
        static_cast<void>(world.authorizeAnnotation());

        auto const scope = operator_runtime::ObservedInstanceWorldScope::run(
            "window-0",
            1
        );
        REQUIRE(scope.has_value());

        {
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
            REQUIRE(lifecycle.has_value());

            auto session = lifecycle->startExplorationSession(
                world.ports(delivered, "body-trace.jsonl", captures),
                std::stop_token{}
            );
            REQUIRE(session.has_value());

            // A MEASURING TOOL OUTSIDE A BODY IS REFUSED BY NAME. It binds by
            // call position to the innermost open frame and takes no frame
            // handle, so outside every body there is nothing for it to bind to
            // and it says so rather than capturing one of its own.
            auto const stray = (*session)->evaluate(
                R"lua(
                    local tools = require("@umbraflow/tools")
                    local answer = tools.call(
                        "framework.screen.read_lines",
                        { x = 0, y = 0, width = 1, height = 1 }
                    )
                    if answer.state == "confirmed" then return "measured" end
                    return tostring(answer.result.message)
                )lua",
                "annotation-stray-measurement"
            );
            REQUIRE(stray.has_value());
            REQUIRE(stray->text() != nullptr);
            CHECK_MESSAGE(
                std::string_view{*stray->text()}.find(
                    "there is no open observation frame"
                ) != std::string_view::npos,
                "a measurement outside a body was not refused by name: ",
                *stray->text()
            );

            auto const before = *captures;
            auto const bodied = (*session)->evaluate(
                R"lua(
                    local screen = require("@umbraflow/screen")
                    local tools = require("@umbraflow/tools")
                    screen.observe(function()
                        tools.call("framework.screen.read_lines", {
                            x = 0, y = 0, width = 1, height = 1,
                        })
                        tools.call("framework.screen.probe", {
                            x = 0, y = 0, width = 1, height = 1,
                            colour_red = 0, colour_green = 0,
                            colour_blue = 0, tolerance = 12, removes = false,
                        })
                    end)
                    return true
                )lua",
                "annotation-two-measurements"
            );
            auto const bodiedWhy = bodied.has_value()
                ? std::string{}
                : std::string{bodied.error().message()};
            REQUIRE_MESSAGE(bodied.has_value(), bodiedWhy);

            // ONE FRAME. Two measurements inside one observation cost exactly
            // one capture; two would mean each verb measured a different
            // screen, which is the drift the frame exists to delete.
            CHECK(*captures - before == 1U);

            session->reset();
            CHECK(lifecycle->shutdown().has_value());
        }

        auto const recorded = world.recordedToolCalls();
        auto const observe  = std::ranges::find(
            recorded,
            std::string{k_observeTool},
            &ToolCallRow::toolName
        );
        REQUIRE(observe != recorded.end());
        CHECK(observe->state == "confirmed");

        // The measurement each Tool CONFIRMED, which is the one written inside
        // the body: the stray call above left a terminal-failure row of its own
        // for framework.screen.read_lines, and a search that took the first row
        // by name would be reading that one.
        auto const confirmedCall = [&recorded](std::string_view toolName)
        {
            return std::ranges::find_if(
                recorded,
                [toolName](ToolCallRow const& row)
                {
                    return row.toolName == toolName && row.state == "confirmed";
                }
            );
        };
        auto const readLines = confirmedCall(k_readLinesTool);
        auto const probe     = confirmedCall(k_probeTool);
        REQUIRE(readLines != recorded.end());
        REQUIRE(probe != recorded.end());

        // CHILDREN OF THE OBSERVE NODE. Both measurements are parented on the
        // observation's own durable position rather than on the run's root,
        // which is what "recorded in the ledger as child calls of that observe
        // node" asks for.
        CHECK(readLines->parentIdentity == observe->callIdentity);
        CHECK(probe->parentIdentity == observe->callIdentity);

        // And the refused one is a row too, terminal and attributed, rather
        // than an act that happened with nothing recording it.
        auto const strayRow = std::ranges::find_if(
            recorded,
            [](ToolCallRow const& row)
            {
                return row.toolName == k_readLinesTool
                    && row.state == "terminal_failure";
            }
        );
        REQUIRE(strayRow != recorded.end());
        CHECK(strayRow->parentIdentity != observe->callIdentity);
    }

    TEST_CASE(
        "a hold body observes while input is engaged and releases on every exit"
    )
    {
        auto const world     = ExploreDoorWorld{};
        auto const delivered = std::make_shared<uint32>();
        auto const captures  = std::make_shared<uint32>();
        auto const held      = std::make_shared<HeldInputLog>();
        static_cast<void>(world.authorizeAnnotation());

        auto const scope = operator_runtime::ObservedInstanceWorldScope::run(
            "window-0",
            1
        );
        REQUIRE(scope.has_value());

        auto lifecycle = service::ProductLifecycle::start(
            service::ProductStart{
                .projectDirectory          = world.project(),
                .runtimeDirectory          = world.runtime(),
                .authenticatedControllerId = "umbra-flow-explore",
                .controllerCapabilities = {
                    std::string{
                        operator_runtime::conformance::k_operateCapability
                    },
                },
                .controlledTargetId = "window-0",
                .kind               = operator_runtime::ControllerKind::Human,
                .agentProfileJcs = std::string{
                    operator_runtime::k_unboundedAgentProfileJcs
                },
                .worldScope = *scope,
            }
        );
        REQUIRE(lifecycle.has_value());
        auto session = lifecycle->startExplorationSession(
            world.ports(delivered, "hold-body-trace.jsonl", captures, held),
            std::stop_token{}
        );
        REQUIRE(session.has_value());

        SUBCASE("hold observe measure is the motivating case")
        {
            auto const before = *captures;
            auto const observed = (*session)->evaluate(
            R"lua(
                local screen = require("@umbraflow/screen")
                local tools = require("@umbraflow/tools")
                tools.call("framework.input.deliver", {
                    action = "hold", x = 0, y = 0,
                }, function()
                    screen.observe(function()
                        tools.call("framework.screen.read_lines", {
                            x = 0, y = 0, width = 1, height = 1,
                        })
                        tools.call("framework.screen.probe", {
                            x = 0, y = 0, width = 1, height = 1,
                            colour_red = 0, colour_green = 0,
                            colour_blue = 0, tolerance = 12, removes = false,
                        })
                    end)
                end)
                return true
            )lua",
            "hold-observe-measure"
        );
            auto const observedWhy = observed.has_value()
                ? std::string{}
                : std::string{observed.error().message()};
            REQUIRE_MESSAGE(observed.has_value(), observedWhy);
            CHECK(held->observedWhileHeld);
            CHECK(held->engaged == 1U);
            CHECK(held->released == 1U);
            CHECK_FALSE(held->held);
            // One capture to aim the input, then exactly one for the observe
            // whose body made two measurements. Per-measurement capture makes 4.
            CHECK(*captures - before == 2U);
        }

        SUBCASE("a raising body still releases")
        {
            auto const raised = (*session)->evaluate(
            R"lua(
                local tools = require("@umbraflow/tools")
                local answer = tools.call("framework.input.deliver", {
                    action = "hold", x = 0, y = 0,
                }, function()
                    error("the hold body raised")
                end)
                local result = rawget(answer, "result")
                return tostring(rawget(result, "reason") or rawget(result, "message"))
            )lua",
            "hold-body-raises"
        );
            REQUIRE(raised.has_value());
            REQUIRE(raised->text() != nullptr);
            CHECK(
                std::string_view{*raised->text()}.contains(
                    "the hold body raised"
                )
            );
            CHECK(held->engaged == 1U);
            CHECK(held->released == 1U);
            CHECK_FALSE(held->held);
        }

        SUBCASE("an empty body is refused by the click arm's exact name")
        {
            auto const empty = (*session)->evaluate(
            R"lua(
                local tools = require("@umbraflow/tools")
                local answer = tools.call("framework.input.deliver", {
                    action = "hold", x = 0, y = 0,
                }, function() end)
                local result = rawget(answer, "result")
                return tostring(rawget(result, "reason") or rawget(result, "message"))
            )lua",
            "empty-hold-body"
        );
            REQUIRE(empty.has_value());
            REQUIRE(empty->text() != nullptr);
            CHECK(
                std::string_view{*empty->text()}.contains(
                    "a hold body is empty; press and release is the `click` arm."
                )
            );
            CHECK(held->engaged == 1U);
            CHECK(held->released == 1U);
            CHECK_FALSE(held->held);
        }

        SUBCASE("an input refusal is not mislabeled as an empty body")
        {
            auto const refused = (*session)->evaluate(
            R"lua(
                local screen = require("@umbraflow/screen")
                local tools = require("@umbraflow/tools")
                local answer = tools.call("framework.input.deliver", {
                    action = "hold", x = 999, y = 999,
                }, function()
                    screen.observe(function() end)
                end)
                local result = rawget(answer, "result")
                return tostring(rawget(result, "reason") or rawget(result, "message"))
            )lua",
            "refused-hold-body"
        );
            REQUIRE(refused.has_value());
            REQUIRE(refused->text() != nullptr);
            CHECK(
                std::string_view{*refused->text()}.contains(
                    "is outside the"
                )
            );
            CHECK(
                std::string_view{*refused->text()}.contains(
                    "target surface"
                )
            );
            CHECK_FALSE(
                std::string_view{*refused->text()}.contains(
                    "hold body is empty"
                )
            );
            CHECK(held->engaged == 0U);
            CHECK(held->released == 0U);
            CHECK_FALSE(held->held);
        }

        SUBCASE("an observation body still refuses a mutating child")
        {
            auto const refused = (*session)->evaluate(
                R"lua(
                    local screen = require("@umbraflow/screen")
                    local tools = require("@umbraflow/tools")
                    local answer = screen.observe(function()
                        tools.call("framework.input.deliver", {
                            action = "click", x = 0, y = 0,
                        })
                    end)
                    local result = rawget(answer, "result")
                    return tostring(rawget(result, "reason") or rawget(result, "message"))
                )lua",
                "observe-mutating-child"
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(
                std::string_view{refused.error().message()}.contains(
                    "Parent Tool framework.screen.observe declares no child "
                    "effect for framework.input.deliver"
                )
            );
            CHECK(*delivered == 0U);
        }

        session->reset();
        CHECK(lifecycle->shutdown().has_value());
    }

    // The authoring-write line, both ways round. It is the second thing an
    // Operator's file has to grant, and the first annotation stroke is where a
    // developer meets it.
    TEST_CASE("an authoring write needs the Operator's grant and is refused by name without it")
    {
        auto const scope = operator_runtime::ObservedInstanceWorldScope::run(
            "window-0",
            1
        );
        REQUIRE(scope.has_value());

        auto const runWrite = [&scope](
                                  ExploreDoorWorld const& world,
                                  std::vector<std::string> capabilities,
                                  std::string_view trace
                              ) -> std::string
        {
            auto const delivered = std::make_shared<uint32>();
            auto lifecycle = service::ProductLifecycle::start(
                service::ProductStart{
                    .projectDirectory          = world.project(),
                    .runtimeDirectory          = world.runtime(),
                    .authenticatedControllerId = "umbra-flow-explore",
                    .controllerCapabilities    = std::move(capabilities),
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
                world.ports(delivered, trace),
                std::stop_token{}
            );
            REQUIRE(session.has_value());
            auto const written = (*session)->evaluate(
                R"lua(
                    local tools = require("@umbraflow/tools")
                    local answer = tools.call("framework.project.write", {
                        action = "text",
                        path = "runtime/annotation.txt",
                        content = "a stroke",
                    })
                    return if tools.state(answer) == "confirmed"
                        then "written"
                        else tostring(rawget(rawget(answer, "result"), "reason"))
                )lua",
                "annotation-authoring-write"
            );
            auto answer = std::string{};
            if (written.has_value())
            {
                REQUIRE(written->text() != nullptr);
                answer = *written->text();
            }
            else
            {
                answer = written.error().message();
            }
            session->reset();
            CHECK(lifecycle->shutdown().has_value());
            return answer;
        };

        SUBCASE("without the grant")
        {
            auto const world   = ExploreDoorWorld{};
            auto const refused = runWrite(world, {}, "unauthorised-trace.jsonl");
            CHECK_MESSAGE(
                refused.find(
                    "Operator policy grants no Privileged surface to tool "
                    "framework.project.write"
                ) != std::string::npos,
                "an authoring write under deny-all was not refused by name: ",
                refused
            );
            CHECK_FALSE(
                std::filesystem::exists(
                    world.project() / "runtime" / "annotation.txt"
                )
            );
        }

        SUBCASE("with the grant")
        {
            auto const world = ExploreDoorWorld{};
            static_cast<void>(world.authorizeAnnotation());
            auto const answer = runWrite(
                world,
                {std::string{
                    operator_runtime::conformance::k_operateCapability
                }},
                "authorised-trace.jsonl"
            );
            CHECK_MESSAGE(answer == std::string{"written"}, answer);
            CHECK(
                std::filesystem::exists(
                    world.project() / "runtime" / "annotation.txt"
                )
            );
        }
    }
}
