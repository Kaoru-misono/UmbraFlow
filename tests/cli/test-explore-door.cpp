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

#include <image/png.hpp>

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
#include <limits>
#include <memory>
#include <optional>
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
        constexpr auto k_captureTool = std::string_view{
            "framework.screen.capture"
        };
        constexpr auto k_deliverInputTool = std::string_view{
            "framework.input.click"
        };
        constexpr auto k_holdInputTool = std::string_view{
            "framework.input.hold"
        };

        // The exact sentence the Operator's admission refuses a Privileged
        // surface with. Written out rather than matched on a substring: what
        // this case is about is that the refusal NAMES the missing grant and the
        // Tool it was missing for, and a case asking only whether an injection
        // failed would pass on a malformed argument.
        constexpr auto k_privilegedRefusal = std::string_view{
            "Operator policy grants no Privileged surface to tool "
            "framework.input.click"
        };

        constexpr auto k_projectWriteFileTool = std::string_view{
            "framework.project.write_file"
        };
        constexpr auto k_projectWriteTextTool = std::string_view{
            "framework.project.write_text"
        };
        constexpr auto k_readLinesTool = std::string_view{
            "framework.screen.read_lines"
        };
        constexpr auto k_probeTool = std::string_view{
            "framework.screen.probe"
        };
        constexpr auto k_cropTool = std::string_view{
            "framework.screen.crop"
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
            uint32                            engaged{};
            uint32                            released{};
            bool                              held{};
            bool                              observedWhileHeld{};
            bool                              failCaptureWhileHeld{};
            std::shared_ptr<std::stop_source> cancelOnEngage{};
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
        // IT IS HOW "ONE SCREENSHOT" IS PROVED. Multiple measurements naming
        // one retained digest cost exactly one live capture; a measuring Tool
        // that captured for itself would cost more and answer about a screen
        // the caller did not name.
        class CountingFrameSource final : public engine::IFrameSource
        {
            std::vector<Frame>            m_frames;
            std::size_t                   m_next{};
            std::shared_ptr<uint32>       m_captures;
            std::shared_ptr<HeldInputLog> m_held;

        public:
            CountingFrameSource(
                Frame frame,
                std::shared_ptr<uint32> captures,
                std::shared_ptr<HeldInputLog> held
            ) noexcept
                : m_frames{std::move(frame)}
                , m_captures{std::move(captures)}
                , m_held{std::move(held)}
            {
            }

            CountingFrameSource(
                std::vector<Frame> frames,
                std::shared_ptr<uint32> captures,
                std::shared_ptr<HeldInputLog> held
            ) noexcept
                : m_frames{std::move(frames)}
                , m_captures{std::move(captures)}
                , m_held{std::move(held)}
            {
            }

            [[nodiscard]]
            auto capture(CaptureBudget const&) -> Result<Frame> override
            {
                REQUIRE_FALSE(m_frames.empty());
                auto const index = (std::min)(m_next, m_frames.size() - 1U);
                ++m_next;
                ++*m_captures;
                if (m_held->held)
                {
                    m_held->observedWhileHeld = true;
                    if (m_held->failCaptureWhileHeld)
                    {
                        return fail(
                            AutomationErrorKind::IoFailure,
                            "fixture capture failed while the hold was engaged"
                        );
                    }
                }
                return m_frames.at(index);
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
                if (m_held->cancelOnEngage)
                {
                    m_held->cancelOnEngage->request_stop();
                }
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

            [[nodiscard]] auto probePng() const -> std::vector<std::byte>
            {
                return m_probe;
            }

            [[nodiscard]]
            auto ports(
                std::shared_ptr<uint32> delivered,
                std::string_view trace,
                std::shared_ptr<uint32> captures = std::make_shared<uint32>(),
                std::shared_ptr<HeldInputLog> held =
                    std::make_shared<HeldInputLog>(),
                uint64 maximumPixelComparisons = k_defaultPixelComparisonBudget
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
                    .maximumPixelComparisons = maximumPixelComparisons,
                    .recognitionTimeout      = k_defaultRecognitionTimeout,
                    .tracePath               = path(trace),
                };
            }

            [[nodiscard]]
            auto sequencePorts(
                std::shared_ptr<uint32> delivered,
                std::string_view trace,
                std::shared_ptr<uint32> captures
            ) const -> task::TaskRunConfig
            {
                auto decoded = image::decodePng(m_probe, "sequence-frame.png");
                REQUIRE(decoded.has_value());
                REQUIRE(decoded->pixels.size() >= 4U);

                auto firstPixels = decoded->pixels;
                firstPixels.at(0U) = std::byte{11U};
                firstPixels.at(1U) = std::byte{22U};
                firstPixels.at(2U) = std::byte{33U};
                firstPixels.at(3U) = std::byte{255U};
                auto first = image::encodeRgbaPng(
                    "first-sequence-frame.png",
                    decoded->width,
                    decoded->height,
                    firstPixels
                );
                REQUIRE(first.has_value());

                auto secondPixels = decoded->pixels;
                secondPixels.at(0U) = std::byte{201U};
                secondPixels.at(1U) = std::byte{202U};
                secondPixels.at(2U) = std::byte{203U};
                secondPixels.at(3U) = std::byte{255U};
                auto second = image::encodeRgbaPng(
                    "second-sequence-frame.png",
                    decoded->width,
                    decoded->height,
                    secondPixels
                );
                REQUIRE(second.has_value());

                auto frames = std::vector<Frame>{};
                frames.emplace_back(
                    operator_runtime::conformance::observationFrame(
                        *first,
                        FrameId{4101}
                    )
                );
                frames.emplace_back(
                    operator_runtime::conformance::observationFrame(
                        *second,
                        FrameId{4102}
                    )
                );
                auto const fingerprint = ProjectFingerprint::create(
                    k_recordedWidth,
                    k_recordedHeight,
                    k_recordedDpi,
                    k_recordedDpi
                );
                REQUIRE(fingerprint.has_value());
                auto held = std::make_shared<HeldInputLog>();
                return task::TaskRunConfig{
                    .frameSource = std::make_unique<CountingFrameSource>(
                        std::move(frames),
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
            // first injected input needs this. It grants the exact acting and
            // authoring Tools used below, and it is written by the Operator
            // rather than scaffolded, because a permission the requester signs
            // for itself is a rubber stamp.
            auto authorizeAnnotation() const -> ContentHash
            {
                auto const policy =
                    operator_runtime::conformance::policyArtifactBytes(
                        operatorProtocolSchemaHash(),
                        std::vector<std::string>{
                            std::string{k_deliverInputTool},
                            std::string{k_holdInputTool},
                            std::string{k_projectWriteFileTool},
                            std::string{k_projectWriteTextTool},
                        },
                        // The measuring Tools are granted here too so their
                        // tests reach the explicit screenshot reference and
                        // measurement providers rather than stopping at the
                        // Privileged-surface policy gate.
                        std::vector<std::string>{
                            std::string{k_deliverInputTool},
                            std::string{k_holdInputTool},
                            std::string{k_cropTool},
                            std::string{k_probeTool},
                            std::string{k_projectWriteFileTool},
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

        [[nodiscard]]
        auto evaluateAuthoringChunk(
            ExploreDoorWorld const& world,
            std::string_view chunk,
            std::string_view chunkName
        ) -> Result<script::ScriptValue>
        {
            UF_TRY_VALUE(
                scope,
                operator_runtime::ObservedInstanceWorldScope::run("window-0", 1)
            );
            UF_TRY_VALUE(
                lifecycle,
                service::ProductLifecycle::start(service::ProductStart{
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
                    .worldScope = scope,
                })
            );
            auto delivered = std::make_shared<uint32>();
            UF_TRY_VALUE(
                session,
                lifecycle.startExplorationSession(
                    world.ports(
                        delivered,
                        std::string{chunkName} + ".jsonl"
                    ),
                    std::stop_token{}
                )
            );
            auto evaluated = session->evaluate(chunk, chunkName);
            session.reset();
            auto closed = lifecycle.shutdown();
            if (!evaluated)
            {
                return std::unexpected{std::move(evaluated).error()};
            }
            UF_TRY(std::move(closed));
            return std::move(evaluated);
        }

        // A project that has annotated nothing, against an Operator root that
        // does not exist yet. Between them they hold no file anybody wrote by
        // hand.
        //
        // The project's declared artifact is replaced by the genesis one --
        // exactly the two files `project init` scaffolds and nothing else,
        // because the artifact's file closure admits the manifest and the page
        // model alone. The runtime directory is deliberately NOT created here:
        // what creates it is ProductLifecycle::start, and the genesis
        // generation it materializes as layout is the case's whole subject.
        class GenesisDoorWorld final
        {
            std::filesystem::path  m_root{};
            std::filesystem::path  m_project{};
            std::filesystem::path  m_runtime{};
            std::vector<std::byte> m_probe{};

        public:
            GenesisDoorWorld()
            {
                m_root = (
                    std::filesystem::temp_directory_path()
                    / std::filesystem::path{
                        "uf-genesis-door-"
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
                m_probe = readAll(m_project / "runtime" / "probe-frame.png");

                auto const artifact = m_project / "runtime" / "artifact";
                std::filesystem::remove_all(artifact);
                std::filesystem::create_directories(artifact);
                auto const manifest = task::genesisRuntimeArtifactManifestJcs();
                REQUIRE(manifest.has_value());
                writeExact(
                    artifact / std::string{task::k_runtimeModelFileName},
                    task::k_genesisRuntimeModelToml
                );
                writeExact(
                    artifact
                        / std::string{task::k_runtimeArtifactManifestFileName},
                    *manifest
                );
            }

            GenesisDoorWorld(GenesisDoorWorld const&)                    = delete;
            GenesisDoorWorld(GenesisDoorWorld&&)                         = delete;
            auto operator=(GenesisDoorWorld const&) -> GenesisDoorWorld& = delete;
            auto operator=(GenesisDoorWorld&&) -> GenesisDoorWorld&      = delete;

            ~GenesisDoorWorld()
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

            // Genesis declares the smallest legal extent at the reference DPI,
            // which is the only geometry a model declaring nothing can carry.
            // The recorded frame beside it is never captured by the case below
            // and is here because a run configuration always names its ports.
            [[nodiscard]]
            auto ports(std::string_view trace) const -> task::TaskRunConfig
            {
                auto const fingerprint = ProjectFingerprint::create(
                    1U,
                    1U,
                    k_recordedDpi,
                    k_recordedDpi
                );
                REQUIRE(fingerprint.has_value());
                return task::TaskRunConfig{
                    .frameSource = std::make_unique<CountingFrameSource>(
                        operator_runtime::conformance::observationFrame(
                            m_probe,
                            FrameId{5001}
                        ),
                        std::make_shared<uint32>(),
                        std::make_shared<HeldInputLog>()
                    ),
                    .actionSink = std::make_unique<CountingActionSink>(
                        std::make_shared<uint32>(),
                        std::make_shared<HeldInputLog>()
                    ),
                    .ocrEngine               = std::make_unique<SilentReader>(),
                    .liveFingerprint         = *fingerprint,
                    .maximumPixelComparisons = k_defaultPixelComparisonBudget,
                    .recognitionTimeout      = k_defaultRecognitionTimeout,
                    .tracePath               = m_root / std::filesystem::path{trace},
                };
            }

        private:
            [[nodiscard]]
            static auto readAll(std::filesystem::path const& file)
                -> std::vector<std::byte>
            {
                auto stream = std::ifstream{file, std::ios::binary};
                REQUIRE(stream.good());
                auto const text = std::string{
                    std::istreambuf_iterator<char>{stream},
                    std::istreambuf_iterator<char>{},
                };
                auto bytes = std::vector<std::byte>{};
                bytes.reserve(text.size());
                for (auto const value : text)
                {
                    bytes.emplace_back(
                        static_cast<std::byte>(static_cast<unsigned char>(value))
                    );
                }
                return bytes;
            }

            static auto writeExact(
                std::filesystem::path const& file,
                std::string_view bytes
            ) -> void
            {
                auto stream = std::ofstream{
                    file,
                    std::ios::binary | std::ios::trunc,
                };
                stream.write(
                    bytes.data(),
                    static_cast<std::streamsize>(bytes.size())
                );
                stream.flush();
                REQUIRE(stream.good());
            }
        };
    }

    // The corrected onboarding claim, executed:
    //
    //   "From nothing to a first exploration session is still `init` then
    //    `explore --runtime <root>`, with zero project-authored files -- and in
    //    fact zero hand-written files by anyone: the genesis runtime artifact
    //    gives the session its pin, the absent policy resolves to deny-all, and
    //    deny-all still admits read-only screen observation. What deny-all does
    //    not admit is input injection or an authoring write."
    //    -- docs/decisions/2026-08-24-the-annotation-policy-is-the-operators.md
    //
    // Nothing installs anything into the Operator root here, and the root does
    // not exist when the case starts. OperatorCoordinator::open creates it, and
    // the genesis generation is part of what it creates -- which is what gives
    // this session something to pin. Deleting the ensureGenesisGeneration call
    // from open reds this at the first REQUIRE.
    TEST_CASE(
        "a first exploration session binds H_genesis against a root that holds "
        "only it"
    )
    {
        auto const world       = GenesisDoorWorld{};
        auto const genesisHash = task::genesisArtifactRootHash();
        REQUIRE(genesisHash.has_value());
        REQUIRE_FALSE(std::filesystem::exists(world.runtime()));

        auto const scope = operator_runtime::ObservedInstanceWorldScope::run(
            "window-0",
            1
        );
        REQUIRE(scope.has_value());

        // Exactly the ProductStart cli::exploreProject builds. There is one
        // door, and a project with no model of its own reaches the same one.
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
        REQUIRE_MESSAGE(
            lifecycle.has_value(),
            "a session opens against a root that holds only genesis: ",
            lifecycleWhy
        );

        auto const identity = lifecycle->identity();
        CHECK_MESSAGE(
            identity.runtimeModel.artifactRootHash() == *genesisHash,
            "the session binds H_genesis"
        );
        CHECK_MESSAGE(
            identity.installedGeneration == 0U,
            "H_genesis is bound at the genesis generation, which is 0"
        );

        auto session = lifecycle->startExplorationSession(
            world.ports("genesis-trace.jsonl"),
            std::stop_token{}
        );
        auto const sessionWhy = session.has_value()
            ? std::string{}
            : std::string{session.error().message()};
        REQUIRE_MESSAGE(session.has_value(), sessionWhy);

        // Read-only under deny-all. The Operator wrote no policy artifact into
        // this root -- there was no root to write one into until a moment ago --
        // so the resolution is deny-all, and deny-all refuses the injection by
        // naming the grant that was never made.
        REQUIRE_FALSE(std::filesystem::exists(
            world.runtime() / "policy-artifact.json"
        ));
        auto const injected = lifecycle->invokeTool(
            explorationCall(
                k_deliverInputTool,
                R"({"screenshot_sha256":"0000000000000000000000000000000000000000000000000000000000000000","x":0,"y":0})"
            ),
            (*session)->context()
        );
        REQUIRE_FALSE(injected.has_value());
        CHECK_MESSAGE(
            injected.error().message() == k_privilegedRefusal,
            "a session pinned to genesis runs read-only under deny-all"
        );
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

        // Capture and observation are Semantic and read-only, so deny-all admits
        // both. The immutable screenshot digest is explicit between them.
        auto const measured = (*session)->evaluate(
            R"lua(
                local screen = require("@umbraflow/screen")
                local shot = screen.capture{}.screenshot_sha256
                local resolved = screen.observe{ screenshot_sha256 = shot }
                return resolved.observation_reference ~= nil
            )lua",
            "annotation-explicit-screenshot"
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
                local alpha = require("@umbraflow/fixture/alpha")
                return alpha["observe-1"]{ value = 1 }.outcome
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
                local input = require("@umbraflow/input")
                input.click{
                    screenshot_sha256 = string.rep("0", 64), x = 0, y = 0,
                }
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
        auto const captured = lifecycle->invokeTool(
            explorationCall(k_captureTool, "{}"),
            context
        );
        REQUIRE(captured.has_value());
        REQUIRE(captured->payload.has_value());
        auto const captureReceipt = json::parse(captured->payload->bytes());
        REQUIRE(captureReceipt.has_value());
        auto const* const p_screenshot = captureReceipt->find(
            "screenshot_sha256"
        );
        REQUIRE(p_screenshot != nullptr);
        auto const screenshotArguments = R"({"screenshot_sha256":")"
            + std::string{p_screenshot->string()} + R"("})";
        auto const observed = lifecycle->invokeTool(
            explorationCall(k_observeTool, screenshotArguments),
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

        auto clickArguments = screenshotArguments;
        clickArguments.pop_back();
        clickArguments += R"(,"x":0,"y":0})";

        // And the half the whole four-step order exists to repay: an injection
        // is refused at admission, by name, before the sink is reached.
        auto const injected = lifecycle->invokeTool(
            explorationCall(
                k_deliverInputTool,
                clickArguments
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
                    local screen = require("@umbraflow/screen")
                    local input  = require("@umbraflow/input")
                    local hash = screen.capture{}.screenshot_sha256
                    return input.click{
                        screenshot_sha256 = hash, x = 0, y = 0,
                    }.delivered == true
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
        auto const inputArguments = json::parse(input->canonicalArgs);
        REQUIRE(inputArguments.has_value());
        CHECK(inputArguments->find("screenshot_sha256") != nullptr);
        CHECK(inputArguments->find("x")->number() == 0.0);
        CHECK(inputArguments->find("y")->number() == 0.0);
    }


    TEST_CASE("two measurements passed the same hash measure one screenshot")
    {
        auto const world     = ExploreDoorWorld{};
        auto const delivered = std::make_shared<uint32>();
        auto const captures  = std::make_shared<uint32>();
        static_cast<void>(world.authorizeAnnotation());

        auto const scope = operator_runtime::ObservedInstanceWorldScope::run(
            "window-0",
            1
        );
        REQUIRE(scope.has_value());

        auto screenshotHash = std::string{};
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
                world.ports(delivered, "explicit-hash-trace.jsonl", captures),
                std::stop_token{}
            );
            REQUIRE(session.has_value());

            auto const before = *captures;
            auto const measured = (*session)->evaluate(
                R"lua(
                    local screen = require("@umbraflow/screen")
                    local hash = screen.capture{}.screenshot_sha256
                    -- Neither measurement is checked for success: a call
                    -- that did not confirm raises, so reaching the return
                    -- IS the proof that both confirmed.
                    screen.read_lines{
                        screenshot_sha256 = hash,
                        x = 0, y = 0, width = 1, height = 1,
                    }
                    screen.probe{
                        screenshot_sha256 = hash,
                        x = 0, y = 0, width = 1, height = 1,
                        colour_red = 0, colour_green = 0,
                        colour_blue = 0, tolerance = 12, removes = false,
                    }
                    return hash
                )lua",
                "annotation-two-explicit-measurements"
            );
            auto const measuredWhy = measured.has_value()
                ? std::string{}
                : std::string{measured.error().message()};
            REQUIRE_MESSAGE(measured.has_value(), measuredWhy);
            REQUIRE(measured->text() != nullptr);
            screenshotHash = *measured->text();
            CHECK_MESSAGE(
                *captures - before == 1U,
                "two measurements passed the same hash measure one screenshot"
            );

            session->reset();
            CHECK(lifecycle->shutdown().has_value());
        }

        auto const recorded = world.recordedToolCalls();
        auto const readLines = std::ranges::find(
            recorded,
            std::string{k_readLinesTool},
            &ToolCallRow::toolName
        );
        auto const probe = std::ranges::find(
            recorded,
            std::string{k_probeTool},
            &ToolCallRow::toolName
        );
        REQUIRE(readLines != recorded.end());
        REQUIRE(probe != recorded.end());
        auto const oneScreenshot = readLines->canonicalArgs.contains(
            screenshotHash
        ) && probe->canonicalArgs.contains(screenshotHash);
        CHECK_MESSAGE(
            oneScreenshot,
            "two measurements passed the same hash measure one screenshot"
        );
    }

    TEST_CASE("a measurement against a screenshot hash measures that screenshot")
    {
        auto const world     = ExploreDoorWorld{};
        auto const delivered = std::make_shared<uint32>();
        auto const captures  = std::make_shared<uint32>();
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
            world.sequencePorts(
                delivered,
                "first-screenshot-trace.jsonl",
                captures
            ),
            std::stop_token{}
        );
        REQUIRE(session.has_value());

        auto const measured = (*session)->evaluate(
            R"lua(
                local screen = require("@umbraflow/screen")
                local first = screen.capture{}.screenshot_sha256
                local second = screen.capture{}.screenshot_sha256
                if first == second then
                    error("the two fixture screens had one digest")
                end
                return screen.probe{
                    screenshot_sha256 = first,
                    x = 0, y = 0, width = 1, height = 1,
                    colour_red = 0, colour_green = 0, colour_blue = 0,
                    tolerance = 0, removes = false,
                }.dominant_red
            )lua",
            "measure-first-screenshot-after-second-capture"
        );
        auto const measuredWhy = measured.has_value()
            ? std::string{}
            : std::string{measured.error().message()};
        REQUIRE_MESSAGE(measured.has_value(), measuredWhy);
        CHECK_MESSAGE(
            measured->number() == std::optional<double>{11.0},
            "a measurement against a screenshot hash measures that screenshot"
        );
        CHECK(*captures == 2U);

        session->reset();
        CHECK(lifecycle->shutdown().has_value());
    }

    TEST_CASE("a named screenshot that does not exist is refused by name")
    {
        auto const world     = ExploreDoorWorld{};
        auto const delivered = std::make_shared<uint32>();
        auto const captures  = std::make_shared<uint32>();
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
            world.ports(delivered, "missing-screenshot-trace.jsonl", captures),
            std::stop_token{}
        );
        REQUIRE(session.has_value());

        constexpr auto k_missing = std::string_view{
            "0000000000000000000000000000000000000000000000000000000000000000"
        };
        auto const refused = lifecycle->invokeTool(
            explorationCall(
                k_observeTool,
                R"({"screenshot_sha256":"0000000000000000000000000000000000000000000000000000000000000000"})"
            ),
            (*session)->context()
        );
        REQUIRE(refused.has_value());
        CHECK(refused->state == operator_runtime::ToolCallState::TerminalFailure);
        REQUIRE(refused->payload.has_value());
        auto const namedMissing = refused->payload->bytes().contains(k_missing)
            && refused->payload->bytes().contains("missing or expired");
        CHECK_MESSAGE(
            namedMissing,
            "a named screenshot that does not exist is refused by name and says which hash was missing"
        );
        CHECK(*captures == 0U);

        session->reset();
        CHECK(lifecycle->shutdown().has_value());
    }

    TEST_CASE("a screenshot receipt is committed only after its blob is durable")
    {
        auto const world = ExploreDoorWorld{};
        auto const png   = world.probePng();
        auto const frame = operator_runtime::conformance::observationFrame(
            png,
            FrameId{4201}
        );
        auto coordinator = operator_runtime::OperatorCoordinator::open(
            world.runtime()
        );
        REQUIRE(coordinator.has_value());
        auto const published = coordinator->publishEvidenceArtifact(
            operator_runtime::EvidenceArtifactSpec{
                .bytes         = png,
                .mediaType     = "image/png",
                .width         = frame.width(),
                .height        = frame.height(),
                .frameIdentity = FrameIdentity::fromFrame(frame),
            }
        );
        REQUIRE(published.has_value());

        // This is the interruption seam: publication has returned, so the blob
        // is durable, but no Tool completion has yet committed the receipt.
        auto const durable = coordinator->readEvidenceArtifact(
            published->contentHash,
            image::k_maximumPngFileBytes
        );
        auto const committed = coordinator->evidenceArtifactReceipt(
            published->contentHash
        );
        REQUIRE(committed.has_value());
        auto const durableBeforeReceipt = durable.has_value()
            && durable->size() == png.size() && !committed->has_value();
        CHECK_MESSAGE(
            durableBeforeReceipt,
            "a screenshot receipt is committed only after its blob is durable"
        );
    }

    TEST_CASE(
        "reclaim sweeps unreferenced evidence and retains a run's screenshot"
    )
    {
        auto const world     = ExploreDoorWorld{};
        auto const delivered = std::make_shared<uint32>();
        static_cast<void>(world.authorizeAnnotation());
        auto const scope = operator_runtime::ObservedInstanceWorldScope::run(
            "window-0",
            1
        );
        REQUIRE(scope.has_value());

        auto retainedHash = ContentHash::parse(
            "sha256:0000000000000000000000000000000000000000000000000000000000000000"
        );
        REQUIRE(retainedHash.has_value());
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
                world.ports(delivered, "retained-evidence-trace.jsonl"),
                std::stop_token{}
            );
            REQUIRE(session.has_value());
            auto const captured = lifecycle->invokeTool(
                explorationCall(k_captureTool, "{}"),
                (*session)->context()
            );
            REQUIRE(captured.has_value());
            REQUIRE(captured->payload.has_value());
            auto const receipt = json::parse(captured->payload->bytes());
            REQUIRE(receipt.has_value());
            auto const* const p_hash = receipt->find("screenshot_sha256");
            REQUIRE(p_hash != nullptr);
            CHECK(receipt->find("byte_count") != nullptr);
            CHECK(receipt->find("frame_identity") != nullptr);
            CHECK(receipt->find("height") != nullptr);
            CHECK(receipt->find("width") != nullptr);
            auto const* const p_media = receipt->find("media_type");
            REQUIRE(p_media != nullptr);
            CHECK(p_media->string() == "image/png");
            retainedHash = ContentHash::parse(
                "sha256:" + std::string{p_hash->string()}
            );
            REQUIRE(retainedHash.has_value());
            auto const carried = lifecycle->readScreenshot(*retainedHash);
            REQUIRE(carried.has_value());
            CHECK_FALSE(carried->empty());

            auto const cropArguments = R"({"height":1,"screenshot_sha256":")"
                + retainedHash->hex() + R"(","width":1,"x":0,"y":0})";
            auto const cropped = lifecycle->invokeTool(
                explorationCall(k_cropTool, cropArguments),
                (*session)->context()
            );
            REQUIRE(cropped.has_value());
            CHECK(cropped->state == operator_runtime::ToolCallState::Confirmed);
            REQUIRE(cropped->payload.has_value());
            auto const cropReceipt = json::parse(cropped->payload->bytes());
            REQUIRE(cropReceipt.has_value());
            auto const* const p_rectangle = cropReceipt->find("rectangle");
            REQUIRE(p_rectangle != nullptr);
            auto const* const p_cropWidth = cropReceipt->find("width");
            auto const* const p_cropHeight = cropReceipt->find("height");
            REQUIRE(p_cropWidth != nullptr);
            REQUIRE(p_cropHeight != nullptr);
            CHECK(p_cropWidth->number() == 1.0);
            CHECK(p_cropHeight->number() == 1.0);
            session->reset();
            CHECK(lifecycle->shutdown().has_value());
        }

        auto alternate = image::decodePng(
            world.probePng(),
            "unreferenced-evidence.png"
        );
        REQUIRE(alternate.has_value());
        REQUIRE_FALSE(alternate->pixels.empty());
        alternate->pixels.at(0U) = std::byte{77U};
        auto orphanPng = image::encodeRgbaPng(
            "unreferenced-evidence.png",
            alternate->width,
            alternate->height,
            alternate->pixels
        );
        REQUIRE(orphanPng.has_value());
        auto const orphanFrame = operator_runtime::conformance::observationFrame(
            *orphanPng,
            FrameId{4301}
        );
        auto orphanHash = *retainedHash;
        {
            auto coordinator = operator_runtime::OperatorCoordinator::open(
                world.runtime()
            );
            REQUIRE(coordinator.has_value());
            auto const orphan = coordinator->publishEvidenceArtifact(
                operator_runtime::EvidenceArtifactSpec{
                    .bytes         = *orphanPng,
                    .mediaType     = "image/png",
                    .width         = orphanFrame.width(),
                    .height        = orphanFrame.height(),
                    .frameIdentity = FrameIdentity::fromFrame(orphanFrame),
                }
            );
            REQUIRE(orphan.has_value());
            orphanHash = orphan->contentHash;
            CHECK(orphanHash != *retainedHash);
        }

        auto const reclaimed = service::reclaimOperatorStores(
            world.runtime(),
            (std::numeric_limits<uint64>::max)()
        );
        REQUIRE(reclaimed.has_value());
        auto coordinator = operator_runtime::OperatorCoordinator::open(
            world.runtime()
        );
        REQUIRE(coordinator.has_value());
        auto const retained = coordinator->readEvidenceArtifact(
            *retainedHash,
            image::k_maximumPngFileBytes
        );
        auto const orphan = coordinator->readEvidenceArtifact(
            orphanHash,
            image::k_maximumPngFileBytes
        );
        auto const sweptOnlyOrphan = reclaimed->evidence.blobs == 1U
            && retained.has_value() && !orphan.has_value();
        CHECK_MESSAGE(
            sweptOnlyOrphan,
            "reclaim sweeps an unreferenced evidence blob and does not sweep one a retained run still references"
        );
    }

    TEST_CASE(
        "a flat hold captures while pressed and releases on every exit"
    )
    {
        auto const world     = ExploreDoorWorld{};
        auto const delivered = std::make_shared<uint32>();
        auto const captures  = std::make_shared<uint32>();
        auto const held      = std::make_shared<HeldInputLog>();
        auto const cancellation = std::make_shared<std::stop_source>();
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
            cancellation->get_token()
        );
        REQUIRE(session.has_value());


        SUBCASE("capture succeeds before release")
        {
            auto const captured = (*session)->evaluate(
            R"lua(
                local screen = require("@umbraflow/screen")
                local input  = require("@umbraflow/input")
                local hash = screen.capture{}.screenshot_sha256
                return input.hold{
                    duration_ms = 0,
                    return_screen = "capture",
                    screenshot_sha256 = hash,
                    x = 0,
                    y = 0,
                }.screen.screenshot_sha256
            )lua",
            "flat-hold-captures"
        );
            REQUIRE(captured.has_value());
            REQUIRE(captured->text() != nullptr);
            CHECK_FALSE(captured->text()->empty());
            CHECK(held->engaged == 1U);
            CHECK(held->released == 1U);
            CHECK_FALSE(held->held);
            CHECK(held->observedWhileHeld);
            CHECK(*captures == 2U);
        }

        SUBCASE("a capture failure still releases")
        {
            held->failCaptureWhileHeld = true;
            auto const failed = (*session)->evaluate(
            R"lua(
                local screen = require("@umbraflow/screen")
                local input  = require("@umbraflow/input")
                local hash = screen.capture{}.screenshot_sha256
                return input.hold{
                    duration_ms = 0,
                    return_screen = "capture",
                    screenshot_sha256 = hash,
                    x = 0,
                    y = 0,
                }.screen.screenshot_sha256
            )lua",
            "flat-hold-capture-fails"
        );
            REQUIRE_FALSE(failed.has_value());
            CHECK(held->engaged == 1U);
            CHECK(held->released == 1U);
            CHECK_FALSE(held->held);
        }

        SUBCASE("cancellation during the dwell still releases")
        {
            held->cancelOnEngage = cancellation;
            auto const cancelled = (*session)->evaluate(
            R"lua(
                local screen = require("@umbraflow/screen")
                local input  = require("@umbraflow/input")
                local hash = screen.capture{}.screenshot_sha256
                input.hold{
                    duration_ms = 250,
                    screenshot_sha256 = hash,
                    x = 0,
                    y = 0,
                }
                return "held"
            )lua",
            "flat-hold-cancelled"
        );
            REQUIRE_FALSE(cancelled.has_value());
            CHECK(held->engaged == 1U);
            CHECK(held->released == 1U);
            CHECK_FALSE(held->held);
        }


        session->reset();
        CHECK(lifecycle->shutdown().has_value());
    }

    // WHAT THIS CASE ACTUALLY PROVES, restated 2026-08-26. It was named for a
    // failing internal observation and asserted only that evaluate() errored --
    // which it did for the wrong reason: the chunk returned the answer TABLE and
    // the result transport refuses a table. Under the flat call surface the
    // chunk returns the Tool's own result, so the wrong reason is gone, and with
    // it the illusion: at a zero pixel-comparison budget the observation
    // SUCCEEDS and the hold confirms. The invariant worth having is the one
    // below and it is asserted directly -- a hold that observes while engaged
    // releases on the way out.
    TEST_CASE("a flat hold that observes while engaged still releases")
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
            world.ports(
                delivered,
                "hold-observe-failure-trace.jsonl",
                captures,
                held,
                0U
            ),
            std::stop_token{}
        );
        REQUIRE(session.has_value());

        auto const failed = (*session)->evaluate(
            R"lua(
                local screen = require("@umbraflow/screen")
                local input  = require("@umbraflow/input")
                local hash = screen.capture{}.screenshot_sha256
                return input.hold{
                    duration_ms = 0,
                    return_screen = "observe",
                    screenshot_sha256 = hash,
                    x = 0,
                    y = 0,
                }.screen.observation_id
            )lua",
            "flat-hold-observes-while-engaged"
        );
        auto const why = failed.has_value()
            ? std::string{}
            : std::string{failed.error().message()};
        REQUIRE_MESSAGE(failed.has_value(), why);
        REQUIRE(failed->text() != nullptr);
        CHECK_MESSAGE(
            !failed->text()->empty(),
            "a hold that observes while engaged must answer with what it saw"
        );
        CHECK(held->engaged == 1U);
        CHECK(held->released == 1U);
        CHECK_FALSE(held->held);
        CHECK(held->observedWhileHeld);

        session->reset();
        CHECK(lifecycle->shutdown().has_value());
    }

    TEST_CASE(
        "framework.project.write_file refuses a file_sha256 with no retained "
        "evidence blob"
    )
    {
        auto const world = ExploreDoorWorld{};
        static_cast<void>(world.authorizeAnnotation());

        auto const evaluated = evaluateAuthoringChunk(
            world,
            R"lua(
                local project = require("@umbraflow/project")
                local ok, raised = pcall(project.write_file, {
                    path = "runtime/missing.png",
                    file_sha256 = string.rep("0", 64),
                })
                if ok then
                    return "unexpected success"
                end
                return raised.error.message
            )lua",
            "project-write-file-missing-evidence"
        );
        REQUIRE(evaluated.has_value());
        REQUIRE(evaluated->text() != nullptr);
        CHECK_MESSAGE(
            *evaluated->text()
                == "file_sha256 " + std::string(64U, '0')
                    + " is missing or expired: no committed evidence receipt "
                    "names a retained blob",
            "framework.project.write_file must refuse a file_sha256 naming no "
            "retained evidence blob"
        );
        CHECK_FALSE(
            std::filesystem::exists(world.project() / "runtime" / "missing.png")
        );
    }

    TEST_CASE("framework.project.write_file writes exactly the retained artifact bytes")
    {
        auto const world = ExploreDoorWorld{};
        static_cast<void>(world.authorizeAnnotation());

        auto const evaluated = evaluateAuthoringChunk(
            world,
            R"lua(
                local screen  = require("@umbraflow/screen")
                local project = require("@umbraflow/project")
                local fileSha256 = screen.capture{}.screenshot_sha256
                project.write_file{
                    path = "runtime/copied.png",
                    file_sha256 = fileSha256,
                }
                return fileSha256
            )lua",
            "project-write-file-exact-bytes"
        );
        REQUIRE(evaluated.has_value());
        REQUIRE(evaluated->text() != nullptr);
        auto const artifactHash = ContentHash::parse(
            "sha256:" + *evaluated->text()
        );
        REQUIRE(artifactHash.has_value());

        auto stream = std::ifstream{
            world.project() / "runtime" / "copied.png",
            std::ios::binary,
        };
        REQUIRE(stream.good());
        auto const copied = std::string{
            std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{},
        };
        auto const copiedHash = sha256(
            std::as_bytes(std::span{std::string_view{copied}})
        );
        REQUIRE(copiedHash.has_value());
        CHECK_MESSAGE(
            *copiedHash == *artifactHash,
            "framework.project.write_file must write exactly the bytes held by "
            "file_sha256"
        );
    }

    TEST_CASE(
        "framework.project.read_text and write_text refuse paths outside the "
        "authoring store"
    )
    {
        auto const world = ExploreDoorWorld{};
        static_cast<void>(world.authorizeAnnotation());
        {
            auto outside = std::ofstream{
                world.path("outside.txt"),
                std::ios::binary | std::ios::trunc,
            };
            REQUIRE(outside.good());
            outside << "outside";
            REQUIRE(outside.good());
        }

        auto const evaluated = evaluateAuthoringChunk(
            world,
            R"lua(
                local project = require("@umbraflow/project")
                local function outcome(name, called, raised)
                    if called then
                        return name .. ":unexpected success"
                    end
                    return name .. ":" .. raised.error.message
                end
                local readOk, readRaised = pcall(
                    project.read_text,
                    { path = "../outside.txt" }
                )
                local writeOk, writeRaised = pcall(
                    project.write_text,
                    { path = "../outside.txt", content = "escaped" }
                )
                return outcome("read", readOk, readRaised)
                    .. "|"
                    .. outcome("write", writeOk, writeRaised)
            )lua",
            "project-text-path-confinement"
        );
        REQUIRE(evaluated.has_value());
        REQUIRE(evaluated->text() != nullptr);
        CHECK_MESSAGE(
            evaluated->text()->contains(
                "read:project file name ../outside.txt contains forbidden "
                "parent traversal"
            ),
            "framework.project.read_text must refuse a path that leaves the "
            "authoring store"
        );
        CHECK_MESSAGE(
            evaluated->text()->contains(
                "write:project file name ../outside.txt contains forbidden "
                "parent traversal"
            ),
            "framework.project.write_text must refuse a path that leaves the "
            "authoring store"
        );
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
                    local project = require("@umbraflow/project")
                    local ok, raised = pcall(
                        project.write_text,
                        {
                            path = "runtime/annotation.txt",
                            content = "a stroke",
                        }
                    )
                    return if ok then "written" else tostring(raised.error.message)
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
                    "Policy operator-deny-all names no rule for effect type "
                    "framework.project.write_text"
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
