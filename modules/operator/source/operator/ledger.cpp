#include "ledger.hpp"
#include "runtime-installation.hpp"

#include <core/error/contracts.hpp>
#include <core/safety/annotations.hpp>
#include <core/text/json-text.hpp>
#include <core/text/utf8.hpp>
#include <core/time/monotonic-time.hpp>

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
#include <optional>
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
            auto const* const bytes = sqlite3_column_text(statement, index);
            auto const size = sqlite3_column_bytes(statement, index);
            if (bytes == nullptr || size <= 0)
            {
                return {};
            }
            // SAFETY: sqlite3_column_bytes reports the length of the very
            // buffer sqlite3_column_text returned for the same statement and
            // column, and both stay valid until the next step, reset or
            // finalize. The count arrives beside the pointer rather than within
            // it, so no expression can restate the bound; every read below is
            // bounded by the span this one statement builds.
            UF_UNSAFE_BUFFER_BEGIN
            auto const text = std::span<unsigned char const>{
                bytes,
                static_cast<std::size_t>(size)
            };
            UF_UNSAFE_BUFFER_END
            auto value = std::string{};
            value.reserve(text.size());
            for (auto const byte : text)
            {
                value.push_back(static_cast<char>(byte));
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

        [[nodiscard]]
        auto readDatabaseInteger(
            sqlite3* database,
            std::string_view sql
        ) -> Result<uint64>
        {
            UF_TRY_VALUE(statement, prepare(database, sql));
            if (sqlite3_step(statement.get()) != SQLITE_ROW)
            {
                return databaseFailure(database, "could not read database identity");
            }
            return static_cast<uint64>(sqlite3_column_int64(statement.get(), 0));
        }

        [[nodiscard]]
        auto readDatabaseText(
            sqlite3* database,
            std::string_view sql
        ) -> Result<std::string>
        {
            UF_TRY_VALUE(statement, prepare(database, sql));
            if (sqlite3_step(statement.get()) != SQLITE_ROW)
            {
                return databaseFailure(database, "could not read database schema");
            }
            return columnText(statement.get(), 0);
        }

        // The layout refusal both doors share. open() runs it after creating the
        // directory it names; readInstalledRuntimeArtifact runs it instead of
        // creating one, so the two refuse the same shapes for the same reasons.
        [[nodiscard]]
        auto requirePlainDirectory(
            std::filesystem::path const& directory,
            std::string_view description
        ) -> Status
        {
            auto error        = std::error_code{};
            auto const status = std::filesystem::symlink_status(directory, error);
            if (
                error
                || !std::filesystem::is_directory(status)
                || std::filesystem::is_symlink(status)
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format("{} must be a plain directory", description),
                    error
                );
            }
            return ok();
        }

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
            "sha256:500c07b10eb263c0f2d6001e0a8b9a90ddd2afd951130cef71f5dbbfbd66085a"
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

        // "Is this file the exact Operator runtime schema v1", and nothing else.
        // Every statement it runs is a read, which is why the read-only door can
        // apply the same identity gate open() does rather than a weaker one of
        // its own.
        [[nodiscard]]
        auto verifyOperatorSchemaV1(sqlite3* database) -> Status
        {
            constexpr auto applicationIdentity = uint64{0x55464F50U};
            constexpr auto expectedTables      = std::string_view{
                "agent_budgets,approvals,authority_decisions,control_leases,"
                "control_transitions,dispatches,external_input_findings,"
                "fencing_high_water,journal_events,ledger_events,"
                "operation_plans,operation_steps,operations,"
                "project_instances,project_observations,"
                "project_registrations,project_state,reconciliations,"
                "runtime_artifacts,runtime_installations,runtime_state,"
                "sessions,snapshots"
            };

            UF_TRY_VALUE(
                applicationId,
                readDatabaseInteger(database, "PRAGMA application_id")
            );
            UF_TRY_VALUE(
                userVersion,
                readDatabaseInteger(database, "PRAGMA user_version")
            );
            if (applicationId != applicationIdentity || userVersion != 1U)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Database is not the exact Operator runtime schema v1"
                );
            }
            UF_TRY_VALUE(
                tables,
                readDatabaseText(
                    database,
                    "SELECT group_concat(name, ',') FROM (SELECT name FROM sqlite_master "
                    "WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name)"
                )
            );
            if (tables != expectedTables)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Operator database table set does not match schema v1"
                );
            }
            UF_TRY(verifyExactDatabaseSchema(database));
            UF_TRY_VALUE(integrity, readDatabaseText(database, "PRAGMA quick_check"));
            if (integrity != "ok")
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    "Operator database quick_check failed"
                );
            }
            return ok();
        }

        // The one query that answers "does this ledger pin that generation to
        // that artifact". Shared, so the coordinator's door and the read-only
        // door cannot come to answer it differently.
        [[nodiscard]]
        auto requireInstalledArtifactPin(
            sqlite3* database,
            uint64 installedGeneration,
            ContentHash const& artifactRootHash
        ) -> Status
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
                    database,
                    "SELECT 1 FROM runtime_installations WHERE installed_generation=?1 "
                    "AND artifact_root_hash=?2"
                )
            );
            UF_TRY(bindInteger(database, query.get(), 1, installedGeneration));
            UF_TRY(bindText(database, query.get(), 2, artifactRootHash.hex()));
            if (sqlite3_step(query.get()) != SQLITE_ROW)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "RuntimeArtifact root is not pinned to the requested installed generation"
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

            UF_TRY_VALUE(journalMode, readDatabaseText(database, "PRAGMA journal_mode"));
            UF_TRY_VALUE(
                foreignKeys,
                readDatabaseInteger(database, "PRAGMA foreign_keys")
            );
            UF_TRY_VALUE(
                synchronous,
                readDatabaseInteger(database, "PRAGMA synchronous")
            );
            UF_TRY_VALUE(
                trustedSchema,
                readDatabaseInteger(database, "PRAGMA trusted_schema")
            );
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

            UF_TRY_VALUE(
                applicationId,
                readDatabaseInteger(database, "PRAGMA application_id")
            );
            UF_TRY_VALUE(
                userVersion,
                readDatabaseInteger(database, "PRAGMA user_version")
            );
            UF_TRY_VALUE(
                tableCount,
                readDatabaseInteger(
                    database,
                    "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                    "AND name NOT LIKE 'sqlite_%'"
                )
            );
            if (applicationId != 0U || userVersion != 0U || tableCount != 0U)
            {
                return verifyOperatorSchemaV1(database);
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

                    -- One row per distinct reading a ProjectPlugin.derive
                    -- produced. Its revision is its own line: it advances when
                    -- any input the derive fingerprint covers moved and stays
                    -- put when the same world is observed again, so a snapshot
                    -- can name a reading rather than an occasion.
                    CREATE TABLE IF NOT EXISTS project_observations(
                        plugin_id TEXT NOT NULL,
                        project_instance_key TEXT NOT NULL,
                        revision INTEGER NOT NULL CHECK(revision > 0),
                        project_registration_hash TEXT NOT NULL
                            REFERENCES project_registrations(registration_hash),
                        plugin_hash TEXT NOT NULL,
                        observation_schema_hash TEXT NOT NULL,
                        state_resolution_hash TEXT NOT NULL,
                        project_state_revision INTEGER NOT NULL
                            CHECK(project_state_revision >= 0),
                        project_state_hash TEXT NOT NULL,
                        canonical_observation TEXT NOT NULL,
                        observation_hash TEXT NOT NULL,
                        FOREIGN KEY(plugin_id, project_instance_key)
                            REFERENCES project_instances(plugin_id, project_instance_key),
                        PRIMARY KEY(plugin_id, project_instance_key, revision)
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
                        controlled_target_id TEXT NOT NULL,
                        project_instance_key TEXT NOT NULL,
                        mode TEXT NOT NULL CHECK(mode IN ('read', 'write')),
                        -- Which of the three operators holds this session. It
                        -- is part of the immutable pinned tuple, so a
                        -- controller cannot become another kind between two
                        -- commands, and bindController reads it here rather
                        -- than accepting it.
                        controller_kind TEXT NOT NULL
                            CHECK(controller_kind IN ('script', 'agent', 'human')),
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
                        controlled_target_id TEXT PRIMARY KEY,
                        fencing_token INTEGER NOT NULL CHECK(fencing_token > 0)
                    ) STRICT;

                    CREATE TABLE IF NOT EXISTS control_leases(
                        controlled_target_id TEXT PRIMARY KEY,
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
                        controlled_target_id TEXT NOT NULL,
                        session_id TEXT NOT NULL,
                        controller_id TEXT NOT NULL,
                        lease_id TEXT NOT NULL,
                        session_epoch INTEGER NOT NULL,
                        fencing_token INTEGER NOT NULL,
                        transition TEXT NOT NULL,
                        reason TEXT NOT NULL
                    ) STRICT;

                    -- The one append-only sequence every controller-visible
                    -- fact is appended to, in the same transaction that causes
                    -- it. It exists because control_transitions.sequence and
                    -- reconciliations.sequence are independent counters and
                    -- three counters cannot be one cursor.
                    --
                    -- The kind vocabulary lists exactly what is appended today.
                    -- A value nothing writes would be a promise with no code,
                    -- so the enumeration grows with its producer rather than
                    -- ahead of it.
                    CREATE TABLE IF NOT EXISTS ledger_events(
                        sequence INTEGER PRIMARY KEY AUTOINCREMENT,
                        session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),
                        controlled_target_id TEXT NOT NULL,
                        kind TEXT NOT NULL CHECK(kind IN (
                            'operation_created', 'control_transitioned',
                            'external_input_detected'
                        )),
                        subject_id TEXT NOT NULL
                    ) STRICT;

                    -- canonical_parts is the exact SnapshotParts JCS and is the
                    -- only thing identity_hash and decision_basis_hash are
                    -- recomputable from, which is what lets a test falsify the
                    -- derivation. The scalar columns below it are not a second
                    -- spelling of the same fact: they are the join keys, and
                    -- SQL cannot join through JSON text. submitCommand
                    -- compares project_state_revision and
                    -- project_observation_revision against the live rows, so a
                    -- token goes stale when the composed world moves and not
                    -- only when the lease does. One test asserts each scalar
                    -- equals its member in canonical_parts.
                    --
                    -- token and snapshot_revision are deliberately outside
                    -- canonical_parts: they are record naming and not composed
                    -- state, which is what makes two snapshots over an
                    -- identical world share an identity_hash.
                    CREATE TABLE IF NOT EXISTS snapshots(
                        token TEXT PRIMARY KEY,
                        session_id TEXT NOT NULL REFERENCES sessions(session_id),
                        snapshot_revision INTEGER NOT NULL
                            CHECK(snapshot_revision > 0),
                        session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),
                        identity_hash TEXT NOT NULL,
                        decision_basis_hash TEXT NOT NULL,
                        canonical_parts TEXT NOT NULL,
                        lease_revision INTEGER NOT NULL CHECK(lease_revision > 0),
                        plugin_id TEXT NOT NULL,
                        project_instance_key TEXT NOT NULL,
                        observation_id TEXT NOT NULL,
                        target_generation INTEGER NOT NULL
                            CHECK(target_generation > 0),
                        state_resolution_hash TEXT NOT NULL,
                        project_observation_revision INTEGER NOT NULL
                            CHECK(project_observation_revision > 0),
                        project_state_revision INTEGER NOT NULL
                            CHECK(project_state_revision >= 0),
                        availability_revision INTEGER NOT NULL
                            CHECK(availability_revision >= 0),
                        UNIQUE(session_id, snapshot_revision),
                        FOREIGN KEY(plugin_id, project_instance_key,
                                    project_observation_revision)
                            REFERENCES project_observations(
                                plugin_id, project_instance_key, revision
                            )
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
                        controlled_target_id TEXT NOT NULL,
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
                    ON operations(controlled_target_id)
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
)sql"
                // One statement sequence, two literals: MSVC caps a single
                // string literal and the schema outgrew it here. Adjacent
                // literals concatenate before anything reads them, so the SQL
                // text -- and therefore k_exactSchemaV1Fingerprint, which
                // covers the STORED DDL -- is byte-identical to the unsplit
                // block. The seam must add no character of its own: it sits
                // between a newline and a newline, and it moves to wherever the
                // cap requires without moving the fingerprint, because the two
                // halves concatenate to the same bytes wherever it sits.
                R"sql(

                    -- operation_id is the primary key, so a plan freezes once
                    -- and a second freeze is a constraint violation rather than
                    -- a policy check. There is no update path and none may be
                    -- added. required_approvals is 0 or 1 because the only
                    -- approval kind the ledger has is the single human token in
                    -- approvals; a list would be a column nothing reads.
                    CREATE TABLE IF NOT EXISTS operation_plans(
                        operation_id TEXT PRIMARY KEY
                            REFERENCES operations(operation_id),
                        plan_hash TEXT NOT NULL,
                        command_fingerprint TEXT NOT NULL,
                        decision_basis_hash TEXT NOT NULL,
                        effect_envelope_hash TEXT NOT NULL,
                        project_registration_hash TEXT NOT NULL
                            REFERENCES project_registrations(registration_hash),
                        risk TEXT NOT NULL CHECK(risk IN (
                            'read_only', 'low', 'medium', 'high', 'critical'
                        )),
                        required_approvals INTEGER NOT NULL
                            CHECK(required_approvals IN (0, 1)),
                        maximum_steps INTEGER NOT NULL CHECK(maximum_steps > 0),
                        maximum_dispatches INTEGER NOT NULL
                            CHECK(maximum_dispatches > 0),
                        maximum_observations INTEGER NOT NULL
                            CHECK(maximum_observations > 0),
                        maximum_waits INTEGER NOT NULL CHECK(maximum_waits >= 0),
                        maximum_elapsed_ms INTEGER NOT NULL
                            CHECK(maximum_elapsed_ms > 0),
                        canonical_plan TEXT NOT NULL
                    ) STRICT;

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

                    -- The outcome vocabulary is a database fact rather than a
                    -- C++ string comparison, because commitReconciliation's
                    -- proof of absence is spelled delivery_outcome
                    -- <>'not_delivered' and a fourth spelling would silently
                    -- read as "an effect may have happened". delivery_reason is
                    -- required for exactly the two values that are not
                    -- delivered, which is the schema's own DeliveryOutcome rule
                    -- and closes the gap where the Host's reason for refusing to
                    -- act was discarded.
                    CREATE TABLE IF NOT EXISTS dispatches(
                        operation_id TEXT NOT NULL REFERENCES operations(operation_id),
                        dispatch_sequence INTEGER NOT NULL CHECK(dispatch_sequence > 0),
                        decision_basis_hash TEXT NOT NULL,
                        frozen_plan_hash TEXT NOT NULL,
                        authority_decision_id TEXT NOT NULL
                            REFERENCES authority_decisions(authority_decision_id),
                        delivery_outcome TEXT
                            CHECK(delivery_outcome IN (
                                'not_delivered', 'delivered', 'transport_unknown'
                            )),
                        delivery_reason TEXT,
                        CHECK(
                            (delivery_outcome IS NULL AND delivery_reason IS NULL)
                            OR (delivery_outcome = 'delivered'
                                AND delivery_reason IS NULL)
                            OR (delivery_outcome IN ('not_delivered',
                                                     'transport_unknown')
                                AND delivery_reason IS NOT NULL)
                        ),
                        PRIMARY KEY(operation_id, dispatch_sequence)
                    ) STRICT;

                    -- step_index is dense and monotone because it comes from
                    -- MAX(step_index) + 1 read inside the inserting
                    -- transaction, so there is no gap to slip a step into.
                    -- dispatch_sequence is NULL until reserveDispatch links the
                    -- step to its dispatch, and "at most one UI-action step
                    -- awaiting dispatch" is deliberately enforced only by
                    -- mintNextStep: a partial unique index beside that check
                    -- would keep its test green after the check was deleted.
                    CREATE TABLE IF NOT EXISTS operation_steps(
                        operation_id TEXT NOT NULL
                            REFERENCES operation_plans(operation_id),
                        step_index INTEGER NOT NULL CHECK(step_index > 0),
                        step_kind TEXT NOT NULL
                            CHECK(step_kind IN ('ui_action', 'wait')),
                        step_key TEXT NOT NULL,
                        step_intent_hash TEXT NOT NULL,
                        canonical_step TEXT NOT NULL,
                        dispatch_sequence INTEGER,
                        PRIMARY KEY(operation_id, step_index),
                        FOREIGN KEY(operation_id, dispatch_sequence)
                            REFERENCES dispatches(operation_id, dispatch_sequence)
                    ) STRICT;

                    CREATE TABLE IF NOT EXISTS approvals(
                        token TEXT PRIMARY KEY,
                        operation_id TEXT NOT NULL REFERENCES operations(operation_id),
                        session_id TEXT NOT NULL REFERENCES sessions(session_id),
                        controller_id TEXT NOT NULL,
                        controlled_target_id TEXT NOT NULL,
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

                    -- This row IS JR:`JournalEvent`, member for member, so its
                    -- columns carry that record's member names and no storage
                    -- vocabulary of their own. opaque_project_payload holds the
                    -- project's payload alone and never a serialized event.
                    -- contract-agent-a04 binds this column set to the schema's
                    -- required list, so a divergence here is a failing gate
                    -- rather than a name only a cross-repository read finds.
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
                        opaque_project_payload TEXT NOT NULL,
                        provenance TEXT NOT NULL,
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

                    -- What out-of-band human input leaves behind. It
                    -- deliberately has no tool_name, tool_version,
                    -- canonical_args, command_fingerprint, client_request_id or
                    -- snapshot_token column, and no state: an auditor tells a
                    -- command from a finding by which table the row is in, and
                    -- the two column sets are disjoint by construction. A
                    -- finding cannot be spelled as an Operation and an
                    -- Operation cannot be spelled as a finding.
                    --
                    -- invalidated_snapshot_revision is the snapshot revision
                    -- this target had reached when the input was seen; every
                    -- token at or below it is refused afterwards, which is how
                    -- a keystroke stops the automation without terminating it.
                    CREATE TABLE IF NOT EXISTS external_input_findings(
                        finding_id TEXT PRIMARY KEY,
                        controlled_target_id TEXT NOT NULL,
                        session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),
                        reporter_session_id TEXT NOT NULL
                            REFERENCES sessions(session_id),
                        detected_after_cursor INTEGER NOT NULL
                            CHECK(detected_after_cursor >= 0),
                        invalidated_snapshot_revision INTEGER NOT NULL
                            CHECK(invalidated_snapshot_revision >= 0),
                        operation_id TEXT REFERENCES operations(operation_id),
                        required_action TEXT NOT NULL
                            CHECK(required_action IN (
                                'freeze_and_reobserve', 'freeze_and_reconcile'
                            )),
                        reason TEXT NOT NULL
                    ) STRICT;

                    -- What one online Agent binding has left to spend, and the
                    -- marker the no-progress rule compares each step against.
                    -- pinSession writes it from the exact AgentProfile bytes
                    -- the session manifest's agent_profile_hash names, so there
                    -- is no path from a ControllerBinding to any of these
                    -- numbers except downwards.
                    --
                    -- Each CHECK is the enforcement of its own ceiling and not
                    -- a second guard beside one in C++: a charge is an
                    -- unconditional decrement and the constraint is what
                    -- refuses it at zero. Relaxing a CHECK therefore lets one
                    -- more command through, which is what makes the ceiling
                    -- falsifiable.
                    --
                    -- The row is inert after a restart rather than reset: a new
                    -- session epoch deactivates every session the previous one
                    -- left behind, so no binding can be minted against this row
                    -- again and a re-pinned session starts from a new one.
                    -- There is deliberately no agent_profile_hash column. The
                    -- session row already names the manifest this budget was
                    -- pinned with, and that manifest names the profile, so a
                    -- column here would be a second spelling of a fact the
                    -- session already determines -- and one nothing reads.
                    CREATE TABLE IF NOT EXISTS agent_budgets(
                        session_id TEXT PRIMARY KEY REFERENCES sessions(session_id),
                        deadline_steady_millis INTEGER NOT NULL
                            CHECK(deadline_steady_millis > 0),
                        remaining_tool_calls INTEGER NOT NULL
                            CHECK(remaining_tool_calls >= 0),
                        remaining_mutations INTEGER NOT NULL
                            CHECK(remaining_mutations >= 0),
                        remaining_observations INTEGER NOT NULL
                            CHECK(remaining_observations >= 0),
                        remaining_risk_units INTEGER NOT NULL
                            CHECK(remaining_risk_units >= 0),
                        last_state_fingerprint TEXT NOT NULL,
                        last_command_fingerprint TEXT NOT NULL,
                        consecutive_no_progress_steps INTEGER NOT NULL
                            CHECK(consecutive_no_progress_steps >= 0)
                    ) STRICT;

                    -- This row IS JR:`ProjectState`, member for member, for the
                    -- reason journal_events carries, and project_registrations
                    -- spells project_state_schema_hash the same way.
                    -- contract-state-s06 binds this column set to the schema's
                    -- required list.
                    CREATE TABLE IF NOT EXISTS project_state(
                        plugin_id TEXT NOT NULL,
                        project_instance_key TEXT NOT NULL,
                        revision INTEGER NOT NULL CHECK(revision >= 0),
                        project_registration_hash TEXT NOT NULL,
                        project_state_schema_hash TEXT NOT NULL,
                        last_journal_sequence INTEGER NOT NULL CHECK(last_journal_sequence >= 0),
                        canonical_opaque_payload TEXT NOT NULL,
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
            task::DeliveryOutcome outcome
        ) noexcept -> std::string_view
        {
            switch (outcome)
            {
            case task::DeliveryOutcome::NotDelivered: return "not_delivered";
            case task::DeliveryOutcome::Delivered: return "delivered";
            case task::DeliveryOutcome::TransportUnknown: return "transport_unknown";
            }

            UF_UNREACHABLE_MSG("Unknown DeliveryOutcome value");
        }

        // Drives every dispatch nobody has answered for to transport_unknown and
        // its Operation to reconciling, one checked-increment CAS per row. An
        // empty controlledTargetId means every target, which is what a restart
        // sweeps; a takeover names the one target it seized. Never
        // not_delivered: a dispatch the Host may already have posted is exactly
        // what the third value exists for.
        //
        // It runs inside the caller's transaction rather than opening one, so a
        // takeover's fence bump and the resolution it forces commit together or
        // not at all.
        [[nodiscard]]
        auto resolveUnansweredDispatches(
            sqlite3* database,
            std::string_view controlledTargetId,
            std::string_view reason
        ) -> Result<uint64>
        {
            auto scan = std::string{
                "SELECT o.operation_id, o.revision, d.dispatch_sequence "
                "FROM operations o JOIN dispatches d ON d.operation_id=o.operation_id "
                "WHERE o.state='running' AND d.delivery_outcome IS NULL"
            };
            if (!controlledTargetId.empty())
            {
                scan += " AND o.controlled_target_id=?1";
            }
            UF_TRY_VALUE(query, prepare(database, scan));
            if (!controlledTargetId.empty())
            {
                UF_TRY(bindText(database, query.get(), 1, controlledTargetId));
            }

            auto pending   = std::vector<std::tuple<std::string, uint64, uint64>>{};
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
                return databaseFailure(database, "could not scan pending dispatches");
            }

            for (auto const& [operationId, revision, dispatchSequence] : pending)
            {
                UF_TRY_VALUE(
                    nextRevision,
                    checkedSqlIncrement(revision, "Operation revision")
                );
                UF_TRY_VALUE(
                    dispatchUpdate,
                    prepare(
                        database,
                        "UPDATE dispatches SET delivery_outcome='transport_unknown', "
                        "delivery_reason=?1 WHERE operation_id=?2 "
                        "AND dispatch_sequence=?3 AND delivery_outcome IS NULL"
                    )
                );
                UF_TRY(bindText(database, dispatchUpdate.get(), 1, reason));
                UF_TRY(bindText(database, dispatchUpdate.get(), 2, operationId));
                UF_TRY(bindInteger(database, dispatchUpdate.get(), 3, dispatchSequence));
                UF_TRY(expectDone(database, dispatchUpdate.get()));
                if (sqlite3_changes(database) != 1)
                {
                    return fail(
                        AutomationErrorKind::ActionRejected,
                        "Pending dispatch resolution lost its CAS"
                    );
                }

                UF_TRY_VALUE(
                    operationUpdate,
                    prepare(
                        database,
                        "UPDATE operations SET state='reconciling', revision=?1 "
                        "WHERE operation_id=?2 AND state='running' AND revision=?3"
                    )
                );
                UF_TRY(bindInteger(database, operationUpdate.get(), 1, nextRevision));
                UF_TRY(bindText(database, operationUpdate.get(), 2, operationId));
                UF_TRY(bindInteger(database, operationUpdate.get(), 3, revision));
                UF_TRY(expectDone(database, operationUpdate.get()));
                if (sqlite3_changes(database) != 1)
                {
                    return fail(
                        AutomationErrorKind::ActionRejected,
                        "Pending Operation resolution lost its CAS"
                    );
                }
            }
            return static_cast<uint64>(pending.size());
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
        auto ledgerEventWireName(LedgerEventKind kind) noexcept -> std::string_view
        {
            switch (kind)
            {
            case LedgerEventKind::OperationCreated: return "operation_created";
            case LedgerEventKind::ControlTransitioned: return "control_transitioned";
            case LedgerEventKind::ExternalInputDetected: return "external_input_detected";
            }

            UF_UNREACHABLE_MSG("Unknown LedgerEventKind value");
        }

        [[nodiscard]]
        auto parseLedgerEventKind(std::string_view value) -> Result<LedgerEventKind>
        {
            constexpr auto kinds = std::array{
                LedgerEventKind::OperationCreated,
                LedgerEventKind::ControlTransitioned,
                LedgerEventKind::ExternalInputDetected,
            };
            auto const match = std::ranges::find_if(
                kinds,
                [value](LedgerEventKind candidate)
                {
                    return ledgerEventWireName(candidate) == value;
                }
            );
            if (match == kinds.end())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format("Unknown ledger event kind: {}", value)
                );
            }
            return *match;
        }

        [[nodiscard]]
        auto externalInputActionWireName(
            ExternalInputAction action
        ) noexcept -> std::string_view
        {
            switch (action)
            {
            case ExternalInputAction::FreezeAndReobserve: return "freeze_and_reobserve";
            case ExternalInputAction::FreezeAndReconcile: return "freeze_and_reconcile";
            }

            UF_UNREACHABLE_MSG("Unknown ExternalInputAction value");
        }

        // The cursor as it stands right now: the sequence of the last appended
        // event, or 0 before the first one. Read inside the caller's
        // transaction, so nothing can commit between reading it and using it.
        [[nodiscard]]
        auto currentEventCursor(sqlite3* database) -> Result<uint64>
        {
            UF_TRY_VALUE(
                query,
                prepare(
                    database,
                    "SELECT COALESCE(MAX(sequence), 0) FROM ledger_events"
                )
            );
            if (sqlite3_step(query.get()) != SQLITE_ROW)
            {
                return databaseFailure(database, "could not read the event cursor");
            }
            return static_cast<uint64>(sqlite3_column_int64(query.get(), 0));
        }

        // Appended in the same transaction as the fact it records, never after
        // it. SQLite allows one writer at a time and every mutating path here
        // opens BEGIN IMMEDIATE, so sequences are assigned in commit order and
        // a rolled-back append leaves no gap.
        [[nodiscard]]
        auto appendLedgerEvent(
            sqlite3* database,
            uint64 sessionEpoch,
            std::string_view controlledTargetId,
            LedgerEventKind kind,
            std::string_view subjectId
        ) -> Status
        {
            UF_TRY_VALUE(
                insert,
                prepare(
                    database,
                    "INSERT INTO ledger_events(session_epoch, controlled_target_id, "
                    "kind, subject_id) VALUES(?1, ?2, ?3, ?4)"
                )
            );
            UF_TRY(bindInteger(database, insert.get(), 1, sessionEpoch));
            UF_TRY(bindText(database, insert.get(), 2, controlledTargetId));
            UF_TRY(bindText(database, insert.get(), 3, ledgerEventWireName(kind)));
            UF_TRY(bindText(database, insert.get(), 4, subjectId));
            return expectDone(database, insert.get());
        }

        // The clock the Agent time budget is measured on, read by the Operator
        // and never supplied by a caller: a controller that could state the
        // current instant could state one before its own deadline, and the
        // budget would be a suggestion.
        //
        // Steady rather than wall, because a budget a clock adjustment can
        // widen is not a budget. The stored deadline is therefore meaningful
        // only inside the process that wrote it -- which is exactly the
        // lifetime of the session epoch it belongs to, and the same reason the
        // budget does not survive a restart.
        [[nodiscard]]
        auto steadyMillisecondsNow() -> Result<uint64>
        {
            auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                MonotonicInstant::now().timePoint().time_since_epoch()
            ).count();
            if (elapsed < 0)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "Steady clock reads before its own epoch"
                );
            }
            return static_cast<uint64>(elapsed);
        }

        // One binding's remaining ceilings and its progress marker.
        struct AgentBudgetState final
        {
            std::string lastStateFingerprint{};
            std::string lastCommandFingerprint{};
            uint64      deadlineSteadyMillis{};
            uint64      remainingToolCalls{};
            uint64      remainingMutations{};
            uint64      remainingObservations{};
            uint64      remainingRiskUnits{};
            uint64      consecutiveNoProgressSteps{};
        };

        // Whether this session has budgets is ControllerProfile's answer, never
        // an inference from the absence of a row: a missing row for a kind that
        // requires one is a broken invariant, not a controller that happens to
        // run without ceilings.
        [[nodiscard]]
        auto readAgentBudget(
            sqlite3* database,
            std::string_view sessionId,
            ControllerKind kind
        ) -> Result<std::optional<AgentBudgetState>>
        {
            UF_TRY_VALUE(
                query,
                prepare(
                    database,
                    "SELECT deadline_steady_millis, remaining_tool_calls, "
                    "remaining_mutations, remaining_observations, "
                    "remaining_risk_units, last_state_fingerprint, "
                    "last_command_fingerprint, consecutive_no_progress_steps "
                    "FROM agent_budgets WHERE session_id=?1"
                )
            );
            UF_TRY(bindText(database, query.get(), 1, sessionId));
            auto const present = sqlite3_step(query.get()) == SQLITE_ROW;
            if (present != controllerProfile(kind).budgetsRequired)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "Agent budget row does not match what the controller kind requires"
                );
            }
            if (!present)
            {
                return std::optional<AgentBudgetState>{};
            }
            return std::optional{AgentBudgetState{
                .lastStateFingerprint   = columnText(query.get(), 5),
                .lastCommandFingerprint = columnText(query.get(), 6),
                .deadlineSteadyMillis   = static_cast<uint64>(
                    sqlite3_column_int64(query.get(), 0)
                ),
                .remainingToolCalls     = static_cast<uint64>(
                    sqlite3_column_int64(query.get(), 1)
                ),
                .remainingMutations     = static_cast<uint64>(
                    sqlite3_column_int64(query.get(), 2)
                ),
                .remainingObservations  = static_cast<uint64>(
                    sqlite3_column_int64(query.get(), 3)
                ),
                .remainingRiskUnits     = static_cast<uint64>(
                    sqlite3_column_int64(query.get(), 4)
                ),
                .consecutiveNoProgressSteps = static_cast<uint64>(
                    sqlite3_column_int64(query.get(), 7)
                ),
            }};
        }

        // Compared, never decremented: elapsed time is not a quantity the
        // ledger hands out. Inclusive at the limit, so a step submitted at the
        // deadline itself is still inside the budget and only one past it is
        // not.
        [[nodiscard]]
        auto requireWithinAgentDeadline(AgentBudgetState const& budget) -> Status
        {
            UF_TRY_VALUE(now, steadyMillisecondsNow());
            if (now > budget.deadlineSteadyMillis)
            {
                return fail(
                    AutomationErrorKind::Timeout,
                    "Agent time budget expired before this call"
                );
            }
            return ok();
        }

        // Spends one column of one binding's budget. There is deliberately no
        // comparison here: the column's own CHECK is what refuses the spend at
        // zero, so the ceiling has exactly one spelling and relaxing that
        // constraint lets one more call through.
        [[nodiscard]]
        auto chargeAgentBudget(
            sqlite3* database,
            std::string_view sessionId,
            std::string_view sql,
            uint64 amount,
            std::string_view exhausted
        ) -> Status
        {
            UF_TRY_VALUE(update, prepare(database, sql));
            UF_TRY(bindText(database, update.get(), 1, sessionId));
            UF_TRY(bindInteger(database, update.get(), 2, amount));
            auto const step = sqlite3_step(update.get());
            if (step == SQLITE_DONE)
            {
                return ok();
            }
            if ((step & 0xFF) == SQLITE_CONSTRAINT)
            {
                return fail(AutomationErrorKind::ActionRejected, std::string{exhausted});
            }
            return databaseFailure(database, "could not charge an agent budget");
        }

        [[nodiscard]]
        auto parseSessionMode(std::string_view value) -> Result<SessionMode>
        {
            constexpr auto modes = std::array{SessionMode::Read, SessionMode::Write};
            auto const match = std::ranges::find_if(
                modes,
                [value](SessionMode candidate)
                {
                    return sessionModeWireName(candidate) == value;
                }
            );
            if (match == modes.end())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format("Unknown session mode: {}", value)
                );
            }
            return *match;
        }

        // Re-reads the pinned row a binding names and refuses one whose session
        // is no longer active in this epoch. The binding is evidence of who the
        // caller was when bindController minted it; this row is the authority
        // now, so every entry point that takes a binding starts here.
        //
        // Activity is the whole of what the row can tell us that the binding
        // cannot. Every other member -- the controller id, the capability hash,
        // the controlled target, the kind -- was copied out of this same row,
        // and the row is immutable, so comparing them back would be comparing a
        // value against the column it came from. Only bindController can mint a
        // binding, so there is no forged one for such a comparison to catch.
        //
        // The epoch is not re-tested here either, and that is the same
        // reduction rather than a second one: opening the database begins a new
        // session epoch and deactivates every session the previous one left
        // behind, so "from a dead epoch" and "inactive" are one fact recorded
        // twice. bindController still names the epoch because that is where a
        // binding's epoch value is established; testing it again on every call
        // afterwards is a conjunct that cannot fail on its own.
        [[nodiscard]]
        auto requireLiveBinding(
            sqlite3* database,
            ControllerBinding const& controller
        ) -> Result<SessionMode>
        {
            UF_TRY_VALUE(
                query,
                prepare(
                    database,
                    "SELECT mode FROM sessions WHERE session_id=?1 AND active=1"
                )
            );
            UF_TRY(bindText(database, query.get(), 1, controller.sessionId()));
            if (sqlite3_step(query.get()) != SQLITE_ROW)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "ControllerBinding names no active session"
                );
            }
            return parseSessionMode(columnText(query.get(), 0));
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

        auto appendHashMember(std::string& output, ContentHash const& hash) -> void
        {
            appendJsonString(output, hash.hex());
        }

        // The exact bytes ProjectPlugin.derive is called with, assembled here
        // for reduceEnvelopeJcs's reason: a caller that supplied the derive
        // input could have the snapshot record one world while the derivation
        // saw another, and the recorded decision basis would then certify a
        // world that was never true at any instant.
        //
        // JCS orders members by UTF-16 code unit, which is why
        // pending_operation_transition precedes
        // pinned_project_artifact_identities precedes
        // prior_project_observation precedes project_state precedes ui_snapshot.
        //
        // The two optional members carry the literal `null` rather than being
        // absent, for the reason reduceEnvelopeJcs already gives: a plugin must
        // be able to tell "no prior reading" from "a prior reading I failed to
        // read".
        //
        // ui_snapshot carries the canonical StateResolution document and never
        // the observation id. That is what makes a semantically equivalent
        // recapture produce an identical derive input, an identical
        // project_observation_hash and an identical decision_basis_hash.
        struct DeriveEnvelopeInputs final
        {
            std::string_view             pendingOperationJcs{};
            std::span<ContentHash const> pinnedArtifactRoots{};
            std::string_view             priorObservationJcs{};
            std::string_view             projectStateJcs{};
            std::string_view             uiSnapshotJcs{};
        };

        [[nodiscard]]
        auto deriveEnvelopeJcs(DeriveEnvelopeInputs const& inputs) -> std::string
        {
            auto envelope = std::string{"{\"pending_operation_transition\":"};
            envelope += inputs.pendingOperationJcs;
            envelope += ",\"pinned_project_artifact_identities\":[";
            auto first = true;
            for (auto const& root : inputs.pinnedArtifactRoots)
            {
                if (!first)
                {
                    envelope.push_back(',');
                }
                first = false;
                appendHashMember(envelope, root);
            }
            envelope += "],\"prior_project_observation\":";
            envelope += inputs.priorObservationJcs;
            envelope += ",\"project_state\":";
            envelope += inputs.projectStateJcs;
            envelope += ",\"ui_snapshot\":";
            envelope += inputs.uiSnapshotJcs;
            envelope.push_back('}');
            return envelope;
        }

        // The exact bytes ProjectPlugin.plan is called with, assembled here for
        // deriveEnvelopeJcs's reason: a caller that supplied the plan input
        // could have the plugin propose a plan for one command while the
        // Operation records another, and command_fingerprint would then name a
        // command the plan was never about. Every part is a column the freezing
        // transaction read.
        //
        // JCS orders members by UTF-16 code unit, which is why canonical_args
        // precedes project_observation precedes project_state precedes
        // tool_name precedes tool_version.
        struct PlanEnvelopeInputs final
        {
            std::string_view canonicalArgs{};
            std::string_view projectObservationJcs{};
            std::string_view projectStateJcs{};
            std::string_view toolName{};
            std::string_view toolVersion{};
        };

        [[nodiscard]]
        auto planEnvelopeJcs(PlanEnvelopeInputs const& inputs) -> std::string
        {
            auto envelope = std::string{"{\"canonical_args\":"};
            envelope += inputs.canonicalArgs;
            envelope += ",\"project_observation\":";
            envelope += inputs.projectObservationJcs;
            envelope += ",\"project_state\":";
            envelope += inputs.projectStateJcs;
            envelope += ",\"tool_name\":";
            appendJsonString(envelope, inputs.toolName);
            envelope += ",\"tool_version\":";
            appendJsonString(envelope, inputs.toolVersion);
            envelope.push_back('}');
            return envelope;
        }

        // The exact bytes ProjectPlugin.next_step is called with. It carries
        // the frozen plan and the index the Operator is about to mint at, so a
        // plugin cannot be told one position and answer for another, and the
        // world as the last snapshot composed it.
        //
        // JCS order again: frozen_plan_hash precedes project_observation
        // precedes project_state precedes step_index.
        struct StepEnvelopeInputs final
        {
            std::string_view frozenPlanHashHex{};
            std::string_view projectObservationJcs{};
            std::string_view projectStateJcs{};
            uint64           stepIndex{};
        };

        [[nodiscard]]
        auto stepEnvelopeJcs(StepEnvelopeInputs const& inputs) -> std::string
        {
            auto envelope = std::string{"{\"frozen_plan_hash\":"};
            appendJsonString(envelope, inputs.frozenPlanHashHex);
            envelope += ",\"project_observation\":";
            envelope += inputs.projectObservationJcs;
            envelope += ",\"project_state\":";
            envelope += inputs.projectStateJcs;
            envelope += ",\"step_index\":";
            envelope += std::to_string(inputs.stepIndex);
            envelope.push_back('}');
            return envelope;
        }

        // OP:`DecisionBasis`, whose fifth member is the digest over the other
        // four and therefore cannot be inside it. Which four is the whole
        // requirement: everything the snapshot identity carries and this does
        // not -- lease, fencing token, epoch, token, every revision counter,
        // every observation and target identifier, every wall clock -- is
        // authority, naming or progress rather than decision input, and folding
        // any of it in would make an identical world observed after a takeover
        // read as a different decision.
        struct DecisionBasisParts final
        {
            ContentHash projectObservationHash;
            ContentHash projectStateHash;
            ContentHash sessionManifestHash;
            ContentHash stateResolutionHash;
        };

        [[nodiscard]]
        auto deriveDecisionBasis(
            DecisionBasisParts const& parts
        ) -> Result<ContentHash>
        {
            auto material = std::string{"{\"project_observation_hash\":"};
            appendHashMember(material, parts.projectObservationHash);
            material += ",\"project_state_hash\":";
            appendHashMember(material, parts.projectStateHash);
            material += ",\"session_manifest_hash\":";
            appendHashMember(material, parts.sessionManifestHash);
            material += ",\"state_resolution_hash\":";
            appendHashMember(material, parts.stateResolutionHash);
            material.push_back('}');
            return sha256(std::as_bytes(std::span{material}));
        }

        // OP:`SnapshotParts`, all fifteen members in JCS order. It is the exact
        // text stored as snapshots.canonical_parts, so identity_hash is
        // recomputable from the row and a test can falsify the derivation
        // rather than only compare it against itself.
        struct SnapshotPartsInputs final
        {
            ContentHash      decisionBasisHash;
            ContentHash      projectObservationHash;
            ContentHash      projectStateHash;
            ContentHash      sessionManifestHash;
            ContentHash      stateResolutionHash;
            std::string_view controlledTargetId{};
            std::string_view leaseId{};
            std::string_view observationId{};
            std::string_view projectInstanceKey{};
            uint64           availabilityRevision{};
            uint64           fencingToken{};
            uint64           projectObservationRevision{};
            uint64           projectStateRevision{};
            uint64           sessionEpoch{};
            uint64           targetGeneration{};
        };

        [[nodiscard]]
        auto snapshotPartsJcs(SnapshotPartsInputs const& parts) -> std::string
        {
            auto output = std::string{"{\"availability_revision\":"};
            output += std::to_string(parts.availabilityRevision);
            output += ",\"controlled_target_id\":";
            appendJsonString(output, parts.controlledTargetId);
            output += ",\"decision_basis_hash\":";
            appendHashMember(output, parts.decisionBasisHash);
            output += ",\"fencing_token\":";
            output += std::to_string(parts.fencingToken);
            output += ",\"lease_id\":";
            appendJsonString(output, parts.leaseId);
            output += ",\"observation_id\":";
            appendJsonString(output, parts.observationId);
            output += ",\"project_instance_key\":";
            appendJsonString(output, parts.projectInstanceKey);
            output += ",\"project_observation_hash\":";
            appendHashMember(output, parts.projectObservationHash);
            output += ",\"project_observation_revision\":";
            output += std::to_string(parts.projectObservationRevision);
            output += ",\"project_state_hash\":";
            appendHashMember(output, parts.projectStateHash);
            output += ",\"project_state_revision\":";
            output += std::to_string(parts.projectStateRevision);
            output += ",\"session_epoch\":";
            output += std::to_string(parts.sessionEpoch);
            output += ",\"session_manifest_hash\":";
            appendHashMember(output, parts.sessionManifestHash);
            output += ",\"state_resolution_hash\":";
            appendHashMember(output, parts.stateResolutionHash);
            output += ",\"target_generation\":";
            output += std::to_string(parts.targetGeneration);
            output.push_back('}');
            return output;
        }

        // Every column of the live lease row compared against the value the
        // caller presented. It is one helper rather than four copies of the
        // same eight bindings because a copy that drops a column is a lease
        // check that passes for a superseded controller.
        [[nodiscard]]
        auto requireLiveLease(
            sqlite3* database,
            ControlLease const& lease,
            std::string_view staleMessage
        ) -> Status
        {
            UF_TRY_VALUE(
                query,
                prepare(
                    database,
                    "SELECT 1 FROM control_leases WHERE controlled_target_id=?1 "
                    "AND lease_id=?2 AND session_id=?3 AND controller_id=?4 "
                    "AND session_epoch=?5 AND fencing_token=?6 AND revision=?7 "
                    "AND capability_profile_hash=?8"
                )
            );
            UF_TRY(bindText(database, query.get(), 1, lease.controlledTargetId));
            UF_TRY(bindText(database, query.get(), 2, lease.leaseId));
            UF_TRY(bindText(database, query.get(), 3, lease.sessionId));
            UF_TRY(bindText(database, query.get(), 4, lease.controllerId));
            UF_TRY(bindInteger(database, query.get(), 5, lease.sessionEpoch));
            UF_TRY(bindInteger(database, query.get(), 6, lease.fencingToken));
            UF_TRY(bindInteger(database, query.get(), 7, lease.revision));
            UF_TRY(bindText(
                database,
                query.get(),
                8,
                lease.capabilityProfileHash.hex()
            ));
            if (sqlite3_step(query.get()) != SQLITE_ROW)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    std::string{staleMessage}
                );
            }
            return ok();
        }

        // The controller's vocabulary mapped onto the machine's. It is a table
        // rather than a chain of comparisons for the reason parseOperationState
        // already is one: the set is closed, and a chain lets a new signal be
        // added without anyone deciding what it means.
        //
        // Eight OperationEvent values have no OperationSignal at all. Four are
        // privileged edges an atomic ledger method owns (DispatchStarted,
        // ApprovalObtained, HostOutcomeObserved, CorrectionCommitted plus the
        // three reconciliation dispositions), and four are the plan lifecycle
        // freezePlan and mintNextStep decide.
        struct SignalRule final
        {
            OperationSignal signal;
            OperationEvent  event;
        };

        constexpr auto k_signalRules = std::array{
            SignalRule{OperationSignal::ReadCompleted, OperationEvent::ReadCompleted},
            SignalRule{
                OperationSignal::DecisionInputsChanged,
                OperationEvent::DecisionInputsChanged,
            },
            SignalRule{OperationSignal::Revalidated, OperationEvent::Revalidated},
            SignalRule{OperationSignal::Invalidated, OperationEvent::Invalidated},
            SignalRule{OperationSignal::Denied, OperationEvent::Denied},
            SignalRule{OperationSignal::Cancelled, OperationEvent::Cancelled},
            SignalRule{OperationSignal::DeadlineExpired, OperationEvent::DeadlineExpired},
            SignalRule{OperationSignal::NewEvidence, OperationEvent::NewEvidence},
            SignalRule{
                OperationSignal::PostDispatchAbort,
                OperationEvent::PostDispatchAbort,
            },
        };

        [[nodiscard]]
        auto operationEventFor(OperationSignal signal) -> Result<OperationEvent>
        {
            auto const found = std::ranges::find(
                k_signalRules,
                signal,
                &SignalRule::signal
            );
            if (found == k_signalRules.end())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Unknown Operation signal"
                );
            }
            return found->event;
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
            "ON lease.controlled_target_id=o.controlled_target_id "
            "AND lease.session_id=o.session_id "
        };

        // The content address of an artifact directory, recorded in its own
        // transaction BEFORE the directory is written. A publication that then
        // fails its compare-and-swap leaves a runtime_artifacts row no
        // installation names, which is exactly what
        // reclaimUnreferencedRuntimeArtifacts removes; recording it inside the
        // installing transaction instead would roll the row back and strand the
        // directory with nothing in the database naming it.
        [[nodiscard]]
        auto registerArtifactRoot(
            sqlite3* database,
            std::string_view artifactRootHash
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY_VALUE(
                insert,
                prepare(
                    database,
                    "INSERT OR IGNORE INTO runtime_artifacts(artifact_root_hash) "
                    "VALUES(?1)"
                )
            );
            UF_TRY(bindText(database, insert.get(), 1, artifactRootHash));
            UF_TRY(expectDone(database, insert.get()));
            return transaction.commit();
        }

        // One coordinator owns a runtime directory at a time, and this is what
        // makes that true rather than assumed. beginSessionEpoch below clears
        // every control lease and deactivates every session on the reading that
        // whatever those rows describe died with the process that wrote them.
        // Without an exclusive lock a second open performs those clears against
        // a coordinator that is still running and strips live leases. sqlite
        // holds the lock for the connection's lifetime under this locking mode,
        // so the refusal below is the whole of the enforcement.
        [[nodiscard]]
        auto claimExclusiveOwnership(sqlite3* database) -> Status
        {
            UF_TRY(execute(database, "PRAGMA locking_mode=EXCLUSIVE"));
            auto const code = sqlite3_exec(
                database,
                "BEGIN EXCLUSIVE",
                nullptr,
                nullptr,
                nullptr
            );
            if ((code & 0xFF) == SQLITE_BUSY)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Another Operator coordinator holds this runtime directory"
                );
            }
            if (code != SQLITE_OK)
            {
                return databaseFailure(database, "could not claim the runtime directory");
            }
            return execute(database, "COMMIT");
        }

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

        UF_TRY(requirePlainDirectory(runtimeDirectory, "Operator runtime root"));

        auto const databasePath        = runtimeDirectory / "operator-runtime.sqlite";
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
        UF_TRY(requirePlainDirectory(
            runtimeArtifactRoot,
            "Production RuntimeArtifact root"
        ));

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
        UF_TRY(requirePlainDirectory(
            stagingRoot,
            "Production RuntimeArtifact staging root"
        ));
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

        // Before the schema is touched, so a second coordinator is refused by
        // name rather than by whichever statement happens to hit the lock.
        UF_TRY(claimExclusiveOwnership(database.get()));
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

        UF_TRY(registerArtifactRoot(
            m_impl->database.get(),
            release.artifactRootHash.hex()
        ));
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

        UF_TRY(transaction.commit());
        return task::InstalledRuntimeArtifact{
            std::move(artifact),
            nextGeneration,
        };
    }

    auto OperatorCoordinator::reclaimUnreferencedRuntimeArtifacts()
        -> Result<ReclaimedRuntimeArtifacts>
    {
        // The removals happen INSIDE the write transaction, so the whole
        // reference set is read against one consistent state rather than
        // row by row while it moves.
        //
        // Rolling back after a directory has been removed is safe in the one
        // direction it can happen: a runtime_artifacts row whose directory is
        // missing is still unreferenced, so the next pass finishes the job.
        //
        // There is no in-flight-publication exemption and none may be added.
        // claimExclusiveOwnership refuses a second coordinator for the lifetime
        // of the connection, and OperatorCoordinator carries no synchronization
        // of its own, so a publisher concurrent with this sweep is not a state
        // the design admits. A table recording claims against it was carried
        // until 2026-08-11 and was unreachable in every path.
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
        UF_TRY(requireInstalledArtifactPin(
            m_impl->database.get(),
            installedGeneration,
            artifactRootHash
        ));
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

    auto OperatorCoordinator::readInstalledRuntimeArtifact(
        std::filesystem::path const& runtimeDirectory,
        uint64 installedGeneration,
        ContentHash const& artifactRootHash
    ) -> Result<task::InstalledRuntimeArtifact>
    {
        if (runtimeDirectory.empty())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Operator runtime directory must not be empty"
            );
        }

        // Checked, never created. open() may bootstrap a layout because a
        // coordinator that owns an empty directory is a coordinator that can
        // still install into it; an absent layout holds no installation to read,
        // so creating one here would answer a question about a ledger this call
        // had just invented.
        UF_TRY(requirePlainDirectory(runtimeDirectory, "Operator runtime root"));
        auto const runtimeArtifactRoot = runtimeDirectory / "runtime-artifacts";
        UF_TRY(requirePlainDirectory(
            runtimeArtifactRoot,
            "Production RuntimeArtifact root"
        ));

        auto const databasePath   = runtimeDirectory / "operator-runtime.sqlite";
        auto error                = std::error_code{};
        auto const databaseStatus = std::filesystem::symlink_status(databasePath, error);
        if (
            error
            || !std::filesystem::is_regular_file(databaseStatus)
            || std::filesystem::is_symlink(databaseStatus)
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Operator database path must be an existing plain file",
                error
            );
        }

        // SQLITE_OPEN_READONLY is the whole of the guarantee this function's
        // declaration makes. Without SQLITE_OPEN_CREATE a missing file is
        // refused rather than made, and under READONLY the library itself
        // returns SQLITE_READONLY for any statement that would write, so a
        // later edit that adds one fails at runtime rather than passing review.
        auto* rawDatabase = static_cast<sqlite3*>(nullptr);
        auto const openCode = sqlite3_open_v2(
            pathToUtf8(databasePath).c_str(),
            &rawDatabase,
            SQLITE_OPEN_READONLY
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
                std::format("Could not open Operator database read-only: {}", detail)
            );
        }

        // Before any statement reads sqlite_schema, because the file this call
        // was pointed at is not one it wrote. No busy timeout is installed: a
        // coordinator holding this directory holds SQLite's exclusive lock, and
        // an immediate refusal is a better answer than a stall.
        UF_TRY(execute(database.get(), "PRAGMA trusted_schema=OFF"));
        UF_TRY(verifyOperatorSchemaV1(database.get()));
        UF_TRY(requireInstalledArtifactPin(
            database.get(),
            installedGeneration,
            artifactRootHash
        ));
        UF_TRY_VALUE(
            artifact,
            detail::openProductionRuntimeArtifact(runtimeArtifactRoot, artifactRootHash)
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
            resolved,
            resolveUnansweredDispatches(
                m_impl->database.get(),
                {},
                "operator restart found this dispatch unanswered"
            )
        );
        UF_TRY(transaction.commit());
        return resolved;
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
                "namespaced_event_type, payload_schema_hash, opaque_project_payload, "
                "provenance) "
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
                "project_registration_hash, project_state_schema_hash, last_journal_sequence, "
                "canonical_opaque_payload, state_hash) VALUES(?1, ?2, 0, ?3, ?4, 0, ?5, ?6)"
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
        SessionManifest const& manifest,
        std::optional<AgentProfile> const& agentProfile
    ) -> Status
    {
        UF_TRY(requireName(pin.sessionId, "session_id"));
        UF_TRY(requireName(pin.authenticatedControllerId, "authenticated_controller_id"));
        UF_TRY(requireName(pin.idempotencyNamespace, "idempotency_namespace"));
        UF_TRY(requireName(pin.controlledTargetId, "controlled_target_id"));
        UF_TRY(requireName(pin.projectInstanceKey, "project_instance_key"));
        if (pin.projectRegistrationHash != manifest.projectRegistrationHash())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "SessionManifest does not bind the selected "
                    "ProjectRegistration: manifest names {}, pin selected {}",
                    manifest.projectRegistrationHash().hex(),
                    pin.projectRegistrationHash.hex()
                )
            );
        }
        if (agentProfile.has_value() != controllerProfile(pin.kind).budgetsRequired)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Session controller kind and AgentProfile presence disagree"
            );
        }
        if (
            agentProfile.has_value()
            && agentProfile->sessionManifestHash() != manifest.hash()
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "AgentProfile was verified against a different SessionManifest"
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
            // No row means no ProjectInstance exists for this exact pair --
            // there is no "actual" registration hash to print beside it. The
            // table's natural key also needs plugin_id, which this pin does
            // not carry, so which registration (if any) the instance key
            // really is pinned to cannot be named here without a further
            // lookup this refusal does not owe. What it does already hold is
            // the pair it searched for.
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "Session requires an existing ProjectInstance pinned to "
                    "project_registration_hash {} at project_instance_key {}",
                    pin.projectRegistrationHash.hex(),
                    pin.projectInstanceKey
                )
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
                "controlled_target_id, project_instance_key, mode, controller_kind, "
                "active) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, 1)"
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
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 10, pin.controlledTargetId));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 11, pin.projectInstanceKey));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            12,
            sessionModeWireName(pin.mode)
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            13,
            controllerKindWireName(pin.kind)
        ));
        UF_TRY(expectDone(m_impl->database.get(), insert.get()));

        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                "SELECT authenticated_controller_id, idempotency_namespace, manifest_hash, "
                "runtime_artifact_root_hash, installed_generation, "
                "project_registration_hash, capability_profile_hash, session_epoch, "
                "controlled_target_id, project_instance_key, mode, controller_kind, "
                "active "
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
            && columnText(query.get(), 8) == pin.controlledTargetId
            && columnText(query.get(), 9) == pin.projectInstanceKey
            && columnText(query.get(), 10) == sessionModeWireName(pin.mode)
            && columnText(query.get(), 11) == controllerKindWireName(pin.kind)
            && sqlite3_column_int(query.get(), 12) == 1;
        if (!matches)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "session_id already names a different immutable session tuple"
            );
        }

        if (agentProfile.has_value())
        {
            auto const budget = agentProfile->budget();
            UF_TRY_VALUE(now, steadyMillisecondsNow());
            constexpr auto ceiling = static_cast<uint64>(
                std::numeric_limits<sqlite3_int64>::max()
            );
            if (budget.maximumElapsedMillis > ceiling - now)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Agent time budget exhausted SQLite's integer range"
                );
            }

            // OR IGNORE, so that re-pinning an existing session leaves its
            // budget exactly as spent. A conflict rule that replaced the row
            // would make pinning again the one way to refresh a spent budget,
            // and an exhausted Agent would only have to ask for its own session
            // twice.
            UF_TRY_VALUE(
                budgetInsert,
                prepare(
                    m_impl->database.get(),
                    "INSERT OR IGNORE INTO agent_budgets(session_id, "
                    "deadline_steady_millis, remaining_tool_calls, "
                    "remaining_mutations, remaining_observations, "
                    "remaining_risk_units, last_state_fingerprint, "
                    "last_command_fingerprint, consecutive_no_progress_steps) "
                    "VALUES(?1, ?2, ?3, ?4, ?5, ?6, '', '', 0)"
                )
            );
            UF_TRY(bindText(m_impl->database.get(), budgetInsert.get(), 1, pin.sessionId));
            UF_TRY(bindInteger(
                m_impl->database.get(),
                budgetInsert.get(),
                2,
                now + budget.maximumElapsedMillis
            ));
            UF_TRY(bindInteger(
                m_impl->database.get(),
                budgetInsert.get(),
                3,
                budget.maximumToolCalls
            ));
            UF_TRY(bindInteger(
                m_impl->database.get(),
                budgetInsert.get(),
                4,
                budget.maximumMutations
            ));
            UF_TRY(bindInteger(
                m_impl->database.get(),
                budgetInsert.get(),
                5,
                budget.maximumObservations
            ));
            UF_TRY(bindInteger(
                m_impl->database.get(),
                budgetInsert.get(),
                6,
                budget.maximumRiskUnits
            ));
            UF_TRY(expectDone(m_impl->database.get(), budgetInsert.get()));
        }
        return transaction.commit();
    }

    auto OperatorCoordinator::bindController(
        std::string const& sessionId
    ) -> Result<ControllerBinding>
    {
        UF_TRY(requireName(sessionId, "session_id"));
        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                "SELECT authenticated_controller_id, controlled_target_id, "
                "capability_profile_hash, session_epoch, controller_kind "
                "FROM sessions WHERE session_id=?1 AND active=1 AND session_epoch=?2"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), query.get(), 1, sessionId));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            query.get(),
            2,
            m_impl->sessionEpoch
        ));
        if (sqlite3_step(query.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Cannot bind a controller to an unknown current-epoch session"
            );
        }
        UF_TRY_VALUE(capabilityHash, parseHashColumn(columnText(query.get(), 2)));
        UF_TRY_VALUE(kind, parseControllerKind(columnText(query.get(), 4)));
        return ControllerBinding{
            sessionId,
            columnText(query.get(), 0),
            columnText(query.get(), 1),
            capabilityHash,
            static_cast<uint64>(sqlite3_column_int64(query.get(), 3)),
            kind,
        };
    }

    auto OperatorCoordinator::acquireLease(
        ControllerBinding const& controller
    ) -> Result<ControlLease>
    {
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            mode,
            requireLiveBinding(m_impl->database.get(), controller)
        );
        if (mode != SessionMode::Write)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Cannot acquire a lease for a read-mode session"
            );
        }
        auto const& sessionId = controller.sessionId();
        auto const& target = controller.controlledTargetId();
        auto const& controllerId = controller.controllerId();
        auto const capabilityProfileHash = controller.capabilityProfileHash().hex();
        auto const capabilityHash = controller.capabilityProfileHash();
        auto const sessionEpoch = controller.sessionEpoch();

        UF_TRY_VALUE(
            activeQuery,
            prepare(
                m_impl->database.get(),
                "SELECT 1 FROM control_leases WHERE controlled_target_id=?1"
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
                "SELECT fencing_token FROM fencing_high_water WHERE controlled_target_id=?1"
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
                "INSERT INTO fencing_high_water(controlled_target_id, fencing_token) "
                "VALUES(?1, ?2) ON CONFLICT(controlled_target_id) DO UPDATE SET "
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
                "INSERT INTO control_leases(controlled_target_id, lease_id, session_id, "
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
                "INSERT INTO control_transitions(controlled_target_id, session_id, controller_id, "
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

        UF_TRY(appendLedgerEvent(
            m_impl->database.get(),
            sessionEpoch,
            target,
            LedgerEventKind::ControlTransitioned,
            leaseId
        ));

        UF_TRY(transaction.commit());
        return ControlLease{
            .leaseId               = std::move(leaseId),
            .sessionId             = sessionId,
            .controlledTargetId    = target,
            .controllerId          = controllerId,
            .sessionEpoch          = sessionEpoch,
            .fencingToken          = nextFence,
            .revision              = nextFence,
            .capabilityProfileHash = capabilityHash,
        };
    }

    auto controlFence(ControlLease const& lease) -> task::ControlFence
    {
        return task::ControlFence{
            .controlledTargetId = lease.controlledTargetId,
            .sessionEpoch       = lease.sessionEpoch,
            .fencingToken       = lease.fencingToken,
        };
    }

    auto OperatorCoordinator::takeoverLease(
        ControllerBinding const& controller,
        std::string const& reason
    ) -> Result<ControlTakeover>
    {
        UF_TRY(requireName(reason, "takeover reason"));
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            mode,
            requireLiveBinding(m_impl->database.get(), controller)
        );
        if (mode != SessionMode::Write)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Cannot take over control with a read-mode session"
            );
        }
        auto const& sessionId = controller.sessionId();
        auto const& target = controller.controlledTargetId();
        auto const& controllerId = controller.controllerId();
        auto const capabilityProfileHash = controller.capabilityProfileHash().hex();
        auto const capabilityHash = controller.capabilityProfileHash();
        auto const sessionEpoch = controller.sessionEpoch();

        auto previousFence = uint64{0};
        UF_TRY_VALUE(
            highWaterQuery,
            prepare(
                m_impl->database.get(),
                "SELECT fencing_token FROM fencing_high_water WHERE controlled_target_id=?1"
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
                "INSERT INTO fencing_high_water(controlled_target_id, fencing_token) "
                "VALUES(?1, ?2) ON CONFLICT(controlled_target_id) DO UPDATE SET "
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
                "INSERT INTO control_leases(controlled_target_id, lease_id, session_id, "
                "controller_id, session_epoch, fencing_token, revision, "
                "capability_profile_hash) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?6, ?7) "
                "ON CONFLICT(controlled_target_id) DO UPDATE SET lease_id=excluded.lease_id, "
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
                "INSERT INTO control_transitions(controlled_target_id, session_id, controller_id, "
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

        // Inside the same transaction as the fence bump, so there is no instant
        // at which the displaced controller has lost the lease and its dispatch
        // is still unanswered. It cannot un-click what may already have landed;
        // what it prevents is the ledger ever claiming the effect did not
        // happen, because transport_unknown is not not_delivered.
        UF_TRY_VALUE(
            resolved,
            resolveUnansweredDispatches(
                m_impl->database.get(),
                target,
                "a human takeover found this dispatch unanswered"
            )
        );

        UF_TRY(appendLedgerEvent(
            m_impl->database.get(),
            sessionEpoch,
            target,
            LedgerEventKind::ControlTransitioned,
            leaseId
        ));

        UF_TRY(transaction.commit());
        return ControlTakeover{
            .lease = ControlLease{
                .leaseId               = std::move(leaseId),
                .sessionId             = sessionId,
                .controlledTargetId    = target,
                .controllerId          = controllerId,
                .sessionEpoch          = sessionEpoch,
                .fencingToken          = nextFence,
                .revision              = nextFence,
                .capabilityProfileHash = capabilityHash,
            },
            .resolvedDispatches = resolved,
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
                "SELECT 1 FROM control_leases WHERE controlled_target_id=?1 "
                "AND lease_id=?2 AND session_id=?3 AND controller_id=?4 "
                "AND session_epoch=?5 AND fencing_token=?6 AND revision=?7 "
                "AND capability_profile_hash=?8"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), leaseQuery.get(), 1, lease.controlledTargetId));
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
                "WHERE controlled_target_id=?2 AND fencing_token=?3"
            )
        );
        UF_TRY(bindInteger(m_impl->database.get(), highWaterUpdate.get(), 1, nextFence));
        UF_TRY(bindText(
            m_impl->database.get(),
            highWaterUpdate.get(),
            2,
            lease.controlledTargetId
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
                "DELETE FROM control_leases WHERE controlled_target_id=?1 AND lease_id=?2"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            leaseDelete.get(),
            1,
            lease.controlledTargetId
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
                "INSERT INTO control_transitions(controlled_target_id, session_id, controller_id, "
                "lease_id, session_epoch, fencing_token, transition, reason) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, 'release', 'explicit release')"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            transitionWrite.get(),
            1,
            lease.controlledTargetId
        ));
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 2, lease.sessionId));
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 3, lease.controllerId));
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 4, lease.leaseId));
        UF_TRY(bindInteger(m_impl->database.get(), transitionWrite.get(), 5, lease.sessionEpoch));
        UF_TRY(bindInteger(m_impl->database.get(), transitionWrite.get(), 6, nextFence));
        UF_TRY(expectDone(m_impl->database.get(), transitionWrite.get()));

        UF_TRY(appendLedgerEvent(
            m_impl->database.get(),
            lease.sessionEpoch,
            lease.controlledTargetId,
            LedgerEventKind::ControlTransitioned,
            lease.leaseId
        ));

        UF_TRY(transaction.commit());
        return nextFence;
    }

    auto OperatorCoordinator::createSnapshot(
        ControlLease const& lease,
        ProjectPluginHandle const& plugin,
        task::UiObservationSnapshot const& observation
    ) -> Result<SnapshotRecord>
    {
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            leaseQuery,
            prepare(
                m_impl->database.get(),
                "SELECT lease_id, session_id, controller_id, session_epoch, fencing_token, "
                "revision, capability_profile_hash FROM control_leases "
                "WHERE controlled_target_id=?1"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            leaseQuery.get(),
            1,
            lease.controlledTargetId
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

        UF_TRY_VALUE(
            sessionQuery,
            prepare(
                m_impl->database.get(),
                "SELECT session.project_instance_key, session.manifest_hash, "
                "session.controlled_target_id, session.runtime_artifact_root_hash, "
                "registration.plugin_id, registration.plugin_hash, "
                "session.project_registration_hash, session.controller_kind "
                "FROM sessions session JOIN project_registrations registration "
                "ON registration.registration_hash=session.project_registration_hash "
                "WHERE session.session_id=?1 AND session.active=1 "
                "AND session.session_epoch=?2"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), sessionQuery.get(), 1, lease.sessionId));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            sessionQuery.get(),
            2,
            m_impl->sessionEpoch
        ));
        if (sqlite3_step(sessionQuery.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Snapshot session is not active in this epoch"
            );
        }
        auto const projectInstanceKey  = columnText(sessionQuery.get(), 0);
        auto const sessionManifestHex  = columnText(sessionQuery.get(), 1);
        auto const controlledTargetId = columnText(sessionQuery.get(), 2);
        auto const sessionArtifactRoot = columnText(sessionQuery.get(), 3);
        auto const pluginId            = columnText(sessionQuery.get(), 4);

        // The same three-way check commitReconciliation makes: a handle for
        // another registration is a different project reading this world.
        if (
            plugin.pluginId() != pluginId
            || plugin.pluginHash().hex() != columnText(sessionQuery.get(), 5)
            || plugin.projectRegistrationHash().hex() != columnText(sessionQuery.get(), 6)
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Snapshot ProjectPlugin does not match the pinned session registration"
            );
        }

        // Without this a snapshot can be composed from a UI observation taken
        // through a RuntimeArtifact the session never pinned, and
        // session_manifest_hash would attest to a model that produced none of
        // the evidence.
        if (observation.artifactRootHash().hex() != sessionArtifactRoot)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "UI observation was taken through a RuntimeArtifact this session "
                "did not pin"
            );
        }

        // Charged here rather than after the composition, and inside the same
        // BEGIN IMMEDIATE: a refused observation budget must not first pay for
        // a plugin derive under the write lock. There is no ControllerBinding
        // parameter and none is wanted -- the lease already names the session
        // this observation is charged to, and a binding beside it would be a
        // second spelling of one identity that would then have to be kept
        // equal. There is no caller-supplied instant either, for the reason
        // steadyMillisecondsNow states.
        UF_TRY_VALUE(snapshotKind, parseControllerKind(columnText(sessionQuery.get(), 7)));
        UF_TRY_VALUE(
            snapshotBudget,
            readAgentBudget(m_impl->database.get(), lease.sessionId, snapshotKind)
        );
        if (snapshotBudget)
        {
            UF_TRY(requireWithinAgentDeadline(*snapshotBudget));
            UF_TRY(chargeAgentBudget(
                m_impl->database.get(),
                lease.sessionId,
                "UPDATE agent_budgets SET remaining_observations = "
                "remaining_observations - ?2 WHERE session_id=?1",
                1U,
                "Agent observation budget is exhausted"
            ));
        }

        UF_TRY_VALUE(
            stateQuery,
            prepare(
                m_impl->database.get(),
                "SELECT revision, state_hash, canonical_opaque_payload, project_registration_hash "
                "FROM project_state WHERE plugin_id=?1 AND project_instance_key=?2"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), stateQuery.get(), 1, pluginId));
        UF_TRY(bindText(m_impl->database.get(), stateQuery.get(), 2, projectInstanceKey));
        if (sqlite3_step(stateQuery.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "Snapshot requires an existing provisioned ProjectState baseline"
            );
        }
        auto const projectStateRevision = static_cast<uint64>(
            sqlite3_column_int64(stateQuery.get(), 0)
        );
        auto const projectStateHex = columnText(stateQuery.get(), 1);
        auto const projectStateJcs = columnText(stateQuery.get(), 2);

        UF_TRY_VALUE(
            priorQuery,
            prepare(
                m_impl->database.get(),
                "SELECT revision, canonical_observation, observation_hash, "
                "state_resolution_hash, project_state_revision, project_state_hash, "
                "plugin_hash, project_registration_hash FROM project_observations "
                "WHERE plugin_id=?1 AND project_instance_key=?2 "
                "ORDER BY revision DESC LIMIT 1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), priorQuery.get(), 1, pluginId));
        UF_TRY(bindText(m_impl->database.get(), priorQuery.get(), 2, projectInstanceKey));
        auto priorRevision      = uint64{};
        auto priorObservation   = std::string{"null"};
        auto priorObservationId = std::string{};
        auto priorFingerprint   = std::string{};
        if (sqlite3_step(priorQuery.get()) == SQLITE_ROW)
        {
            priorRevision      = static_cast<uint64>(sqlite3_column_int64(priorQuery.get(), 0));
            priorObservation   = columnText(priorQuery.get(), 1);
            priorObservationId = columnText(priorQuery.get(), 2);
            priorFingerprint   = columnText(priorQuery.get(), 3);
            priorFingerprint += '\0';
            priorFingerprint += std::to_string(
                static_cast<uint64>(sqlite3_column_int64(priorQuery.get(), 4))
            );
            priorFingerprint += '\0';
            priorFingerprint += columnText(priorQuery.get(), 5);
            priorFingerprint += '\0';
            priorFingerprint += columnText(priorQuery.get(), 6);
            priorFingerprint += '\0';
            priorFingerprint += columnText(priorQuery.get(), 7);
            priorFingerprint += '\0';
            priorFingerprint += priorObservationId;
        }

        // The design's pending_operation_transition: a read-only summary of the
        // one non-terminal Operation this instance may have, so the project can
        // read its own world without the Operator interpreting it.
        UF_TRY_VALUE(
            pendingQuery,
            prepare(
                m_impl->database.get(),
                "SELECT operation_id, state, revision FROM operations "
                "WHERE plugin_id=?1 AND project_instance_key=?2 AND state IN "
                "('proposed', 'awaiting_approval', 'ready', 'needs_revalidation', "
                "'running', 'reconciling', 'ambiguous') "
                "ORDER BY operation_id LIMIT 1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), pendingQuery.get(), 1, pluginId));
        UF_TRY(bindText(m_impl->database.get(), pendingQuery.get(), 2, projectInstanceKey));
        auto pendingOperationJcs = std::string{"null"};
        if (sqlite3_step(pendingQuery.get()) == SQLITE_ROW)
        {
            pendingOperationJcs = "{\"operation_id\":";
            appendJsonString(pendingOperationJcs, columnText(pendingQuery.get(), 0));
            pendingOperationJcs += ",\"revision\":";
            pendingOperationJcs += std::to_string(
                static_cast<uint64>(sqlite3_column_int64(pendingQuery.get(), 2))
            );
            pendingOperationJcs += ",\"state\":";
            appendJsonString(pendingOperationJcs, columnText(pendingQuery.get(), 1));
            pendingOperationJcs.push_back('}');
        }

        auto const pinnedRoots = plugin.projectArtifactRootHashes();

        // The plugin runs inside the transaction, as reduce already does: the
        // read of its inputs and the derivation from them must be one BEGIN
        // IMMEDIATE, or a concurrent writer moves the state between the two.
        // The plugin VM is quota-bound, so holding the write lock across it is
        // bounded.
        UF_TRY_VALUE(
            deriveInput,
            plugin.canonicalize(deriveEnvelopeJcs(DeriveEnvelopeInputs{
                .pendingOperationJcs = pendingOperationJcs,
                .pinnedArtifactRoots = pinnedRoots,
                .priorObservationJcs = priorObservation,
                .projectStateJcs     = projectStateJcs,
                .uiSnapshotJcs       = observation.canonicalJcs(),
            }))
        );
        UF_TRY_VALUE(derived, plugin.derive(deriveInput));
        if (
            derived.projectRegistrationHash() != plugin.projectRegistrationHash()
            || derived.function() != ProjectPluginFunction::Derive
            || derived.direction() != ProjectDocumentDirection::Output
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Derived ProjectObservation does not match the pinned observation schema"
            );
        }

        auto const observationHex        = derived.contentHash().hex();
        auto const stateResolutionHex    = observation.stateResolutionHash().hex();
        auto const observationSchemaHex  = plugin.projectObservationSchemaHash().hex();
        auto currentFingerprint          = stateResolutionHex;
        currentFingerprint += '\0';
        currentFingerprint += std::to_string(projectStateRevision);
        currentFingerprint += '\0';
        currentFingerprint += projectStateHex;
        currentFingerprint += '\0';
        currentFingerprint += plugin.pluginHash().hex();
        currentFingerprint += '\0';
        currentFingerprint += plugin.projectRegistrationHash().hex();
        currentFingerprint += '\0';
        currentFingerprint += observationHex;

        // "任一 UI/artifact/plugin/project-state 输入变化" made executable: an
        // identical reading of an identical world keeps its revision, so a
        // re-observation does not invent a new state kind revision and does not
        // move the snapshot identity.
        auto observationRevision = priorRevision;
        if (priorRevision == 0U || priorFingerprint != currentFingerprint)
        {
            UF_TRY_VALUE(
                nextRevision,
                checkedSqlIncrement(priorRevision, "ProjectObservation revision")
            );
            observationRevision = nextRevision;
            UF_TRY_VALUE(
                observationInsert,
                prepare(
                    m_impl->database.get(),
                    "INSERT INTO project_observations(plugin_id, project_instance_key, "
                    "revision, project_registration_hash, plugin_hash, "
                    "observation_schema_hash, state_resolution_hash, "
                    "project_state_revision, project_state_hash, canonical_observation, "
                    "observation_hash) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)"
                )
            );
            UF_TRY(bindText(m_impl->database.get(), observationInsert.get(), 1, pluginId));
            UF_TRY(bindText(
                m_impl->database.get(),
                observationInsert.get(),
                2,
                projectInstanceKey
            ));
            UF_TRY(bindInteger(
                m_impl->database.get(),
                observationInsert.get(),
                3,
                observationRevision
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                observationInsert.get(),
                4,
                plugin.projectRegistrationHash().hex()
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                observationInsert.get(),
                5,
                plugin.pluginHash().hex()
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                observationInsert.get(),
                6,
                observationSchemaHex
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                observationInsert.get(),
                7,
                stateResolutionHex
            ));
            UF_TRY(bindInteger(
                m_impl->database.get(),
                observationInsert.get(),
                8,
                projectStateRevision
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                observationInsert.get(),
                9,
                projectStateHex
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                observationInsert.get(),
                10,
                derived.bytes()
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                observationInsert.get(),
                11,
                observationHex
            ));
            UF_TRY(expectDone(m_impl->database.get(), observationInsert.get()));
        }

        UF_TRY_VALUE(projectStateHash, parseHashColumn(projectStateHex));
        UF_TRY_VALUE(sessionManifestHash, parseHashColumn(sessionManifestHex));

        // ProjectObservation is minted here, in the member function body: its
        // single friend is OperatorCoordinator, which reaches its member
        // functions and neither Impl nor the file-local helpers above.
        auto projectObservation = ProjectObservation{
            plugin.projectRegistrationHash(),
            plugin.pluginHash(),
            projectInstanceKey,
            observation.stateResolutionHash(),
            projectStateRevision,
            projectStateHash,
            observationRevision,
            derived,
        };

        UF_TRY_VALUE(
            decisionBasisHash,
            deriveDecisionBasis(DecisionBasisParts{
                .projectObservationHash = projectObservation.hash(),
                .projectStateHash       = projectStateHash,
                .sessionManifestHash    = sessionManifestHash,
                .stateResolutionHash    = observation.stateResolutionHash(),
            })
        );

        // Operator-owned, monotonic, and moved by acquire/takeover/release,
        // which is three of the four triggers the design lists for ControlState.
        // Policy is the fourth and has no store yet.
        UF_TRY_VALUE(
            availabilityQuery,
            prepare(
                m_impl->database.get(),
                "SELECT COALESCE(MAX(sequence), 0) FROM control_transitions "
                "WHERE controlled_target_id=?1"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            availabilityQuery.get(),
            1,
            controlledTargetId
        ));
        if (sqlite3_step(availabilityQuery.get()) != SQLITE_ROW)
        {
            return databaseFailure(
                m_impl->database.get(),
                "could not read the control availability revision"
            );
        }
        auto const availabilityRevision = static_cast<uint64>(
            sqlite3_column_int64(availabilityQuery.get(), 0)
        );

        UF_TRY_VALUE(
            revisionQuery,
            prepare(
                m_impl->database.get(),
                "SELECT COALESCE(MAX(snapshot_revision), 0) FROM snapshots "
                "WHERE session_id=?1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), revisionQuery.get(), 1, lease.sessionId));
        if (sqlite3_step(revisionQuery.get()) != SQLITE_ROW)
        {
            return databaseFailure(
                m_impl->database.get(),
                "could not read the session snapshot revision"
            );
        }
        UF_TRY_VALUE(
            snapshotRevision,
            checkedSqlIncrement(
                static_cast<uint64>(sqlite3_column_int64(revisionQuery.get(), 0)),
                "snapshot revision"
            )
        );

        auto const canonicalParts = snapshotPartsJcs(SnapshotPartsInputs{
            .decisionBasisHash          = decisionBasisHash,
            .projectObservationHash     = projectObservation.hash(),
            .projectStateHash           = projectStateHash,
            .sessionManifestHash        = sessionManifestHash,
            .stateResolutionHash        = observation.stateResolutionHash(),
            .controlledTargetId         = controlledTargetId,
            .leaseId                    = lease.leaseId,
            .observationId              = observation.observationId(),
            .projectInstanceKey         = projectInstanceKey,
            .availabilityRevision       = availabilityRevision,
            .fencingToken               = lease.fencingToken,
            .projectObservationRevision = observationRevision,
            .projectStateRevision       = projectStateRevision,
            .sessionEpoch               = lease.sessionEpoch,
            .targetGeneration           = observation.targetGeneration().value(),
        });
        UF_TRY_VALUE(
            identityHash,
            sha256(std::as_bytes(std::span{canonicalParts}))
        );

        UF_TRY_VALUE(token, randomToken(m_impl->database.get()));
        UF_TRY_VALUE(
            insert,
            prepare(
                m_impl->database.get(),
                "INSERT INTO snapshots(token, session_id, snapshot_revision, session_epoch, "
                "identity_hash, decision_basis_hash, canonical_parts, lease_revision, "
                "plugin_id, project_instance_key, observation_id, target_generation, "
                "state_resolution_hash, project_observation_revision, "
                "project_state_revision, availability_revision) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 1, token));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 2, lease.sessionId));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 3, snapshotRevision));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 4, lease.sessionEpoch));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 5, identityHash.hex()));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 6, decisionBasisHash.hex()));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 7, canonicalParts));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 8, lease.revision));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 9, pluginId));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 10, projectInstanceKey));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            11,
            observation.observationId()
        ));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            insert.get(),
            12,
            observation.targetGeneration().value()
        ));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 13, stateResolutionHex));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 14, observationRevision));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 15, projectStateRevision));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 16, availabilityRevision));
        UF_TRY(expectDone(m_impl->database.get(), insert.get()));

        // The join point, read inside the transaction that published this
        // record. Nothing can commit between the composition and this read, so
        // a controller subscribing from here is handed exactly what happened
        // after the world it is looking at.
        UF_TRY_VALUE(eventCursor, currentEventCursor(m_impl->database.get()));
        UF_TRY(transaction.commit());
        return SnapshotRecord{
            .token                = std::move(token),
            .sessionId            = lease.sessionId,
            .identityHash         = identityHash,
            .decisionBasisHash    = decisionBasisHash,
            .stateResolutionHash  = observation.stateResolutionHash(),
            .projectStateHash     = projectStateHash,
            .canonicalParts       = canonicalParts,
            .sessionEpoch         = lease.sessionEpoch,
            .leaseRevision        = lease.revision,
            .snapshotRevision     = snapshotRevision,
            .projectStateRevision = projectStateRevision,
            .availabilityRevision = availabilityRevision,
            .observation          = std::move(projectObservation),
            .eventCursor          = SubscriptionCursor{eventCursor},
        };
    }

    auto OperatorCoordinator::submitCommand(
        ControllerBinding const& controller,
        CommandRequest const& request,
        ValidatedToolInvocation const& invocation
    ) -> Result<AcceptedCommand>
    {
        UF_TRY(requireName(request.snapshotToken, "snapshot_token"));
        UF_TRY(requireName(request.idempotencyNamespace, "idempotency_namespace"));
        UF_TRY(requireName(request.clientRequestId, "client_request_id"));

        // The accept side of p03, evaluated again over the same predicate the
        // offer side used. Both halves are required: the offer keeps an online
        // Agent from ever holding a Privileged invocation, and this keeps it
        // from presenting one another controller was offered.
        if (!toolSurfaceAllowed(controller.profile(), invocation.surface()))
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Controller restricted to semantic tools submitted a privileged tool"
            );
        }

        auto const& toolName = invocation.toolName();
        auto const& toolVersion = invocation.toolVersion();
        auto const& canonicalArgs = invocation.canonicalArgs().bytes();
        auto const mutating = invocation.mutability() == ToolMutability::Mutating;

        // The fingerprint covers exactly what the catalog decided the command
        // is. Mutability and surface are absent on purpose: they are functions
        // of the tool and its version, so including them would let two
        // fingerprints disagree about one tool without any of the fingerprinted
        // bytes differing. Who submitted it is absent for the same reason and
        // one more: Script, Agent and Human naming the identical tool and
        // arguments are naming one command, and a fingerprint that separated
        // them would make the shared Operation path unprovable.
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
        UF_TRY(requireLiveBinding(m_impl->database.get(), controller));

        // Read before the idempotency lookup, so that an expired Agent is
        // refused whichever branch its request would have taken. A replay of a
        // request the ledger already accepted charges nothing further: the
        // counters record what was accepted, and it was charged when it was.
        UF_TRY_VALUE(
            budget,
            readAgentBudget(
                m_impl->database.get(),
                controller.sessionId(),
                controller.kind()
            )
        );
        if (budget)
        {
            UF_TRY(requireWithinAgentDeadline(*budget));
        }

        UF_TRY_VALUE(
            sessionQuery,
            prepare(
                m_impl->database.get(),
                "SELECT session.controlled_target_id, session.project_instance_key, "
                "registration.plugin_id, session.idempotency_namespace, "
                "session.project_registration_hash "
                "FROM sessions session JOIN project_registrations "
                "registration ON registration.registration_hash="
                "session.project_registration_hash "
                "WHERE session.session_id=?1 AND session.active=1"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            sessionQuery.get(),
            1,
            controller.sessionId()
        ));
        if (sqlite3_step(sessionQuery.get()) != SQLITE_ROW)
        {
            return fail(AutomationErrorKind::ActionRejected, "Unknown authenticated session");
        }
        auto const controlledTargetId = columnText(sessionQuery.get(), 0);
        auto const projectInstanceKey = columnText(sessionQuery.get(), 1);
        auto const pluginId = columnText(sessionQuery.get(), 2);
        auto const idempotencyNamespace = columnText(sessionQuery.get(), 3);
        auto const sessionEpoch = controller.sessionEpoch();
        if (request.idempotencyNamespace != idempotencyNamespace)
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
        if (invocation.projectRegistrationHash().hex() != columnText(sessionQuery.get(), 4))
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
            if (columnText(existingQuery.get(), 10) != controller.sessionId())
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
            return AcceptedCommand{
                .operation = StoredOperation{
                    .operationId   = std::move(operationId),
                    .lookup        = CommandLookup::Existing,
                    .state         = state,
                    .revision      = revision,
                    .planFrozen    = frozen,
                    .hasDispatched = dispatched,
                },
                .commandFingerprint = commandFingerprint,
            };
        }

        UF_TRY_VALUE(
            snapshotQuery,
            prepare(
                m_impl->database.get(),
                // The last two clauses are what make the token a reference to a
                // COMPOSITION rather than to a lease: the snapshot goes stale
                // when ProjectState moves under it, not only when control
                // does. obs.project_state_revision=state.revision is not
                // redundant with the clause above it -- it refuses a snapshot
                // whose observation revision was reused from an earlier
                // ProjectState, which is the only way the reuse rule in
                // createSnapshot could otherwise carry a stale reading forward.
                //
                // The NOT EXISTS clause is what makes out-of-band human input
                // stop the automation: a finding records the snapshot revision
                // its target had reached, and every token at or below that
                // revision is refused afterwards. The controller has to look
                // again before it acts, which is the whole effect a finding is
                // allowed to have.
                "SELECT session.controlled_target_id, s.decision_basis_hash "
                "FROM snapshots s JOIN sessions session "
                "ON session.session_id=s.session_id JOIN control_leases lease "
                "ON lease.controlled_target_id=session.controlled_target_id "
                "JOIN project_state state ON state.plugin_id=s.plugin_id "
                "AND state.project_instance_key=s.project_instance_key "
                "JOIN project_observations obs ON obs.plugin_id=s.plugin_id "
                "AND obs.project_instance_key=s.project_instance_key "
                "AND obs.revision=s.project_observation_revision "
                "WHERE s.token=?1 AND s.session_id=?2 AND s.session_epoch=?3 AND "
                "s.lease_revision=lease.revision AND lease.session_id=s.session_id "
                "AND s.project_state_revision=state.revision "
                "AND obs.project_state_revision=state.revision "
                "AND NOT EXISTS(SELECT 1 FROM external_input_findings finding "
                "WHERE finding.controlled_target_id=session.controlled_target_id "
                "AND finding.session_epoch=s.session_epoch "
                "AND finding.invalidated_snapshot_revision>=s.snapshot_revision)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), snapshotQuery.get(), 1, request.snapshotToken));
        UF_TRY(bindText(
            m_impl->database.get(),
            snapshotQuery.get(),
            2,
            controller.sessionId()
        ));
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
        if (columnText(snapshotQuery.get(), 0) != controlledTargetId)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "Session target changed while validating SnapshotToken"
            );
        }
        auto const stateFingerprint = columnText(snapshotQuery.get(), 1);

        if (mutating)
        {
            UF_TRY_VALUE(
                mutationQuery,
                prepare(
                    m_impl->database.get(),
                    "SELECT operation_id FROM operations WHERE controlled_target_id=?1 "
                    "AND mutating=1 AND state IN ('proposed', 'awaiting_approval', 'ready', "
                    "'needs_revalidation', 'running', 'reconciling', 'ambiguous') LIMIT 1"
                )
            );
            UF_TRY(bindText(
                m_impl->database.get(),
                mutationQuery.get(),
                1,
                controlledTargetId
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

        if (budget)
        {
            // Progress is "the world is different" or "I asked for something
            // different". The state fingerprint IS the snapshot's
            // decision_basis_hash, which the Operator composed; no second
            // composition is defined here. The command fingerprint is the one
            // above, which covers the tool, its version and its canonical
            // arguments and NOT client_request_id -- so resubmitting the
            // identical command under a fresh request id yields the identical
            // fingerprint and correctly buys no progress.
            //
            // Either differing is enough. State alone would punish an Agent
            // legitimately trying three tools against an unchanging screen;
            // command alone would let it observe, observe, observe for ever.
            // The Agent is stuck only when it asks the same thing of the same
            // world.
            auto const progressed = stateFingerprint != budget->lastStateFingerprint
                || commandFingerprint.hex() != budget->lastCommandFingerprint;
            auto const repetitions = progressed
                ? uint64{0}
                : budget->consecutiveNoProgressSteps + 1U;
            if (repetitions > k_agentNoProgressCeiling)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Agent asked the same thing of the same world too many times"
                );
            }

            UF_TRY(chargeAgentBudget(
                m_impl->database.get(),
                controller.sessionId(),
                "UPDATE agent_budgets SET remaining_tool_calls = "
                "remaining_tool_calls - ?2 WHERE session_id=?1",
                1U,
                "Agent tool-call budget is exhausted"
            ));

            // Sourced from the Tool Catalog descriptor, which is what
            // operations.mutating and the mutation chain already run on, and
            // deliberately not from the plan's declared risk: a plugin may
            // under-declare its own effects, so risk cannot stand in for
            // "changes something". Two differently sourced counts over a
            // partly overlapping set are two facts.
            if (mutating)
            {
                UF_TRY(chargeAgentBudget(
                    m_impl->database.get(),
                    controller.sessionId(),
                    "UPDATE agent_budgets SET remaining_mutations = "
                    "remaining_mutations - ?2 WHERE session_id=?1",
                    1U,
                    "Agent mutation budget is exhausted"
                ));
            }

            UF_TRY_VALUE(
                markerUpdate,
                prepare(
                    m_impl->database.get(),
                    "UPDATE agent_budgets SET last_state_fingerprint=?2, "
                    "last_command_fingerprint=?3, consecutive_no_progress_steps=?4 "
                    "WHERE session_id=?1"
                )
            );
            UF_TRY(bindText(
                m_impl->database.get(),
                markerUpdate.get(),
                1,
                controller.sessionId()
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                markerUpdate.get(),
                2,
                stateFingerprint
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                markerUpdate.get(),
                3,
                commandFingerprint.hex()
            ));
            UF_TRY(bindInteger(m_impl->database.get(), markerUpdate.get(), 4, repetitions));
            UF_TRY(expectDone(m_impl->database.get(), markerUpdate.get()));
        }

        UF_TRY_VALUE(operationId, randomToken(m_impl->database.get()));
        UF_TRY_VALUE(
            insert,
            prepare(
                m_impl->database.get(),
                "INSERT INTO operations(operation_id, session_id, snapshot_token, idempotency_namespace, "
                "client_request_id, command_fingerprint, tool_name, tool_version, canonical_args, "
                "controlled_target_id, mutating, state, revision, plugin_id, "
                "project_instance_key) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, "
                "?10, ?11, 'proposed', 1, ?12, ?13)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 1, operationId));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 2, controller.sessionId()));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 3, request.snapshotToken));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 4, request.idempotencyNamespace));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 5, request.clientRequestId));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 6, commandFingerprint.hex()));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 7, toolName));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 8, toolVersion));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 9, canonicalArgs));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 10, controlledTargetId));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 11, mutating ? 1U : 0U));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 12, pluginId));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 13, projectInstanceKey));
        UF_TRY(expectDone(m_impl->database.get(), insert.get()));
        UF_TRY(appendLedgerEvent(
            m_impl->database.get(),
            sessionEpoch,
            controlledTargetId,
            LedgerEventKind::OperationCreated,
            operationId
        ));
        UF_TRY(transaction.commit());
        return AcceptedCommand{
            .operation = StoredOperation{
                .operationId   = std::move(operationId),
                .lookup        = CommandLookup::Created,
                .state         = OperationState::Proposed,
                .revision      = 1U,
                .planFrozen    = false,
                .hasDispatched = false,
            },
            .commandFingerprint = commandFingerprint,
        };
    }

    auto OperatorCoordinator::recordExternalInput(
        ControllerBinding const& reporter,
        ExternalInputReport const& report
    ) -> Result<RecordedExternalInput>
    {
        UF_TRY(requireName(report.reason, "external input reason"));
        if (!reporter.profile().mayReportExternalInput)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "This controller kind may not report external input about a third party"
            );
        }
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY(requireLiveBinding(m_impl->database.get(), reporter));

        auto const& target = reporter.controlledTargetId();
        auto const epoch   = reporter.sessionEpoch();

        // Read before this finding's own append, so a finding never claims to
        // have been detected after itself.
        UF_TRY_VALUE(cursor, currentEventCursor(m_impl->database.get()));

        UF_TRY_VALUE(
            revisionQuery,
            prepare(
                m_impl->database.get(),
                "SELECT COALESCE(MAX(s.snapshot_revision), 0) FROM snapshots s "
                "JOIN sessions session ON session.session_id=s.session_id "
                "WHERE session.controlled_target_id=?1 AND s.session_epoch=?2"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), revisionQuery.get(), 1, target));
        UF_TRY(bindInteger(m_impl->database.get(), revisionQuery.get(), 2, epoch));
        if (sqlite3_step(revisionQuery.get()) != SQLITE_ROW)
        {
            return databaseFailure(
                m_impl->database.get(),
                "could not read the snapshot revision an external input invalidates"
            );
        }
        auto const invalidatedRevision = static_cast<uint64>(
            sqlite3_column_int64(revisionQuery.get(), 0)
        );

        // At most one Operation on a target is non-terminal, so the finding
        // freezes it by name rather than by a sweep. DecisionInputsChanged is
        // the only signal that fits: the inputs a decision was taken on have
        // moved, and nothing about the Operation itself was judged.
        UF_TRY_VALUE(
            pendingQuery,
            prepare(
                m_impl->database.get(),
                "SELECT operation_id, revision, state, frozen_plan_hash, "
                "EXISTS(SELECT 1 FROM dispatches d WHERE "
                "d.operation_id=operations.operation_id) FROM operations "
                "WHERE controlled_target_id=?1 AND state IN "
                "('proposed', 'awaiting_approval', 'ready') LIMIT 1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), pendingQuery.get(), 1, target));
        auto frozenOperation = std::optional<std::string>{};
        if (sqlite3_step(pendingQuery.get()) == SQLITE_ROW)
        {
            auto operationId = columnText(pendingQuery.get(), 0);
            auto const revision = static_cast<uint64>(
                sqlite3_column_int64(pendingQuery.get(), 1)
            );
            UF_TRY_VALUE(state, parseOperationState(columnText(pendingQuery.get(), 2)));
            auto const planFrozen =
                sqlite3_column_type(pendingQuery.get(), 3) != SQLITE_NULL;
            auto const dispatched = sqlite3_column_int(pendingQuery.get(), 4) != 0;
            UF_TRY_VALUE(machine, OperationMachine::restore(state, planFrozen, dispatched));
            UF_TRY_VALUE(
                nextState,
                machine.transition(OperationEvent::DecisionInputsChanged)
            );
            UF_TRY_VALUE(
                nextRevision,
                checkedSqlIncrement(revision, "Operation revision")
            );
            UF_TRY_VALUE(
                update,
                prepare(
                    m_impl->database.get(),
                    "UPDATE operations SET state=?1, revision=?2 "
                    "WHERE operation_id=?3 AND revision=?4"
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
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Operation revision lost its CAS to an external input finding"
                );
            }
            frozenOperation = std::move(operationId);
        }

        UF_TRY_VALUE(findingId, randomToken(m_impl->database.get()));
        UF_TRY_VALUE(
            insert,
            prepare(
                m_impl->database.get(),
                "INSERT INTO external_input_findings(finding_id, controlled_target_id, "
                "session_epoch, reporter_session_id, detected_after_cursor, "
                "invalidated_snapshot_revision, operation_id, required_action, reason) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 1, findingId));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 2, target));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 3, epoch));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            4,
            reporter.sessionId()
        ));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 5, cursor));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 6, invalidatedRevision));
        if (frozenOperation.has_value())
        {
            UF_TRY(bindText(m_impl->database.get(), insert.get(), 7, *frozenOperation));
        }
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            8,
            externalInputActionWireName(report.requiredAction)
        ));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 9, report.reason));
        UF_TRY(expectDone(m_impl->database.get(), insert.get()));

        UF_TRY(appendLedgerEvent(
            m_impl->database.get(),
            epoch,
            target,
            LedgerEventKind::ExternalInputDetected,
            findingId
        ));

        // Read back inside the transaction rather than returning the locals
        // that were bound. A caller who is told what the Operator computed
        // learns nothing about what the Operator stored, and a finding whose
        // stored cursor disagreed with the reported one would be invisible --
        // the audit reads the row, not the return value.
        UF_TRY_VALUE(
            stored,
            prepare(
                m_impl->database.get(),
                "SELECT detected_after_cursor, invalidated_snapshot_revision, "
                "operation_id FROM external_input_findings WHERE finding_id=?1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), stored.get(), 1, findingId));
        if (sqlite3_step(stored.get()) != SQLITE_ROW)
        {
            return databaseFailure(
                m_impl->database.get(),
                "could not read back the recorded external input finding"
            );
        }
        auto const storedCursor = static_cast<uint64>(
            sqlite3_column_int64(stored.get(), 0)
        );
        auto const storedRevision = static_cast<uint64>(
            sqlite3_column_int64(stored.get(), 1)
        );
        auto storedOperation = std::optional<std::string>{};
        if (sqlite3_column_type(stored.get(), 2) != SQLITE_NULL)
        {
            storedOperation = columnText(stored.get(), 2);
        }
        UF_TRY(transaction.commit());
        return RecordedExternalInput{
            .findingId                   = std::move(findingId),
            .detectedAfterCursor         = storedCursor,
            .invalidatedSnapshotRevision = storedRevision,
            .operationId                 = std::move(storedOperation),
        };
    }

    auto OperatorCoordinator::subscribe(
        ControllerBinding const& controller,
        SubscriptionCursor after,
        uint32 maximumEvents
    ) -> Result<SubscriptionRead>
    {
        if (maximumEvents == 0U)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "A subscription read must ask for at least one event"
            );
        }

        // One transaction for the whole read, so the head, the oldest available
        // sequence and the rows are one consistent view. It is BEGIN IMMEDIATE
        // like every other path here rather than a lighter read transaction,
        // because one writer at a time is what makes the sequence commit-ordered
        // and a second kind of transaction would be a second set of rules.
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY(requireLiveBinding(m_impl->database.get(), controller));
        UF_TRY_VALUE(currentCursor, currentEventCursor(m_impl->database.get()));

        // Derived rather than assumed. Nothing prunes ledger_events, so this is
        // 0 whenever the table is non-empty and the head when it is empty; the
        // day a retention pass lands, this is already the value it moves.
        UF_TRY_VALUE(
            oldestQuery,
            prepare(
                m_impl->database.get(),
                "SELECT COALESCE(MIN(sequence) - 1, ?1) FROM ledger_events"
            )
        );
        UF_TRY(bindInteger(m_impl->database.get(), oldestQuery.get(), 1, currentCursor));
        if (sqlite3_step(oldestQuery.get()) != SQLITE_ROW)
        {
            return databaseFailure(
                m_impl->database.get(),
                "could not read the oldest available cursor"
            );
        }
        auto const oldestCursor = static_cast<uint64>(
            sqlite3_column_int64(oldestQuery.get(), 0)
        );

        // A cursor past the head is a cursor from another database or another
        // epoch. It is refused rather than answered with an empty batch,
        // because "nothing has happened yet" and "you are reading a stream that
        // is not this one" are different answers and only one of them means
        // keep waiting.
        if (after.value > currentCursor)
        {
            UF_TRY(transaction.commit());
            return SubscriptionRead{ResyncRequired{
                .requestedCursor       = after,
                .oldestAvailableCursor = SubscriptionCursor{oldestCursor},
                .currentCursor         = SubscriptionCursor{currentCursor},
            }};
        }

        // Scoped to the controlled target and not to the binding's own session:
        // a controller that could see only its own events could not notice that
        // a human took control away from it, which is the one thing it most
        // needs to notice.
        UF_TRY_VALUE(
            events,
            prepare(
                m_impl->database.get(),
                "SELECT sequence, kind, controlled_target_id, subject_id "
                "FROM ledger_events WHERE controlled_target_id=?1 AND sequence>?2 "
                "ORDER BY sequence LIMIT ?3"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            events.get(),
            1,
            controller.controlledTargetId()
        ));
        UF_TRY(bindInteger(m_impl->database.get(), events.get(), 2, after.value));
        UF_TRY(bindInteger(m_impl->database.get(), events.get(), 3, maximumEvents));

        auto batch = SubscriptionBatch{.events = {}, .nextCursor = after};
        auto step  = sqlite3_step(events.get());
        while (step == SQLITE_ROW)
        {
            UF_TRY_VALUE(kind, parseLedgerEventKind(columnText(events.get(), 1)));
            auto const sequence = static_cast<uint64>(
                sqlite3_column_int64(events.get(), 0)
            );
            batch.events.push_back(LedgerEvent{
                .sequence           = SubscriptionCursor{sequence},
                .kind               = kind,
                .controlledTargetId = columnText(events.get(), 2),
                .subjectId          = columnText(events.get(), 3),
            });

            // The cursor follows what was delivered, never the head: a batch cut
            // short by maximumEvents whose cursor named the head would silently
            // skip every event the cut left behind.
            batch.nextCursor = SubscriptionCursor{sequence};
            step             = sqlite3_step(events.get());
        }
        if (step != SQLITE_DONE)
        {
            return databaseFailure(m_impl->database.get(), "could not read the event stream");
        }
        UF_TRY(transaction.commit());
        return SubscriptionRead{std::move(batch)};
    }

    auto OperatorCoordinator::remainingBudget(
        ControllerBinding const& controller
    ) -> Result<AgentBudgetRemaining>
    {
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY(requireLiveBinding(m_impl->database.get(), controller));
        UF_TRY_VALUE(
            budget,
            readAgentBudget(
                m_impl->database.get(),
                controller.sessionId(),
                controller.kind()
            )
        );
        if (!budget)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "This controller kind carries no budget to report"
            );
        }
        UF_TRY_VALUE(now, steadyMillisecondsNow());
        UF_TRY(transaction.commit());
        return AgentBudgetRemaining{
            .toolCalls                  = budget->remainingToolCalls,
            .mutations                  = budget->remainingMutations,
            .observations               = budget->remainingObservations,
            .riskUnits                  = budget->remainingRiskUnits,
            .elapsedMillisRemaining     = now > budget->deadlineSteadyMillis
                ? uint64{0}
                : budget->deadlineSteadyMillis - now,
            .consecutiveNoProgressSteps = budget->consecutiveNoProgressSteps,
        };
    }

    auto OperatorCoordinator::transitionOperation(
        std::string const& operationId,
        uint64 expectedRevision,
        OperationSignal signal
    ) -> Result<StoredOperation>
    {
        UF_TRY_VALUE(event, operationEventFor(signal));
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
        if (event == OperationEvent::ReadCompleted && mutating)
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

    auto OperatorCoordinator::freezePlan(
        std::string const& operationId,
        uint64 expectedRevision,
        ControlLease const& lease,
        ProjectPluginHandle const& plugin,
        OperatorPlanAuthority const& planAuthority
    ) -> Result<FrozenPlan>
    {
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                "SELECT o.state, o.revision, o.mutating, o.command_fingerprint, "
                "o.tool_name, o.tool_version, o.canonical_args, o.session_id, "
                "o.controlled_target_id, o.plugin_id, "
                "session.project_registration_hash, registration.plugin_hash, "
                "snapshot.decision_basis_hash, observation.canonical_observation, "
                "state.canonical_opaque_payload, session.controller_kind FROM operations o "
                + std::string{k_liveControllerJoin}
                + "JOIN project_registrations registration "
                "ON registration.registration_hash=session.project_registration_hash "
                "JOIN snapshots snapshot ON snapshot.token=o.snapshot_token "
                "JOIN project_observations observation "
                "ON observation.plugin_id=snapshot.plugin_id "
                "AND observation.project_instance_key=snapshot.project_instance_key "
                "AND observation.revision=snapshot.project_observation_revision "
                "JOIN project_state state ON state.plugin_id=o.plugin_id "
                "AND state.project_instance_key=o.project_instance_key "
                "WHERE o.operation_id=?1 AND session.active=1 "
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
        if (state != OperationState::Proposed)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "A plan freezes from a proposed Operation only"
            );
        }
        // A read-only Operation has no effect to declare and no dispatch to
        // authorise, so a plan for one would be an audit record about nothing.
        auto const mutating = sqlite3_column_int(query.get(), 2) != 0;
        if (!mutating)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Read-only Operations carry no frozen plan"
            );
        }
        if (
            columnText(query.get(), 7) != lease.sessionId
            || columnText(query.get(), 8) != lease.controlledTargetId
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Plan lease does not own the Operation target"
            );
        }
        UF_TRY(requireLiveLease(m_impl->database.get(), lease, "Plan lease is stale"));

        UF_TRY_VALUE(planKind, parseControllerKind(columnText(query.get(), 15)));
        UF_TRY_VALUE(
            planBudget,
            readAgentBudget(m_impl->database.get(), lease.sessionId, planKind)
        );
        if (planBudget)
        {
            UF_TRY(requireWithinAgentDeadline(*planBudget));
        }

        auto const registrationHex = columnText(query.get(), 10);
        if (
            plugin.pluginId() != columnText(query.get(), 9)
            || plugin.pluginHash().hex() != columnText(query.get(), 11)
            || plugin.projectRegistrationHash().hex() != registrationHex
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Plan ProjectPlugin does not match the pinned session registration"
            );
        }

        auto const commandFingerprintHex = columnText(query.get(), 3);
        auto const toolName              = columnText(query.get(), 4);
        auto const toolVersion           = columnText(query.get(), 5);
        auto const canonicalArgs         = columnText(query.get(), 6);
        auto const decisionBasisHex      = columnText(query.get(), 12);
        UF_TRY_VALUE(commandFingerprint, parseHashColumn(commandFingerprintHex));
        UF_TRY_VALUE(decisionBasisHash, parseHashColumn(decisionBasisHex));
        UF_TRY_VALUE(projectRegistrationHash, parseHashColumn(registrationHex));

        // The plugin runs inside the transaction, as derive and reduce already
        // do: the read of its inputs and the freeze against them must be one
        // BEGIN IMMEDIATE, or a concurrent writer moves the world between them.
        auto const observationJcs = columnText(query.get(), 13);
        auto const projectStateJcs = columnText(query.get(), 14);
        auto const planEnvelope    = planEnvelopeJcs(PlanEnvelopeInputs{
            .canonicalArgs         = canonicalArgs,
            .projectObservationJcs = observationJcs,
            .projectStateJcs       = projectStateJcs,
            .toolName              = toolName,
            .toolVersion           = toolVersion,
        });
        UF_TRY_VALUE(planInput, plugin.canonicalize(planEnvelope));
        UF_TRY_VALUE(proposal, plugin.plan(planInput));
        UF_TRY_VALUE(
            plan,
            planAuthority.mintPlan(PlanMintInputs{
                .proposal                = proposal,
                .operationId             = operationId,
                .toolName                = toolName,
                .toolVersion             = toolVersion,
                .canonicalArgs           = canonicalArgs,
                .projectRegistrationHash = projectRegistrationHash,
                .commandFingerprint      = commandFingerprint,
                .decisionBasisHash       = decisionBasisHash,
            })
        );

        // Charged here and nowhere earlier, because risk does not exist before
        // the plan is minted: it is derived from the effects the plugin
        // declared for this command against this world. The charge precedes the
        // operation_plans insert, so a refused risk budget leaves the Operation
        // proposed with no plan row at all.
        if (planBudget)
        {
            UF_TRY(chargeAgentBudget(
                m_impl->database.get(),
                lease.sessionId,
                "UPDATE agent_budgets SET remaining_risk_units = "
                "remaining_risk_units - ?2 WHERE session_id=?1",
                riskUnits(plan.risk()),
                "Agent risk budget is exhausted"
            ));
        }

        // The Operation's own edge is decided here, from the derived risk. The
        // caller has no signal for either of these two events for exactly that
        // reason.
        UF_TRY_VALUE(machine, OperationMachine::restore(state, false, false));
        UF_TRY_VALUE(
            nextState,
            machine.transition(
                plan.approvalRequired()
                    ? OperationEvent::ApprovalRequired
                    : OperationEvent::ReadyWithoutApproval
            )
        );

        auto const limits = plan.limits();
        UF_TRY_VALUE(
            insert,
            prepare(
                m_impl->database.get(),
                "INSERT INTO operation_plans(operation_id, plan_hash, "
                "command_fingerprint, decision_basis_hash, effect_envelope_hash, "
                "project_registration_hash, risk, required_approvals, maximum_steps, "
                "maximum_dispatches, maximum_observations, maximum_waits, "
                "maximum_elapsed_ms, canonical_plan) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 1, operationId));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 2, plan.planHash().hex()));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 3, commandFingerprintHex));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 4, decisionBasisHex));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            5,
            plan.effectEnvelopeHash().hex()
        ));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 6, registrationHex));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 7, riskWireName(plan.risk())));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            insert.get(),
            8,
            plan.approvalRequired() ? 1U : 0U
        ));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 9, limits.maximumSteps));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            insert.get(),
            10,
            limits.maximumDispatches
        ));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            insert.get(),
            11,
            limits.maximumObservations
        ));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 12, limits.maximumWaits));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            insert.get(),
            13,
            limits.maximumElapsedMillis
        ));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 14, plan.canonicalPlan()));
        UF_TRY(expectDone(m_impl->database.get(), insert.get()));

        UF_TRY_VALUE(nextRevision, checkedSqlIncrement(revision, "Operation revision"));
        UF_TRY_VALUE(
            update,
            prepare(
                m_impl->database.get(),
                "UPDATE operations SET state=?1, revision=?2 "
                "WHERE operation_id=?3 AND revision=?4"
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
        return FrozenPlan{
            .operation = StoredOperation{
                .operationId   = operationId,
                .lookup        = CommandLookup::Existing,
                .state         = nextState,
                .revision      = nextRevision,
                .planFrozen    = machine.planFrozen(),
                .hasDispatched = machine.hasDispatched(),
            },
            .planHash           = plan.planHash(),
            .decisionBasisHash  = plan.decisionBasisHash(),
            .effectEnvelopeHash = plan.effectEnvelopeHash(),
            .limits             = limits,
            .risk               = plan.risk(),
            .approvalRequired   = plan.approvalRequired(),
        };
    }

    auto OperatorCoordinator::mintNextStep(
        std::string const& operationId,
        uint64 expectedRevision,
        ControlLease const& lease,
        ProjectPluginHandle const& plugin,
        OperatorPlanAuthority const& planAuthority
    ) -> Result<PlannedStep>
    {
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                "SELECT o.state, o.revision, o.session_id, o.controlled_target_id, "
                "o.plugin_id, session.project_registration_hash, "
                "registration.plugin_hash, plan.plan_hash, plan.canonical_plan, "
                "plan.maximum_steps, plan.required_approvals, "
                "COALESCE((SELECT MAX(step_index) FROM operation_steps step "
                "WHERE step.operation_id=o.operation_id), 0), "
                "EXISTS(SELECT 1 FROM operation_steps step "
                "WHERE step.operation_id=o.operation_id AND step.step_kind='ui_action' "
                "AND step.dispatch_sequence IS NULL), "
                "observation.canonical_observation, state.canonical_opaque_payload, "
                "o.frozen_plan_hash, EXISTS(SELECT 1 FROM dispatches d "
                "WHERE d.operation_id=o.operation_id), "
                "session.runtime_artifact_root_hash FROM operations o "
                + std::string{k_liveControllerJoin}
                + "JOIN project_registrations registration "
                "ON registration.registration_hash=session.project_registration_hash "
                "JOIN operation_plans plan ON plan.operation_id=o.operation_id "
                "JOIN snapshots snapshot ON snapshot.token=o.snapshot_token "
                "JOIN project_observations observation "
                "ON observation.plugin_id=snapshot.plugin_id "
                "AND observation.project_instance_key=snapshot.project_instance_key "
                "AND observation.revision=snapshot.project_observation_revision "
                "JOIN project_state state ON state.plugin_id=o.plugin_id "
                "AND state.project_instance_key=o.project_instance_key "
                "WHERE o.operation_id=?1 AND session.active=1 "
                "AND session.session_epoch=?2"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), query.get(), 1, operationId));
        UF_TRY(bindInteger(m_impl->database.get(), query.get(), 2, m_impl->sessionEpoch));
        if (sqlite3_step(query.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Unknown operation_id, no frozen plan, or its session lost the target"
            );
        }
        auto const revision = static_cast<uint64>(sqlite3_column_int64(query.get(), 1));
        if (revision != expectedRevision)
        {
            return fail(AutomationErrorKind::ActionRejected, "Operation revision is stale");
        }
        UF_TRY_VALUE(state, parseOperationState(columnText(query.get(), 0)));
        if (
            state != OperationState::Ready
            && state != OperationState::AwaitingApproval
            && state != OperationState::Reconciling
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "A workflow step is minted from a ready, awaiting or reconciling Operation"
            );
        }
        if (
            columnText(query.get(), 2) != lease.sessionId
            || columnText(query.get(), 3) != lease.controlledTargetId
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Step lease does not own the Operation target"
            );
        }
        UF_TRY(requireLiveLease(m_impl->database.get(), lease, "Step lease is stale"));
        if (
            plugin.pluginId() != columnText(query.get(), 4)
            || plugin.pluginHash().hex() != columnText(query.get(), 6)
            || plugin.projectRegistrationHash().hex() != columnText(query.get(), 5)
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Step ProjectPlugin does not match the pinned session registration"
            );
        }

        // At most one UI-action step may await its dispatch. The check lives
        // here and nowhere else on purpose: a partial unique index expressing
        // the same rule would keep its test green after this line was deleted.
        auto const pendingStep = sqlite3_column_int(query.get(), 12) != 0;
        if (pendingStep)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "A UI-action step is still awaiting its dispatch"
            );
        }

        auto const maximumSteps = static_cast<uint64>(sqlite3_column_int64(query.get(), 9));
        UF_TRY_VALUE(
            stepIndex,
            checkedSqlIncrement(
                static_cast<uint64>(sqlite3_column_int64(query.get(), 11)),
                "workflow step index"
            )
        );
        // Running out of budget stops the workflow and never terminates the
        // Operation: it stays exactly where it was, plan frozen and mutation
        // chain still held, because only a reconciliation may conclude.
        if (stepIndex > maximumSteps)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Workflow step budget is exhausted for this frozen plan"
            );
        }

        auto const planHashHex   = columnText(query.get(), 7);
        auto const canonicalPlan = columnText(query.get(), 8);
        auto const sessionArtifactRoot = columnText(query.get(), 17);
        UF_TRY_VALUE(planHash, parseHashColumn(planHashHex));
        auto const observationJcs  = columnText(query.get(), 13);
        auto const projectStateJcs = columnText(query.get(), 14);
        auto const stepEnvelope    = stepEnvelopeJcs(StepEnvelopeInputs{
            .frozenPlanHashHex     = planHashHex,
            .projectObservationJcs = observationJcs,
            .projectStateJcs       = projectStateJcs,
            .stepIndex             = stepIndex,
        });
        UF_TRY_VALUE(stepInput, plugin.canonicalize(stepEnvelope));
        UF_TRY_VALUE(intent, plugin.nextStep(stepInput));
        UF_TRY_VALUE(
            step,
            planAuthority.mintStep(StepMintInputs{
                .intent                  = intent,
                .canonicalPlan           = canonicalPlan,
                .operationId             = operationId,
                .planHash                = planHash,
                .stepIndex               = stepIndex,
                .runtimeArtifactRootHash = sessionArtifactRoot,
            })
        );

        UF_TRY_VALUE(
            insert,
            prepare(
                m_impl->database.get(),
                "INSERT INTO operation_steps(operation_id, step_index, step_kind, "
                "step_key, step_intent_hash, canonical_step, dispatch_sequence) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, NULL)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 1, operationId));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 2, stepIndex));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            3,
            stepKindWireName(step.kind())
        ));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 4, step.stepKey()));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            5,
            step.stepIntentHash().hex()
        ));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 6, step.canonicalStep()));
        UF_TRY(expectDone(m_impl->database.get(), insert.get()));

        // Only a UI-action step minted after a reconciliation moves the
        // Operation, and the edge it takes is decided by the frozen plan's
        // required_approvals rather than by the caller. A wait produces no
        // dispatch, so moving to Running for one would leave a state with no
        // outgoing edge; the first step of a plan is minted while the Operation
        // is already Ready or AwaitingApproval and needs no edge at all.
        auto const frozen         = sqlite3_column_type(query.get(), 15) != SQLITE_NULL;
        auto const dispatched     = sqlite3_column_int(query.get(), 16) != 0;
        auto const approvalNeeded = sqlite3_column_int64(query.get(), 10) != 0;
        auto nextState      = state;
        auto nextFrozen     = frozen;
        auto nextDispatched = dispatched;
        if (state == OperationState::Reconciling && step.kind() == StepKind::UiAction)
        {
            UF_TRY_VALUE(machine, OperationMachine::restore(state, frozen, dispatched));
            UF_TRY_VALUE(
                advanced,
                machine.transition(
                    approvalNeeded
                        ? OperationEvent::NextStepApprovalRequired
                        : OperationEvent::NextStepReady
                )
            );
            nextState      = advanced;
            nextFrozen     = machine.planFrozen();
            nextDispatched = machine.hasDispatched();
        }

        UF_TRY_VALUE(nextRevision, checkedSqlIncrement(revision, "Operation revision"));
        UF_TRY_VALUE(
            update,
            prepare(
                m_impl->database.get(),
                "UPDATE operations SET state=?1, revision=?2 "
                "WHERE operation_id=?3 AND revision=?4"
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
        return PlannedStep{
            .operation = StoredOperation{
                .operationId   = operationId,
                .lookup        = CommandLookup::Existing,
                .state         = nextState,
                .revision      = nextRevision,
                .planFrozen    = nextFrozen,
                .hasDispatched = nextDispatched,
            },
            .stepIntentHash = step.stepIntentHash(),
            .stepKey        = step.stepKey(),
            .stepIndex      = stepIndex,
            .kind           = step.kind(),
        };
    }

    auto OperatorCoordinator::reserveDispatch(
        std::string const& operationId,
        uint64 expectedRevision,
        ControlLease const& lease,
        GenerationId runtimeGeneration,
        AuthorityDecisionId const& authorityDecisionId,
        std::optional<ApprovalGrant> const& approval
    ) -> Result<DispatchReservation>
    {
        UF_TRY_VALUE(currentUnixMillis, unixTimeMilliseconds());
        UF_TRY(requireName(authorityDecisionId.value(), "authority_decision_id"));
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                // The three hashes are read here rather than taken from the
                // caller: an audit record a caller could name is an audit
                // record that can be made to say anything, and an approval
                // matched on caller-supplied hashes authorises whatever it was
                // handed. The pending step is found rather than selected, so
                // the dispatch names nothing at all.
                "SELECT o.state, o.revision, o.frozen_plan_hash, "
                "COALESCE((SELECT MAX(dispatch_sequence) FROM dispatches d WHERE "
                "d.operation_id=o.operation_id), 0), "
                "(SELECT delivery_outcome FROM dispatches d WHERE "
                "d.operation_id=o.operation_id ORDER BY dispatch_sequence DESC LIMIT 1) "
                ", o.session_id, o.controlled_target_id, o.mutating, "
                "plan.plan_hash, plan.decision_basis_hash, plan.maximum_dispatches, "
                "step.step_index, step.step_intent_hash, "
                "(SELECT COUNT(*) FROM dispatches d WHERE d.operation_id=o.operation_id), "
                // The target generation the composed world was observed at. It
                // is read here rather than accepted, because an authority
                // naming a generation nobody observed would carry the Host's
                // permission to act on a world the ledger never saw.
                "snapshot.target_generation "
                "FROM operations o "
                "JOIN operation_plans plan ON plan.operation_id=o.operation_id "
                "JOIN snapshots snapshot ON snapshot.token=o.snapshot_token "
                "JOIN operation_steps step ON step.operation_id=o.operation_id "
                "AND step.step_kind='ui_action' AND step.dispatch_sequence IS NULL "
                "WHERE o.operation_id=?1 "
                "ORDER BY step.step_index LIMIT 1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), query.get(), 1, operationId));
        if (sqlite3_step(query.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Unknown operation_id, no frozen plan, or no UI-action step awaits dispatch"
            );
        }
        auto const revision = static_cast<uint64>(sqlite3_column_int64(query.get(), 1));
        if (revision != expectedRevision)
        {
            return fail(AutomationErrorKind::ActionRejected, "Operation revision is stale");
        }
        UF_TRY_VALUE(state, parseOperationState(columnText(query.get(), 0)));
        if (
            columnText(query.get(), 5) != lease.sessionId
            || columnText(query.get(), 6) != lease.controlledTargetId
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
        UF_TRY(requireLiveLease(m_impl->database.get(), lease, "Dispatch lease is stale"));

        auto const planHashHex      = columnText(query.get(), 8);
        auto const decisionBasisHex = columnText(query.get(), 9);
        UF_TRY_VALUE(frozenPlanHash, parseHashColumn(planHashHex));
        UF_TRY_VALUE(decisionBasisHash, parseHashColumn(decisionBasisHex));
        auto const maximumDispatches = static_cast<uint64>(
            sqlite3_column_int64(query.get(), 10)
        );
        auto const stepIndex = static_cast<uint64>(sqlite3_column_int64(query.get(), 11));
        auto const stepIntentHex = columnText(query.get(), 12);
        UF_TRY_VALUE(stepIntentHash, parseHashColumn(stepIntentHex));
        auto const dispatchCount = static_cast<uint64>(
            sqlite3_column_int64(query.get(), 13)
        );
        auto const targetGeneration = TargetGeneration::fromValue(
            static_cast<uint64>(sqlite3_column_int64(query.get(), 14))
        );
        // Two counters because a wait step consumes a step and no dispatch.
        if (dispatchCount >= maximumDispatches)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Workflow dispatch budget is exhausted for this frozen plan"
            );
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
                    "AND controller_id=?5 AND controlled_target_id=?6 AND lease_id=?7 "
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
                lease.controlledTargetId
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
                approval->authorityDecisionId.value()
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
        UF_TRY(bindText(
            m_impl->database.get(),
            authorityInsert.get(),
            1,
            authorityDecisionId.value()
        ));
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
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            5,
            authorityDecisionId.value()
        ));
        UF_TRY(expectDone(m_impl->database.get(), insert.get()));

        // The step is linked after the dispatch row exists, so the composite
        // foreign key holds at every point inside the transaction.
        UF_TRY_VALUE(
            linkStep,
            prepare(
                m_impl->database.get(),
                "UPDATE operation_steps SET dispatch_sequence=?1 "
                "WHERE operation_id=?2 AND step_index=?3 AND dispatch_sequence IS NULL"
            )
        );
        UF_TRY(bindInteger(m_impl->database.get(), linkStep.get(), 1, sequence));
        UF_TRY(bindText(m_impl->database.get(), linkStep.get(), 2, operationId));
        UF_TRY(bindInteger(m_impl->database.get(), linkStep.get(), 3, stepIndex));
        UF_TRY(expectDone(m_impl->database.get(), linkStep.get()));
        if (sqlite3_changes(m_impl->database.get()) != 1)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "The pending workflow step was linked to another dispatch"
            );
        }

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
            .authority = task::DispatchAuthority{
                .controlledTargetId  = lease.controlledTargetId,
                .leaseId             = lease.leaseId,
                .operationId         = operationId,
                .authorityDecisionId = authorityDecisionId.value(),
                .frozenPlanHash      = frozenPlanHash,
                .runtimeGeneration   = runtimeGeneration,
                .targetGeneration    = targetGeneration,
                .sessionEpoch        = lease.sessionEpoch,
                .fencingToken        = lease.fencingToken,
                .dispatchSequence    = sequence,
            },
            .decisionBasisHash = decisionBasisHash,
            .stepIntentHash    = stepIntentHash,
            .operationRevision = nextRevision,
            .stepIndex         = stepIndex,
        };
    }

    auto OperatorCoordinator::recordDeliveryOutcome(
        ControlLease const& lease,
        uint64 expectedRevision,
        task::HostDeliveryReport const& report
    ) -> Result<StoredOperation>
    {
        auto const& authority = report.authority();
        // Checked in C++ before the statement so the refusal names its reason.
        // The lease the caller presents must be the lease the report was
        // authorized by; the statement below then requires that same lease to
        // still be the live row. Two refusals, and neither implies the other.
        if (
            authority.controlledTargetId != lease.controlledTargetId
            || authority.leaseId != lease.leaseId
            || authority.sessionEpoch != lease.sessionEpoch
            || authority.fencingToken != lease.fencingToken
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Host delivery report was not authorized by the presented lease"
            );
        }

        auto const& operationId     = authority.operationId;
        auto const dispatchSequence = authority.dispatchSequence;
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                // Every identity the reservation minted is matched against the
                // rows that minted it. A report produced before a takeover and
                // presented after it fails here on the lease predicate and again
                // on the outcome CAS below, and the two are independent so each
                // is separately falsifiable.
                "SELECT o.state, o.revision, d.delivery_outcome FROM operations o "
                + std::string{k_liveControllerJoin}
                + "JOIN dispatches d ON d.operation_id=o.operation_id "
                  "JOIN authority_decisions a "
                  "ON a.authority_decision_id=d.authority_decision_id "
                  "JOIN snapshots snapshot ON snapshot.token=o.snapshot_token "
                  "WHERE o.operation_id=?1 AND d.dispatch_sequence=?2 "
                  "AND session.active=1 AND session.session_epoch=?3 "
                  "AND o.controlled_target_id=?4 "
                  "AND lease.lease_id=?5 AND lease.fencing_token=?6 "
                  "AND lease.revision=?7 AND lease.session_epoch=?3 "
                  "AND a.dispatch_sequence=?2 AND a.authority_decision_id=?8 "
                  "AND a.lease_id=?5 AND a.fencing_token=?6 AND a.session_epoch=?3 "
                  "AND d.frozen_plan_hash=?9 "
                  "AND snapshot.target_generation=?10"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), query.get(), 1, operationId));
        UF_TRY(bindInteger(m_impl->database.get(), query.get(), 2, dispatchSequence));
        UF_TRY(bindInteger(m_impl->database.get(), query.get(), 3, m_impl->sessionEpoch));
        UF_TRY(bindText(
            m_impl->database.get(),
            query.get(),
            4,
            lease.controlledTargetId
        ));
        UF_TRY(bindText(m_impl->database.get(), query.get(), 5, lease.leaseId));
        UF_TRY(bindInteger(m_impl->database.get(), query.get(), 6, lease.fencingToken));
        UF_TRY(bindInteger(m_impl->database.get(), query.get(), 7, lease.revision));
        UF_TRY(bindText(
            m_impl->database.get(),
            query.get(),
            8,
            authority.authorityDecisionId
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            query.get(),
            9,
            authority.frozenPlanHash.hex()
        ));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            query.get(),
            10,
            authority.targetGeneration.value()
        ));
        if (sqlite3_step(query.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "No live dispatch matches this Host delivery report"
            );
        }
        auto const revision = static_cast<uint64>(sqlite3_column_int64(query.get(), 1));
        if (revision != expectedRevision)
        {
            return fail(AutomationErrorKind::ActionRejected, "Operation revision is stale");
        }
        if (sqlite3_column_type(query.get(), 2) != SQLITE_NULL)
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
                "UPDATE dispatches SET delivery_outcome=?1, delivery_reason=?2 "
                "WHERE operation_id=?3 AND dispatch_sequence=?4 "
                "AND delivery_outcome IS NULL"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            dispatchUpdate.get(),
            1,
            deliveryOutcomeWireName(report.outcome())
        ));
        // The Host's own words for why it did not act. Empty exactly when the
        // outcome is delivered, which is the shape the table's CHECK requires.
        if (report.reason().empty())
        {
            if (sqlite3_bind_null(dispatchUpdate.get(), 2) != SQLITE_OK)
            {
                return databaseFailure(
                    m_impl->database.get(),
                    "could not bind absent delivery reason"
                );
            }
        }
        else
        {
            UF_TRY(bindText(
                m_impl->database.get(),
                dispatchUpdate.get(),
                2,
                report.reason()
            ));
        }
        UF_TRY(bindText(m_impl->database.get(), dispatchUpdate.get(), 3, operationId));
        UF_TRY(bindInteger(m_impl->database.get(), dispatchUpdate.get(), 4, dispatchSequence));
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
        AuthorityDecisionId const& authorityDecisionId
    ) -> Result<ApprovalGrant>
    {
        UF_TRY(requireName(authorityDecisionId.value(), "authority_decision_id"));
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
                // The four hashes an approval is matched on are read here, not
                // taken from the approver: an approval issued for hashes its
                // holder chose authorises whatever those hashes name, which is
                // how one step's approval comes to authorise another's.
                "SELECT o.state, o.session_id, o.controlled_target_id, "
                "o.command_fingerprint, o.frozen_plan_hash, plan.plan_hash, "
                "plan.decision_basis_hash, plan.effect_envelope_hash, "
                "step.step_intent_hash FROM operations o "
                "JOIN operation_plans plan ON plan.operation_id=o.operation_id "
                "JOIN operation_steps step ON step.operation_id=o.operation_id "
                "AND step.step_kind='ui_action' AND step.dispatch_sequence IS NULL "
                "WHERE o.operation_id=?1 ORDER BY step.step_index LIMIT 1"
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
            return fail(
                AutomationErrorKind::InvalidResource,
                "Unknown operation_id, no frozen plan, or no UI-action step awaits dispatch"
            );
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
            || columnText(operationQuery.get(), 2) != request.lease.controlledTargetId
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Approval lease does not own the Operation target"
            );
        }
        auto const frozenPlanHex = columnText(operationQuery.get(), 5);
        if (
            sqlite3_column_type(operationQuery.get(), 4) != SQLITE_NULL
            && columnText(operationQuery.get(), 4) != frozenPlanHex
        )
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "The Operation dispatched under a plan hash operation_plans does not hold"
            );
        }
        UF_TRY(requireLiveLease(
            m_impl->database.get(),
            request.lease,
            "Approval lease is stale"
        ));

        UF_TRY_VALUE(token, randomToken(m_impl->database.get()));
        UF_TRY_VALUE(
            insert,
            prepare(
                m_impl->database.get(),
                "INSERT INTO approvals(token, operation_id, session_id, controller_id, "
                "controlled_target_id, lease_id, session_epoch, fencing_token, "
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
            request.lease.controlledTargetId
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
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 10, frozenPlanHex));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            11,
            columnText(operationQuery.get(), 8)
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            12,
            columnText(operationQuery.get(), 6)
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            13,
            columnText(operationQuery.get(), 7)
        ));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 14, request.policyHash.hex()));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 15, request.approverPrincipal));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            16,
            request.approverCapabilityHash.hex()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            17,
            authorityDecisionId.value()
        ));
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
                "SELECT revision, project_registration_hash, project_state_schema_hash, "
                "last_journal_sequence, canonical_opaque_payload FROM project_state "
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
                    "namespaced_event_type, payload_schema_hash, opaque_project_payload, "
                    "provenance) "
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
                    "UPDATE project_state SET revision=?1, canonical_opaque_payload=?2, state_hash=?3, "
                    "last_journal_sequence=?4 WHERE plugin_id=?5 AND project_instance_key=?6 "
                    "AND project_registration_hash=?7 AND project_state_schema_hash=?8 AND revision=?9"
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
