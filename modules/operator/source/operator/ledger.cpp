#include "ledger.hpp"
#include "runtime-installation.hpp"

#include <core/error/contracts.hpp>
#include <core/text/json-text.hpp>
#include <core/text/utf8.hpp>

#include <domain/error.hpp>

#include <task/platform/confined-file.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        struct DatabaseCloser final
        {
            auto operator()(sqlite3* database) const noexcept -> void
            {
                static_cast<void>(sqlite3_close_v2(database));
            }
        };

        using Database = std::unique_ptr<sqlite3, DatabaseCloser>;

        class Statement final
        {
            sqlite3_stmt* m_statement;

        public:
            explicit Statement(sqlite3_stmt* statement) noexcept
                : m_statement{statement}
            {
            }

            Statement(Statement&& other) noexcept
                : m_statement{std::exchange(other.m_statement, nullptr)}
            {
            }

            auto operator=(Statement&& other) noexcept -> Statement&
            {
                if (this != &other)
                {
                    static_cast<void>(sqlite3_finalize(m_statement));
                    m_statement = std::exchange(other.m_statement, nullptr);
                }
                return *this;
            }

            Statement(Statement const&) = delete;
            auto operator=(Statement const&) -> Statement& = delete;

            ~Statement()
            {
                static_cast<void>(sqlite3_finalize(m_statement));
            }

            [[nodiscard]] auto get() const noexcept -> sqlite3_stmt*
            {
                return m_statement;
            }
        };

        [[nodiscard]]
        auto databaseFailure(
            sqlite3* database,
            std::string_view action
        ) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::IoFailure,
                std::format("Operator database {}: {}", action, sqlite3_errmsg(database))
            );
        }

        [[nodiscard]]
        auto execute(
            sqlite3* database,
            std::string_view sql
        ) -> Status
        {
            auto* message = static_cast<char*>(nullptr);
            auto const code = sqlite3_exec(
                database,
                sql.data(),
                nullptr,
                nullptr,
                &message
            );
            if (code == SQLITE_OK)
            {
                return ok();
            }

            auto detail = message == nullptr
                ? std::string{sqlite3_errmsg(database)}
                : std::string{message};
            sqlite3_free(message);
            return fail(
                AutomationErrorKind::IoFailure,
                std::format("Operator database statement failed: {}", detail)
            );
        }

        [[nodiscard]]
        auto prepare(
            sqlite3* database,
            std::string_view sql
        ) -> Result<Statement>
        {
            auto* statement = static_cast<sqlite3_stmt*>(nullptr);
            auto const code = sqlite3_prepare_v3(
                database,
                sql.data(),
                static_cast<int>(sql.size()),
                SQLITE_PREPARE_PERSISTENT,
                &statement,
                nullptr
            );
            if (code != SQLITE_OK)
            {
                return databaseFailure(database, "could not prepare statement");
            }
            return Statement{statement};
        }

        [[nodiscard]]
        auto bindText(
            sqlite3* database,
            sqlite3_stmt* statement,
            int index,
            std::string_view value
        ) -> Status
        {
            auto const code = sqlite3_bind_text64(
                statement,
                index,
                value.data(),
                static_cast<sqlite3_uint64>(value.size()),
                SQLITE_TRANSIENT,
                SQLITE_UTF8
            );
            if (code != SQLITE_OK)
            {
                return databaseFailure(database, "could not bind text");
            }
            return ok();
        }

        [[nodiscard]]
        auto bindInteger(
            sqlite3* database,
            sqlite3_stmt* statement,
            int index,
            uint64 value
        ) -> Status
        {
            if (value > static_cast<uint64>(std::numeric_limits<sqlite3_int64>::max()))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Operator integer exceeds SQLite signed range"
                );
            }
            auto const code = sqlite3_bind_int64(
                statement,
                index,
                static_cast<sqlite3_int64>(value)
            );
            if (code != SQLITE_OK)
            {
                return databaseFailure(database, "could not bind integer");
            }
            return ok();
        }

        [[nodiscard]]
        auto checkedSqlIncrement(
            uint64 value,
            std::string_view field
        ) -> Result<uint64>
        {
            if (value == static_cast<uint64>(std::numeric_limits<sqlite3_int64>::max()))
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    std::format("{} exhausted SQLite's integer range", field)
                );
            }
            return value + 1U;
        }

        [[nodiscard]]
        auto unixTimeMilliseconds() -> Result<uint64>
        {
            auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            if (elapsed < 0)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "System wall clock precedes the Unix epoch"
                );
            }
            return static_cast<uint64>(elapsed);
        }

        [[nodiscard]]
        auto expectDone(
            sqlite3* database,
            sqlite3_stmt* statement
        ) -> Status
        {
            if (sqlite3_step(statement) != SQLITE_DONE)
            {
                return databaseFailure(database, "write failed");
            }
            return ok();
        }

        [[nodiscard]]
        auto columnText(
            sqlite3_stmt* statement,
            int index
        ) -> std::string
        {
            auto const* bytes = sqlite3_column_text(statement, index);
            auto const size = sqlite3_column_bytes(statement, index);
            if (bytes == nullptr || size <= 0)
            {
                return {};
            }
            auto value = std::string{};
            value.reserve(static_cast<std::size_t>(size));
            for (auto byteIndex = 0; byteIndex < size; ++byteIndex)
            {
                value.push_back(static_cast<char>(bytes[byteIndex]));
            }
            return value;
        }

        // Every hash column in this database holds bare lowercase hex, because
        // that is the form each comparison against ContentHash::hex() needs.
        // Rebuilding a ContentHash from a column therefore has to restore the
        // canonical prefix that ContentHash::parse requires.
        [[nodiscard]]
        auto parseHashColumn(std::string_view columnHex) -> Result<ContentHash>
        {
            return ContentHash::parse(std::format("sha256:{}", columnHex));
        }

        [[nodiscard]]
        auto requireName(
            std::string_view value,
            std::string_view field
        ) -> Status
        {
            if (value.empty() || !isValidUtf8(value))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format("{} must be non-empty valid UTF-8", field)
                );
            }
            return ok();
        }

        class Transaction final
        {
            sqlite3* m_database;
            bool m_active{true};

            explicit Transaction(sqlite3* database) noexcept
                : m_database{database}
            {
            }

        public:
            Transaction(Transaction&& other) noexcept
                : m_database{other.m_database}
                , m_active{std::exchange(other.m_active, false)}
            {
            }

            Transaction(Transaction const&) = delete;
            auto operator=(Transaction const&) -> Transaction& = delete;
            auto operator=(Transaction&&) -> Transaction& = delete;

            ~Transaction()
            {
                if (m_active)
                {
                    static_cast<void>(sqlite3_exec(
                        m_database,
                        "ROLLBACK",
                        nullptr,
                        nullptr,
                        nullptr
                    ));
                }
            }

            [[nodiscard]]
            static auto begin(sqlite3* database) -> Result<Transaction>
            {
                UF_TRY(execute(database, "BEGIN IMMEDIATE"));
                return Transaction{database};
            }

            [[nodiscard]] auto commit() -> Status
            {
                UF_TRY(execute(m_database, "COMMIT"));
                m_active = false;
                return ok();
            }
        };

        // sha256 over the canonicalization verifyExactDatabaseSchema builds:
        // every sqlite_schema row ordered by (type, name), each of its four
        // columns written as <byte length>:<value>. It therefore covers the
        // STORED DDL TEXT -- reindenting the R"sql(...)" block below changes it
        // even when the schema is identical, and so does adding a comment
        // inside it. Any change to that block recomputes this value and the
        // expectedTables list in the same change, from a freshly created
        // database rather than by hand. initialize() verifies immediately after
        // creating the schema, so a forgotten recomputation cannot ship green.
        constexpr auto k_exactSchemaV1Fingerprint = std::string_view{
            "sha256:5738e6f98534efbdfc3114413de70c032b64e2cbaa84d4c152ec6cbb512120a4"
        };

        [[nodiscard]]
        auto verifyExactDatabaseSchema(sqlite3* database) -> Status
        {
            UF_TRY_VALUE(
                query,
                prepare(
                    database,
                    "SELECT type, name, tbl_name, coalesce(sql, '') FROM sqlite_schema "
                    "WHERE name NOT LIKE 'sqlite_%' ORDER BY type, name"
                )
            );
            auto canonical = std::string{};
            auto step      = sqlite3_step(query.get());
            while (step == SQLITE_ROW)
            {
                for (auto column = 0; column < 4; ++column)
                {
                    auto const value = columnText(query.get(), column);
                    canonical += std::to_string(value.size());
                    canonical.push_back(':');
                    canonical += value;
                }
                step = sqlite3_step(query.get());
            }
            if (step != SQLITE_DONE)
            {
                return databaseFailure(database, "could not fingerprint exact schema");
            }
            UF_TRY_VALUE(
                actual,
                sha256(std::as_bytes(std::span{canonical}))
            );
            UF_TRY_VALUE(expected, ContentHash::parse(k_exactSchemaV1Fingerprint));
            if (actual != expected)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Operator database schema bytes do not match exact v1"
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto randomToken(sqlite3* database) -> Result<std::string>
        {
            UF_TRY_VALUE(
                statement,
                prepare(database, "SELECT lower(hex(randomblob(32)))")
            );
            if (sqlite3_step(statement.get()) != SQLITE_ROW)
            {
                return databaseFailure(database, "could not mint opaque token");
            }
            return columnText(statement.get(), 0);
        }

        [[nodiscard]]
        auto initialize(sqlite3* database) -> Status
        {
            if (sqlite3_busy_timeout(database, 5'000) != SQLITE_OK)
            {
                return databaseFailure(database, "could not set busy timeout");
            }

            UF_TRY(execute(
                database,
                "PRAGMA journal_mode=WAL;"
                "PRAGMA foreign_keys=ON;"
                "PRAGMA synchronous=FULL;"
                "PRAGMA trusted_schema=OFF;"
            ));

            auto readInteger = [database](std::string_view sql) -> Result<uint64>
            {
                UF_TRY_VALUE(statement, prepare(database, sql));
                if (sqlite3_step(statement.get()) != SQLITE_ROW)
                {
                    return databaseFailure(database, "could not read database identity");
                }
                return static_cast<uint64>(sqlite3_column_int64(statement.get(), 0));
            };
            auto readText = [database](std::string_view sql) -> Result<std::string>
            {
                UF_TRY_VALUE(statement, prepare(database, sql));
                if (sqlite3_step(statement.get()) != SQLITE_ROW)
                {
                    return databaseFailure(database, "could not read database schema");
                }
                return columnText(statement.get(), 0);
            };

            UF_TRY_VALUE(journalMode, readText("PRAGMA journal_mode"));
            UF_TRY_VALUE(foreignKeys, readInteger("PRAGMA foreign_keys"));
            UF_TRY_VALUE(synchronous, readInteger("PRAGMA synchronous"));
            UF_TRY_VALUE(trustedSchema, readInteger("PRAGMA trusted_schema"));
            if (
                journalMode != "wal"
                || foreignKeys != 1U
                || synchronous != 2U
                || trustedSchema != 0U
            )
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    "Operator database safety PRAGMA read-back failed"
                );
            }

            UF_TRY_VALUE(applicationId, readInteger("PRAGMA application_id"));
            UF_TRY_VALUE(userVersion, readInteger("PRAGMA user_version"));
            UF_TRY_VALUE(
                tableCount,
                readInteger(
                    "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                    "AND name NOT LIKE 'sqlite_%'"
                )
            );
            constexpr auto applicationIdentity = uint64{0x55464F50U};
            if (applicationId != 0U || userVersion != 0U || tableCount != 0U)
            {
                if (applicationId != applicationIdentity || userVersion != 1U)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "Database is not the exact Operator runtime schema v1"
                    );
                }
                UF_TRY_VALUE(
                    tables,
                    readText(
                        "SELECT group_concat(name, ',') FROM (SELECT name FROM sqlite_master "
                        "WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name)"
                    )
                );
                constexpr auto expectedTables = std::string_view{
                    "approvals,authority_decisions,control_leases,control_transitions,"
                    "dispatches,fencing_high_water,journal_events,operations,"
                    "project_instances,project_registrations,project_state,reconciliations,"
                    "runtime_artifacts,runtime_installations,runtime_publications,"
                    "runtime_state,sessions,snapshots"
                };
                if (tables != expectedTables)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "Operator database table set does not match schema v1"
                    );
                }
                UF_TRY(verifyExactDatabaseSchema(database));
                UF_TRY_VALUE(integrity, readText("PRAGMA quick_check"));
                if (integrity != "ok")
                {
                    return fail(
                        AutomationErrorKind::IoFailure,
                        "Operator database quick_check failed"
                    );
                }
                return ok();
            }

            UF_TRY(execute(
                database,
                R"sql(
                    BEGIN IMMEDIATE;
                    CREATE TABLE IF NOT EXISTS runtime_artifacts(
                        artifact_root_hash TEXT PRIMARY KEY
                    ) STRICT;

                    CREATE TABLE IF NOT EXISTS runtime_state(
                        singleton INTEGER PRIMARY KEY CHECK(singleton = 1),
                        current_session_epoch INTEGER NOT NULL
                            CHECK(current_session_epoch >= 0),
                        installed_generation INTEGER NOT NULL
                            CHECK(installed_generation >= 0),
                        active_runtime_artifact_root_hash TEXT
                            REFERENCES runtime_artifacts(artifact_root_hash)
                    ) STRICT;
                    INSERT INTO runtime_state(
                        singleton,
                        current_session_epoch,
                        installed_generation,
                        active_runtime_artifact_root_hash
                    ) VALUES(1, 0, 0, NULL);

                    CREATE TABLE IF NOT EXISTS runtime_installations(
                        installed_generation INTEGER PRIMARY KEY
                            CHECK(installed_generation > 0),
                        artifact_root_hash TEXT NOT NULL
                            REFERENCES runtime_artifacts(artifact_root_hash),
                        release_manifest_hash TEXT NOT NULL,
                        UNIQUE(installed_generation, artifact_root_hash)
                    ) STRICT;

                    -- One row per publication in flight: taken before anything
                    -- is written under the production root and gone before the
                    -- installing call returns. Together with the two foreign
                    -- keys above it is the whole reference count on an artifact
                    -- directory, and SQLite refuses to delete a runtime_artifacts
                    -- row while any of the three still points at it.
                    CREATE TABLE IF NOT EXISTS runtime_publications(
                        staging_token TEXT PRIMARY KEY,
                        artifact_root_hash TEXT NOT NULL
                            REFERENCES runtime_artifacts(artifact_root_hash)
                    ) STRICT;

                    CREATE TABLE IF NOT EXISTS project_registrations(
                        registration_hash TEXT PRIMARY KEY,
                        plugin_id TEXT NOT NULL,
                        plugin_hash TEXT NOT NULL,
                        project_state_schema_hash TEXT NOT NULL,
                        canonical_manifest TEXT NOT NULL
                    ) STRICT;

                    CREATE TABLE IF NOT EXISTS project_instances(
                        plugin_id TEXT NOT NULL,
                        project_instance_key TEXT NOT NULL,
                        project_registration_hash TEXT NOT NULL
                            REFERENCES project_registrations(registration_hash),
                        baseline_event_id TEXT NOT NULL UNIQUE,
                        PRIMARY KEY(plugin_id, project_instance_key),
                        UNIQUE(project_registration_hash, project_instance_key)
                    ) STRICT;

                    CREATE TABLE IF NOT EXISTS sessions(
                        session_id TEXT PRIMARY KEY,
                        authenticated_controller_id TEXT NOT NULL,
                        idempotency_namespace TEXT NOT NULL,
                        manifest_hash TEXT NOT NULL,
                        runtime_artifact_root_hash TEXT NOT NULL,
                        installed_generation INTEGER NOT NULL
                            CHECK(installed_generation > 0),
                        project_registration_hash TEXT NOT NULL
                            REFERENCES project_registrations(registration_hash),
                        capability_profile_hash TEXT NOT NULL,
                        session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),
                        controlled_target_key TEXT NOT NULL,
                        project_instance_key TEXT NOT NULL,
                        mode TEXT NOT NULL CHECK(mode IN ('read', 'write')),
                        active INTEGER NOT NULL CHECK(active IN (0, 1)),
                        FOREIGN KEY(project_registration_hash, project_instance_key)
                            REFERENCES project_instances(
                                project_registration_hash,
                                project_instance_key
                            ),
                        FOREIGN KEY(installed_generation, runtime_artifact_root_hash)
                            REFERENCES runtime_installations(
                                installed_generation,
                                artifact_root_hash
                            )
                    ) STRICT;

                    CREATE UNIQUE INDEX IF NOT EXISTS one_active_write_session_per_instance
                    ON sessions(project_registration_hash, project_instance_key)
                    WHERE mode='write' AND active=1;

                    CREATE TABLE IF NOT EXISTS fencing_high_water(
                        controlled_target_key TEXT PRIMARY KEY,
                        fencing_token INTEGER NOT NULL CHECK(fencing_token > 0)
                    ) STRICT;

                    CREATE TABLE IF NOT EXISTS control_leases(
                        controlled_target_key TEXT PRIMARY KEY,
                        lease_id TEXT NOT NULL UNIQUE,
                        session_id TEXT NOT NULL REFERENCES sessions(session_id),
                        controller_id TEXT NOT NULL,
                        session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),
                        fencing_token INTEGER NOT NULL CHECK(fencing_token > 0),
                        revision INTEGER NOT NULL CHECK(revision > 0),
                        capability_profile_hash TEXT NOT NULL
                    ) STRICT;

                    CREATE TABLE IF NOT EXISTS control_transitions(
                        sequence INTEGER PRIMARY KEY AUTOINCREMENT,
                        controlled_target_key TEXT NOT NULL,
                        session_id TEXT NOT NULL,
                        controller_id TEXT NOT NULL,
                        lease_id TEXT NOT NULL,
                        session_epoch INTEGER NOT NULL,
                        fencing_token INTEGER NOT NULL,
                        transition TEXT NOT NULL,
                        reason TEXT NOT NULL
                    ) STRICT;

                    CREATE TABLE IF NOT EXISTS snapshots(
                        token TEXT PRIMARY KEY,
                        session_id TEXT NOT NULL REFERENCES sessions(session_id),
                        session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),
                        identity_hash TEXT NOT NULL,
                        lease_revision INTEGER NOT NULL CHECK(lease_revision > 0)
                    ) STRICT;

                    CREATE TABLE IF NOT EXISTS operations(
                        operation_id TEXT PRIMARY KEY,
                        session_id TEXT NOT NULL REFERENCES sessions(session_id),
                        snapshot_token TEXT NOT NULL REFERENCES snapshots(token),
                        idempotency_namespace TEXT NOT NULL,
                        client_request_id TEXT NOT NULL,
                        command_fingerprint TEXT NOT NULL,
                        tool_name TEXT NOT NULL,
                        tool_version TEXT NOT NULL,
                        canonical_args TEXT NOT NULL,
                        controlled_target_key TEXT NOT NULL,
                        mutating INTEGER NOT NULL CHECK(mutating IN (0, 1)),
                        state TEXT NOT NULL,
                        frozen_plan_hash TEXT,
                        revision INTEGER NOT NULL CHECK(revision > 0),
                        plugin_id TEXT NOT NULL,
                        project_instance_key TEXT NOT NULL,
                        FOREIGN KEY(plugin_id, project_instance_key)
                            REFERENCES project_instances(plugin_id, project_instance_key),
                        UNIQUE(
                            idempotency_namespace,
                            plugin_id,
                            project_instance_key,
                            client_request_id
                        )
                    ) STRICT;

                    CREATE UNIQUE INDEX IF NOT EXISTS one_active_mutation_per_target
                    ON operations(controlled_target_key)
                    WHERE mutating=1 AND state IN (
                        'proposed', 'awaiting_approval', 'ready', 'needs_revalidation',
                        'running', 'reconciling', 'ambiguous'
                    );

                    CREATE UNIQUE INDEX IF NOT EXISTS one_active_mutation_per_project_instance
                    ON operations(plugin_id, project_instance_key)
                    WHERE mutating=1 AND state IN (
                        'proposed', 'awaiting_approval', 'ready', 'needs_revalidation',
                        'running', 'reconciling', 'ambiguous'
                    );

                    CREATE TABLE IF NOT EXISTS authority_decisions(
                        authority_decision_id TEXT PRIMARY KEY,
                        operation_id TEXT NOT NULL REFERENCES operations(operation_id),
                        dispatch_sequence INTEGER NOT NULL CHECK(dispatch_sequence > 0),
                        session_id TEXT NOT NULL REFERENCES sessions(session_id),
                        controller_id TEXT NOT NULL,
                        lease_id TEXT NOT NULL,
                        session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),
                        fencing_token INTEGER NOT NULL CHECK(fencing_token > 0),
                        decision_basis_hash TEXT NOT NULL,
                        frozen_plan_hash TEXT NOT NULL,
                        step_intent_hash TEXT NOT NULL,
                        approval_token TEXT,
                        UNIQUE(operation_id, dispatch_sequence)
                    ) STRICT;

                    CREATE TABLE IF NOT EXISTS dispatches(
                        operation_id TEXT NOT NULL REFERENCES operations(operation_id),
                        dispatch_sequence INTEGER NOT NULL CHECK(dispatch_sequence > 0),
                        decision_basis_hash TEXT NOT NULL,
                        frozen_plan_hash TEXT NOT NULL,
                        authority_decision_id TEXT NOT NULL
                            REFERENCES authority_decisions(authority_decision_id),
                        delivery_outcome TEXT,
                        PRIMARY KEY(operation_id, dispatch_sequence)
                    ) STRICT;

                    CREATE TABLE IF NOT EXISTS approvals(
                        token TEXT PRIMARY KEY,
                        operation_id TEXT NOT NULL REFERENCES operations(operation_id),
                        session_id TEXT NOT NULL REFERENCES sessions(session_id),
                        controller_id TEXT NOT NULL,
                        controlled_target_key TEXT NOT NULL,
                        lease_id TEXT NOT NULL,
                        session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),
                        fencing_token INTEGER NOT NULL CHECK(fencing_token > 0),
                        command_fingerprint TEXT NOT NULL,
                        frozen_plan_hash TEXT NOT NULL,
                        step_intent_hash TEXT NOT NULL,
                        decision_basis_hash TEXT NOT NULL,
                        effect_envelope_hash TEXT NOT NULL,
                        policy_hash TEXT NOT NULL,
                        approver_principal TEXT NOT NULL,
                        approver_capability_hash TEXT NOT NULL,
                        authority_decision_id TEXT NOT NULL,
                        expires_at_unix_millis INTEGER NOT NULL CHECK(expires_at_unix_millis > 0),
                        consumed INTEGER NOT NULL DEFAULT 0 CHECK(consumed IN (0, 1)),
                        consumed_by_dispatch INTEGER,
                        UNIQUE(authority_decision_id)
                    ) STRICT;

                    CREATE TABLE IF NOT EXISTS journal_events(
                        event_id TEXT PRIMARY KEY,
                        plugin_id TEXT NOT NULL,
                        project_instance_key TEXT NOT NULL,
                        sequence INTEGER NOT NULL CHECK(sequence >= 0),
                        prior_project_state_revision INTEGER,
                        session_manifest_hash TEXT NOT NULL,
                        operation_id TEXT REFERENCES operations(operation_id),
                        namespaced_event_type TEXT NOT NULL,
                        payload_schema_hash TEXT NOT NULL,
                        canonical_event TEXT NOT NULL,
                        canonical_provenance TEXT NOT NULL,
                        FOREIGN KEY(plugin_id, project_instance_key)
                            REFERENCES project_instances(plugin_id, project_instance_key),
                        UNIQUE(plugin_id, project_instance_key, sequence)
                    ) STRICT;

                    CREATE TABLE IF NOT EXISTS reconciliations(
                        sequence INTEGER PRIMARY KEY AUTOINCREMENT,
                        operation_id TEXT NOT NULL REFERENCES operations(operation_id),
                        disposition TEXT NOT NULL,
                        proposal_hash TEXT NOT NULL,
                        canonical_proposal TEXT NOT NULL
                    ) STRICT;

                    CREATE TABLE IF NOT EXISTS project_state(
                        plugin_id TEXT NOT NULL,
                        project_instance_key TEXT NOT NULL,
                        revision INTEGER NOT NULL CHECK(revision >= 0),
                        project_registration_hash TEXT NOT NULL,
                        state_schema_hash TEXT NOT NULL,
                        last_journal_sequence INTEGER NOT NULL CHECK(last_journal_sequence >= 0),
                        canonical_state TEXT NOT NULL,
                        state_hash TEXT NOT NULL,
                        FOREIGN KEY(plugin_id, project_instance_key)
                            REFERENCES project_instances(plugin_id, project_instance_key),
                        PRIMARY KEY(plugin_id, project_instance_key)
                    ) STRICT;

                    PRAGMA application_id=1430671184;
                    PRAGMA user_version=1;
                    COMMIT;
                )sql"
            ));
            return verifyExactDatabaseSchema(database);
        }

        [[nodiscard]]
        auto pathToUtf8(std::filesystem::path const& path) -> std::string
        {
            auto const encoded = path.generic_u8string();
            return std::string{encoded.begin(), encoded.end()};
        }

        [[nodiscard]]
        auto deliveryOutcomeWireName(
            DeliveryOutcome outcome
        ) noexcept -> std::string_view
        {
            switch (outcome)
            {
            case DeliveryOutcome::NotDelivered: return "not_delivered";
            case DeliveryOutcome::Delivered: return "delivered";
            case DeliveryOutcome::TransportUnknown: return "transport_unknown";
            }

            UF_UNREACHABLE_MSG("Unknown DeliveryOutcome value");
        }

        [[nodiscard]]
        auto sessionModeWireName(SessionMode mode) noexcept -> std::string_view
        {
            switch (mode)
            {
            case SessionMode::Read: return "read";
            case SessionMode::Write: return "write";
            }

            UF_UNREACHABLE_MSG("Unknown SessionMode value");
        }

        [[nodiscard]]
        auto reconciliationWireName(
            ReconcileDisposition disposition
        ) noexcept -> std::string_view
        {
            switch (disposition)
            {
            case ReconcileDisposition::Continue: return "continue";
            case ReconcileDisposition::Confirmed: return "confirmed";
            case ReconcileDisposition::Rejected: return "rejected";
            case ReconcileDisposition::Ambiguous: return "ambiguous";
            case ReconcileDisposition::Diverged: return "diverged";
            }

            UF_UNREACHABLE_MSG("Unknown ReconcileDisposition value");
        }

        [[nodiscard]]
        auto operationStateFor(
            ReconcileDisposition disposition
        ) noexcept -> OperationState
        {
            switch (disposition)
            {
            case ReconcileDisposition::Continue: return OperationState::Reconciling;
            case ReconcileDisposition::Confirmed: return OperationState::Confirmed;
            case ReconcileDisposition::Rejected: return OperationState::Rejected;
            case ReconcileDisposition::Ambiguous: return OperationState::Ambiguous;
            case ReconcileDisposition::Diverged: return OperationState::Diverged;
            }

            UF_UNREACHABLE_MSG("Unknown ReconcileDisposition value");
        }

        // The exact bytes the reducer is called with. JCS orders members by
        // their UTF-16 code units, which is why journal_events precedes
        // prior_project_state and, inside an event, namespaced_event_type
        // precedes opaque_project_payload precedes provenance.
        //
        // The Operator assembles this instead of accepting it, because a caller
        // that supplied the reducer's input could have the Journal record event
        // A while the materialized ProjectState was reduced from event B, and
        // the Journal prefix would no longer be the only source of
        // ProjectState. Every part comes from a value the schema owner minted
        // or from a column the database already holds.
        //
        // priorProjectStateJcs carries the literal `null` for a baseline, which
        // is a JSON value here rather than an absent member: a reducer must be
        // able to tell "no state yet" from "a state I failed to read".
        [[nodiscard]]
        auto reduceEnvelopeJcs(
            std::span<JournalAppend const> journalEvents,
            std::string_view priorProjectStateJcs
        ) -> std::string
        {
            auto envelope = std::string{"{\"journal_events\":["};
            auto first    = true;
            for (auto const& append : journalEvents)
            {
                if (!first)
                {
                    envelope += ',';
                }
                first = false;

                envelope += "{\"namespaced_event_type\":";
                appendJsonString(envelope, append.entry.namespacedEventType());
                envelope += ",\"opaque_project_payload\":";
                envelope += append.entry.payload().bytes();
                envelope += ",\"provenance\":";
                envelope += append.entry.provenance().bytes();
                envelope += '}';
            }
            envelope += "],\"prior_project_state\":";
            envelope += priorProjectStateJcs;
            envelope += '}';
            return envelope;
        }

        // An Operation may only be advanced by the session that owns it, while
        // that session is still active at this process epoch AND still holds
        // the lease on its target. The lease clause is separate from the epoch
        // one because takeoverLease replaces the lease row without deactivating
        // the session it replaced: a human takeover would otherwise leave the
        // displaced controller able to append to the Journal.
        constexpr auto k_liveControllerJoin = std::string_view{
            "JOIN sessions session ON session.session_id=o.session_id "
            "JOIN control_leases lease "
            "ON lease.controlled_target_key=o.controlled_target_key "
            "AND lease.session_id=o.session_id "
        };

        [[nodiscard]]
        auto discardPublication(
            sqlite3* database,
            std::string_view stagingToken
        ) -> Status
        {
            UF_TRY_VALUE(
                statement,
                prepare(
                    database,
                    "DELETE FROM runtime_publications WHERE staging_token=?1"
                )
            );
            UF_TRY(bindText(database, statement.get(), 1, stagingToken));
            return expectDone(database, statement.get());
        }

        // A row in runtime_publications for the whole of one installation. It
        // is taken before anything is written under the production root, so a
        // reclamation transaction that runs while the directory is being filled
        // sees the hash as referenced and leaves it alone.
        //
        // The destructor drops the row best-effort: a failure there costs a
        // directory that stays until the next open, because beginSessionEpoch
        // clears the table.
        class PublicationHold final
        {
            sqlite3*    m_database;
            std::string m_stagingToken;
            bool        m_held{true};

            PublicationHold(sqlite3* database, std::string stagingToken) noexcept
                : m_database{database}
                , m_stagingToken{std::move(stagingToken)}
            {
            }

        public:
            PublicationHold(PublicationHold&& other) noexcept
                : m_database{other.m_database}
                , m_stagingToken{std::move(other.m_stagingToken)}
                , m_held{std::exchange(other.m_held, false)}
            {
            }

            PublicationHold(PublicationHold const&) = delete;
            auto operator=(PublicationHold const&) -> PublicationHold& = delete;
            auto operator=(PublicationHold&&) -> PublicationHold& = delete;

            ~PublicationHold()
            {
                if (m_held)
                {
                    static_cast<void>(discardPublication(m_database, m_stagingToken));
                }
            }

            [[nodiscard]]
            static auto take(
                sqlite3* database,
                std::string_view stagingToken,
                std::string_view artifactRootHash
            ) -> Result<PublicationHold>
            {
                UF_TRY_VALUE(transaction, Transaction::begin(database));
                UF_TRY_VALUE(
                    artifactInsert,
                    prepare(
                        database,
                        "INSERT OR IGNORE INTO runtime_artifacts(artifact_root_hash) "
                        "VALUES(?1)"
                    )
                );
                UF_TRY(bindText(database, artifactInsert.get(), 1, artifactRootHash));
                UF_TRY(expectDone(database, artifactInsert.get()));
                UF_TRY_VALUE(
                    publicationInsert,
                    prepare(
                        database,
                        "INSERT INTO runtime_publications(staging_token, "
                        "artifact_root_hash) VALUES(?1, ?2)"
                    )
                );
                UF_TRY(bindText(database, publicationInsert.get(), 1, stagingToken));
                UF_TRY(bindText(database, publicationInsert.get(), 2, artifactRootHash));
                UF_TRY(expectDone(database, publicationInsert.get()));
                UF_TRY(transaction.commit());
                return PublicationHold{database, std::string{stagingToken}};
            }

            // The installing transaction deletes the row itself, so that the
            // hold and the installation row that takes over from it commit
            // together rather than leaving a window between them.
            auto markReleased() noexcept -> void { m_held = false; }
        };

        [[nodiscard]]
        auto beginSessionEpoch(sqlite3* database) -> Result<uint64>
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY_VALUE(
                query,
                prepare(
                    database,
                    "SELECT current_session_epoch FROM runtime_state WHERE singleton=1"
                )
            );
            if (sqlite3_step(query.get()) != SQLITE_ROW)
            {
                return databaseFailure(database, "could not read session epoch");
            }
            auto const prior = static_cast<uint64>(sqlite3_column_int64(query.get(), 0));
            if (prior == static_cast<uint64>(std::numeric_limits<sqlite3_int64>::max()))
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "Operator session epoch exhausted"
                );
            }
            auto const next = prior + 1U;
            UF_TRY_VALUE(
                update,
                prepare(
                    database,
                    "UPDATE runtime_state SET current_session_epoch=?1 "
                    "WHERE singleton=1 AND current_session_epoch=?2"
                )
            );
            UF_TRY(bindInteger(database, update.get(), 1, next));
            UF_TRY(bindInteger(database, update.get(), 2, prior));
            UF_TRY(expectDone(database, update.get()));
            if (sqlite3_changes(database) != 1)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Operator session epoch lost its startup CAS"
                );
            }
            UF_TRY(execute(database, "DELETE FROM control_leases"));
            UF_TRY(execute(database, "UPDATE sessions SET active=0 WHERE active=1"));

            // A publication claim outlives its process only after a crash, and
            // the epoch bump above has just fenced that process out. Dropping
            // the claims here is what keeps a crash from pinning an artifact
            // directory against reclamation forever.
            UF_TRY(execute(database, "DELETE FROM runtime_publications"));
            UF_TRY(transaction.commit());
            return next;
        }
    }

    struct OperatorCoordinator::Impl final
    {
        Database              database;
        std::filesystem::path path;
        std::filesystem::path runtimeArtifactRoot;
        uint64                sessionEpoch{};
    };

    OperatorCoordinator::OperatorCoordinator(std::unique_ptr<Impl> implementation)
        : m_impl{std::move(implementation)}
    {
    }

    OperatorCoordinator::OperatorCoordinator(OperatorCoordinator&&) noexcept = default;
    auto OperatorCoordinator::operator=(OperatorCoordinator&&) noexcept
        -> OperatorCoordinator& = default;
    OperatorCoordinator::~OperatorCoordinator() = default;

    auto OperatorCoordinator::open(
        std::filesystem::path const& runtimeDirectory
    ) -> Result<OperatorCoordinator>
    {
        if (runtimeDirectory.empty())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Operator runtime directory must not be empty"
            );
        }

        auto error = std::error_code{};
        std::filesystem::create_directories(runtimeDirectory, error);
        if (error)
        {
            return fail(
                AutomationErrorKind::IoFailure,
                "Could not create Operator runtime directory",
                error
            );
        }

        auto const runtimeStatus = std::filesystem::symlink_status(runtimeDirectory, error);
        if (
            error
            || !std::filesystem::is_directory(runtimeStatus)
            || std::filesystem::is_symlink(runtimeStatus)
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Operator runtime root must be a plain directory",
                error
            );
        }

        auto const databasePath = runtimeDirectory / "operator-runtime.sqlite";
        auto const runtimeArtifactRoot = runtimeDirectory / "runtime-artifacts";
        std::filesystem::create_directories(runtimeArtifactRoot, error);
        if (error)
        {
            return fail(
                AutomationErrorKind::IoFailure,
                "Could not create production RuntimeArtifact root",
                error
            );
        }
        auto const artifactRootStatus = std::filesystem::symlink_status(
            runtimeArtifactRoot,
            error
        );
        if (
            error
            || !std::filesystem::is_directory(artifactRootStatus)
            || std::filesystem::is_symlink(artifactRootStatus)
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Production RuntimeArtifact root must be a plain directory",
                error
            );
        }

        // The staging directory is part of the production layout rather than
        // something an installation creates on its way past, because
        // reclamation sweeps it and both need one owner for the name.
        auto const stagingRoot = runtimeArtifactRoot
            / std::string{detail::k_stagingDirectoryName};
        std::filesystem::create_directories(stagingRoot, error);
        if (error)
        {
            return fail(
                AutomationErrorKind::IoFailure,
                "Could not create production RuntimeArtifact staging root",
                error
            );
        }
        auto const stagingRootStatus = std::filesystem::symlink_status(stagingRoot, error);
        if (
            error
            || !std::filesystem::is_directory(stagingRootStatus)
            || std::filesystem::is_symlink(stagingRootStatus)
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Production RuntimeArtifact staging root must be a plain directory",
                error
            );
        }
        auto const databaseStatus = std::filesystem::symlink_status(databasePath, error);
        if (!error && std::filesystem::exists(databaseStatus))
        {
            if (
                !std::filesystem::is_regular_file(databaseStatus)
                || std::filesystem::is_symlink(databaseStatus)
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Operator database path must be a plain file"
                );
            }
        }
        else if (error && error != std::errc::no_such_file_or_directory)
        {
            return fail(
                AutomationErrorKind::IoFailure,
                "Could not inspect Operator database path",
                error
            );
        }
        auto* rawDatabase = static_cast<sqlite3*>(nullptr);
        auto const openCode = sqlite3_open_v2(
            pathToUtf8(databasePath).c_str(),
            &rawDatabase,
            SQLITE_OPEN_CREATE
                | SQLITE_OPEN_READWRITE
                | SQLITE_OPEN_FULLMUTEX
                | SQLITE_OPEN_EXRESCODE
                | SQLITE_OPEN_NOFOLLOW,
            nullptr
        );
        auto database = Database{rawDatabase};
        if (openCode != SQLITE_OK || database == nullptr)
        {
            auto const detail = database == nullptr
                ? std::string{"SQLite returned no database handle"}
                : std::string{sqlite3_errmsg(database.get())};
            return fail(
                AutomationErrorKind::IoFailure,
                std::format("Could not open Operator database: {}", detail)
            );
        }

        UF_TRY(initialize(database.get()));
        UF_TRY_VALUE(sessionEpoch, beginSessionEpoch(database.get()));
        auto coordinator = OperatorCoordinator{std::make_unique<Impl>(
            Impl{
                .database            = std::move(database),
                .path                = databasePath,
                .runtimeArtifactRoot = runtimeArtifactRoot,
                .sessionEpoch        = sessionEpoch,
            }
        )};
        UF_TRY(coordinator.recoverUncertainDispatches());
        return coordinator;
    }

    auto OperatorCoordinator::databasePath() const -> std::filesystem::path
    {
        return m_impl->path;
    }

    auto OperatorCoordinator::installRuntimeArtifact(
        RuntimeArtifactInstallRequest const& request
    ) -> Result<task::InstalledRuntimeArtifact>
    {
        // The artifact directory is published before this transaction and is
        // deliberately NOT removed when the compare-and-swap below fails. It is
        // content-addressed and re-verified on every open, and a concurrent
        // publisher may have put the identical bytes there first -- which is
        // one of the ways the CAS fails. Deleting it on our own failure would
        // break their installation to tidy ours.
        //
        // What the failure leaves behind instead is a runtime_artifacts row no
        // installation names, which reclaimUnreferencedRuntimeArtifacts is free
        // to remove once nothing else does either.
        UF_TRY_VALUE(stagingToken, randomToken(m_impl->database.get()));
        UF_TRY_VALUE(
            release,
            detail::readRuntimeRelease(
                m_impl->runtimeArtifactRoot,
                request.handoffRoot,
                request.expectedReleaseManifestHash
            )
        );

        // The hold goes in before the first byte is written under the
        // production root, so there is no instant in which the directory exists
        // and the database says nothing references it.
        UF_TRY_VALUE(
            hold,
            PublicationHold::take(
                m_impl->database.get(),
                stagingToken,
                release.artifactRootHash.hex()
            )
        );
        UF_TRY_VALUE(
            artifact,
            detail::publishRuntimeArtifact(
                m_impl->runtimeArtifactRoot,
                release,
                stagingToken
            )
        );
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            stateQuery,
            prepare(
                m_impl->database.get(),
                "SELECT installed_generation, active_runtime_artifact_root_hash "
                "FROM runtime_state WHERE singleton=1"
            )
        );
        if (sqlite3_step(stateQuery.get()) != SQLITE_ROW)
        {
            return databaseFailure(
                m_impl->database.get(),
                "could not read installed RuntimeArtifact generation"
            );
        }
        auto const currentGeneration = static_cast<uint64>(
            sqlite3_column_int64(stateQuery.get(), 0)
        );
        auto const currentRoot = columnText(stateQuery.get(), 1);
        if (currentGeneration != request.expectedInstalledGeneration)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "RuntimeArtifact installed-generation compare-and-swap failed"
            );
        }
        if (currentRoot == release.artifactRootHash.hex())
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "RuntimeArtifact is already the active installed generation"
            );
        }
        UF_TRY_VALUE(
            nextGeneration,
            checkedSqlIncrement(currentGeneration, "installed RuntimeArtifact generation")
        );

        UF_TRY_VALUE(
            installationInsert,
            prepare(
                m_impl->database.get(),
                "INSERT INTO runtime_installations(installed_generation, "
                "artifact_root_hash, release_manifest_hash) VALUES(?1, ?2, ?3)"
            )
        );
        UF_TRY(bindInteger(
            m_impl->database.get(),
            installationInsert.get(),
            1,
            nextGeneration
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            installationInsert.get(),
            2,
            release.artifactRootHash.hex()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            installationInsert.get(),
            3,
            release.releaseManifestHash.hex()
        ));
        UF_TRY(expectDone(m_impl->database.get(), installationInsert.get()));

        UF_TRY_VALUE(
            update,
            prepare(
                m_impl->database.get(),
                "UPDATE runtime_state SET installed_generation=?1, "
                "active_runtime_artifact_root_hash=?2 WHERE singleton=1 "
                "AND installed_generation=?3"
            )
        );
        UF_TRY(bindInteger(m_impl->database.get(), update.get(), 1, nextGeneration));
        UF_TRY(bindText(
            m_impl->database.get(),
            update.get(),
            2,
            release.artifactRootHash.hex()
        ));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            update.get(),
            3,
            request.expectedInstalledGeneration
        ));
        UF_TRY(expectDone(m_impl->database.get(), update.get()));
        if (sqlite3_changes(m_impl->database.get()) != 1)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "RuntimeArtifact installed-generation compare-and-swap lost its transaction"
            );
        }

        // The installation row now references the hash, so the hold has nothing
        // left to protect; both facts commit at once.
        UF_TRY(discardPublication(m_impl->database.get(), stagingToken));
        UF_TRY(transaction.commit());
        hold.markReleased();
        return task::InstalledRuntimeArtifact{
            std::move(artifact),
            nextGeneration,
        };
    }

    auto OperatorCoordinator::reclaimUnreferencedRuntimeArtifacts()
        -> Result<ReclaimedRuntimeArtifacts>
    {
        // The removals happen INSIDE the write transaction, and that is the
        // whole defence against a concurrent publisher: PublicationHold::take
        // is itself a BEGIN IMMEDIATE, so a claim on one of these hashes either
        // lands before this transaction opens -- and the query below sees it --
        // or after this one commits, by which point the directory is gone and
        // the publisher materializes it again.
        //
        // Rolling back after a directory has been removed is safe in the one
        // direction it can happen: a runtime_artifacts row whose directory is
        // missing is still unreferenced, so the next pass finishes the job.
        UF_TRY_VALUE(
            artifactDirectory,
            task_platform::ConfinedRoot::open(m_impl->runtimeArtifactRoot)
        );
        UF_TRY_VALUE(
            stagingDirectory,
            task_platform::ConfinedRoot::open(
                m_impl->runtimeArtifactRoot / std::string{detail::k_stagingDirectoryName}
            )
        );
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));

        // Every row is read before the first write, because a statement that is
        // still stepping does not see its own transaction's later changes.
        UF_TRY_VALUE(
            orphanQuery,
            prepare(
                m_impl->database.get(),
                "SELECT artifact_root_hash FROM runtime_artifacts WHERE "
                "artifact_root_hash NOT IN "
                "(SELECT artifact_root_hash FROM runtime_installations) AND "
                "artifact_root_hash NOT IN "
                "(SELECT artifact_root_hash FROM runtime_publications) AND "
                "artifact_root_hash NOT IN "
                "(SELECT active_runtime_artifact_root_hash FROM runtime_state "
                "WHERE singleton=1 AND active_runtime_artifact_root_hash IS NOT NULL) "
                "ORDER BY artifact_root_hash"
            )
        );
        auto orphans    = std::vector<std::string>{};
        auto orphanStep = sqlite3_step(orphanQuery.get());
        while (orphanStep == SQLITE_ROW)
        {
            orphans.emplace_back(columnText(orphanQuery.get(), 0));
            orphanStep = sqlite3_step(orphanQuery.get());
        }
        if (orphanStep != SQLITE_DONE)
        {
            return databaseFailure(
                m_impl->database.get(),
                "could not scan unreferenced RuntimeArtifacts"
            );
        }

        UF_TRY_VALUE(
            claimedQuery,
            prepare(
                m_impl->database.get(),
                "SELECT staging_token FROM runtime_publications"
            )
        );
        auto claimed     = std::vector<std::string>{};
        auto claimedStep = sqlite3_step(claimedQuery.get());
        while (claimedStep == SQLITE_ROW)
        {
            claimed.emplace_back(columnText(claimedQuery.get(), 0));
            claimedStep = sqlite3_step(claimedQuery.get());
        }
        if (claimedStep != SQLITE_DONE)
        {
            return databaseFailure(
                m_impl->database.get(),
                "could not scan in-flight RuntimeArtifact publications"
            );
        }

        for (auto const& hash : orphans)
        {
            UF_TRY(artifactDirectory.removeTree(hash));
            UF_TRY_VALUE(
                deletion,
                prepare(
                    m_impl->database.get(),
                    "DELETE FROM runtime_artifacts WHERE artifact_root_hash=?1"
                )
            );
            UF_TRY(bindText(m_impl->database.get(), deletion.get(), 1, hash));
            UF_TRY(expectDone(m_impl->database.get(), deletion.get()));
        }

        UF_TRY_VALUE(stagingNames, stagingDirectory.childNames());
        auto reclaimedStagings = uint64{};
        for (auto const& name : stagingNames)
        {
            if (std::ranges::contains(claimed, name))
            {
                continue;
            }
            UF_TRY(stagingDirectory.removeTree(name));
            ++reclaimedStagings;
        }

        UF_TRY(transaction.commit());
        return ReclaimedRuntimeArtifacts{
            .artifactDirectories = static_cast<uint64>(orphans.size()),
            .stagingDirectories  = reclaimedStagings,
        };
    }

    auto OperatorCoordinator::openInstalledRuntimeArtifact(
        uint64 installedGeneration,
        ContentHash const& artifactRootHash
    ) -> Result<task::InstalledRuntimeArtifact>
    {
        if (installedGeneration == 0U)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Installed RuntimeArtifact generation must be positive"
            );
        }
        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                "SELECT 1 FROM runtime_installations WHERE installed_generation=?1 "
                "AND artifact_root_hash=?2"
            )
        );
        UF_TRY(bindInteger(
            m_impl->database.get(),
            query.get(),
            1,
            installedGeneration
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            query.get(),
            2,
            artifactRootHash.hex()
        ));
        if (sqlite3_step(query.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "RuntimeArtifact root is not pinned to the requested installed generation"
            );
        }
        UF_TRY_VALUE(
            artifact,
            detail::openProductionRuntimeArtifact(
                m_impl->runtimeArtifactRoot,
                artifactRootHash
            )
        );
        return task::InstalledRuntimeArtifact{
            std::move(artifact),
            installedGeneration,
        };
    }

    auto OperatorCoordinator::recoverUncertainDispatches() -> Result<uint64>
    {
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                "SELECT o.operation_id, o.revision, d.dispatch_sequence "
                "FROM operations o JOIN dispatches d ON d.operation_id=o.operation_id "
                "WHERE o.state='running' AND d.delivery_outcome IS NULL AND "
                "d.dispatch_sequence=(SELECT MAX(latest.dispatch_sequence) FROM dispatches latest "
                "WHERE latest.operation_id=o.operation_id)"
            )
        );
        auto pending = std::vector<std::tuple<std::string, uint64, uint64>>{};
        auto queryStep = sqlite3_step(query.get());
        while (queryStep == SQLITE_ROW)
        {
            pending.emplace_back(
                columnText(query.get(), 0),
                static_cast<uint64>(sqlite3_column_int64(query.get(), 1)),
                static_cast<uint64>(sqlite3_column_int64(query.get(), 2))
            );
            queryStep = sqlite3_step(query.get());
        }
        if (queryStep != SQLITE_DONE)
        {
            return databaseFailure(m_impl->database.get(), "could not scan pending dispatches");
        }

        for (auto const& [operationId, revision, dispatchSequence] : pending)
        {
            if (revision == static_cast<uint64>(std::numeric_limits<sqlite3_int64>::max()))
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "Operation revision exhausted during dispatch recovery"
                );
            }
            UF_TRY_VALUE(
                dispatchUpdate,
                prepare(
                    m_impl->database.get(),
                    "UPDATE dispatches SET delivery_outcome='transport_unknown' "
                    "WHERE operation_id=?1 AND dispatch_sequence=?2 AND delivery_outcome IS NULL"
                )
            );
            UF_TRY(bindText(m_impl->database.get(), dispatchUpdate.get(), 1, operationId));
            UF_TRY(bindInteger(
                m_impl->database.get(),
                dispatchUpdate.get(),
                2,
                dispatchSequence
            ));
            UF_TRY(expectDone(m_impl->database.get(), dispatchUpdate.get()));
            if (sqlite3_changes(m_impl->database.get()) != 1)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Pending dispatch recovery lost its CAS"
                );
            }

            UF_TRY_VALUE(
                operationUpdate,
                prepare(
                    m_impl->database.get(),
                    "UPDATE operations SET state='reconciling', revision=?1 "
                    "WHERE operation_id=?2 AND state='running' AND revision=?3"
                )
            );
            UF_TRY(bindInteger(m_impl->database.get(), operationUpdate.get(), 1, revision + 1U));
            UF_TRY(bindText(m_impl->database.get(), operationUpdate.get(), 2, operationId));
            UF_TRY(bindInteger(m_impl->database.get(), operationUpdate.get(), 3, revision));
            UF_TRY(expectDone(m_impl->database.get(), operationUpdate.get()));
            if (sqlite3_changes(m_impl->database.get()) != 1)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Pending Operation recovery lost its CAS"
                );
            }
        }

        UF_TRY(transaction.commit());
        return static_cast<uint64>(pending.size());
    }

    auto OperatorCoordinator::registerProject(
        VerifiedProjectRegistration const& registration
    ) -> Status
    {
        auto const registrationHash = registration.hash();
        auto const pluginId = registration.pluginId();
        auto const& canonicalManifest = registration.canonicalJcs();
        UF_TRY(requireName(pluginId, "plugin_id"));
        UF_TRY(requireName(canonicalManifest, "canonical project registration"));
        UF_TRY_VALUE(
            computedHash,
            sha256(std::as_bytes(std::span{canonicalManifest}))
        );
        if (computedHash != registrationHash)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Project registration hash does not match canonical bytes"
            );
        }
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));

        UF_TRY_VALUE(
            insert,
            prepare(
                m_impl->database.get(),
                "INSERT OR IGNORE INTO project_registrations"
                "(registration_hash, plugin_id, plugin_hash, project_state_schema_hash, "
                "canonical_manifest) VALUES(?1, ?2, ?3, ?4, ?5)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 1, registrationHash.hex()));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 2, pluginId));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 3, registration.pluginHash().hex()));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            4,
            registration.projectStateSchemaHash().hex()
        ));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 5, canonicalManifest));
        UF_TRY(expectDone(m_impl->database.get(), insert.get()));

        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                "SELECT plugin_id, plugin_hash, project_state_schema_hash, canonical_manifest "
                "FROM project_registrations "
                "WHERE registration_hash=?1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), query.get(), 1, registrationHash.hex()));
        if (sqlite3_step(query.get()) != SQLITE_ROW)
        {
            return databaseFailure(m_impl->database.get(), "could not verify registration");
        }
        if (
            columnText(query.get(), 0) != pluginId
            || columnText(query.get(), 1) != registration.pluginHash().hex()
            || columnText(query.get(), 2) != registration.projectStateSchemaHash().hex()
            || columnText(query.get(), 3) != canonicalManifest
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Project registration hash already names different immutable bytes"
            );
        }
        return transaction.commit();
    }

    auto OperatorCoordinator::provisionProjectInstance(
        VerifiedProjectRegistration const& registration,
        ProjectPluginHandle const& plugin,
        ProjectInstanceBaseline const& baseline
    ) -> Status
    {
        UF_TRY(requireName(baseline.projectInstanceKey, "project_instance_key"));
        UF_TRY(requireName(baseline.eventId, "baseline event_id"));
        if (
            baseline.entry.projectRegistrationHash() != registration.hash()
            || baseline.entry.projectRegistrationHash()
                != plugin.projectRegistrationHash()
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Project baseline Journal data does not match the exact registration"
            );
        }
        if (
            baseline.entry.namespacedEventType()
            != registration.baselineEventType()
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Project baseline event type does not match ProjectRegistration"
            );
        }
        if (
            plugin.pluginId() != registration.pluginId()
            || plugin.pluginHash() != registration.pluginHash()
            || plugin.projectRegistrationHash() != registration.hash()
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "ProjectInstance provision requires the exact pinned ProjectPlugin"
            );
        }
        // A baseline reduces exactly one Journal prefix: its own creation event
        // against no prior state. Nothing here reads the database, so it is
        // correct outside the transaction below.
        auto const baselineEvents = std::array{
            JournalAppend{.eventId = baseline.eventId, .entry = baseline.entry},
        };
        UF_TRY_VALUE(
            reducerInput,
            plugin.canonicalize(reduceEnvelopeJcs(baselineEvents, "null"))
        );
        UF_TRY_VALUE(reducedState, plugin.reduce(reducerInput));
        if (
            reducedState.projectRegistrationHash() != registration.hash()
            || reducedState.function() != ProjectPluginFunction::Reduce
            || reducedState.direction() != ProjectDocumentDirection::Output
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Reduced baseline state does not match the registered ProjectState schema"
            );
        }

        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            registrationQuery,
            prepare(
                m_impl->database.get(),
                "SELECT plugin_id, plugin_hash, project_state_schema_hash, canonical_manifest "
                "FROM project_registrations WHERE registration_hash=?1"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            registrationQuery.get(),
            1,
            registration.hash().hex()
        ));
        if (
            sqlite3_step(registrationQuery.get()) != SQLITE_ROW
            || columnText(registrationQuery.get(), 0) != registration.pluginId()
            || columnText(registrationQuery.get(), 1) != registration.pluginHash().hex()
            || columnText(registrationQuery.get(), 2)
                != registration.projectStateSchemaHash().hex()
            || columnText(registrationQuery.get(), 3) != registration.canonicalJcs()
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "ProjectInstance requires the exact registered ProjectRegistration"
            );
        }

        UF_TRY_VALUE(
            existing,
            prepare(
                m_impl->database.get(),
                "SELECT 1 FROM project_instances WHERE plugin_id=?1 "
                "AND project_instance_key=?2"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            existing.get(),
            1,
            registration.pluginId()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            existing.get(),
            2,
            baseline.projectInstanceKey
        ));
        if (sqlite3_step(existing.get()) == SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "project_instance_key is immutable and already provisioned"
            );
        }

        UF_TRY_VALUE(
            instanceInsert,
            prepare(
                m_impl->database.get(),
                "INSERT INTO project_instances(project_instance_key, plugin_id, "
                "project_registration_hash, baseline_event_id) VALUES(?1, ?2, ?3, ?4)"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            instanceInsert.get(),
            1,
            baseline.projectInstanceKey
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            instanceInsert.get(),
            2,
            registration.pluginId()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            instanceInsert.get(),
            3,
            registration.hash().hex()
        ));
        UF_TRY(bindText(m_impl->database.get(), instanceInsert.get(), 4, baseline.eventId));
        UF_TRY(expectDone(m_impl->database.get(), instanceInsert.get()));

        UF_TRY_VALUE(
            eventInsert,
            prepare(
                m_impl->database.get(),
                "INSERT INTO journal_events(event_id, plugin_id, project_instance_key, "
                "sequence, prior_project_state_revision, session_manifest_hash, operation_id, "
                "namespaced_event_type, payload_schema_hash, canonical_event, "
                "canonical_provenance) "
                "VALUES(?1, ?2, ?3, 0, NULL, ?4, NULL, ?5, ?6, ?7, ?8)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), eventInsert.get(), 1, baseline.eventId));
        UF_TRY(bindText(
            m_impl->database.get(),
            eventInsert.get(),
            2,
            registration.pluginId()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            eventInsert.get(),
            3,
            baseline.projectInstanceKey
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            eventInsert.get(),
            4,
            baseline.sessionManifestHash.hex()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            eventInsert.get(),
            5,
            baseline.entry.namespacedEventType()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            eventInsert.get(),
            6,
            baseline.entry.payloadSchemaHash().hex()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            eventInsert.get(),
            7,
            baseline.entry.payload().bytes()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            eventInsert.get(),
            8,
            baseline.entry.provenance().bytes()
        ));
        UF_TRY(expectDone(m_impl->database.get(), eventInsert.get()));

        UF_TRY_VALUE(
            stateInsert,
            prepare(
                m_impl->database.get(),
                "INSERT INTO project_state(plugin_id, project_instance_key, revision, "
                "project_registration_hash, state_schema_hash, last_journal_sequence, "
                "canonical_state, state_hash) VALUES(?1, ?2, 0, ?3, ?4, 0, ?5, ?6)"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            stateInsert.get(),
            1,
            registration.pluginId()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            stateInsert.get(),
            2,
            baseline.projectInstanceKey
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            stateInsert.get(),
            3,
            registration.hash().hex()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            stateInsert.get(),
            4,
            registration.projectStateSchemaHash().hex()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            stateInsert.get(),
            5,
            reducedState.bytes()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            stateInsert.get(),
            6,
            reducedState.contentHash().hex()
        ));
        UF_TRY(expectDone(m_impl->database.get(), stateInsert.get()));

        return transaction.commit();
    }

    auto OperatorCoordinator::pinSession(
        SessionPin const& pin,
        SessionManifest const& manifest
    ) -> Status
    {
        UF_TRY(requireName(pin.sessionId, "session_id"));
        UF_TRY(requireName(pin.authenticatedControllerId, "authenticated_controller_id"));
        UF_TRY(requireName(pin.idempotencyNamespace, "idempotency_namespace"));
        UF_TRY(requireName(pin.controlledTargetKey, "controlled_target_key"));
        UF_TRY(requireName(pin.projectInstanceKey, "project_instance_key"));
        if (pin.projectRegistrationHash != manifest.projectRegistrationHash())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "SessionManifest does not bind the selected ProjectRegistration"
            );
        }
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));

        auto const runtimeArtifactRootHash = manifest.runtimeModelArtifactRootHash();
        UF_TRY_VALUE(
            runtimeArtifactQuery,
            prepare(
                m_impl->database.get(),
                "SELECT installed_generation FROM runtime_state WHERE singleton=1 "
                "AND active_runtime_artifact_root_hash=?1"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            runtimeArtifactQuery.get(),
            1,
            runtimeArtifactRootHash.hex()
        ));
        if (sqlite3_step(runtimeArtifactQuery.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "SessionManifest RuntimeArtifact is not production-installed"
            );
        }
        auto const installedGeneration = static_cast<uint64>(
            sqlite3_column_int64(runtimeArtifactQuery.get(), 0)
        );

        UF_TRY_VALUE(
            instanceQuery,
            prepare(
                m_impl->database.get(),
                "SELECT 1 FROM project_instances WHERE project_registration_hash=?1 "
                "AND project_instance_key=?2"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            instanceQuery.get(),
            1,
            pin.projectRegistrationHash.hex()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            instanceQuery.get(),
            2,
            pin.projectInstanceKey
        ));
        if (
            sqlite3_step(instanceQuery.get()) != SQLITE_ROW
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Session requires an existing ProjectInstance pinned to the exact registration"
            );
        }

        UF_TRY_VALUE(
            insert,
            prepare(
                m_impl->database.get(),
                "INSERT OR IGNORE INTO sessions"
                "(session_id, authenticated_controller_id, idempotency_namespace, "
                "manifest_hash, runtime_artifact_root_hash, installed_generation, "
                "project_registration_hash, capability_profile_hash, session_epoch, "
                "controlled_target_key, project_instance_key, mode, active) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, 1)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 1, pin.sessionId));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            2,
            pin.authenticatedControllerId
        ));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 3, pin.idempotencyNamespace));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 4, manifest.hash().hex()));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            5,
            runtimeArtifactRootHash.hex()
        ));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            insert.get(),
            6,
            installedGeneration
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            7,
            pin.projectRegistrationHash.hex()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            8,
            pin.capabilityProfileHash.hex()
        ));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 9, m_impl->sessionEpoch));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 10, pin.controlledTargetKey));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 11, pin.projectInstanceKey));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            12,
            sessionModeWireName(pin.mode)
        ));
        UF_TRY(expectDone(m_impl->database.get(), insert.get()));

        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                "SELECT authenticated_controller_id, idempotency_namespace, manifest_hash, "
                "runtime_artifact_root_hash, installed_generation, "
                "project_registration_hash, capability_profile_hash, session_epoch, "
                "controlled_target_key, project_instance_key, mode, active "
                "FROM sessions WHERE session_id=?1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), query.get(), 1, pin.sessionId));
        if (sqlite3_step(query.get()) != SQLITE_ROW)
        {
            return databaseFailure(m_impl->database.get(), "could not pin session");
        }
        auto const matches = columnText(query.get(), 0) == pin.authenticatedControllerId
            && columnText(query.get(), 1) == pin.idempotencyNamespace
            && columnText(query.get(), 2) == manifest.hash().hex()
            && columnText(query.get(), 3) == runtimeArtifactRootHash.hex()
            && static_cast<uint64>(sqlite3_column_int64(query.get(), 4))
                == installedGeneration
            && columnText(query.get(), 5) == pin.projectRegistrationHash.hex()
            && columnText(query.get(), 6) == pin.capabilityProfileHash.hex()
            && static_cast<uint64>(sqlite3_column_int64(query.get(), 7))
                == m_impl->sessionEpoch
            && columnText(query.get(), 8) == pin.controlledTargetKey
            && columnText(query.get(), 9) == pin.projectInstanceKey
            && columnText(query.get(), 10) == sessionModeWireName(pin.mode)
            && sqlite3_column_int(query.get(), 11) == 1;
        if (!matches)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "session_id already names a different immutable session tuple"
            );
        }
        return transaction.commit();
    }

    auto OperatorCoordinator::acquireLease(
        std::string const& sessionId
    ) -> Result<ControlLease>
    {
        UF_TRY(requireName(sessionId, "session_id"));
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));

        UF_TRY_VALUE(
            sessionQuery,
            prepare(
                m_impl->database.get(),
                "SELECT controlled_target_key, authenticated_controller_id, "
                "capability_profile_hash, session_epoch FROM sessions WHERE session_id=?1 "
                "AND mode='write' AND active=1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), sessionQuery.get(), 1, sessionId));
        if (sqlite3_step(sessionQuery.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Cannot acquire a lease for an unknown session"
            );
        }
        auto const target = columnText(sessionQuery.get(), 0);
        auto const controllerId = columnText(sessionQuery.get(), 1);
        auto const capabilityProfileHash = columnText(sessionQuery.get(), 2);
        UF_TRY_VALUE(capabilityHash, parseHashColumn(capabilityProfileHash));
        auto const sessionEpoch = static_cast<uint64>(
            sqlite3_column_int64(sessionQuery.get(), 3)
        );
        if (sessionEpoch != m_impl->sessionEpoch)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Cannot acquire a lease for a session from a prior process epoch"
            );
        }

        UF_TRY_VALUE(
            activeQuery,
            prepare(
                m_impl->database.get(),
                "SELECT 1 FROM control_leases WHERE controlled_target_key=?1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), activeQuery.get(), 1, target));
        if (sqlite3_step(activeQuery.get()) == SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "ControlledTarget already has an active write lease"
            );
        }

        auto previousFence = uint64{0};
        UF_TRY_VALUE(
            highWaterQuery,
            prepare(
                m_impl->database.get(),
                "SELECT fencing_token FROM fencing_high_water WHERE controlled_target_key=?1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), highWaterQuery.get(), 1, target));
        if (sqlite3_step(highWaterQuery.get()) == SQLITE_ROW)
        {
            previousFence = static_cast<uint64>(sqlite3_column_int64(highWaterQuery.get(), 0));
        }
        if (previousFence == static_cast<uint64>(std::numeric_limits<sqlite3_int64>::max()))
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "Control fencing token exhausted"
            );
        }
        auto const nextFence = previousFence + 1U;
        UF_TRY_VALUE(leaseId, randomToken(m_impl->database.get()));

        UF_TRY_VALUE(
            highWaterWrite,
            prepare(
                m_impl->database.get(),
                "INSERT INTO fencing_high_water(controlled_target_key, fencing_token) "
                "VALUES(?1, ?2) ON CONFLICT(controlled_target_key) DO UPDATE SET "
                "fencing_token=excluded.fencing_token"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), highWaterWrite.get(), 1, target));
        UF_TRY(bindInteger(m_impl->database.get(), highWaterWrite.get(), 2, nextFence));
        UF_TRY(expectDone(m_impl->database.get(), highWaterWrite.get()));

        UF_TRY_VALUE(
            leaseWrite,
            prepare(
                m_impl->database.get(),
                "INSERT INTO control_leases(controlled_target_key, lease_id, session_id, "
                "controller_id, session_epoch, fencing_token, revision, "
                "capability_profile_hash) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?6, ?7)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), leaseWrite.get(), 1, target));
        UF_TRY(bindText(m_impl->database.get(), leaseWrite.get(), 2, leaseId));
        UF_TRY(bindText(m_impl->database.get(), leaseWrite.get(), 3, sessionId));
        UF_TRY(bindText(m_impl->database.get(), leaseWrite.get(), 4, controllerId));
        UF_TRY(bindInteger(m_impl->database.get(), leaseWrite.get(), 5, sessionEpoch));
        UF_TRY(bindInteger(m_impl->database.get(), leaseWrite.get(), 6, nextFence));
        UF_TRY(bindText(
            m_impl->database.get(),
            leaseWrite.get(),
            7,
            capabilityProfileHash
        ));
        UF_TRY(expectDone(m_impl->database.get(), leaseWrite.get()));

        UF_TRY_VALUE(
            transitionWrite,
            prepare(
                m_impl->database.get(),
                "INSERT INTO control_transitions(controlled_target_key, session_id, controller_id, "
                "lease_id, session_epoch, fencing_token, transition, reason) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, 'acquire', 'ordinary acquire')"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 1, target));
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 2, sessionId));
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 3, controllerId));
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 4, leaseId));
        UF_TRY(bindInteger(m_impl->database.get(), transitionWrite.get(), 5, sessionEpoch));
        UF_TRY(bindInteger(m_impl->database.get(), transitionWrite.get(), 6, nextFence));
        UF_TRY(expectDone(m_impl->database.get(), transitionWrite.get()));

        UF_TRY(transaction.commit());
        return ControlLease{
            .leaseId               = std::move(leaseId),
            .sessionId             = sessionId,
            .controlledTargetKey   = target,
            .controllerId          = controllerId,
            .sessionEpoch          = sessionEpoch,
            .fencingToken          = nextFence,
            .revision              = nextFence,
            .capabilityProfileHash = capabilityHash,
        };
    }

    auto OperatorCoordinator::takeoverLease(
        std::string const& sessionId,
        std::string const& reason
    ) -> Result<ControlLease>
    {
        UF_TRY(requireName(sessionId, "session_id"));
        UF_TRY(requireName(reason, "takeover reason"));
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));

        UF_TRY_VALUE(
            sessionQuery,
            prepare(
                m_impl->database.get(),
                "SELECT controlled_target_key, authenticated_controller_id, "
                "capability_profile_hash, session_epoch FROM sessions WHERE session_id=?1 "
                "AND mode='write' AND active=1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), sessionQuery.get(), 1, sessionId));
        if (sqlite3_step(sessionQuery.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Cannot take over control for an unknown session"
            );
        }
        auto const target = columnText(sessionQuery.get(), 0);
        auto const controllerId = columnText(sessionQuery.get(), 1);
        auto const capabilityProfileHash = columnText(sessionQuery.get(), 2);
        UF_TRY_VALUE(capabilityHash, parseHashColumn(capabilityProfileHash));
        auto const sessionEpoch = static_cast<uint64>(
            sqlite3_column_int64(sessionQuery.get(), 3)
        );
        if (sessionEpoch != m_impl->sessionEpoch)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Cannot take over with a session from a prior process epoch"
            );
        }

        auto previousFence = uint64{0};
        UF_TRY_VALUE(
            highWaterQuery,
            prepare(
                m_impl->database.get(),
                "SELECT fencing_token FROM fencing_high_water WHERE controlled_target_key=?1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), highWaterQuery.get(), 1, target));
        if (sqlite3_step(highWaterQuery.get()) == SQLITE_ROW)
        {
            previousFence = static_cast<uint64>(
                sqlite3_column_int64(highWaterQuery.get(), 0)
            );
        }
        if (previousFence == static_cast<uint64>(std::numeric_limits<sqlite3_int64>::max()))
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "Control fencing token exhausted"
            );
        }
        auto const nextFence = previousFence + 1U;
        UF_TRY_VALUE(leaseId, randomToken(m_impl->database.get()));

        UF_TRY_VALUE(
            highWaterWrite,
            prepare(
                m_impl->database.get(),
                "INSERT INTO fencing_high_water(controlled_target_key, fencing_token) "
                "VALUES(?1, ?2) ON CONFLICT(controlled_target_key) DO UPDATE SET "
                "fencing_token=excluded.fencing_token"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), highWaterWrite.get(), 1, target));
        UF_TRY(bindInteger(m_impl->database.get(), highWaterWrite.get(), 2, nextFence));
        UF_TRY(expectDone(m_impl->database.get(), highWaterWrite.get()));

        UF_TRY_VALUE(
            leaseWrite,
            prepare(
                m_impl->database.get(),
                "INSERT INTO control_leases(controlled_target_key, lease_id, session_id, "
                "controller_id, session_epoch, fencing_token, revision, "
                "capability_profile_hash) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?6, ?7) "
                "ON CONFLICT(controlled_target_key) DO UPDATE SET lease_id=excluded.lease_id, "
                "session_id=excluded.session_id, controller_id=excluded.controller_id, "
                "session_epoch=excluded.session_epoch, fencing_token=excluded.fencing_token, "
                "revision=excluded.revision, "
                "capability_profile_hash=excluded.capability_profile_hash"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), leaseWrite.get(), 1, target));
        UF_TRY(bindText(m_impl->database.get(), leaseWrite.get(), 2, leaseId));
        UF_TRY(bindText(m_impl->database.get(), leaseWrite.get(), 3, sessionId));
        UF_TRY(bindText(m_impl->database.get(), leaseWrite.get(), 4, controllerId));
        UF_TRY(bindInteger(m_impl->database.get(), leaseWrite.get(), 5, sessionEpoch));
        UF_TRY(bindInteger(m_impl->database.get(), leaseWrite.get(), 6, nextFence));
        UF_TRY(bindText(
            m_impl->database.get(),
            leaseWrite.get(),
            7,
            capabilityProfileHash
        ));
        UF_TRY(expectDone(m_impl->database.get(), leaseWrite.get()));

        UF_TRY_VALUE(
            transitionWrite,
            prepare(
                m_impl->database.get(),
                "INSERT INTO control_transitions(controlled_target_key, session_id, controller_id, "
                "lease_id, session_epoch, fencing_token, transition, reason) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, 'takeover', ?7)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 1, target));
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 2, sessionId));
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 3, controllerId));
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 4, leaseId));
        UF_TRY(bindInteger(m_impl->database.get(), transitionWrite.get(), 5, sessionEpoch));
        UF_TRY(bindInteger(m_impl->database.get(), transitionWrite.get(), 6, nextFence));
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 7, reason));
        UF_TRY(expectDone(m_impl->database.get(), transitionWrite.get()));

        UF_TRY(transaction.commit());
        return ControlLease{
            .leaseId               = std::move(leaseId),
            .sessionId             = sessionId,
            .controlledTargetKey   = target,
            .controllerId          = controllerId,
            .sessionEpoch          = sessionEpoch,
            .fencingToken          = nextFence,
            .revision              = nextFence,
            .capabilityProfileHash = capabilityHash,
        };
    }

    auto OperatorCoordinator::releaseLease(
        ControlLease const& lease
    ) -> Result<uint64>
    {
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            leaseQuery,
            prepare(
                m_impl->database.get(),
                "SELECT 1 FROM control_leases WHERE controlled_target_key=?1 "
                "AND lease_id=?2 AND session_id=?3 AND controller_id=?4 "
                "AND session_epoch=?5 AND fencing_token=?6 AND revision=?7 "
                "AND capability_profile_hash=?8"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), leaseQuery.get(), 1, lease.controlledTargetKey));
        UF_TRY(bindText(m_impl->database.get(), leaseQuery.get(), 2, lease.leaseId));
        UF_TRY(bindText(m_impl->database.get(), leaseQuery.get(), 3, lease.sessionId));
        UF_TRY(bindText(m_impl->database.get(), leaseQuery.get(), 4, lease.controllerId));
        UF_TRY(bindInteger(m_impl->database.get(), leaseQuery.get(), 5, lease.sessionEpoch));
        UF_TRY(bindInteger(m_impl->database.get(), leaseQuery.get(), 6, lease.fencingToken));
        UF_TRY(bindInteger(m_impl->database.get(), leaseQuery.get(), 7, lease.revision));
        UF_TRY(bindText(
            m_impl->database.get(),
            leaseQuery.get(),
            8,
            lease.capabilityProfileHash.hex()
        ));
        if (sqlite3_step(leaseQuery.get()) != SQLITE_ROW)
        {
            return fail(AutomationErrorKind::ActionRejected, "Control lease is stale");
        }
        if (
            lease.sessionEpoch != m_impl->sessionEpoch
            || lease.fencingToken
                == static_cast<uint64>(std::numeric_limits<sqlite3_int64>::max())
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Control lease cannot be released in this session epoch"
            );
        }
        auto const nextFence = lease.fencingToken + 1U;

        UF_TRY_VALUE(
            highWaterUpdate,
            prepare(
                m_impl->database.get(),
                "UPDATE fencing_high_water SET fencing_token=?1 "
                "WHERE controlled_target_key=?2 AND fencing_token=?3"
            )
        );
        UF_TRY(bindInteger(m_impl->database.get(), highWaterUpdate.get(), 1, nextFence));
        UF_TRY(bindText(
            m_impl->database.get(),
            highWaterUpdate.get(),
            2,
            lease.controlledTargetKey
        ));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            highWaterUpdate.get(),
            3,
            lease.fencingToken
        ));
        UF_TRY(expectDone(m_impl->database.get(), highWaterUpdate.get()));
        if (sqlite3_changes(m_impl->database.get()) != 1)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Control fencing high-water lost its release CAS"
            );
        }

        UF_TRY_VALUE(
            leaseDelete,
            prepare(
                m_impl->database.get(),
                "DELETE FROM control_leases WHERE controlled_target_key=?1 AND lease_id=?2"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            leaseDelete.get(),
            1,
            lease.controlledTargetKey
        ));
        UF_TRY(bindText(m_impl->database.get(), leaseDelete.get(), 2, lease.leaseId));
        UF_TRY(expectDone(m_impl->database.get(), leaseDelete.get()));
        if (sqlite3_changes(m_impl->database.get()) != 1)
        {
            return fail(AutomationErrorKind::ActionRejected, "Control lease lost its release CAS");
        }

        UF_TRY_VALUE(
            transitionWrite,
            prepare(
                m_impl->database.get(),
                "INSERT INTO control_transitions(controlled_target_key, session_id, controller_id, "
                "lease_id, session_epoch, fencing_token, transition, reason) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, 'release', 'explicit release')"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            transitionWrite.get(),
            1,
            lease.controlledTargetKey
        ));
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 2, lease.sessionId));
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 3, lease.controllerId));
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 4, lease.leaseId));
        UF_TRY(bindInteger(m_impl->database.get(), transitionWrite.get(), 5, lease.sessionEpoch));
        UF_TRY(bindInteger(m_impl->database.get(), transitionWrite.get(), 6, nextFence));
        UF_TRY(expectDone(m_impl->database.get(), transitionWrite.get()));

        UF_TRY(transaction.commit());
        return nextFence;
    }

    auto OperatorCoordinator::createSnapshot(
        ControlLease const& lease,
        ContentHash const& identityHash
    ) -> Result<SnapshotRecord>
    {
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            leaseQuery,
            prepare(
                m_impl->database.get(),
                "SELECT lease_id, session_id, controller_id, session_epoch, fencing_token, "
                "revision, capability_profile_hash FROM control_leases "
                "WHERE controlled_target_key=?1"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            leaseQuery.get(),
            1,
            lease.controlledTargetKey
        ));
        if (sqlite3_step(leaseQuery.get()) != SQLITE_ROW)
        {
            return fail(AutomationErrorKind::ActionRejected, "Control lease is not active");
        }
        auto const matches = columnText(leaseQuery.get(), 0) == lease.leaseId
            && columnText(leaseQuery.get(), 1) == lease.sessionId
            && columnText(leaseQuery.get(), 2) == lease.controllerId
            && static_cast<uint64>(sqlite3_column_int64(leaseQuery.get(), 3))
                == lease.sessionEpoch
            && static_cast<uint64>(sqlite3_column_int64(leaseQuery.get(), 4))
                == lease.fencingToken
            && static_cast<uint64>(sqlite3_column_int64(leaseQuery.get(), 5))
                == lease.revision
            && columnText(leaseQuery.get(), 6) == lease.capabilityProfileHash.hex()
            && lease.sessionEpoch == m_impl->sessionEpoch;
        if (!matches)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Control lease was superseded before snapshot creation"
            );
        }

        UF_TRY_VALUE(token, randomToken(m_impl->database.get()));
        UF_TRY_VALUE(
            insert,
            prepare(
                m_impl->database.get(),
                "INSERT INTO snapshots(token, session_id, session_epoch, identity_hash, "
                "lease_revision) VALUES(?1, ?2, ?3, ?4, ?5)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 1, token));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 2, lease.sessionId));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 3, lease.sessionEpoch));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 4, identityHash.hex()));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 5, lease.revision));
        UF_TRY(expectDone(m_impl->database.get(), insert.get()));
        UF_TRY(transaction.commit());
        return SnapshotRecord{
            .token         = std::move(token),
            .sessionId     = lease.sessionId,
            .identityHash  = identityHash,
            .sessionEpoch  = lease.sessionEpoch,
            .leaseRevision = lease.revision,
        };
    }

    auto OperatorCoordinator::createOrLoadOperation(
        CommandRequest const& request,
        ValidatedToolInvocation const& invocation
    ) -> Result<StoredOperation>
    {
        UF_TRY(requireName(request.sessionId, "session_id"));
        UF_TRY(requireName(request.snapshotToken, "snapshot_token"));
        UF_TRY(requireName(request.idempotencyNamespace, "idempotency_namespace"));
        UF_TRY(requireName(request.clientRequestId, "client_request_id"));

        auto const& toolName = invocation.toolName();
        auto const& toolVersion = invocation.toolVersion();
        auto const& canonicalArgs = invocation.canonicalArgs().bytes();
        auto const mutating = invocation.mutability() == ToolMutability::Mutating;

        // The fingerprint covers exactly what the catalog decided the command
        // is. Mutability is absent on purpose: it is a function of the tool and
        // its version, so including it would let two fingerprints disagree
        // about one tool without any of the fingerprinted bytes differing.
        auto fingerprintMaterial = toolName;
        fingerprintMaterial.push_back('\0');
        fingerprintMaterial += toolVersion;
        fingerprintMaterial.push_back('\0');
        fingerprintMaterial += canonicalArgs;
        UF_TRY_VALUE(
            commandFingerprint,
            sha256(std::as_bytes(std::span{fingerprintMaterial}))
        );
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));

        UF_TRY_VALUE(
            sessionQuery,
            prepare(
                m_impl->database.get(),
                "SELECT session.controlled_target_key, session.project_instance_key, "
                "registration.plugin_id, session.idempotency_namespace, session.session_epoch, "
                "session.project_registration_hash "
                "FROM sessions session JOIN project_registrations "
                "registration ON registration.registration_hash="
                "session.project_registration_hash "
                "WHERE session.session_id=?1 AND session.active=1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), sessionQuery.get(), 1, request.sessionId));
        if (sqlite3_step(sessionQuery.get()) != SQLITE_ROW)
        {
            return fail(AutomationErrorKind::ActionRejected, "Unknown authenticated session");
        }
        auto const controlledTargetKey = columnText(sessionQuery.get(), 0);
        auto const projectInstanceKey = columnText(sessionQuery.get(), 1);
        auto const pluginId = columnText(sessionQuery.get(), 2);
        auto const idempotencyNamespace = columnText(sessionQuery.get(), 3);
        auto const sessionEpoch = static_cast<uint64>(sqlite3_column_int64(sessionQuery.get(), 4));
        if (
            request.idempotencyNamespace != idempotencyNamespace
            || sessionEpoch != m_impl->sessionEpoch
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Command authority does not match the authenticated current-epoch session"
            );
        }
        // Comparing the registration root also pins the Tool Catalog: the root
        // hashes the canonical registration JCS, and tool_catalog_hash is one of
        // its members, so an invocation minted against another catalog cannot
        // present this root.
        if (invocation.projectRegistrationHash().hex() != columnText(sessionQuery.get(), 5))
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool invocation was minted for a different ProjectRegistration"
            );
        }

        UF_TRY_VALUE(
            existingQuery,
            prepare(
                m_impl->database.get(),
                "SELECT operation_id, command_fingerprint, tool_name, tool_version, "
                "canonical_args, mutating, state, revision, "
                "frozen_plan_hash, EXISTS(SELECT 1 FROM dispatches d WHERE "
                "d.operation_id=operations.operation_id), session_id FROM operations "
                "WHERE idempotency_namespace=?1 AND plugin_id=?2 "
                "AND project_instance_key=?3 AND client_request_id=?4"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            existingQuery.get(),
            1,
            request.idempotencyNamespace
        ));
        UF_TRY(bindText(m_impl->database.get(), existingQuery.get(), 2, pluginId));
        UF_TRY(bindText(m_impl->database.get(), existingQuery.get(), 3, projectInstanceKey));
        UF_TRY(bindText(m_impl->database.get(), existingQuery.get(), 4, request.clientRequestId));
        if (sqlite3_step(existingQuery.get()) == SQLITE_ROW)
        {
            if (
                columnText(existingQuery.get(), 1) != commandFingerprint.hex()
                || columnText(existingQuery.get(), 2) != toolName
                || columnText(existingQuery.get(), 3) != toolVersion
                || columnText(existingQuery.get(), 4) != canonicalArgs
                || (sqlite3_column_int(existingQuery.get(), 5) != 0) != mutating
            )
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "client_request_id was already used for different canonical command bytes"
                );
            }

            // The idempotency key does not carry a session, and nothing makes
            // an idempotency_namespace unique to one. Without this, the hit
            // path hands another session's operation id and revision back,
            // which is all transitionOperation needs to terminate it.
            if (columnText(existingQuery.get(), 10) != request.sessionId)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "client_request_id belongs to another session"
                );
            }
            // Every column is read into a local before the commit. Reading a
            // row after committing its transaction happens to work in this
            // SQLite, which is exactly why it should not be relied on.
            UF_TRY_VALUE(state, parseOperationState(columnText(existingQuery.get(), 6)));
            auto operationId  = columnText(existingQuery.get(), 0);
            auto const revision = static_cast<uint64>(
                sqlite3_column_int64(existingQuery.get(), 7)
            );
            auto const frozen     = sqlite3_column_type(existingQuery.get(), 8) != SQLITE_NULL;
            auto const dispatched = sqlite3_column_int(existingQuery.get(), 9) != 0;
            UF_TRY(transaction.commit());
            return StoredOperation{
                .operationId   = std::move(operationId),
                .lookup        = CommandLookup::Existing,
                .state         = state,
                .revision      = revision,
                .planFrozen    = frozen,
                .hasDispatched = dispatched,
            };
        }

        UF_TRY_VALUE(
            snapshotQuery,
            prepare(
                m_impl->database.get(),
                "SELECT session.controlled_target_key FROM snapshots s JOIN sessions session "
                "ON session.session_id=s.session_id JOIN control_leases lease "
                "ON lease.controlled_target_key=session.controlled_target_key "
                "WHERE s.token=?1 AND s.session_id=?2 AND s.session_epoch=?3 AND "
                "s.lease_revision=lease.revision AND lease.session_id=s.session_id"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), snapshotQuery.get(), 1, request.snapshotToken));
        UF_TRY(bindText(m_impl->database.get(), snapshotQuery.get(), 2, request.sessionId));
        UF_TRY(bindInteger(m_impl->database.get(), snapshotQuery.get(), 3, sessionEpoch));
        if (
            sqlite3_step(snapshotQuery.get()) != SQLITE_ROW
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "SnapshotToken is unknown, belongs to another session, or is stale"
            );
        }
        if (columnText(snapshotQuery.get(), 0) != controlledTargetKey)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "Session target changed while validating SnapshotToken"
            );
        }

        if (mutating)
        {
            UF_TRY_VALUE(
                mutationQuery,
                prepare(
                    m_impl->database.get(),
                    "SELECT operation_id FROM operations WHERE controlled_target_key=?1 "
                    "AND mutating=1 AND state IN ('proposed', 'awaiting_approval', 'ready', "
                    "'needs_revalidation', 'running', 'reconciling', 'ambiguous') LIMIT 1"
                )
            );
            UF_TRY(bindText(
                m_impl->database.get(),
                mutationQuery.get(),
                1,
                controlledTargetKey
            ));
            if (sqlite3_step(mutationQuery.get()) == SQLITE_ROW)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "ControlledTarget already has a non-terminal mutating Operation"
                );
            }

            UF_TRY_VALUE(
                instanceMutationQuery,
                prepare(
                    m_impl->database.get(),
                    "SELECT operation_id FROM operations WHERE plugin_id=?1 "
                    "AND project_instance_key=?2 AND mutating=1 AND state IN "
                    "('proposed', 'awaiting_approval', 'ready', 'needs_revalidation', "
                    "'running', 'reconciling', 'ambiguous') LIMIT 1"
                )
            );
            UF_TRY(bindText(
                m_impl->database.get(),
                instanceMutationQuery.get(),
                1,
                pluginId
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                instanceMutationQuery.get(),
                2,
                projectInstanceKey
            ));
            if (sqlite3_step(instanceMutationQuery.get()) == SQLITE_ROW)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "ProjectInstance already has a non-terminal mutating Operation"
                );
            }
        }

        UF_TRY_VALUE(operationId, randomToken(m_impl->database.get()));
        UF_TRY_VALUE(
            insert,
            prepare(
                m_impl->database.get(),
                "INSERT INTO operations(operation_id, session_id, snapshot_token, idempotency_namespace, "
                "client_request_id, command_fingerprint, tool_name, tool_version, canonical_args, "
                "controlled_target_key, mutating, state, revision, plugin_id, "
                "project_instance_key) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, "
                "?10, ?11, 'proposed', 1, ?12, ?13)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 1, operationId));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 2, request.sessionId));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 3, request.snapshotToken));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 4, request.idempotencyNamespace));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 5, request.clientRequestId));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 6, commandFingerprint.hex()));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 7, toolName));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 8, toolVersion));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 9, canonicalArgs));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 10, controlledTargetKey));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 11, mutating ? 1U : 0U));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 12, pluginId));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 13, projectInstanceKey));
        UF_TRY(expectDone(m_impl->database.get(), insert.get()));
        UF_TRY(transaction.commit());
        return StoredOperation{
            .operationId   = std::move(operationId),
            .lookup        = CommandLookup::Created,
            .state         = OperationState::Proposed,
            .revision      = 1U,
            .planFrozen    = false,
            .hasDispatched = false,
        };
    }

    auto OperatorCoordinator::transitionOperation(
        std::string const& operationId,
        uint64 expectedRevision,
        OperationEvent event
    ) -> Result<StoredOperation>
    {
        if (
            event == OperationEvent::DispatchStarted
            || event == OperationEvent::ApprovalObtained
            || event == OperationEvent::HostOutcomeObserved
            || event == OperationEvent::ReconciliationContinued
            || event == OperationEvent::ReconciliationConfirmed
            || event == OperationEvent::ReconciliationRejected
            || event == OperationEvent::ReconciliationAmbiguous
            || event == OperationEvent::CorrectionCommitted
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Privileged Operation event requires its atomic ledger method"
            );
        }
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                "SELECT o.state, o.revision, o.frozen_plan_hash, o.mutating, "
                "EXISTS(SELECT 1 FROM dispatches d "
                "WHERE d.operation_id=o.operation_id) FROM operations o "
                + std::string{k_liveControllerJoin}
                + "WHERE o.operation_id=?1 AND session.active=1 "
                "AND session.session_epoch=?2"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), query.get(), 1, operationId));
        UF_TRY(bindInteger(m_impl->database.get(), query.get(), 2, m_impl->sessionEpoch));
        if (sqlite3_step(query.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Unknown operation_id, or its session no longer controls the target"
            );
        }
        auto const revision = static_cast<uint64>(sqlite3_column_int64(query.get(), 1));
        if (revision != expectedRevision)
        {
            return fail(AutomationErrorKind::ActionRejected, "Operation revision is stale");
        }
        UF_TRY_VALUE(state, parseOperationState(columnText(query.get(), 0)));
        auto const frozen = sqlite3_column_type(query.get(), 2) != SQLITE_NULL;
        auto const mutating = sqlite3_column_int(query.get(), 3) != 0;
        auto const dispatched = sqlite3_column_int(query.get(), 4) != 0;
        if (
            (event == OperationEvent::ReadCompleted && mutating)
            || (
                (event == OperationEvent::ApprovalRequired
                    || event == OperationEvent::ReadyWithoutApproval
                    || event == OperationEvent::NextStepApprovalRequired
                    || event == OperationEvent::NextStepReady)
                && !mutating
            )
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Operation event contradicts the command mutability"
            );
        }
        UF_TRY_VALUE(machine, OperationMachine::restore(state, frozen, dispatched));
        UF_TRY_VALUE(nextState, machine.transition(event));
        UF_TRY_VALUE(nextRevision, checkedSqlIncrement(revision, "Operation revision"));

        UF_TRY_VALUE(
            update,
            prepare(
                m_impl->database.get(),
                "UPDATE operations SET state=?1, revision=?2 WHERE operation_id=?3 AND revision=?4"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            update.get(),
            1,
            operationStateWireName(nextState)
        ));
        UF_TRY(bindInteger(m_impl->database.get(), update.get(), 2, nextRevision));
        UF_TRY(bindText(m_impl->database.get(), update.get(), 3, operationId));
        UF_TRY(bindInteger(m_impl->database.get(), update.get(), 4, revision));
        UF_TRY(expectDone(m_impl->database.get(), update.get()));
        if (sqlite3_changes(m_impl->database.get()) != 1)
        {
            return fail(AutomationErrorKind::ActionRejected, "Operation revision lost its CAS");
        }
        UF_TRY(transaction.commit());
        return StoredOperation{
            .operationId   = operationId,
            .lookup        = CommandLookup::Existing,
            .state         = nextState,
            .revision      = nextRevision,
            .planFrozen    = machine.planFrozen(),
            .hasDispatched = machine.hasDispatched(),
        };
    }

    auto OperatorCoordinator::reserveDispatch(
        std::string const& operationId,
        uint64 expectedRevision,
        ControlLease const& lease,
        ContentHash const& decisionBasisHash,
        ContentHash const& frozenPlanHash,
        ContentHash const& stepIntentHash,
        std::string const& authorityDecisionId,
        std::optional<ApprovalGrant> const& approval
    ) -> Result<DispatchReservation>
    {
        UF_TRY_VALUE(currentUnixMillis, unixTimeMilliseconds());
        UF_TRY(requireName(authorityDecisionId, "authority_decision_id"));
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                "SELECT state, revision, frozen_plan_hash, "
                "COALESCE((SELECT MAX(dispatch_sequence) FROM dispatches d WHERE "
                "d.operation_id=operations.operation_id), 0), "
                "(SELECT delivery_outcome FROM dispatches d WHERE "
                "d.operation_id=operations.operation_id ORDER BY dispatch_sequence DESC LIMIT 1) "
                ", session_id, controlled_target_key, mutating "
                "FROM operations "
                "WHERE operation_id=?1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), query.get(), 1, operationId));
        if (sqlite3_step(query.get()) != SQLITE_ROW)
        {
            return fail(AutomationErrorKind::InvalidResource, "Unknown operation_id");
        }
        auto const revision = static_cast<uint64>(sqlite3_column_int64(query.get(), 1));
        if (revision != expectedRevision)
        {
            return fail(AutomationErrorKind::ActionRejected, "Operation revision is stale");
        }
        UF_TRY_VALUE(state, parseOperationState(columnText(query.get(), 0)));
        if (
            columnText(query.get(), 5) != lease.sessionId
            || columnText(query.get(), 6) != lease.controlledTargetKey
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Dispatch lease does not own the Operation target"
            );
        }
        if (sqlite3_column_int(query.get(), 7) != 1)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Read-only Operations cannot enter Host dispatch"
            );
        }
        UF_TRY_VALUE(
            leaseQuery,
            prepare(
                m_impl->database.get(),
                "SELECT 1 FROM control_leases WHERE controlled_target_key=?1 "
                "AND lease_id=?2 AND session_id=?3 AND controller_id=?4 "
                "AND session_epoch=?5 AND fencing_token=?6 AND revision=?7 "
                "AND capability_profile_hash=?8"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), leaseQuery.get(), 1, lease.controlledTargetKey));
        UF_TRY(bindText(m_impl->database.get(), leaseQuery.get(), 2, lease.leaseId));
        UF_TRY(bindText(m_impl->database.get(), leaseQuery.get(), 3, lease.sessionId));
        UF_TRY(bindText(m_impl->database.get(), leaseQuery.get(), 4, lease.controllerId));
        UF_TRY(bindInteger(m_impl->database.get(), leaseQuery.get(), 5, lease.sessionEpoch));
        UF_TRY(bindInteger(m_impl->database.get(), leaseQuery.get(), 6, lease.fencingToken));
        UF_TRY(bindInteger(m_impl->database.get(), leaseQuery.get(), 7, lease.revision));
        UF_TRY(bindText(
            m_impl->database.get(),
            leaseQuery.get(),
            8,
            lease.capabilityProfileHash.hex()
        ));
        if (sqlite3_step(leaseQuery.get()) != SQLITE_ROW)
        {
            return fail(AutomationErrorKind::ActionRejected, "Dispatch lease is stale");
        }

        auto const priorSequence = static_cast<uint64>(sqlite3_column_int64(query.get(), 3));
        auto const firstDispatch = priorSequence == 0U;
        if (firstDispatch)
        {
            UF_TRY_VALUE(machine, OperationMachine::restore(state, false, false));
            if (state == OperationState::AwaitingApproval)
            {
                if (!approval.has_value())
                {
                    return fail(
                        AutomationErrorKind::ActionRejected,
                        "Awaiting Operation requires a matching ApprovalToken"
                    );
                }
                UF_TRY(machine.transition(OperationEvent::ApprovalObtained));
            }
            else if (state != OperationState::Ready || approval.has_value())
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "First dispatch authority does not match Operation approval state"
                );
            }
            UF_TRY(machine.transition(OperationEvent::DispatchStarted));
        }
        else
        {
            if (state == OperationState::AwaitingApproval)
            {
                if (!approval.has_value())
                {
                    return fail(
                        AutomationErrorKind::ActionRejected,
                        "Awaiting workflow step requires a matching ApprovalToken"
                    );
                }
                UF_TRY_VALUE(machine, OperationMachine::restore(state, true, true));
                UF_TRY(machine.transition(OperationEvent::ApprovalObtained));
            }
            else if (state != OperationState::Running || approval.has_value())
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "A subsequent dispatch requires a running frozen Operation"
                );
            }
            if (sqlite3_column_type(query.get(), 4) == SQLITE_NULL)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "A new dispatch cannot overtake the prior Host outcome"
                );
            }
            if (columnText(query.get(), 2) != frozenPlanHash.hex())
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "A frozen Operation cannot change plan hash"
                );
            }
        }

        UF_TRY_VALUE(sequence, checkedSqlIncrement(priorSequence, "dispatch sequence"));
        if (approval.has_value())
        {
            UF_TRY_VALUE(
                approvalUpdate,
                prepare(
                    m_impl->database.get(),
                    "UPDATE approvals SET consumed=1, consumed_by_dispatch=?1 "
                    "WHERE token=?2 AND operation_id=?3 AND session_id=?4 "
                    "AND controller_id=?5 AND controlled_target_key=?6 AND lease_id=?7 "
                    "AND session_epoch=?8 AND fencing_token=?9 AND frozen_plan_hash=?10 "
                    "AND step_intent_hash=?11 AND decision_basis_hash=?12 "
                    "AND authority_decision_id=?13 AND expires_at_unix_millis>=?14 "
                    "AND consumed=0"
                )
            );
            UF_TRY(bindInteger(m_impl->database.get(), approvalUpdate.get(), 1, sequence));
            UF_TRY(bindText(m_impl->database.get(), approvalUpdate.get(), 2, approval->token));
            UF_TRY(bindText(m_impl->database.get(), approvalUpdate.get(), 3, operationId));
            UF_TRY(bindText(m_impl->database.get(), approvalUpdate.get(), 4, lease.sessionId));
            UF_TRY(bindText(m_impl->database.get(), approvalUpdate.get(), 5, lease.controllerId));
            UF_TRY(bindText(
                m_impl->database.get(),
                approvalUpdate.get(),
                6,
                lease.controlledTargetKey
            ));
            UF_TRY(bindText(m_impl->database.get(), approvalUpdate.get(), 7, lease.leaseId));
            UF_TRY(bindInteger(m_impl->database.get(), approvalUpdate.get(), 8, lease.sessionEpoch));
            UF_TRY(bindInteger(m_impl->database.get(), approvalUpdate.get(), 9, lease.fencingToken));
            UF_TRY(bindText(m_impl->database.get(), approvalUpdate.get(), 10, frozenPlanHash.hex()));
            UF_TRY(bindText(m_impl->database.get(), approvalUpdate.get(), 11, stepIntentHash.hex()));
            UF_TRY(bindText(
                m_impl->database.get(),
                approvalUpdate.get(),
                12,
                decisionBasisHash.hex()
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                approvalUpdate.get(),
                13,
                approval->authorityDecisionId
            ));
            UF_TRY(bindInteger(m_impl->database.get(), approvalUpdate.get(), 14, currentUnixMillis));
            UF_TRY(expectDone(m_impl->database.get(), approvalUpdate.get()));
            if (sqlite3_changes(m_impl->database.get()) != 1)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "ApprovalToken is stale, expired, mismatched, or already consumed"
                );
            }
        }

        UF_TRY_VALUE(
            authorityInsert,
            prepare(
                m_impl->database.get(),
                "INSERT INTO authority_decisions(authority_decision_id, operation_id, "
                "dispatch_sequence, session_id, controller_id, lease_id, session_epoch, fencing_token, "
                "decision_basis_hash, frozen_plan_hash, step_intent_hash, approval_token) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), authorityInsert.get(), 1, authorityDecisionId));
        UF_TRY(bindText(m_impl->database.get(), authorityInsert.get(), 2, operationId));
        UF_TRY(bindInteger(m_impl->database.get(), authorityInsert.get(), 3, sequence));
        UF_TRY(bindText(m_impl->database.get(), authorityInsert.get(), 4, lease.sessionId));
        UF_TRY(bindText(m_impl->database.get(), authorityInsert.get(), 5, lease.controllerId));
        UF_TRY(bindText(m_impl->database.get(), authorityInsert.get(), 6, lease.leaseId));
        UF_TRY(bindInteger(m_impl->database.get(), authorityInsert.get(), 7, lease.sessionEpoch));
        UF_TRY(bindInteger(m_impl->database.get(), authorityInsert.get(), 8, lease.fencingToken));
        UF_TRY(bindText(m_impl->database.get(), authorityInsert.get(), 9, decisionBasisHash.hex()));
        UF_TRY(bindText(m_impl->database.get(), authorityInsert.get(), 10, frozenPlanHash.hex()));
        UF_TRY(bindText(m_impl->database.get(), authorityInsert.get(), 11, stepIntentHash.hex()));
        if (approval.has_value())
        {
            UF_TRY(bindText(m_impl->database.get(), authorityInsert.get(), 12, approval->token));
        }
        else if (sqlite3_bind_null(authorityInsert.get(), 12) != SQLITE_OK)
        {
            return databaseFailure(m_impl->database.get(), "could not bind absent approval");
        }
        UF_TRY(expectDone(m_impl->database.get(), authorityInsert.get()));

        UF_TRY_VALUE(
            insert,
            prepare(
                m_impl->database.get(),
                "INSERT INTO dispatches(operation_id, dispatch_sequence, decision_basis_hash, "
                "frozen_plan_hash, authority_decision_id, delivery_outcome) "
                "VALUES(?1, ?2, ?3, ?4, ?5, NULL)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 1, operationId));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 2, sequence));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 3, decisionBasisHash.hex()));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 4, frozenPlanHash.hex()));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 5, authorityDecisionId));
        UF_TRY(expectDone(m_impl->database.get(), insert.get()));

        UF_TRY_VALUE(nextRevision, checkedSqlIncrement(revision, "Operation revision"));
        UF_TRY_VALUE(
            update,
            prepare(
                m_impl->database.get(),
                "UPDATE operations SET state='running', frozen_plan_hash=?1, revision=?2 "
                "WHERE operation_id=?3 AND revision=?4"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), update.get(), 1, frozenPlanHash.hex()));
        UF_TRY(bindInteger(m_impl->database.get(), update.get(), 2, nextRevision));
        UF_TRY(bindText(m_impl->database.get(), update.get(), 3, operationId));
        UF_TRY(bindInteger(m_impl->database.get(), update.get(), 4, revision));
        UF_TRY(expectDone(m_impl->database.get(), update.get()));
        if (sqlite3_changes(m_impl->database.get()) != 1)
        {
            return fail(AutomationErrorKind::ActionRejected, "Operation revision lost its CAS");
        }
        UF_TRY(transaction.commit());
        return DispatchReservation{
            .dispatchSequence  = sequence,
            .operationRevision = nextRevision,
        };
    }

    auto OperatorCoordinator::recordDeliveryOutcome(
        std::string const& operationId,
        uint64 dispatchSequence,
        uint64 expectedRevision,
        DeliveryOutcome outcome
    ) -> Result<StoredOperation>
    {
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                "SELECT o.state, o.revision, o.frozen_plan_hash, d.delivery_outcome "
                "FROM operations o JOIN dispatches d ON d.operation_id=o.operation_id "
                "WHERE o.operation_id=?1 AND d.dispatch_sequence=?2"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), query.get(), 1, operationId));
        UF_TRY(bindInteger(m_impl->database.get(), query.get(), 2, dispatchSequence));
        if (sqlite3_step(query.get()) != SQLITE_ROW)
        {
            return fail(AutomationErrorKind::InvalidResource, "Unknown dispatch row");
        }
        auto const revision = static_cast<uint64>(sqlite3_column_int64(query.get(), 1));
        if (revision != expectedRevision)
        {
            return fail(AutomationErrorKind::ActionRejected, "Operation revision is stale");
        }
        if (sqlite3_column_type(query.get(), 3) != SQLITE_NULL)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "DeliveryOutcome is immutable once recorded"
            );
        }
        UF_TRY_VALUE(state, parseOperationState(columnText(query.get(), 0)));
        UF_TRY_VALUE(machine, OperationMachine::restore(state, true, true));
        UF_TRY_VALUE(nextState, machine.transition(OperationEvent::HostOutcomeObserved));

        UF_TRY_VALUE(
            dispatchUpdate,
            prepare(
                m_impl->database.get(),
                "UPDATE dispatches SET delivery_outcome=?1 WHERE operation_id=?2 "
                "AND dispatch_sequence=?3 AND delivery_outcome IS NULL"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            dispatchUpdate.get(),
            1,
            deliveryOutcomeWireName(outcome)
        ));
        UF_TRY(bindText(m_impl->database.get(), dispatchUpdate.get(), 2, operationId));
        UF_TRY(bindInteger(m_impl->database.get(), dispatchUpdate.get(), 3, dispatchSequence));
        UF_TRY(expectDone(m_impl->database.get(), dispatchUpdate.get()));
        if (sqlite3_changes(m_impl->database.get()) != 1)
        {
            return fail(AutomationErrorKind::ActionRejected, "DeliveryOutcome lost its CAS");
        }

        UF_TRY_VALUE(nextRevision, checkedSqlIncrement(revision, "Operation revision"));
        UF_TRY_VALUE(
            operationUpdate,
            prepare(
                m_impl->database.get(),
                "UPDATE operations SET state=?1, revision=?2 WHERE operation_id=?3 AND revision=?4"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            operationUpdate.get(),
            1,
            operationStateWireName(nextState)
        ));
        UF_TRY(bindInteger(m_impl->database.get(), operationUpdate.get(), 2, nextRevision));
        UF_TRY(bindText(m_impl->database.get(), operationUpdate.get(), 3, operationId));
        UF_TRY(bindInteger(m_impl->database.get(), operationUpdate.get(), 4, revision));
        UF_TRY(expectDone(m_impl->database.get(), operationUpdate.get()));
        if (sqlite3_changes(m_impl->database.get()) != 1)
        {
            return fail(AutomationErrorKind::ActionRejected, "Operation revision lost its CAS");
        }

        UF_TRY(transaction.commit());
        return StoredOperation{
            .operationId   = operationId,
            .lookup        = CommandLookup::Existing,
            .state         = nextState,
            .revision      = nextRevision,
            .planFrozen    = true,
            .hasDispatched = true,
        };
    }

    auto OperatorCoordinator::issueApproval(
        ApprovalRequest const& request,
        std::string const& authorityDecisionId
    ) -> Result<ApprovalGrant>
    {
        UF_TRY(requireName(authorityDecisionId, "authority_decision_id"));
        UF_TRY(requireName(request.approverPrincipal, "approver_principal"));
        UF_TRY_VALUE(currentUnixMillis, unixTimeMilliseconds());
        if (request.expiresAtUnixMillis <= currentUnixMillis)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Approval expiry must be in the future"
            );
        }
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            operationQuery,
            prepare(
                m_impl->database.get(),
                "SELECT state, session_id, controlled_target_key, command_fingerprint, "
                "frozen_plan_hash FROM operations WHERE operation_id=?1"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            operationQuery.get(),
            1,
            request.operationId
        ));
        if (sqlite3_step(operationQuery.get()) != SQLITE_ROW)
        {
            return fail(AutomationErrorKind::InvalidResource, "Unknown operation_id");
        }
        UF_TRY_VALUE(state, parseOperationState(columnText(operationQuery.get(), 0)));
        if (state != OperationState::AwaitingApproval)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Approval can only be issued for an awaiting Operation"
            );
        }
        if (
            columnText(operationQuery.get(), 1) != request.lease.sessionId
            || columnText(operationQuery.get(), 2) != request.lease.controlledTargetKey
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Approval lease does not own the Operation target"
            );
        }
        if (
            sqlite3_column_type(operationQuery.get(), 4) != SQLITE_NULL
            && columnText(operationQuery.get(), 4) != request.frozenPlanHash.hex()
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Approval cannot replace an Operation frozen plan"
            );
        }

        UF_TRY_VALUE(
            leaseQuery,
            prepare(
                m_impl->database.get(),
                "SELECT 1 FROM control_leases WHERE controlled_target_key=?1 "
                "AND lease_id=?2 AND session_id=?3 AND controller_id=?4 "
                "AND session_epoch=?5 AND fencing_token=?6 AND revision=?7 "
                "AND capability_profile_hash=?8"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            leaseQuery.get(),
            1,
            request.lease.controlledTargetKey
        ));
        UF_TRY(bindText(m_impl->database.get(), leaseQuery.get(), 2, request.lease.leaseId));
        UF_TRY(bindText(m_impl->database.get(), leaseQuery.get(), 3, request.lease.sessionId));
        UF_TRY(bindText(m_impl->database.get(), leaseQuery.get(), 4, request.lease.controllerId));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            leaseQuery.get(),
            5,
            request.lease.sessionEpoch
        ));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            leaseQuery.get(),
            6,
            request.lease.fencingToken
        ));
        UF_TRY(bindInteger(m_impl->database.get(), leaseQuery.get(), 7, request.lease.revision));
        UF_TRY(bindText(
            m_impl->database.get(),
            leaseQuery.get(),
            8,
            request.lease.capabilityProfileHash.hex()
        ));
        if (sqlite3_step(leaseQuery.get()) != SQLITE_ROW)
        {
            return fail(AutomationErrorKind::ActionRejected, "Approval lease is stale");
        }

        UF_TRY_VALUE(token, randomToken(m_impl->database.get()));
        UF_TRY_VALUE(
            insert,
            prepare(
                m_impl->database.get(),
                "INSERT INTO approvals(token, operation_id, session_id, controller_id, "
                "controlled_target_key, lease_id, session_epoch, fencing_token, "
                "command_fingerprint, frozen_plan_hash, step_intent_hash, decision_basis_hash, "
                "effect_envelope_hash, policy_hash, approver_principal, "
                "approver_capability_hash, authority_decision_id, expires_at_unix_millis) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, "
                "?14, ?15, ?16, ?17, ?18)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 1, token));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 2, request.operationId));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 3, request.lease.sessionId));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 4, request.lease.controllerId));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            5,
            request.lease.controlledTargetKey
        ));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 6, request.lease.leaseId));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 7, request.lease.sessionEpoch));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 8, request.lease.fencingToken));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            9,
            columnText(operationQuery.get(), 3)
        ));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 10, request.frozenPlanHash.hex()));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 11, request.stepIntentHash.hex()));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 12, request.decisionBasisHash.hex()));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 13, request.effectEnvelopeHash.hex()));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 14, request.policyHash.hex()));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 15, request.approverPrincipal));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            16,
            request.approverCapabilityHash.hex()
        ));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 17, authorityDecisionId));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            insert.get(),
            18,
            request.expiresAtUnixMillis
        ));
        UF_TRY(expectDone(m_impl->database.get(), insert.get()));
        UF_TRY(transaction.commit());
        return ApprovalGrant{
            .token               = std::move(token),
            .authorityDecisionId = authorityDecisionId,
        };
    }

    auto OperatorCoordinator::commitReconciliation(
        ProjectPluginHandle const& plugin,
        ReconciliationCommit const& commit
    ) -> Result<StoredOperation>
    {
        UF_TRY(requireName(commit.operationId, "operation_id"));
        if (
            commit.outcome.disposition() == ReconcileDisposition::Diverged
            && commit.journalEvents.empty()
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Diverged requires a committed correction or divergence JournalEvent"
            );
        }
        if (
            (
                commit.outcome.disposition() == ReconcileDisposition::Rejected
                || commit.outcome.disposition() == ReconcileDisposition::Ambiguous
            )
            && !commit.journalEvents.empty()
        )
        {
            // Rejected means the step was refused and Ambiguous means its
            // outcome was never established. Either one appending an event
            // would write a world-changed record for a change nobody can say
            // happened.
            return fail(
                AutomationErrorKind::ActionRejected,
                "Rejected and Ambiguous reconciliations cannot append JournalEvents"
            );
        }
        for (auto const& event : commit.journalEvents)
        {
            UF_TRY(requireName(event.eventId, "journal event_id"));
            if (
                event.entry.projectRegistrationHash()
                != plugin.projectRegistrationHash()
            )
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Journal data does not match the reconciliation ProjectPlugin registration"
                );
            }
        }

        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            operationQuery,
            prepare(
                m_impl->database.get(),
                "SELECT o.state, o.revision, registration.plugin_id, "
                "session.project_instance_key, session.manifest_hash, "
                "registration.plugin_hash, session.project_registration_hash "
                "FROM operations o "
                + std::string{k_liveControllerJoin}
                + "JOIN project_registrations registration ON "
                "registration.registration_hash=session.project_registration_hash "
                "WHERE o.operation_id=?1 AND session.active=1 AND session.session_epoch=?2"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), operationQuery.get(), 1, commit.operationId));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            operationQuery.get(),
            2,
            m_impl->sessionEpoch
        ));
        if (sqlite3_step(operationQuery.get()) != SQLITE_ROW)
        {
            // The epoch and active flag are part of the lookup rather than a
            // later comparison: a restart revokes every lease and deactivates
            // every session, and the Journal is the heaviest write there is, so
            // a process fenced out of dispatch must not reach it either.
            return fail(
                AutomationErrorKind::ActionRejected,
                "Unknown operation_id, or its session is not active in this epoch"
            );
        }
        UF_TRY_VALUE(state, parseOperationState(columnText(operationQuery.get(), 0)));
        if (state != OperationState::Reconciling)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Reconciliation can commit only from the reconciling state"
            );
        }
        auto const operationRevision = static_cast<uint64>(
            sqlite3_column_int64(operationQuery.get(), 1)
        );
        if (operationRevision != commit.expectedOperationRevision)
        {
            return fail(AutomationErrorKind::ActionRejected, "Operation revision is stale");
        }
        auto const pluginId = columnText(operationQuery.get(), 2);
        auto const projectInstanceKey = columnText(operationQuery.get(), 3);
        auto const sessionManifestHash = columnText(operationQuery.get(), 4);
        if (
            plugin.pluginId() != pluginId
            || plugin.pluginHash().hex() != columnText(operationQuery.get(), 5)
            || plugin.projectRegistrationHash().hex() != columnText(operationQuery.get(), 6)
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Reconciliation ProjectPlugin does not match the Operation"
            );
        }
        // The outcome carries the registration its authority was bound to, so
        // asking it rather than only its document also pins which reconcile
        // schema read the disposition.
        if (
            commit.outcome.projectRegistrationHash() != plugin.projectRegistrationHash()
            || commit.outcome.operationId() != commit.operationId
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Reconciliation outcome was minted for a different ProjectRegistration "
                "or a different Operation"
            );
        }

        UF_TRY_VALUE(
            stateQuery,
            prepare(
                m_impl->database.get(),
                "SELECT revision, project_registration_hash, state_schema_hash, "
                "last_journal_sequence, canonical_state FROM project_state "
                "WHERE plugin_id=?1 AND project_instance_key=?2"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), stateQuery.get(), 1, pluginId));
        UF_TRY(bindText(m_impl->database.get(), stateQuery.get(), 2, projectInstanceKey));
        if (sqlite3_step(stateQuery.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "Reconciliation requires an existing provisioned ProjectState baseline"
            );
        }
        // v1.7 failure-and-recovery contract 15: a multi-step Operation with a
        // proven partial effect may not enter Rejected. Refusing an appending
        // Rejected is not enough, because an earlier Continue on the same
        // Operation may already have committed one; the Journal it wrote is
        // where the proof lives, so that is what is asked.
        if (commit.outcome.disposition() == ReconcileDisposition::Rejected)
        {
            // "Journal/outcome 证明部分 effect" -- the outcome half matters as
            // much as the Journal half, and I-13 wants every possible external
            // effect proven ABSENT. Only not_delivered is that proof: a NULL
            // outcome is a dispatch nobody has answered for, transport_unknown
            // is the recovery path's way of saying it does not know, and
            // delivered says it happened. Any of the three leaves Rejected
            // claiming more than the ledger can support.
            UF_TRY_VALUE(
                effectQuery,
                prepare(
                    m_impl->database.get(),
                    "SELECT 1 FROM journal_events WHERE operation_id=?1 "
                    "UNION ALL "
                    "SELECT 1 FROM dispatches WHERE operation_id=?1 AND ("
                    "delivery_outcome IS NULL OR delivery_outcome<>'not_delivered') "
                    "LIMIT 1"
                )
            );
            UF_TRY(bindText(
                m_impl->database.get(),
                effectQuery.get(),
                1,
                commit.operationId
            ));
            if (sqlite3_step(effectQuery.get()) == SQLITE_ROW)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Rejected requires every possible external effect proven not to "
                    "have happened; this Operation has a Journal event or a dispatch "
                    "that is not proven undelivered. Commit the proven facts and "
                    "Continue, or reach Diverged through a correction"
                );
            }
        }

        auto const storedStateRevision = static_cast<uint64>(
            sqlite3_column_int64(stateQuery.get(), 0)
        );
        auto const projectRegistrationHash = columnText(stateQuery.get(), 1);
        auto const projectStateSchemaHash = columnText(stateQuery.get(), 2);
        auto journalSequence = static_cast<uint64>(sqlite3_column_int64(stateQuery.get(), 3));
        auto const priorProjectState = columnText(stateQuery.get(), 4);
        if (storedStateRevision != commit.expectedProjectStateRevision)
        {
            return fail(AutomationErrorKind::ActionRejected, "ProjectState revision is stale");
        }
        if (projectRegistrationHash.empty() || priorProjectState.empty())
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "ProjectState row is missing its registration identity or canonical bytes"
            );
        }

        // The reducer runs here rather than before the transaction, and on
        // bytes built here rather than supplied: its input is the events this
        // commit appends together with the ProjectState the row above holds, so
        // reading that row and reducing it have to be the same BEGIN IMMEDIATE.
        // Anything else lets a concurrent writer move the state between the two.
        // The plugin VM is quota-bound, so holding the write lock across it is
        // bounded.
        auto reducedState = std::optional<ValidatedDocument>{};
        if (!commit.journalEvents.empty())
        {
            UF_TRY_VALUE(
                reducerInput,
                plugin.canonicalize(
                    reduceEnvelopeJcs(commit.journalEvents, priorProjectState)
                )
            );
            UF_TRY_VALUE(reduced, plugin.reduce(reducerInput));
            if (
                reduced.projectRegistrationHash().hex() != projectRegistrationHash
                || reduced.function() != ProjectPluginFunction::Reduce
                || reduced.direction() != ProjectDocumentDirection::Output
            )
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Reduced ProjectState does not match the pinned state schema"
                );
            }
            reducedState.emplace(std::move(reduced));
        }

        for (auto const& event : commit.journalEvents)
        {
            if (
                event.entry.projectRegistrationHash().hex()
                != projectRegistrationHash
            )
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Journal data does not match the Operation registration"
                );
            }
            if (journalSequence == static_cast<uint64>(std::numeric_limits<sqlite3_int64>::max()))
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "Project Journal sequence exhausted"
                );
            }
            ++journalSequence;
            UF_TRY_VALUE(
                eventInsert,
                prepare(
                    m_impl->database.get(),
                    "INSERT INTO journal_events(event_id, plugin_id, project_instance_key, "
                    "sequence, prior_project_state_revision, session_manifest_hash, operation_id, "
                    "namespaced_event_type, payload_schema_hash, canonical_event, "
                    "canonical_provenance) "
                    "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)"
                )
            );
            UF_TRY(bindText(m_impl->database.get(), eventInsert.get(), 1, event.eventId));
            UF_TRY(bindText(m_impl->database.get(), eventInsert.get(), 2, pluginId));
            UF_TRY(bindText(m_impl->database.get(), eventInsert.get(), 3, projectInstanceKey));
            UF_TRY(bindInteger(m_impl->database.get(), eventInsert.get(), 4, journalSequence));
            UF_TRY(bindInteger(
                m_impl->database.get(),
                eventInsert.get(),
                5,
                storedStateRevision
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                eventInsert.get(),
                6,
                sessionManifestHash
            ));
            UF_TRY(bindText(m_impl->database.get(), eventInsert.get(), 7, commit.operationId));
            UF_TRY(bindText(
                m_impl->database.get(),
                eventInsert.get(),
                8,
                event.entry.namespacedEventType()
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                eventInsert.get(),
                9,
                event.entry.payloadSchemaHash().hex()
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                eventInsert.get(),
                10,
                event.entry.payload().bytes()
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                eventInsert.get(),
                11,
                event.entry.provenance().bytes()
            ));
            UF_TRY(expectDone(m_impl->database.get(), eventInsert.get()));
        }

        if (reducedState.has_value())
        {
            if (storedStateRevision == static_cast<uint64>(std::numeric_limits<sqlite3_int64>::max()))
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "ProjectState revision exhausted"
                );
            }
            auto const nextStateRevision = storedStateRevision + 1U;
            UF_TRY_VALUE(
                stateUpdate,
                prepare(
                    m_impl->database.get(),
                    "UPDATE project_state SET revision=?1, canonical_state=?2, state_hash=?3, "
                    "last_journal_sequence=?4 WHERE plugin_id=?5 AND project_instance_key=?6 "
                    "AND project_registration_hash=?7 AND state_schema_hash=?8 AND revision=?9"
                )
            );
            UF_TRY(bindInteger(m_impl->database.get(), stateUpdate.get(), 1, nextStateRevision));
            UF_TRY(bindText(
                m_impl->database.get(),
                stateUpdate.get(),
                2,
                reducedState->bytes()
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                stateUpdate.get(),
                3,
                reducedState->contentHash().hex()
            ));
            UF_TRY(bindInteger(m_impl->database.get(), stateUpdate.get(), 4, journalSequence));
            UF_TRY(bindText(m_impl->database.get(), stateUpdate.get(), 5, pluginId));
            UF_TRY(bindText(
                m_impl->database.get(),
                stateUpdate.get(),
                6,
                projectInstanceKey
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                stateUpdate.get(),
                7,
                projectRegistrationHash
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                stateUpdate.get(),
                8,
                projectStateSchemaHash
            ));
            UF_TRY(bindInteger(
                m_impl->database.get(),
                stateUpdate.get(),
                9,
                storedStateRevision
            ));
            UF_TRY(expectDone(m_impl->database.get(), stateUpdate.get()));
            if (sqlite3_changes(m_impl->database.get()) != 1)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "ProjectState revision lost its CAS"
                );
            }
        }

        UF_TRY_VALUE(
            reconciliationInsert,
            prepare(
                m_impl->database.get(),
                "INSERT INTO reconciliations(operation_id, disposition, proposal_hash, "
                "canonical_proposal) VALUES(?1, ?2, ?3, ?4)"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            reconciliationInsert.get(),
            1,
            commit.operationId
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            reconciliationInsert.get(),
            2,
            reconciliationWireName(commit.outcome.disposition())
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            reconciliationInsert.get(),
            3,
            commit.outcome.proposal().contentHash().hex()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            reconciliationInsert.get(),
            4,
            commit.outcome.proposal().bytes()
        ));
        UF_TRY(expectDone(m_impl->database.get(), reconciliationInsert.get()));

        UF_TRY_VALUE(
            nextOperationRevision,
            checkedSqlIncrement(operationRevision, "Operation revision")
        );
        auto const nextState = operationStateFor(commit.outcome.disposition());
        UF_TRY_VALUE(
            operationUpdate,
            prepare(
                m_impl->database.get(),
                "UPDATE operations SET state=?1, revision=?2 WHERE operation_id=?3 AND revision=?4"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            operationUpdate.get(),
            1,
            operationStateWireName(nextState)
        ));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            operationUpdate.get(),
            2,
            nextOperationRevision
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            operationUpdate.get(),
            3,
            commit.operationId
        ));
        UF_TRY(bindInteger(m_impl->database.get(), operationUpdate.get(), 4, operationRevision));
        UF_TRY(expectDone(m_impl->database.get(), operationUpdate.get()));
        if (sqlite3_changes(m_impl->database.get()) != 1)
        {
            return fail(AutomationErrorKind::ActionRejected, "Operation revision lost its CAS");
        }

        UF_TRY(transaction.commit());
        return StoredOperation{
            .operationId   = commit.operationId,
            .lookup        = CommandLookup::Existing,
            .state         = nextState,
            .revision      = nextOperationRevision,
            .planFrozen    = true,
            .hasDispatched = true,
        };
    }
}
