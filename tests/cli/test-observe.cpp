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
#include <cli/observe.hpp>

#include <conformance/observation-fixture.hpp>

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

#include <ocr/engine.hpp>

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

        [[nodiscard]]
        auto why(Result<ObservedState> const& outcome) -> std::string
        {
            return outcome.has_value()
                ? std::string{"<the observation succeeded>"}
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
            auto longPress(
                Point<ClientSpace>,
                MonotonicInstant::Duration,
                ObservationLease const&
            ) -> Status override
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
                    m_root / "handoff",
                    m_project / "runtime" / "artifact"
                );

                // Scoped, so the SQLite handle this test opened is closed before
                // the verb opens the same root.
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
        };
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
