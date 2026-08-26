// The production read-only path, composed end to end over a recorded target.
//
// Every part below this verb is already covered somewhere else: tests/deployment
// covers the directory load, tests/operator covers the installed-generation CAS,
// and the conformance run covers the resolver. What is only covered here is that
// one entry runs them in order and stops -- so the two cases that matter are the
// ones that go red when it acts on the target, and when it answers a screen it
// could not read as a screen with nothing on it.
//
// The recorded seam is modules/conformance's own: an ObservationFrameSource over
// a decoded capture, declaring TargetWorld::Recorded, which is what
// EngineSession::create matches the sink against.

#include <cli/args.hpp>
#include <cli/cli-result.hpp>
#include <cli/invoke.hpp>
#include <cli/observe.hpp>

#include <conformance/observation-fixture.hpp>
#include <conformance/operator-protocol.hpp>

#include <operator/agent-profile.hpp>
#include <operator/controller.hpp>
#include <operator/ledger.hpp>
#include <operator/project-observation.hpp>

#include <service/product-lifecycle.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/ids.hpp>
#include <domain/key.hpp>
#include <domain/space.hpp>

#include <engine/ports.hpp>
#include <engine/session.hpp>

#include <json/value.hpp>

#include <ocr/engine.hpp>

#include <trace/file-sink.hpp>
#include <trace/recorder.hpp>

#include "../json/repository-path.hpp"

#include <doctest/doctest.h>

#include <sqlite3.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::cli
{
    namespace
    {
        // The geometry this recorded world presents. It is the extent and DPI
        // examples/umbraflow/runtime/artifact/runtime-model.toml declares, which
        // is what a live target would have to present for the engine to accept
        // the capture at all; the conformance run is what holds the model and
        // the capture beside it to each other.
        constexpr auto k_recordedWidth  = uint32{3};
        constexpr auto k_recordedHeight = uint32{1};
        constexpr auto k_recordedDpi    = uint32{96};

        // Templated because two verbs are composed in this file and each
        // answers with its own report; the caller only ever reads this when
        // the call failed.
        template <typename Value>
        [[nodiscard]]
        auto why(Result<Value> const& outcome) -> std::string
        {
            return outcome.has_value()
                ? std::string{"<the call succeeded>"}
                : formatError(outcome.error());
        }

        // Counts every verb the engine was asked to post, through a counter the
        // test owns rather than one the sink holds.
        //
        // EngineSession takes ownership of the sink and destroys it inside the
        // call under test, so conformance::ObservationActionSink -- which
        // answers clicks() off itself -- could only be read through a pointer
        // that is dangling exactly where the assertion needs it. The recorded
        // world is still the fixture's: this differs from it only in where the
        // count lives, and it counts all six verbs rather than clicks alone,
        // because "delivered nothing" is a claim about the whole sink.
        class RecordedActionSink final : public engine::IActionSink
        {
            std::shared_ptr<uint32> m_delivered;

        public:
            explicit RecordedActionSink(std::shared_ptr<uint32> delivered) noexcept
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

            // Deliberately NOT counted: the teardown runs after every delivery
            // and posts nothing of its own, so counting it would make
            // "delivered nothing" impossible to state.
            [[nodiscard]] auto releaseHeldInputs() -> Status override
            {
                return ok();
            }

            [[nodiscard]] auto targetWorld() const noexcept -> TargetWorld override
            {
                return TargetWorld::Recorded;
            }
        };

        // An engine that is present and answers nothing. It stands for a bound
        // OCR adapter and not for a missing one: the case that removes the
        // adapter passes a null pointer, which is the state
        // platform::bindOcrEngine produces from an absent directory.
        class PresentReader final : public ocr::IOcrEngine
        {
        public:
            [[nodiscard]] auto identity() const noexcept -> std::string_view override
            {
                return "test-present-reader";
            }

            [[nodiscard]]
            auto read(
                BgraImage const&,
                ocr::ReadSpec const&
            ) -> Result<ocr::Readout> override
            {
                return ocr::Readout{};
            }
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

        // What one read-only query answers over the Operator's own database,
        // as text. The lease and the session row it belongs to are ledger
        // state and reach no verb's result, so a case that asks whether the
        // Operator was left holding a lease has to ask the Operator's storage.
        //
        // A coordinator holds the file under PRAGMA locking_mode=EXCLUSIVE for
        // its whole lifetime, so this may only run once the call under test has
        // returned and the lifecycle it built has been destroyed.
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

        // One row of the Operator's control_transitions table: what the lease
        // on the controlled target did, and the session it did it to.
        struct ControlTransition final
        {
            std::string transition{};
            std::string sessionId{};
        };

        // One row of the Operator's sessions table, reduced to the pair this
        // file asks about: who authenticated, and which principal the ledger
        // pinned them as.
        struct PinnedController final
        {
            std::string controllerId{};
            std::string kind{};
        };

        // The stream identity every trace line carries. TraceRecorder writes no
        // header, so the identity is only ever visible on an event, and one
        // line is enough: the stream validator refuses a stream whose
        // session_id or session_manifest_hash moves within it.
        struct TraceStreamIdentity final
        {
            std::string sessionId{};
            std::string sessionManifestHash{};
        };

        [[nodiscard]]
        auto jsonStringMember(
            std::string_view line,
            std::string_view member
        ) -> std::string
        {
            auto const opening = "\"" + std::string{member} + "\":\"";
            auto const named   = line.find(opening);
            REQUIRE_MESSAGE(named != std::string_view::npos, member);
            auto const rest   = line.substr(named + opening.size());
            auto const closed = rest.find('"');
            REQUIRE(closed != std::string_view::npos);
            return std::string{rest.substr(0U, closed)};
        }

        [[nodiscard]]
        auto traceStreamIdentity(
            std::filesystem::path const& trace
        ) -> TraceStreamIdentity
        {
            auto stream = std::ifstream{trace, std::ios::binary};
            REQUIRE(stream.good());
            auto line = std::string{};
            REQUIRE(std::getline(stream, line));
            REQUIRE_FALSE(line.empty());
            return TraceStreamIdentity{
                .sessionId           = jsonStringMember(line, "session_id"),
                .sessionManifestHash = jsonStringMember(line, "session_manifest_hash"),
            };
        }

        [[nodiscard]]
        auto frameworkToolIdentity(std::string_view material) -> ContentHash
        {
            auto const hashed = sha256(std::as_bytes(std::span{material}));
            REQUIRE(hashed.has_value());
            return *hashed;
        }

        // The result bytes of the three Framework Tool calls one run issues,
        // named by the position each one occupies rather than by its tool: the
        // two observes are deliberately the same request, and what separates
        // them is only the ordinal the seam gave them.
        struct FrameworkToolPayloads final
        {
            std::string firstObserve{};
            std::string secondObserve{};
            std::string waited{};
            std::string audited{};
            std::string status{};
        };

        // The exemplar copied out of the repository, its RuntimeArtifact
        // installed into a production Operator root beside it, and the capture
        // the project published.
        //
        // Installing here rather than in the verb is the shape under test: a
        // release reaches the CAS through a deployment act, and what the verb
        // does is open what that act left behind.
        class RecordedWorld final
        {
            std::filesystem::path  m_root{};
            std::filesystem::path  m_project{};
            std::filesystem::path  m_runtime{};
            std::vector<std::byte> m_probe{};

        public:
            RecordedWorld()
            {
                m_root = (
                    std::filesystem::temp_directory_path()
                    / std::filesystem::path{
                        "uf-observe-" + std::to_string(std::random_device{}()),
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

                // Scoped, so the SQLite handle this test opened is closed before
                // the verb opens the same root.
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
                    : installed.error().message();
                REQUIRE_MESSAGE(
                    installed.has_value(),
                    installedWhy
                );
            }

            RecordedWorld(RecordedWorld const&)                    = delete;
            RecordedWorld(RecordedWorld&&)                         = delete;
            auto operator=(RecordedWorld const&) -> RecordedWorld& = delete;
            auto operator=(RecordedWorld&&) -> RecordedWorld&      = delete;

            ~RecordedWorld()
            {
                auto discarded = std::error_code{};
                std::filesystem::remove_all(m_root, discarded);
            }

            // Every byte the Operator holds. The coordinator this fixture
            // installed through was destroyed inside the constructor, so WAL
            // frames are already checkpointed into this file and any write a
            // later call commits and closes lands here too.
            [[nodiscard]] auto ledgerBytes() const -> std::string
            {
                auto stream = std::ifstream{
                    m_runtime / "operator-runtime.sqlite",
                    std::ios::binary,
                };
                REQUIRE(stream.good());
                return std::string{
                    std::istreambuf_iterator<char>{stream},
                    std::istreambuf_iterator<char>{},
                };
            }

            // Every control transition the Operator recorded for this world, in
            // the order it recorded them. acquire and release are written by
            // the same transaction that takes and drops the lease, so this is
            // what the store itself says happened to it.
            [[nodiscard]]
            auto controlTransitions() const -> std::vector<ControlTransition>
            {
                auto const rows = ledgerRows(
                    m_runtime / "operator-runtime.sqlite",
                    "SELECT transition, session_id FROM control_transitions "
                    "ORDER BY sequence"
                );
                auto transitions = std::vector<ControlTransition>{};
                for (auto const& row : rows)
                {
                    REQUIRE(row.size() == 2U);
                    transitions.emplace_back(
                        ControlTransition{
                            .transition = row[0],
                            .sessionId  = row[1],
                        }
                    );
                }
                return transitions;
            }

            // The SessionManifest hash the Operator admitted this session
            // under, or nothing when no session of that name was ever pinned.
            [[nodiscard]]
            auto pinnedManifestHash(
                std::string_view sessionId
            ) const -> std::optional<std::string>
            {
                auto const rows = ledgerRows(
                    m_runtime / "operator-runtime.sqlite",
                    "SELECT session_id, manifest_hash FROM sessions"
                );
                for (auto const& row : rows)
                {
                    REQUIRE(row.size() == 2U);
                    if (row[0] == sessionId)
                    {
                        return row[1];
                    }
                }
                return std::nullopt;
            }

            // The principal every pinned session was recorded as, read out of
            // the Operator's own sessions table rather than out of anything
            // the verb returned. A run's controller kind reaches no report and
            // no trace, so this row is the only place a caller can see which
            // principal the ledger actually holds the run to.
            [[nodiscard]]
            auto pinnedControllers() const -> std::vector<PinnedController>
            {
                auto const rows = ledgerRows(
                    m_runtime / "operator-runtime.sqlite",
                    "SELECT authenticated_controller_id, controller_kind "
                    "FROM sessions ORDER BY authenticated_controller_id"
                );
                auto pinned = std::vector<PinnedController>{};
                for (auto const& row : rows)
                {
                    REQUIRE(row.size() == 2U);
                    pinned.emplace_back(
                        PinnedController{
                            .controllerId = row[0],
                            .kind         = row[1],
                        }
                    );
                }
                return pinned;
            }

            // Puts one document beside the project and answers with its path.
            [[nodiscard]]
            auto document(
                std::string_view name,
                std::string_view bytes
            ) const -> std::filesystem::path
            {
                auto const path = m_root / std::filesystem::path{name};
                auto stream = std::ofstream{path, std::ios::binary};
                REQUIRE(stream.good());
                stream << bytes;
                REQUIRE(stream.good());
                return path;
            }

            [[nodiscard]] auto tracePath(std::string_view trace) const
                -> std::filesystem::path
            {
                return m_root / trace;
            }

            // Leaves bytes where a trace is about to be written. FileTraceSink
            // refuses a file that already carries evidence, which is the first
            // failure reachable after the lifecycle has started.
            auto occupyTrace(std::string_view trace) const -> void
            {
                auto stream = std::ofstream{tracePath(trace), std::ios::binary};
                REQUIRE(stream.good());
                stream << "{}\n";
                REQUIRE(stream.good());
            }

            // One trace path per call: FileTraceSink refuses a file that already
            // carries evidence, so two observations in one case need two.
            [[nodiscard]] auto args(std::string_view trace) const -> ObserveArgs
            {
                return ObserveArgs{
                    .project      = m_project,
                    .windowHandle = 0,
                    .runtime      = m_runtime,
                    .ocrModels    = m_project,
                    .trace        = tracePath(trace),
                };
            }

            [[nodiscard]]
            auto sources(
                std::shared_ptr<uint32> delivered,
                std::unique_ptr<ocr::IOcrEngine> ocrEngine
            ) const -> ObserveSources
            {
                auto const fingerprint = ProjectFingerprint::create(
                    k_recordedWidth,
                    k_recordedHeight,
                    k_recordedDpi,
                    k_recordedDpi
                );
                REQUIRE(fingerprint.has_value());
                return ObserveSources{
                    .frameSource = std::make_unique<
                        operator_runtime::conformance::ObservationFrameSource
                    >(
                        operator_runtime::conformance::observationFrame(
                            m_probe,
                            FrameId{4001}
                        )
                    ),
                    .actionSink      = std::make_unique<RecordedActionSink>(
                        std::move(delivered)
                    ),
                    .ocrEngine       = std::move(ocrEngine),
                    .liveFingerprint = *fingerprint,
                };
            }

            // Gives this world the PolicyArtifact its Operator states: the one
            // effect a Framework input Tool declares, and the Privileged
            // surface of the bare-coordinate Tool that declares it.
            //
            // It is written into the production root and nowhere else, because
            // that root is the Operator's. Without it every mutating admission
            // falls to the deny-all artifact ProductLifecycle substitutes for
            // an absent one, and no mutating provider is ever reached. The
            // artifact is built against the published Operator protocol
            // schema's hash because that is the one ProductLifecycle pins into
            // the SessionManifest a policy is verified against.
            auto authorizeMutation() const -> void
            {
                constexpr auto k_operatorSchema = std::string_view{
                    "schema/umbraflow-operator-v1.schema.json"
                };
                auto const root = json::repositoryRoot(k_operatorSchema);
                REQUIRE_FALSE(root.empty());
                auto schemaStream = std::ifstream{
                    root / k_operatorSchema,
                    std::ios::binary,
                };
                REQUIRE(schemaStream.good());
                auto const schemaBytes = std::string{
                    std::istreambuf_iterator<char>{schemaStream},
                    std::istreambuf_iterator<char>{},
                };
                auto const schemaHash = sha256(
                    std::as_bytes(std::span{std::string_view{schemaBytes}})
                );
                REQUIRE(schemaHash.has_value());
                auto const effects = std::vector<std::string>{
                    std::string{"framework.input.click"},
                    std::string{"framework.input.drag"},
                    std::string{"framework.input.hold"},
                    std::string{"framework.input.key"},
                    std::string{"framework.input.move"},
                    std::string{"framework.input.scroll"},
                    std::string{"framework.ui.click"},
                    std::string{"framework.ui.hold"},
                    std::string{"framework.ui.move"},
                };
                auto const granted = std::vector<std::string>{
                    std::string{"framework.input.click"},
                    std::string{"framework.input.drag"},
                    std::string{"framework.input.hold"},
                    std::string{"framework.input.key"},
                    std::string{"framework.input.move"},
                    std::string{"framework.input.scroll"},
                };
                auto const policy =
                    operator_runtime::conformance::policyArtifactBytes(
                        *schemaHash,
                        effects,
                        granted
                    );
                std::filesystem::create_directories(m_runtime);
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
            }
        };

        // Starts one production lifecycle over the recorded world and runs
        // `body` against it and the TaskContext its engine session backs.
        //
        // Everything one run owns -- trace sink, recorder, engine session and
        // context -- lives for exactly the body's extent, which is what makes
        // two runs over one world two incarnations rather than one, and is what
        // lets a case restart a run and watch recorded outcomes replay.
        template <typename Body>
        auto runProductLifecycle(
            ObserveArgs const& args,
            ObserveSources sources,
            std::string_view controllerId,
            std::vector<std::string> capabilities,
            operator_runtime::ObservedInstanceWorldScope const& scope,
            Body&& body
        ) -> void
        {
            auto const liveFingerprint = sources.liveFingerprint;
            auto lifecycle = service::ProductLifecycle::start(
                service::ProductStart{
                    .projectDirectory          = args.project,
                    .runtimeDirectory          = args.runtime,
                    .authenticatedControllerId = std::string{controllerId},
                    .controllerCapabilities    = std::move(capabilities),
                    .controlledTargetId        = "recorded-tool-target",
                    // The kind every case here means. These cases drive the
                    // adapters and the lease, not the per-kind ceilings, and
                    // Human is the one kind that needs no AgentProfile and
                    // reaches the whole Tool surface -- so a case about
                    // something else is never silently also a case about a
                    // budget.
                    .kind            = operator_runtime::ControllerKind::Human,
                    .agentProfileJcs = std::string{
                        operator_runtime::k_unboundedAgentProfileJcs
                    },
                    .worldScope      = scope,
                }
            );
            auto const lifecycleWhy = lifecycle.has_value()
                ? std::string{}
                : lifecycle.error().message();
            REQUIRE_MESSAGE(lifecycle.has_value(), lifecycleWhy);
            auto const identity = lifecycle->identity();

            auto sink = trace::FileTraceSink::createNew(args.trace);
            REQUIRE(sink.has_value());
            auto recorder = trace::TraceRecorder::create(
                std::move(*sink),
                trace::TraceStreamSpec{
                    .sessionId           = identity.sessionId,
                    .sessionManifestHash = identity.sessionManifestHash,
                    .producer            = "framework-tool-adapter-test",
                }
            );
            REQUIRE(recorder.has_value());
            auto session = engine::EngineSession::create(
                std::move(sources.frameSource),
                std::move(sources.actionSink),
                *recorder,
                engine::EngineSessionConfig{
                    .liveFingerprint         = liveFingerprint,
                    .projectFingerprint      = identity.runtimeModel.fingerprint(),
                    .maximumPixelComparisons = args.budget,
                    .recognitionTimeout      = args.recognitionTimeout,
                },
                std::move(sources.ocrEngine)
            );
            REQUIRE(session.has_value());
            auto context = task::TaskContext{std::move(*session), *recorder};

            body(*lifecycle, context);
            CHECK(lifecycle->shutdown().has_value());
        }

        // What one Framework Tool call left in the ledger: the state its row
        // reached and the exact payload bytes it recorded. A case reads both
        // because a mutating call's classification is half of what it proves --
        // proven_absent and confirmed carry the same payload shape and mean
        // opposite things.
        struct FrameworkToolOutcome final
        {
            operator_runtime::ToolCallState state{};
            std::string                     payload{};
        };

        [[nodiscard]]
        auto screenshotArguments(std::string_view receiptPayload) -> std::string
        {
            auto const parsed = json::parse(receiptPayload);
            REQUIRE_MESSAGE(parsed.has_value(), receiptPayload);
            auto const* const p_hash = parsed->find("screenshot_sha256");
            REQUIRE_MESSAGE(p_hash != nullptr, receiptPayload);
            REQUIRE(p_hash->kind() == json::ValueKind::String);
            return R"({"screenshot_sha256":")"
                + std::string{p_hash->string()} + R"("})";
        }

        // The error code every failed native-input answer records. Named rather
        // than matched inside prose so a case that expects one refusal cannot
        // pass on another. Provider failures use the same exact error object the
        // synchronous caller receives.
        [[nodiscard]]
        auto inputErrorCode(std::string_view payload) -> std::string
        {
            auto const parsed = json::parse(payload);
            REQUIRE_MESSAGE(parsed.has_value(), payload);
            auto const* const p_code = parsed->find("code");
            REQUIRE_MESSAGE(p_code != nullptr, payload);
            auto const* const p_retryable = parsed->find("retryable");
            REQUIRE_MESSAGE(p_retryable != nullptr, payload);
            CHECK_FALSE(p_retryable->boolean());
            return std::string{p_code->string()};
        }

        // The other half of a refusal payload: what it says went wrong. A code
        // names the KIND of refusal and the message names the thing --
        // the surface an aim was outside of, the key name that is not one.
        [[nodiscard]]
        auto inputErrorMessage(std::string_view payload) -> std::string
        {
            auto const parsed = json::parse(payload);
            REQUIRE_MESSAGE(parsed.has_value(), payload);
            auto const* const p_message = parsed->find("message");
            REQUIRE_MESSAGE(p_message != nullptr, payload);
            return std::string{p_message->string()};
        }
    }

    TEST_CASE(
        "bare production project reaches first observe without an external internal identifier"
    )
    {
        auto const world     = RecordedWorld{};
        auto const delivered = std::make_shared<uint32>();

        auto const observed = observeProject(
            world.args("resolved.jsonl"),
            world.sources(delivered, std::make_unique<PresentReader>())
        );
        REQUIRE_MESSAGE(
            observed.has_value(),
            "a bare production project must reach first observe without an "
            "external internal identifier: ",
            why(observed)
        );

        // The composition reached every stage: the directory registered a
        // plugin, the Operator answered for the artifact this project names,
        // and the Host resolved a state over the capture.
        CHECK(observed->pluginId == "fixture.alpha");
        CHECK(observed->installedGeneration > 0U);
        CHECK(observed->modelWidth == k_recordedWidth);
        CHECK(observed->liveWidth == k_recordedWidth);
        CHECK_FALSE(observed->artifactRootHash.empty());
        CHECK_MESSAGE(
            observed->stateResolution.contains(R"("kind":"resolved_state")"),
            observed->stateResolution
        );

        // What the verb prints has to let a reader see WHY: the ordered stack
        // is in the document, and so is one entry per Reader that reported.
        CHECK(observed->stateResolution.contains(R"("ordered_surface_stack")"));
        auto const report = formatObservedState(*observed);
        CHECK(report.contains(observed->stateResolution));
        CHECK(report.contains("model declares"));

        // The whole of the phase limit, in one number. A verb that planned,
        // minted a Receipt or dispatched would post here.
        CHECK(*delivered == 0U);
    }

    TEST_CASE(
        "production Framework read-only Tools execute and replay through ProductLifecycle"
    )
    {
        auto const world     = RecordedWorld{};
        auto const delivered = std::make_shared<uint32>();

        auto const scope = operator_runtime::ObservedInstanceWorldScope::run(
            "recorded-tool-target",
            1
        );
        REQUIRE(scope.has_value());

        auto const executionIdentity = operator_runtime::ToolExecutionIdentity{
            .runIdentity = frameworkToolIdentity("framework-tool-run"),
            .frameworkReleaseIdentity = frameworkToolIdentity(
                "framework-release"
            ),
            .toolRuntimeProtocolIdentity = frameworkToolIdentity(
                "tool-runtime-protocol"
            ),
            .environmentIdentity = frameworkToolIdentity(
                "native-adapter-environment"
            ),
        };
        auto const frameworkCall = [&executionIdentity](
                                       std::string_view toolName,
                                       std::string_view exactArgumentsJcs
                                   )
        {
            return service::ToolRootCall{
                .requestKey                 = "recorded-observe-and-wait",
                .exactRootRequestPreimageJcs =
                    R"({"objective":"observe and wait"})",
                .executionIdentity = executionIdentity,
                .toolName          = std::string{toolName},
                .exactArgumentsJcs = std::string{exactArgumentsJcs},
            };
        };

        // One whole run over the recorded world -- its own lifecycle, its own
        // TaskContext, and the five calls it issues under one root request.
        //
        // It is a lambda called twice because the caller no longer names a
        // coordinate: the ordinal is the seam's, so a repeated request inside
        // one run is a NEW position rather than the earlier one's address. A
        // recorded outcome is therefore reachable only by restarting, which is
        // the durability contract the Tool Runtime actually offers.
        auto runFrameworkTools = [&](std::string_view trace)
        {
            auto payloads = FrameworkToolPayloads{};
            runProductLifecycle(
                world.args(trace),
                world.sources(delivered, std::make_unique<PresentReader>()),
                "framework-tool-adapter",
                {},
                *scope,
                [&](service::ProductLifecycle& lifecycle,
                    task::TaskContext& context)
                {
                    auto issued = [&lifecycle, &context, &frameworkCall](
                                      std::string_view toolName,
                                      std::string_view exactArgumentsJcs
                                  )
                    {
                        auto const replay = lifecycle.invokeTool(
                            frameworkCall(toolName, exactArgumentsJcs),
                            context
                        );
                        auto const replayWhy = replay.has_value()
                            ? std::string{}
                            : replay.error().message();
                        REQUIRE_MESSAGE(replay.has_value(), replayWhy);
                        CHECK(
                            replay->state
                            == operator_runtime::ToolCallState::Confirmed
                        );
                        REQUIRE(replay->payload.has_value());
                        return std::string{replay->payload->bytes()};
                    };

                    auto const captured = issued(
                        "framework.screen.capture",
                        "{}"
                    );
                    auto const screenshot = screenshotArguments(captured);

                    // Braced initialization, so the calls are issued in the
                    // declaration order their ordinals follow.
                    payloads = FrameworkToolPayloads{
                        .firstObserve = issued(
                            "framework.screen.observe",
                            screenshot
                        ),
                        .secondObserve = issued(
                            "framework.screen.observe",
                            screenshot
                        ),
                        .waited        = issued(
                            "framework.workflow.wait",
                            R"({"duration_ms":0})"
                        ),
                        .audited = issued(
                            "framework.audit.record",
                            R"({"record":{"note":"observed"}})"
                        ),
                        .status = issued("framework.workflow.status", "{}"),
                    };
                }
            );
            return payloads;
        };

        auto const executed = runFrameworkTools("framework-tools.jsonl");

        auto const parsedPayload = json::parse(executed.firstObserve);
        REQUIRE(parsedPayload.has_value());
        auto const* const p_snapshotRef = parsedPayload->find("snapshot_ref");
        REQUIRE(p_snapshotRef != nullptr);
        CHECK_FALSE(p_snapshotRef->string().empty());
        auto const* const p_stateResolution =
            parsedPayload->find("state_resolution");
        REQUIRE(p_stateResolution != nullptr);
        CHECK(p_stateResolution->kind() == json::ValueKind::Object);
        auto const* const p_target =
            parsedPayload->find("controlled_target_id");
        REQUIRE(p_target != nullptr);
        CHECK(p_target->string() == "recorded-tool-target");
        CHECK(executed.waited == R"({"completed":true,"duration_ms":0})");

        // The observation authority section 6 requires travels in the Tool
        // result as an OBJECT, so a script can hold it, return it and record it
        // with no host object crossing the boundary. Every binding a later
        // native input is judged on is in it.
        auto const* const p_reference =
            parsedPayload->find("observation_reference");
        REQUIRE(p_reference != nullptr);
        CHECK(p_reference->kind() == json::ValueKind::Object);
        for (auto const* const binding : {
                 "controlled_target_id",
                 "expires_at_unix_ms",
                 "frame_identity_hash",
                 "host_generation",
                 "project_registration_hash",
                 "runtime_artifact_root_hash",
                 "screenshot_sha256",
                 "ui_actions",
             })
        {
            CHECK_MESSAGE(p_reference->find(binding) != nullptr, binding);
        }

        // The audit Tool's durable row IS the record: the arguments carry it
        // and the outcome names the hash of exactly those bytes, so nothing
        // beside the ledger has to be consulted to read what a run recorded.
        auto const auditedRecord = json::parse(executed.audited);
        REQUIRE(auditedRecord.has_value());
        auto const* const p_recorded = auditedRecord->find("recorded");
        REQUIRE(p_recorded != nullptr);
        CHECK(p_recorded->boolean());
        auto const recordHash = sha256(
            std::as_bytes(std::span{std::string_view{R"({"note":"observed"})"}})
        );
        REQUIRE(recordHash.has_value());
        auto const* const p_recordHash = auditedRecord->find("record_hash");
        REQUIRE(p_recordHash != nullptr);
        CHECK(p_recordHash->string() == recordHash->hex());

        // Status reports the run rather than the screen: it observes nothing,
        // and a lifecycle that still holds its lease reports itself writable.
        auto const reportedStatus = json::parse(executed.status);
        REQUIRE(reportedStatus.has_value());
        auto const* const p_access = reportedStatus->find("access");
        REQUIRE(p_access != nullptr);
        CHECK(p_access->string() == "writable");

        // The two observes are byte-identical requests, and they still differ:
        // the second one got its own ordinal, executed on its own and minted
        // its own observation. That is what stops a caller from addressing a
        // position it was never granted and collecting its recorded outcome.
        CHECK(executed.firstObserve != executed.secondObserve);

        // Restart. The run reissues the same three calls, its fresh issuing
        // context numbers from 1 again, and every coordinate lands on a
        // recorded outcome. The recorded source returns the same frame, but a
        // re-executed provider would mint a new observation id under a new
        // host nonce -- so byte equality is what proves nothing ran.
        auto const replayed = runFrameworkTools("framework-tools-restart.jsonl");
        CHECK(replayed.firstObserve == executed.firstObserve);
        CHECK(replayed.secondObserve == executed.secondObserve);
        CHECK(replayed.waited == executed.waited);
        CHECK(replayed.audited == executed.audited);

        // Status is the one recorded outcome whose replay is load-bearing on
        // its own: the second run's session id differs from the first's, so
        // equality here is the ledger answering and not the provider.
        CHECK(replayed.status == executed.status);

        CHECK(*delivered == 0U);
    }

    TEST_CASE("each raw input Tool rejects an unretained screenshot by name")
    {
        auto const missingHash = std::string(64U, 'f');
        auto const calls = std::vector{
            std::pair{
                std::string{"framework.input.click"},
                R"({"screenshot_sha256":")" + missingHash
                    + R"(","x":1,"y":0})"
            },
            std::pair{
                std::string{"framework.input.drag"},
                R"({"screenshot_sha256":")" + missingHash
                    + R"(","to_x":2,"to_y":0,"travel_ms":0,"x":1,"y":0})"
            },
            std::pair{
                std::string{"framework.input.hold"},
                R"({"duration_ms":0,"screenshot_sha256":")" + missingHash
                    + R"(","x":1,"y":0})"
            },
            std::pair{
                std::string{"framework.input.key"},
                R"({"key":"ESC","screenshot_sha256":")" + missingHash
                    + R"("})"
            },
            std::pair{
                std::string{"framework.input.move"},
                R"({"screenshot_sha256":")" + missingHash
                    + R"(","x":1,"y":0})"
            },
            std::pair{
                std::string{"framework.input.scroll"},
                R"({"notches":1,"screenshot_sha256":")" + missingHash
                    + R"("})"
            },
        };

        for (auto const& [tool, exactArguments] : calls)
        {
            auto const world = RecordedWorld{};
            world.authorizeMutation();
            auto const delivered = std::make_shared<uint32>();
            auto const scope = operator_runtime::ObservedInstanceWorldScope::run(
                "recorded-tool-target",
                1
            );
            REQUIRE(scope.has_value());

            runProductLifecycle(
                world.args("missing-screenshot.jsonl"),
                world.sources(delivered, std::make_unique<PresentReader>()),
                "missing-screenshot-adapter",
                {std::string{
                    operator_runtime::conformance::k_operateCapability
                }},
                *scope,
                [&](service::ProductLifecycle& lifecycle, task::TaskContext& context)
                {
                    auto const refused = lifecycle.invokeTool(
                        service::ToolRootCall{
                            .requestKey = "missing-screenshot",
                            .exactRootRequestPreimageJcs =
                                R"({"objective":"refuse an unretained screenshot"})",
                            .executionIdentity = operator_runtime::ToolExecutionIdentity{
                                .runIdentity = frameworkToolIdentity("framework-input-run"),
                                .frameworkReleaseIdentity = frameworkToolIdentity(
                                    "framework-release"
                                ),
                                .toolRuntimeProtocolIdentity = frameworkToolIdentity(
                                    "tool-runtime-protocol"
                                ),
                                .environmentIdentity = frameworkToolIdentity(
                                    "native-adapter-environment"
                                ),
                            },
                            .toolName          = tool,
                            .exactArgumentsJcs = exactArguments,
                        },
                        context
                    );
                    auto const refusedWhy = refused.has_value()
                        ? std::string{}
                        : refused.error().message();
                    INFO("tool: ", tool);
                    REQUIRE_MESSAGE(refused.has_value(), refusedWhy);
                    REQUIRE(refused->payload.has_value());
                    auto const refusal = std::string{refused->payload->bytes()};
                    INFO("refusal payload: ", refusal);
                    auto const refusedUnretainedScreenshot =
                        refusal.contains(missingHash)
                        && refusal.contains("retained evidence blob")
                        && *delivered == 0U;
                    CHECK_MESSAGE(refusedUnretainedScreenshot, tool);
                }
            );
        }
    }

    TEST_CASE("semantic input spends one observation")
    {
        auto const world = RecordedWorld{};
        world.authorizeMutation();
        auto const delivered = std::make_shared<uint32>();

        auto const scope = operator_runtime::ObservedInstanceWorldScope::run(
            "recorded-tool-target",
            1
        );
        REQUIRE(scope.has_value());

        auto const executionIdentity = operator_runtime::ToolExecutionIdentity{
            .runIdentity = frameworkToolIdentity("framework-input-run"),
            .frameworkReleaseIdentity = frameworkToolIdentity(
                "framework-release"
            ),
            .toolRuntimeProtocolIdentity = frameworkToolIdentity(
                "tool-runtime-protocol"
            ),
            .environmentIdentity = frameworkToolIdentity(
                "native-adapter-environment"
            ),
        };

        runProductLifecycle(
            world.args("framework-input.jsonl"),
            world.sources(delivered, std::make_unique<PresentReader>()),
            "framework-input-adapter",
            // The capability the fixture policy's allow rule requires. Without
            // it the same artifact denies the same effect, which is what makes
            // the rule a rule rather than a decoration.
            {std::string{
                operator_runtime::conformance::k_operateCapability
            }},
            *scope,
            [&](service::ProductLifecycle& lifecycle, task::TaskContext& context)
            {
                auto issued = [&](std::string_view requestKey,
                                  std::string_view toolName,
                                  std::string_view exactArgumentsJcs)
                {
                    auto const replay = lifecycle.invokeTool(
                        service::ToolRootCall{
                            .requestKey = std::string{requestKey},
                            .exactRootRequestPreimageJcs =
                                R"({"objective":"deliver one input"})",
                            .executionIdentity = executionIdentity,
                            .toolName          = std::string{toolName},
                            .exactArgumentsJcs = std::string{exactArgumentsJcs},
                        },
                        context
                    );
                    auto const replayWhy = replay.has_value()
                        ? std::string{}
                        : replay.error().message();
                    INFO("tool: ", toolName);
                    INFO("arguments: ", exactArgumentsJcs);
                    REQUIRE_MESSAGE(replay.has_value(), replayWhy);
                    REQUIRE(replay->payload.has_value());
                    return FrameworkToolOutcome{
                        .state   = replay->state,
                        .payload = std::string{replay->payload->bytes()},
                    };
                };

                auto const oldDeliver = lifecycle.invokeTool(
                    service::ToolRootCall{
                        .requestKey = "input-root",
                        .exactRootRequestPreimageJcs =
                            R"({"objective":"deliver one input"})",
                        .executionIdentity = executionIdentity,
                        .toolName          = "framework.input.deliver",
                        .exactArgumentsJcs = R"({"action":"click","x":1,"y":0})",
                    },
                    context
                );
                REQUIRE_FALSE(oldDeliver.has_value());

                auto const captured = issued(
                    "input-root",
                    "framework.screen.capture",
                    "{}"
                );
                auto const screenshot = screenshotArguments(captured.payload);
                auto pointArguments = [&screenshot](uint32 x, uint32 y)
                {
                    auto arguments = screenshot;
                    arguments.pop_back();
                    arguments += R"(,"x":)" + std::to_string(x)
                        + R"(,"y":)" + std::to_string(y) + "}";
                    return arguments;
                };

                // The scope of an input injection is the target surface this
                // registration declared and no wider. The recorded world is 3x1
                // and x = 3 is off it, so the aim is refused before anything is
                // captured or posted -- proven absence, naming the surface.
                auto const offSurface = issued(
                    "input-root",
                    "framework.input.click",
                    pointArguments(3U, 0U)
                );
                CHECK(
                    offSurface.state
                    == operator_runtime::ToolCallState::ProvenAbsent
                );
                CHECK(inputErrorCode(offSurface.payload) == "input_refused");
                CHECK(
                    inputErrorMessage(offSurface.payload).find("3x1 target surface")
                    != std::string::npos
                );

                // The same Tool aimed on the surface DELIVERS. The lifecycle
                // binds a Human controller, whose profile is not restricted to
                // semantic tools, so the Privileged surface is what decides who
                // may reach this Tool and being a Framework Tool did not widen
                // it.
                auto const machineAimed = issued(
                    "input-root",
                    "framework.input.click",
                    pointArguments(1U, 0U)
                );
                CHECK(
                    machineAimed.state
                    == operator_runtime::ToolCallState::Confirmed
                );
                auto const machinePayload = json::parse(machineAimed.payload);
                REQUIRE_MESSAGE(
                    machinePayload.has_value(),
                    machineAimed.payload
                );
                auto const* const p_machinePosted =
                    machinePayload->find("delivered");
                REQUIRE_MESSAGE(p_machinePosted != nullptr, machineAimed.payload);
                CHECK(p_machinePosted->boolean());
                CHECK(*delivered == 1U);

                auto const observed = issued(
                    "input-root",
                    "framework.screen.observe",
                    screenshot
                );
                CHECK(
                    observed.state == operator_runtime::ToolCallState::Confirmed
                );
                auto const observedPayload = json::parse(observed.payload);
                REQUIRE(observedPayload.has_value());
                auto const* const p_reference =
                    observedPayload->find("observation_reference");
                REQUIRE(p_reference != nullptr);
                auto const reference = json::canonicalBytes(*p_reference);

                auto presented = [&reference](
                                     std::string_view uiTarget,
                                     std::string_view binding,
                                     std::string_view action
                                 )
                {
                    return R"({"action":")" + std::string{action}
                        + R"(","binding":")" + std::string{binding}
                        + R"(","observation_reference":)" + reference
                        + R"(,"ui_target":")" + std::string{uiTarget} + R"("})";
                };

                // Two refusals on the observation's own bounds, both before any
                // successful consumption. They are ordered first on purpose: a
                // refusal spends nothing, so the authority they refused has to
                // still be available to the call below that is entitled to it.
                auto const unknownTarget = issued(
                    "input-root",
                    "framework.ui.click",
                    presented(
                        "fixture.absent",
                        "fixture.target.primary",
                        "fixture.press"
                    )
                );
                CHECK(
                    unknownTarget.state
                    == operator_runtime::ToolCallState::ProvenAbsent
                );
                CHECK(
                    inputErrorCode(unknownTarget.payload) == "unknown_ui_target"
                );
                CHECK(
                    inputErrorMessage(unknownTarget.payload).contains("fixture.absent")
                );

                auto const unknownBinding = issued(
                    "input-root",
                    "framework.ui.click",
                    presented(
                        "fixture.target",
                        "fixture.missing",
                        "fixture.press"
                    )
                );
                CHECK(
                    inputErrorCode(unknownBinding.payload) == "unknown_binding"
                );
                CHECK(
                    inputErrorMessage(unknownBinding.payload).contains("fixture.missing")
                );

                auto const wrongKind = issued(
                    "input-root",
                    "framework.ui.move",
                    presented(
                        "fixture.target",
                        "fixture.target.primary",
                        "fixture.press"
                    )
                );
                CHECK(inputErrorCode(wrongKind.payload) == "action_kind_mismatch");
                CHECK(inputErrorMessage(wrongKind.payload).contains("fixture.press"));

                // The one call that is entitled to it. It resolves against the
                // same snapshot, registration, RuntimeArtifact, Host generation
                // and issuing coordinate the observation was minted under, and
                // spends the authority.
                auto const delivering = issued(
                    "input-root",
                    "framework.ui.click",
                    presented(
                        "fixture.target",
                        "fixture.target.primary",
                        "fixture.press"
                    )
                );

                // It posts. The Host captures its own frame, resolves
                // fixture.target on it, authorizes fixture.press on the Binding
                // that resolved and delivers the Receipt that mint produced;
                // the classification below is the ledger's reading of what the
                // Host reported and not this provider's.
                CHECK(
                    delivering.state
                    == operator_runtime::ToolCallState::Confirmed
                );
                auto const deliveringPayload = json::parse(delivering.payload);
                REQUIRE_MESSAGE(
                    deliveringPayload.has_value(),
                    delivering.payload
                );
                auto const* const p_posted = deliveringPayload->find("delivered");
                REQUIRE_MESSAGE(p_posted != nullptr, delivering.payload);
                CHECK(p_posted->boolean());
                auto const* const p_deliveredVerdict =
                    deliveringPayload->find("verdict");
                REQUIRE_MESSAGE(p_deliveredVerdict != nullptr, delivering.payload);
                CHECK(p_deliveredVerdict->string() == "delivered");

                // At most one native input consumes one observation authority.
                auto const repeated = issued(
                    "input-root",
                    "framework.ui.click",
                    presented(
                        "fixture.target",
                        "fixture.target.primary",
                        "fixture.press"
                    )
                );
                CHECK(inputErrorCode(repeated.payload) == "already_consumed");

                // Bytes this Framework never minted are refused at the seam,
                // before a durable coordinate exists for them: recognition is
                // byte equality against the recorded wire form and nothing
                // else, so an edited reference is not a reference.
                auto const forged = std::string{
                    R"({"schema":"framework.observation_reference/2"})"
                };
                REQUIRE(forged != reference);
                auto const unminted = lifecycle.invokeTool(
                    service::ToolRootCall{
                        .requestKey = "input-root",
                        .exactRootRequestPreimageJcs =
                            R"({"objective":"deliver one input"})",
                        .executionIdentity = executionIdentity,
                        .toolName          = "framework.ui.click",
                        .exactArgumentsJcs =
                            R"({"action":"fixture.press","binding":"fixture.target.primary","observation_reference":)"
                            + forged + R"(,"ui_target":"fixture.target"})",
                    },
                    context
                );
                REQUIRE_FALSE(unminted.has_value());
                CHECK_MESSAGE(
                    std::string{unminted.error().message()}.contains("unminted"),
                    unminted.error().message()
                );

                auto holdArguments = pointArguments(1U, 0U);
                holdArguments.insert(1U, R"("duration_ms":0,)"
                );
                auto const held = issued(
                    "input-root",
                    "framework.input.hold",
                    holdArguments
                );
                CHECK(
                    held.state == operator_runtime::ToolCallState::Confirmed
                );
            }
        );

        // The raw click, semantic click and raw hold are the only three inputs
        // that reached the sink.
        CHECK(*delivered == 3U);
    }

    TEST_CASE("observe restarts through Coordinator and remains repeatable")
    {
        auto const world     = RecordedWorld{};
        auto const delivered = std::make_shared<uint32>();
        auto const first = observeProject(
            world.args("unchanged-first.jsonl"),
            world.sources(delivered, std::make_unique<PresentReader>())
        );
        REQUIRE_MESSAGE(first.has_value(), why(first));
        auto const afterFirst = world.ledgerBytes();
        CHECK_FALSE(afterFirst.empty());

        // Twice, because "read only" and "idempotent" are different claims and
        // a verb that wrote once on a first open would satisfy only the second.
        auto const second = observeProject(
            world.args("unchanged-second.jsonl"),
            world.sources(delivered, std::make_unique<PresentReader>())
        );
        REQUIRE_MESSAGE(second.has_value(), why(second));
        CHECK_FALSE(world.ledgerBytes().empty());
        CHECK(*delivered == 0U);
    }

    // A leaked control lease has no symptom a later process can see: every
    // OperatorCoordinator::open rotates the session epoch and deletes every
    // control_leases row on the reading that whatever they describe died with
    // the process that wrote them. A case that starts a lifecycle, tears it
    // down and then looks for the lease therefore passes with the release
    // deleted, and so does one that starts a second lifecycle -- open refuses a
    // second coordinator while the first is alive, and clears the leases once
    // it is not, so no second session in any process can ever be blocked by the
    // first session's lease. What the store does keep across both is the
    // transition it wrote when the lease was taken and when it was dropped, and
    // that is what these two cases read.
    TEST_CASE("two observations in one process each drop the lease they took")
    {
        auto const world     = RecordedWorld{};
        auto const delivered = std::make_shared<uint32>();

        auto const first = observeProject(
            world.args("released-first.jsonl"),
            world.sources(delivered, std::make_unique<PresentReader>())
        );
        REQUIRE_MESSAGE(first.has_value(), why(first));
        auto const second = observeProject(
            world.args("released-second.jsonl"),
            world.sources(delivered, std::make_unique<PresentReader>())
        );
        REQUIRE_MESSAGE(second.has_value(), why(second));

        auto const transitions = world.controlTransitions();
        REQUIRE_MESSAGE(
            transitions.size() == 4U,
            "two observations must leave one acquire and one release each: ",
            transitions.size()
        );
        CHECK(transitions[0].transition == "acquire");
        CHECK(transitions[1].transition == "release");
        CHECK(transitions[2].transition == "acquire");
        CHECK(transitions[3].transition == "release");

        // Each release belongs to the session that took the lease, and the two
        // runs are two sessions rather than one name reused.
        CHECK(transitions[0].sessionId == transitions[1].sessionId);
        CHECK(transitions[2].sessionId == transitions[3].sessionId);
        CHECK(transitions[0].sessionId != transitions[2].sessionId);
    }

    TEST_CASE("observe drops the lease on a path that fails after the lifecycle started")
    {
        auto const world     = RecordedWorld{};
        auto const delivered = std::make_shared<uint32>();
        world.occupyTrace("occupied.jsonl");

        auto const failed = observeProject(
            world.args("occupied.jsonl"),
            world.sources(delivered, std::make_unique<PresentReader>())
        );
        REQUIRE_FALSE(failed.has_value());
        CHECK_MESSAGE(
            why(failed).contains("already contains evidence"),
            why(failed)
        );

        auto const transitions = world.controlTransitions();
        REQUIRE_MESSAGE(
            transitions.size() == 2U,
            "a failure after start must still drop the lease start took: ",
            transitions.size()
        );
        CHECK(transitions[0].transition == "acquire");
        CHECK(transitions[1].transition == "release");
    }

    // What --actor actually buys, asserted where it lands rather than where it
    // was typed. A run's controller kind reaches no report, no trace and no
    // return value: it is written into the Operator's sessions row and read
    // back out of it by every later refusal. So the only way to say "invoke
    // pins the principal the caller named" is to run the verb and read that
    // row.
    //
    // The case is worth its seconds because three separate claims land on it
    // at once, and each has its own mutation:
    //
    //  - The kind reaches the ledger. Make principalOf(AgentToolRequest) answer
    //    Human and the recorded kind stops being "agent" -- which is what makes
    //    an agent escaping every Agent gate a red run rather than prose.
    //  - The controller id is composed from that kind. Compose it from anything
    //    else and the id stops being the wire name's, so two actors sharing one
    //    id -- and therefore one durable root identity for one --request-key --
    //    cannot come back unnoticed.
    //  - The AgentProfile verified. An Agent session the ledger will pin at all
    //    is one that presented a budget which hashed to the manifest it was
    //    attested against and satisfied the published AgentBudget definition;
    //    break the profile document or drop it and start fails before any row
    //    exists.
    TEST_CASE("invoke pins the principal --actor names")
    {
        auto const world     = RecordedWorld{};
        auto const delivered = std::make_shared<uint32>();

        // A real AgentBudget document, in the member order RFC 8785 puts them
        // in. Its bytes are what agent_profile_hash attests to, so the numbers
        // below are the session's actual ceilings and not a fixture's decoration.
        auto const profile = world.document(
            "budget.json",
            "{\"maximum_elapsed_ms\":3600000,\"maximum_mutations\":0,"
            "\"maximum_observations\":8,\"maximum_risk_units\":0,"
            "\"maximum_tool_calls\":4}"
        );
        auto const objective = world.document(
            "objective.json",
            "{\"goal\":\"see\"}"
        );
        auto const arguments = world.document("arguments.json", "{}");

        auto const observeArgs = world.args("invoke-agent.jsonl");
        auto const args        = InvokeArgs{
            .project      = observeArgs.project,
            .windowHandle = observeArgs.windowHandle,
            .runtime      = observeArgs.runtime,
            .ocrModels    = observeArgs.ocrModels,
            .requestKey   = "root-agent-1",
            .request      = AgentToolRequest{
                .toolName             = "framework.screen.capture",
                .objectiveDocument    = objective,
                .argumentsDocument    = arguments,
                .agentProfileDocument = profile,
            },
            .trace = observeArgs.trace,
        };

        auto const report = invokeTool(
            args,
            world.sources(delivered, std::make_unique<PresentReader>())
        );
        REQUIRE_MESSAGE(report.has_value(), why(report));
        CHECK(report->actor == "agent");

        // One session, pinned as the principal the actor named, under an id
        // that spells that same kind.
        auto const pinned = world.pinnedControllers();
        REQUIRE(pinned.size() == 1U);
        CHECK(pinned[0].kind == "agent");
        CHECK(pinned[0].controllerId == "umbra-flow-invoke-agent");
    }

    TEST_CASE("destroying an unclosed lifecycle drops the lease it took")
    {
        auto const world = RecordedWorld{};
        auto const args  = world.args("unused.jsonl");

        {
            auto const scope = operator_runtime::ObservedInstanceWorldScope::run(
                "recorded-target",
                1
            );
            REQUIRE(scope.has_value());
            auto lifecycle = service::ProductLifecycle::start(
                service::ProductStart{
                    .projectDirectory          = args.project,
                    .runtimeDirectory          = args.runtime,
                    .authenticatedControllerId = "destructor-fallback",
                    .controllerCapabilities    = {},
                    .controlledTargetId        = "recorded-target",
                    .kind                      = operator_runtime::ControllerKind::Human,
                    .agentProfileJcs           = std::string{
                        operator_runtime::k_unboundedAgentProfileJcs
                    },
                    .worldScope                = *scope,
                }
            );
            CAPTURE(
                lifecycle.has_value()
                    ? std::string{}
                    : lifecycle.error().message()
            );
            REQUIRE(lifecycle.has_value());
        }

        auto const transitions = world.controlTransitions();
        REQUIRE(transitions.size() == 2U);
        CHECK(transitions[0].transition == "acquire");
        CHECK(transitions[1].transition == "release");
        CHECK(transitions[0].sessionId == transitions[1].sessionId);
    }

    TEST_CASE("explicit lifecycle shutdown is reporting and idempotent")
    {
        auto const world = RecordedWorld{};
        auto const args  = world.args("unused.jsonl");

        {
            auto const scope = operator_runtime::ObservedInstanceWorldScope::run(
                "recorded-target",
                1
            );
            REQUIRE(scope.has_value());
            auto lifecycle = service::ProductLifecycle::start(
                service::ProductStart{
                    .projectDirectory          = args.project,
                    .runtimeDirectory          = args.runtime,
                    .authenticatedControllerId = "explicit-shutdown",
                    .controllerCapabilities    = {},
                    .controlledTargetId        = "recorded-target",
                    .kind                      = operator_runtime::ControllerKind::Human,
                    .agentProfileJcs           = std::string{
                        operator_runtime::k_unboundedAgentProfileJcs
                    },
                    .worldScope                = *scope,
                }
            );
            CAPTURE(
                lifecycle.has_value()
                    ? std::string{}
                    : lifecycle.error().message()
            );
            REQUIRE(lifecycle.has_value());
            CHECK(lifecycle->shutdown().has_value());
            CHECK(lifecycle->shutdown().has_value());
        }

        auto const transitions = world.controlTransitions();
        REQUIRE(transitions.size() == 2U);
        CHECK(transitions[0].transition == "acquire");
        CHECK(transitions[1].transition == "release");
    }

    TEST_CASE("the trace stream names the Operator session that produced it")
    {
        auto const world     = RecordedWorld{};
        auto const delivered = std::make_shared<uint32>();

        auto const observed = observeProject(
            world.args("identity.jsonl"),
            world.sources(delivered, std::make_unique<PresentReader>())
        );
        REQUIRE_MESSAGE(observed.has_value(), why(observed));

        auto const stream = traceStreamIdentity(world.tracePath("identity.jsonl"));
        auto const pinned = world.pinnedManifestHash(stream.sessionId);
        REQUIRE_MESSAGE(
            pinned.has_value(),
            "the trace names a session the Operator never pinned: ",
            stream.sessionId
        );

        // The join the stream exists to allow: its manifest hash is the one the
        // Operator admitted that session under. The RuntimeModel's semantic
        // hash is a different value, so writing that one instead cannot pass
        // this by coincidence.
        REQUIRE(observed->modelSemanticHash != *pinned);
        CHECK(stream.sessionManifestHash == *pinned);
        CHECK(stream.sessionManifestHash != observed->modelSemanticHash);
    }

    TEST_CASE("observe refuses a missing OCR engine rather than reading nothing")
    {
        auto const world     = RecordedWorld{};
        auto const delivered = std::make_shared<uint32>();

        auto const observed = observeProject(
            world.args("no-ocr.jsonl"),
            world.sources(delivered, nullptr)
        );
        REQUIRE_FALSE(observed.has_value());
        CHECK(
            automationErrorKind(observed.error())
            == AutomationErrorKind::UnsupportedCapability
        );
        CHECK_MESSAGE(
            why(observed).contains("--ocr-models"),
            why(observed)
        );
    }

    TEST_CASE("observe requires the OCR model directory on the command line")
    {
        auto const raw = std::vector<std::string>{
            "--project",
            "project",
            "--hwnd",
            "0x1",
            "--runtime",
            "production",
        };
        auto const parsed = parseObserveArguments(raw);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(formatError(parsed.error()).contains("--ocr-models"));
    }
}
