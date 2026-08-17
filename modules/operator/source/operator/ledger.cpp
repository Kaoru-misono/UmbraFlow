#include "ledger.hpp"
#include "runtime-installation.hpp"

#include <core/error/contracts.hpp>
#include <core/safety/annotations.hpp>
#include <core/text/json-text.hpp>
#include <core/text/utf8.hpp>
#include <core/time/monotonic-time.hpp>

#include <domain/error.hpp>

#include <json/value.hpp>

#include <task/platform/confined-file.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
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

        // A set of identifiers as the one text the ledger stores it under: a
        // JCS array, sorted and without repeats. Capability sets and
        // required-approval sets are both this shape, and both are hashed or
        // compared as whole documents, so two spellings of one set would be two
        // values.
        [[nodiscard]]
        auto canonicalNameArray(std::vector<std::string> names) -> std::string
        {
            std::ranges::sort(names);
            names.erase(std::ranges::unique(names).begin(), names.end());
            auto output = std::string{"["};
            auto first  = true;
            for (auto const& name : names)
            {
                if (!first)
                {
                    output.push_back(',');
                }
                first = false;
                appendJsonString(output, name);
            }
            output.push_back(']');
            return output;
        }

        // The same set, read back out of the column that holds it. A row this
        // process wrote is the only thing that reaches here, so unparseable
        // text is a broken database rather than bad input.
        [[nodiscard]]
        auto readNameArray(std::string_view stored) -> Result<std::vector<std::string>>
        {
            UF_TRY_VALUE(document, json::parse(stored));
            if (document.kind() != json::ValueKind::Array)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "a stored identifier set is not a JSON array"
                );
            }
            auto names = std::vector<std::string>{};
            for (auto const& item : document.items())
            {
                if (item.kind() != json::ValueKind::String)
                {
                    return fail(
                        AutomationErrorKind::InternalInvariant,
                        "a stored identifier set holds a value that is not a name"
                    );
                }
                names.emplace_back(item.string());
            }
            return names;
        }

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

        // The sole Operator schema identity: sha256 over the canonicalization
        // exactDatabaseSchemaIdentity builds -- every sqlite_schema row ordered
        // by (type, name), each of its four columns written as
        // <byte length>:<value>. It therefore covers the STORED DDL TEXT --
        // reindenting the R"sql(...)" block below changes it even when the
        // schema is identical, and so does adding a comment inside it. Any
        // change to stored DDL below recomputes this value in the same change,
        // from a freshly created database rather than by hand. initialize()
        // verifies immediately after creating the schema, so a forgotten
        // recomputation cannot ship green.
        //
        // PRAGMA user_version has no identity and no upgrade role, so the DDL
        // does not write it. docs/TODO.md "Delete-on-open has a deadline" owns
        // the exact-pair migration policy.
        constexpr auto k_operatorDatabaseSchemaIdentity = std::string_view{
            "sha256:b26344e031574f95020ed445e16e9de396f76442d98c5a3b758a91d84660237e"
        };

        // A transition row records the applied exact pair; neither the row nor
        // its insertion order participates in identity or selects a migration.
        constexpr auto k_schemaIdentityTransitionsDdl = std::string_view{
            "CREATE TABLE schema_identity_transitions("
            "source_identity TEXT NOT NULL,"
            "target_identity TEXT NOT NULL,"
            "PRIMARY KEY(source_identity, target_identity)"
            ") STRICT"
        };

        constexpr auto k_ledgerEventsDdl = std::string_view{
            "CREATE TABLE ledger_events("
            "sequence INTEGER PRIMARY KEY AUTOINCREMENT,"
            "session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),"
            "controlled_target_id TEXT NOT NULL,"
            "kind TEXT NOT NULL,"
            "subject_id TEXT NOT NULL,"
            "detail TEXT,"
            "CHECK((kind IN ('operation_created', 'control_transitioned', "
            "'external_input_detected') AND detail IS NULL) OR "
            "(kind='operation_state_changed' AND detail IN ("
            "'proposed', 'awaiting_approval', 'ready', 'needs_revalidation', "
            "'running', 'reconciling', 'confirmed', 'rejected', 'ambiguous', "
            "'invalid', 'denied', 'cancelled', 'expired', 'diverged')) OR "
            "(kind='delivery_outcome_recorded' AND detail IN ("
            "'not_delivered', 'delivered', 'transport_unknown')))"
            ") STRICT"
        };

        constexpr auto k_sessionPoliciesDdl = std::string_view{
            "CREATE TABLE session_policies("
            "session_id TEXT PRIMARY KEY REFERENCES sessions(session_id),"
            "policy_hash TEXT NOT NULL"
            ") STRICT"
        };

        constexpr auto k_availabilityHeadsDdl = std::string_view{
            "CREATE TABLE availability_heads("
            "controlled_target_id TEXT PRIMARY KEY,"
            "revision INTEGER NOT NULL CHECK(revision > 0),"
            "policy_hash TEXT NOT NULL,"
            "available_tools TEXT NOT NULL"
            ") STRICT"
        };

        constexpr auto k_releaseCapabilityApprovalsDdl = std::string_view{
            "CREATE TABLE release_capability_approvals("
            "artifact_root_hash TEXT NOT NULL,"
            "capability_profile_hash TEXT NOT NULL,"
            "controller_capabilities TEXT NOT NULL,"
            "evidence_hash TEXT NOT NULL,"
            "session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),"
            "PRIMARY KEY(artifact_root_hash, capability_profile_hash)"
            ") STRICT"
        };

        constexpr auto k_runtimeUpgradeFailuresDdl = std::string_view{
            "CREATE TABLE runtime_upgrade_failures("
            "sequence INTEGER PRIMARY KEY AUTOINCREMENT,"
            "attempted_generation INTEGER NOT NULL CHECK(attempted_generation > 0),"
            "attempted_artifact_root_hash TEXT NOT NULL,"
            "restored_generation INTEGER NOT NULL CHECK(restored_generation > 0),"
            "restored_artifact_root_hash TEXT NOT NULL,"
            "reason TEXT NOT NULL"
            ") STRICT"
        };

        // A registration row is its canonical bytes plus the two values a join
        // selects on. It deliberately carries no second copy of a member those
        // bytes already hold: a copy beside a full canonical-bytes comparison
        // refuses nothing the comparison does not, and one more column to keep
        // in step is one more way for the row and the document to disagree.
        constexpr auto k_projectRegistrationsDdl = std::string_view{
            "CREATE TABLE project_registrations("
            "registration_hash TEXT PRIMARY KEY,"
            "plugin_id TEXT NOT NULL,"
            "plugin_hash TEXT NOT NULL,"
            "canonical_manifest TEXT NOT NULL"
            ") STRICT"
        };

        constexpr auto k_projectInstancesDdl = std::string_view{
            "CREATE TABLE project_instances("
            "plugin_id TEXT NOT NULL,"
            "project_instance_key TEXT NOT NULL,"
            "project_registration_hash TEXT NOT NULL "
            "REFERENCES project_registrations(registration_hash),"
            "baseline_event_id TEXT UNIQUE,"
            "PRIMARY KEY(plugin_id, project_instance_key),"
            "UNIQUE(project_registration_hash, project_instance_key)"
            ") STRICT"
        };

        // The session table and its one partial index, as the schema bundle
        // above also stores them. The migration that adds the world-scope
        // columns rebuilds the table from this exact text; SQLite strips only
        // leading and trailing whitespace, so the column indentation below is
        // part of schema identity and not formatting.
        constexpr auto k_sessionsDdl = std::string_view{
            R"sql(
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
                        -- The capability set this session holds, as the exact
                        -- JCS array capability_profile_hash is the sha256 of.
                        -- The hash alone was a caller field with no content
                        -- behind it, so a policy rule naming a required
                        -- capability had nothing to be judged against.
                        controller_capabilities TEXT NOT NULL,
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
                        -- The observed-instance world this session observes in,
                        -- stored under the same three columns
                        -- observed_instance_bindings carry so a snapshot can
                        -- rebuild the scope that minted its observation
                        -- without a second spelling. It is part of the
                        -- immutable pinned tuple.
                        world_scope_kind TEXT NOT NULL
                            CHECK(world_scope_kind IN ('account', 'run')),
                        world_scope_id TEXT NOT NULL,
                        world_scope_generation TEXT NOT NULL
                            CHECK(
                                length(world_scope_generation) > 0
                                AND world_scope_generation NOT GLOB '*[^0-9]*'
                                AND (
                                    world_scope_kind = 'account'
                                    OR world_scope_generation != '0'
                                )
                            ),
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
            )sql"
        };

        // The mint binding and its three immutability triggers, as the schema
        // bundle also stores them. local_ref is the model target the instance
        // was observed at -- the name the proposal's local_ref carried at mint
        // -- and the migration that adds the column backfills rows minted
        // before it with the empty sentinel, which reserveDispatch refuses, so
        // a migrated binding can never be resolved to a target it was never
        // observed at.
        constexpr auto k_observedInstanceBindingsDdl = std::string_view{
            R"sql(
                    -- The bidirectional Operator-private mint binding. It is
                    -- independent of scope lifetime: no scope row owns it and
                    -- no cascade can remove it. This schema implements no
                    -- reference-expiry proof, so cleanup is forbidden rather
                    -- than guessing whether a Journal, Operation, backup or
                    -- audit record still resolves through the binding.
                    CREATE TABLE IF NOT EXISTS observed_instance_bindings(
                        canonical_authority TEXT PRIMARY KEY,
                        observed_instance_id TEXT NOT NULL UNIQUE
                            CHECK(
                                length(observed_instance_id) = 68
                                AND substr(observed_instance_id, 1, 4) = 'oi1_'
                                AND substr(observed_instance_id, 5)
                                    NOT GLOB '*[^0-9a-f]*'
                            ),
                        plugin_id TEXT NOT NULL,
                        project_registration_hash TEXT NOT NULL,
                        project_instance_key TEXT NOT NULL,
                        world_scope_kind TEXT NOT NULL
                            CHECK(world_scope_kind IN ('account', 'run')),
                        world_scope_id TEXT NOT NULL,
                        world_scope_generation TEXT NOT NULL
                            CHECK(
                                length(world_scope_generation) > 0
                                AND world_scope_generation NOT GLOB '*[^0-9]*'
                                AND (
                                    world_scope_kind = 'account'
                                    OR world_scope_generation != '0'
                                )
                            ),
                        -- The model target the instance was observed at, as
                        -- the proposal named it. It is part of the binding,
                        -- not the world scope, because it answers "which
                        -- target did this instance name" and the deliver path
                        -- compares it with the receipt's own target.
                        local_ref TEXT NOT NULL,
                        FOREIGN KEY(plugin_id, project_instance_key)
                            REFERENCES project_instances(plugin_id, project_instance_key),
                        FOREIGN KEY(project_registration_hash, project_instance_key)
                            REFERENCES project_instances(
                                project_registration_hash,
                                project_instance_key
                            )
                    ) STRICT;

                    CREATE TRIGGER forbid_observed_instance_binding_replacement
                    BEFORE INSERT ON observed_instance_bindings
                    WHEN EXISTS(
                        SELECT 1 FROM observed_instance_bindings
                        WHERE canonical_authority = new.canonical_authority
                            OR observed_instance_id = new.observed_instance_id
                    )
                    BEGIN
                        SELECT RAISE(
                            ABORT,
                            'observed instance bindings are immutable'
                        );
                    END;

                    CREATE TRIGGER forbid_observed_instance_binding_mutation
                    BEFORE UPDATE ON observed_instance_bindings
                    BEGIN
                        SELECT RAISE(
                            ABORT,
                            'observed instance bindings are immutable'
                        );
                    END;

                    CREATE TRIGGER forbid_observed_instance_binding_cleanup
                    BEFORE DELETE ON observed_instance_bindings
                    BEGIN
                        SELECT RAISE(
                            ABORT,
                            'observed instance binding cleanup requires a reference-expiry proof'
                        );
                    END;
            )sql"
        };

        constexpr auto k_oneActiveWriteSessionIndexDdl = std::string_view{
            R"sql(
                    CREATE UNIQUE INDEX IF NOT EXISTS one_active_write_session_per_instance
                    ON sessions(project_registration_hash, project_instance_key)
                    WHERE mode='write' AND active=1;
            )sql"
        };

        // Retention is automatic at the write that creates each row. Snapshot
        // and observation rows that an Operation still names are audit input
        // and remain until that Operation has its own retention ruling; the
        // unclaimed history around them is bounded here.
        constexpr auto k_retainedLedgerEvents       = uint64{128};
        constexpr auto k_retainedSnapshotHeads      = uint64{32};
        constexpr auto k_retainedObservationHeads   = uint64{32};

        [[nodiscard]]
        auto exactDatabaseSchemaIdentity(sqlite3* database) -> Result<ContentHash>
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
            return sha256(std::as_bytes(std::span{canonical}));
        }

        [[nodiscard]]
        auto verifyExactDatabaseSchema(
            sqlite3* database,
            std::string_view expectedIdentity
        ) -> Status
        {
            UF_TRY_VALUE(actual, exactDatabaseSchemaIdentity(database));
            UF_TRY_VALUE(
                expected,
                ContentHash::parse(expectedIdentity)
            );
            if (actual != expected)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "Operator database schema identity sha256:{} does not match {}",
                        actual.hex(),
                        expectedIdentity
                    )
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto verifyExactDatabaseSchema(sqlite3* database) -> Status
        {
            return verifyExactDatabaseSchema(
                database,
                k_operatorDatabaseSchemaIdentity
            );
        }

        struct SchemaMigration final
        {
            std::string_view sourceIdentity{};
            std::string_view targetIdentity{};
            auto (*apply)(sqlite3*, SchemaMigration const&) -> Status{};
        };

        [[nodiscard]]
        auto recordSchemaIdentityTransition(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(
                insert,
                prepare(
                    database,
                    "INSERT INTO schema_identity_transitions("
                    "source_identity, target_identity) VALUES(?1, ?2)"
                )
            );
            UF_TRY(bindText(database, insert.get(), 1, migration.sourceIdentity));
            UF_TRY(bindText(database, insert.get(), 2, migration.targetIdentity));
            return expectDone(database, insert.get());
        }

        [[nodiscard]]
        auto addReleaseUpgradeEvidenceTables(sqlite3* database) -> Status
        {
            UF_TRY(execute(database, k_releaseCapabilityApprovalsDdl));
            return execute(database, k_runtimeUpgradeFailuresDdl);
        }

        // project_state_schema_hash was a second copy of a member the same
        // row's canonical_manifest already carries, and both readers compared
        // it inside the same disjunction as a full canonical-bytes comparison.
        // The rebuild drops the column and keeps every registration: the bytes
        // the column copied are still there, still compared.
        [[nodiscard]]
        auto dropRegistrationStateSchemaHash(sqlite3* database) -> Status
        {
            UF_TRY(execute(database, "PRAGMA defer_foreign_keys=ON"));
            UF_TRY(execute(
                database,
                "CREATE TABLE prior_project_registrations("
                "registration_hash TEXT PRIMARY KEY,"
                "plugin_id TEXT NOT NULL,"
                "plugin_hash TEXT NOT NULL,"
                "canonical_manifest TEXT NOT NULL) STRICT"
            ));
            UF_TRY(execute(
                database,
                "INSERT INTO prior_project_registrations SELECT registration_hash, "
                "plugin_id, plugin_hash, canonical_manifest FROM project_registrations"
            ));
            UF_TRY(execute(database, "DROP TABLE project_registrations"));
            UF_TRY(execute(database, k_projectRegistrationsDdl));
            UF_TRY(execute(
                database,
                "INSERT INTO project_registrations SELECT registration_hash, "
                "plugin_id, plugin_hash, canonical_manifest "
                "FROM prior_project_registrations"
            ));
            return execute(database, "DROP TABLE prior_project_registrations");
        }

        [[nodiscard]]
        auto makeProjectBaselineOptional(sqlite3* database) -> Status
        {
            UF_TRY(execute(database, "PRAGMA defer_foreign_keys=ON"));
            UF_TRY(execute(
                database,
                "CREATE TABLE prior_project_instances("
                "plugin_id TEXT NOT NULL,"
                "project_instance_key TEXT NOT NULL,"
                "project_registration_hash TEXT NOT NULL,"
                "baseline_event_id TEXT) STRICT"
            ));
            UF_TRY(execute(
                database,
                "INSERT INTO prior_project_instances SELECT plugin_id, "
                "project_instance_key, project_registration_hash, "
                "baseline_event_id FROM project_instances"
            ));
            UF_TRY(execute(database, "DROP TABLE project_instances"));
            UF_TRY(execute(database, k_projectInstancesDdl));
            UF_TRY(execute(
                database,
                "INSERT INTO project_instances SELECT plugin_id, "
                "project_instance_key, project_registration_hash, "
                "baseline_event_id FROM prior_project_instances"
            ));
            return execute(database, "DROP TABLE prior_project_instances");
        }

        // The corrected snapshot comment is stored DDL and therefore schema
        // identity even though it changes no column. The source identity is an
        // exact precondition, so this rewrite has one expected target row and
        // cannot become a general sqlite_schema editing path.
        [[nodiscard]]
        auto rewriteSnapshotIdentityComment(sqlite3* database) -> Status
        {
            UF_TRY(execute(database, "PRAGMA writable_schema=ON"));
            UF_TRY_VALUE(
                update,
                prepare(
                    database,
                    "UPDATE sqlite_schema SET sql=replace(sql, ?1, ?2) "
                    "WHERE type='table' AND name='snapshots'"
                )
            );
            auto constexpr prior = std::string_view{
                "                        token TEXT PRIMARY KEY,"
            };
            auto constexpr corrected = std::string_view{
                "                        -- token and snapshot_revision are deliberately outside\n"
                "                        -- canonical_parts: they name the stored row rather than\n"
                "                        -- the capture. observation_id remains inside and names\n"
                "                        -- the capture, so recapturing an identical world moves\n"
                "                        -- identity_hash while decision_basis_hash stays stable.\n"
                "                        token TEXT PRIMARY KEY,"
            };
            UF_TRY(bindText(database, update.get(), 1, prior));
            UF_TRY(bindText(database, update.get(), 2, corrected));
            UF_TRY(expectDone(database, update.get()));
            if (sqlite3_changes(database) != 1)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Snapshot DDL comment migration did not rewrite exactly one table"
                );
            }
            return execute(database, "PRAGMA writable_schema=OFF");
        }

        // The three world-scope columns are NOT NULL with no default, so
        // SQLite cannot ADD COLUMN them in place: the table is rebuilt from
        // its exact final DDL. Pre-scope sessions (pinned before the columns
        // existed) cannot claim a world scope -- the U2b/U2c ruling forbids
        // inferring one -- so their rows are backfilled with the
        // empty-account sentinel, which passes the column CHECKs and is
        // refused by restoreSessionWorldScope. Such a session can no longer
        // observe; every other row keeps its bytes.
        [[nodiscard]]
        auto addSessionWorldScopeColumns(sqlite3* database) -> Status
        {
            UF_TRY(execute(database, "PRAGMA defer_foreign_keys=ON"));
            UF_TRY(execute(
                database,
                "CREATE TABLE prior_sessions("
                "session_id TEXT PRIMARY KEY,"
                "authenticated_controller_id TEXT NOT NULL,"
                "idempotency_namespace TEXT NOT NULL,"
                "manifest_hash TEXT NOT NULL,"
                "runtime_artifact_root_hash TEXT NOT NULL,"
                "installed_generation INTEGER NOT NULL,"
                "project_registration_hash TEXT NOT NULL,"
                "controller_capabilities TEXT NOT NULL,"
                "capability_profile_hash TEXT NOT NULL,"
                "session_epoch INTEGER NOT NULL,"
                "controlled_target_id TEXT NOT NULL,"
                "project_instance_key TEXT NOT NULL,"
                "mode TEXT NOT NULL,"
                "controller_kind TEXT NOT NULL,"
                "active INTEGER NOT NULL"
                ") STRICT"
            ));
            UF_TRY(execute(
                database,
                "INSERT INTO prior_sessions(session_id, authenticated_controller_id, "
                "idempotency_namespace, manifest_hash, runtime_artifact_root_hash, "
                "installed_generation, project_registration_hash, "
                "controller_capabilities, capability_profile_hash, session_epoch, "
                "controlled_target_id, project_instance_key, mode, controller_kind, "
                "active) SELECT session_id, authenticated_controller_id, "
                "idempotency_namespace, manifest_hash, runtime_artifact_root_hash, "
                "installed_generation, project_registration_hash, "
                "controller_capabilities, capability_profile_hash, session_epoch, "
                "controlled_target_id, project_instance_key, mode, controller_kind, "
                "active FROM sessions"
            ));
            UF_TRY(execute(database, "DROP TABLE sessions"));
            UF_TRY(execute(database, k_sessionsDdl));
            UF_TRY(execute(
                database,
                "INSERT INTO sessions(session_id, authenticated_controller_id, "
                "idempotency_namespace, manifest_hash, runtime_artifact_root_hash, "
                "installed_generation, project_registration_hash, "
                "controller_capabilities, capability_profile_hash, session_epoch, "
                "controlled_target_id, project_instance_key, mode, controller_kind, "
                "active, world_scope_kind, world_scope_id, world_scope_generation) "
                "SELECT session_id, authenticated_controller_id, "
                "idempotency_namespace, manifest_hash, runtime_artifact_root_hash, "
                "installed_generation, project_registration_hash, "
                "controller_capabilities, capability_profile_hash, session_epoch, "
                "controlled_target_id, project_instance_key, mode, controller_kind, "
                "active, 'account', '', '0' FROM prior_sessions"
            ));
            UF_TRY(execute(database, k_oneActiveWriteSessionIndexDdl));
            return execute(database, "DROP TABLE prior_sessions");
        }

        // local_ref is NOT NULL with no default, so SQLite cannot ADD COLUMN it
        // in place: the table is rebuilt from its exact final DDL, triggers
        // included. Bindings minted before the column existed were never
        // observed under a recorded target, so their rows are backfilled with
        // the empty sentinel, which reserveDispatch refuses -- a migrated
        // binding can never be resolved to a target it never claimed, the same
        // fail-closed ruling the world-scope sentinel follows. Every other
        // byte of every row survives.
        [[nodiscard]]
        auto addObservedInstanceBindingLocalRef(sqlite3* database) -> Status
        {
            UF_TRY(execute(database, "PRAGMA defer_foreign_keys=ON"));
            UF_TRY(execute(
                database,
                "CREATE TABLE prior_observed_instance_bindings("
                "canonical_authority TEXT PRIMARY KEY,"
                "observed_instance_id TEXT NOT NULL UNIQUE,"
                "plugin_id TEXT NOT NULL,"
                "project_registration_hash TEXT NOT NULL,"
                "project_instance_key TEXT NOT NULL,"
                "world_scope_kind TEXT NOT NULL,"
                "world_scope_id TEXT NOT NULL,"
                "world_scope_generation TEXT NOT NULL"
                ") STRICT"
            ));
            UF_TRY(execute(
                database,
                "INSERT INTO prior_observed_instance_bindings("
                "canonical_authority, observed_instance_id, plugin_id, "
                "project_registration_hash, project_instance_key, "
                "world_scope_kind, world_scope_id, world_scope_generation) "
                "SELECT canonical_authority, observed_instance_id, plugin_id, "
                "project_registration_hash, project_instance_key, "
                "world_scope_kind, world_scope_id, world_scope_generation "
                "FROM observed_instance_bindings"
            ));
            UF_TRY(execute(database, "DROP TABLE observed_instance_bindings"));
            UF_TRY(execute(database, k_observedInstanceBindingsDdl));
            UF_TRY(execute(
                database,
                "INSERT INTO observed_instance_bindings("
                "canonical_authority, observed_instance_id, plugin_id, "
                "project_registration_hash, project_instance_key, "
                "world_scope_kind, world_scope_id, world_scope_generation, "
                "local_ref) SELECT canonical_authority, observed_instance_id, "
                "plugin_id, project_registration_hash, project_instance_key, "
                "world_scope_kind, world_scope_id, world_scope_generation, '' "
                "FROM prior_observed_instance_bindings"
            ));
            return execute(database, "DROP TABLE prior_observed_instance_bindings");
        }

        [[nodiscard]]
        auto migrateSessionWorldScope(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(addSessionWorldScopeColumns(database));
            UF_TRY(addObservedInstanceBindingLocalRef(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        [[nodiscard]]
        auto migrateOperatorU9Schema(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(execute(
                database,
                "CREATE TABLE IF NOT EXISTS schema_identity_transitions("
                "source_identity TEXT NOT NULL,"
                "target_identity TEXT NOT NULL,"
                "PRIMARY KEY(source_identity, target_identity)"
                ") STRICT"
            ));
            UF_TRY(execute(database, "ALTER TABLE ledger_events RENAME TO prior_ledger_events"));
            UF_TRY(execute(database, k_ledgerEventsDdl));
            UF_TRY(execute(
                database,
                "INSERT INTO ledger_events(sequence, session_epoch, controlled_target_id, "
                "kind, subject_id, detail) SELECT sequence, session_epoch, "
                "controlled_target_id, kind, subject_id, NULL FROM prior_ledger_events"
            ));
            UF_TRY(execute(database, "DROP TABLE prior_ledger_events"));
            UF_TRY(execute(database, k_sessionPoliciesDdl));
            UF_TRY(execute(database, k_availabilityHeadsDdl));
            UF_TRY(rewriteSnapshotIdentityComment(database));
            UF_TRY(makeProjectBaselineOptional(database));
            UF_TRY(addReleaseUpgradeEvidenceTables(database));
            UF_TRY(dropRegistrationStateSchemaHash(database));
            UF_TRY(addSessionWorldScopeColumns(database));
            UF_TRY(addObservedInstanceBindingLocalRef(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));

            // No migration commits under an identity other than the exact
            // target named by its registration.
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        [[nodiscard]]
        auto migrateTransitionTableOnly(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(execute(database, k_schemaIdentityTransitionsDdl));
            UF_TRY(rewriteSnapshotIdentityComment(database));
            UF_TRY(makeProjectBaselineOptional(database));
            UF_TRY(addReleaseUpgradeEvidenceTables(database));
            UF_TRY(dropRegistrationStateSchemaHash(database));
            UF_TRY(addSessionWorldScopeColumns(database));
            UF_TRY(addObservedInstanceBindingLocalRef(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        [[nodiscard]]
        auto migrateSnapshotIdentityComment(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(rewriteSnapshotIdentityComment(database));
            UF_TRY(makeProjectBaselineOptional(database));
            UF_TRY(addReleaseUpgradeEvidenceTables(database));
            UF_TRY(dropRegistrationStateSchemaHash(database));
            UF_TRY(addSessionWorldScopeColumns(database));
            UF_TRY(addObservedInstanceBindingLocalRef(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        [[nodiscard]]
        auto migrateProjectBaselineOptional(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(makeProjectBaselineOptional(database));
            UF_TRY(addReleaseUpgradeEvidenceTables(database));
            UF_TRY(dropRegistrationStateSchemaHash(database));
            UF_TRY(addSessionWorldScopeColumns(database));
            UF_TRY(addObservedInstanceBindingLocalRef(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        [[nodiscard]]
        auto migrateReleaseUpgradeEvidence(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(addReleaseUpgradeEvidenceTables(database));
            UF_TRY(dropRegistrationStateSchemaHash(database));
            UF_TRY(addSessionWorldScopeColumns(database));
            UF_TRY(addObservedInstanceBindingLocalRef(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        [[nodiscard]]
        auto migrateRegistrationStateSchemaHash(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(dropRegistrationStateSchemaHash(database));
            UF_TRY(addSessionWorldScopeColumns(database));
            UF_TRY(addObservedInstanceBindingLocalRef(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        [[nodiscard]]
        auto migrateObservedInstanceBindingLocalRef(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(addObservedInstanceBindingLocalRef(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        constexpr auto k_schemaMigrations = std::array{
            SchemaMigration{
                .sourceIdentity =
                    "sha256:869fb0a128df4a0026bb429449fae03d6b43244c9cef4e794dfdd648421bcc19",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateRegistrationStateSchemaHash,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:d96860862dc25fb6efb21d09f59dcc99e3eed9508a5b6a6766937a15b3186eb9",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateReleaseUpgradeEvidence,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:584ba6c3f25069a91978c32bc3cf2d1d8a20d1fb1da0e4265b441f7a1d27cd67",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateOperatorU9Schema,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:96c4ef8ffb88bcb8ce85889d42426905ee3cad5cc2d65c7f498fbb2b7b9c4f71",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateOperatorU9Schema,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:d4b8588784db487b928ef99e98e3adeff81f13530ea20375bd25a54a052d3968",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateTransitionTableOnly,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:1c6c1d2002646293e63aa90d258b64270ac35708485f68a35aee0066a527addb",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateSnapshotIdentityComment,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:4acadc866e4214f480492df68dd883af708700b8a4dc0cbc9db6f91b3a7315bf",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateProjectBaselineOptional,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:1b70212548858e70daf7f120a0245d0af93fd3ff1e9cbab48d7dfa271b57f302",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateSnapshotIdentityComment,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:2a8fdd44c39346f1ee7d380b0c1cf0f51fa07b68db396a593446e3029421a23b",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateOperatorU9Schema,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:035e04f2e066eb90c457a0af7440356274551be4abd6496b620879e9d4e3b133",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateSessionWorldScope,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:26a38c2fd4357f538a99cb1b54573f6c2998e19e9a09252e7e9792c45745cec9",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateObservedInstanceBindingLocalRef,
            },
        };

        [[nodiscard]]
        auto upgradeOrVerifyExactDatabaseSchema(sqlite3* database) -> Status
        {
            UF_TRY_VALUE(actual, exactDatabaseSchemaIdentity(database));
            auto const actualIdentity = std::format("sha256:{}", actual.hex());
            if (actualIdentity == k_operatorDatabaseSchemaIdentity)
            {
                return ok();
            }

            auto const migration = std::ranges::find_if(
                k_schemaMigrations,
                [&actualIdentity](SchemaMigration const& candidate)
                {
                    return candidate.sourceIdentity == actualIdentity
                        && candidate.targetIdentity
                            == k_operatorDatabaseSchemaIdentity;
                }
            );
            if (migration == k_schemaMigrations.end())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "Operator database schema identity {} has no registered "
                        "audit-preserving disposition to {}; database left intact",
                        actualIdentity,
                        k_operatorDatabaseSchemaIdentity
                    )
                );
            }
            return migration->apply(database, *migration);
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
        auto activeInstalledGeneration(
            sqlite3* database,
            ContentHash const& compatibleArtifactRootHash
        ) -> Result<uint64>
        {
            UF_TRY_VALUE(
                query,
                prepare(
                    database,
                    "SELECT state.installed_generation FROM runtime_state state "
                    "JOIN runtime_installations installation "
                    "ON installation.installed_generation=state.installed_generation "
                    "AND installation.artifact_root_hash="
                    "state.active_runtime_artifact_root_hash "
                    "WHERE state.singleton=1 "
                    "AND state.active_runtime_artifact_root_hash=?1"
                )
            );
            UF_TRY(bindText(
                database,
                query.get(),
                1,
                compatibleArtifactRootHash.hex()
            ));
            if (sqlite3_step(query.get()) != SQLITE_ROW)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "No active RuntimeArtifact is compatible with the required root"
                );
            }
            return static_cast<uint64>(sqlite3_column_int64(query.get(), 0));
        }

        [[nodiscard]]
        auto requireQuiescentSessionPin(sqlite3* database) -> Status
        {
            UF_TRY_VALUE(
                dispatchQuery,
                prepare(
                    database,
                    "SELECT operation_id FROM dispatches "
                    "WHERE delivery_outcome IS NULL "
                    "ORDER BY operation_id, dispatch_sequence LIMIT 1"
                )
            );
            if (sqlite3_step(dispatchQuery.get()) == SQLITE_ROW)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    std::format(
                        "Session pin refused while Operation {} has a "
                        "dispatch in flight",
                        columnText(dispatchQuery.get(), 0)
                    )
                );
            }

            UF_TRY_VALUE(
                mutationQuery,
                prepare(
                    database,
                    "SELECT operation.operation_id FROM operations operation "
                    "WHERE operation.mutating=1 AND operation.state IN ("
                    "'proposed', 'awaiting_approval', 'ready', "
                    "'needs_revalidation', 'running', 'reconciling', 'ambiguous') "
                    "AND NOT EXISTS(SELECT 1 FROM dispatches dispatch "
                    "WHERE dispatch.operation_id=operation.operation_id) "
                    "ORDER BY operation.operation_id LIMIT 1"
                )
            );
            if (sqlite3_step(mutationQuery.get()) == SQLITE_ROW)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    std::format(
                        "Session pin refused for unterminated mutating "
                        "Operation {}",
                        columnText(mutationQuery.get(), 0)
                    )
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto isReleaseUpgradeSessionPin(
            sqlite3* database,
            ContentHash const& artifactRootHash,
            ContentHash const& projectRegistrationHash,
            std::string_view projectInstanceKey
        ) -> Result<bool>
        {
            UF_TRY_VALUE(
                query,
                prepare(
                    database,
                    "SELECT 1 FROM sessions "
                    "WHERE project_registration_hash=?1 AND project_instance_key=?2 "
                    "AND runtime_artifact_root_hash<>?3 LIMIT 1"
                )
            );
            UF_TRY(bindText(database, query.get(), 1, projectRegistrationHash.hex()));
            UF_TRY(bindText(database, query.get(), 2, projectInstanceKey));
            UF_TRY(bindText(database, query.get(), 3, artifactRootHash.hex()));
            return sqlite3_step(query.get()) == SQLITE_ROW;
        }

        [[nodiscard]]
        auto requireApprovedCapabilityExpansion(
            sqlite3* database,
            ContentHash const& artifactRootHash,
            ContentHash const& projectRegistrationHash,
            std::string_view projectInstanceKey,
            std::vector<std::string> const& controllerCapabilities,
            ContentHash const& capabilityProfileHash
        ) -> Status
        {
            UF_TRY_VALUE(
                priorQuery,
                prepare(
                    database,
                    "SELECT controller_capabilities FROM sessions "
                    "WHERE project_registration_hash=?1 AND project_instance_key=?2 "
                    "AND runtime_artifact_root_hash<>?3 "
                    "ORDER BY session_epoch DESC, session_id LIMIT 1"
                )
            );
            UF_TRY(bindText(
                database,
                priorQuery.get(),
                1,
                projectRegistrationHash.hex()
            ));
            UF_TRY(bindText(database, priorQuery.get(), 2, projectInstanceKey));
            UF_TRY(bindText(database, priorQuery.get(), 3, artifactRootHash.hex()));
            if (sqlite3_step(priorQuery.get()) != SQLITE_ROW)
            {
                return ok();
            }

            UF_TRY_VALUE(
                priorCapabilities,
                readNameArray(columnText(priorQuery.get(), 0))
            );
            auto currentCapabilities = controllerCapabilities;
            std::ranges::sort(currentCapabilities);
            currentCapabilities.erase(
                std::ranges::unique(currentCapabilities).begin(),
                currentCapabilities.end()
            );
            auto const expanded = (
                currentCapabilities.size() > priorCapabilities.size()
                && std::ranges::includes(
                    currentCapabilities,
                    priorCapabilities
                )
            );
            if (!expanded)
            {
                return ok();
            }

            UF_TRY_VALUE(
                approvalQuery,
                prepare(
                    database,
                    "SELECT 1 FROM release_capability_approvals "
                    "WHERE artifact_root_hash=?1 AND capability_profile_hash=?2"
                )
            );
            UF_TRY(bindText(
                database,
                approvalQuery.get(),
                1,
                artifactRootHash.hex()
            ));
            UF_TRY(bindText(
                database,
                approvalQuery.get(),
                2,
                capabilityProfileHash.hex()
            ));
            if (sqlite3_step(approvalQuery.get()) == SQLITE_ROW)
            {
                return ok();
            }

            auto const added = std::ranges::find_if(
                currentCapabilities,
                [&priorCapabilities](std::string const& capability)
                {
                    return !std::ranges::binary_search(
                        priorCapabilities,
                        capability
                    );
                }
            );
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "Session pin refused capability expansion '{}' without "
                    "recorded approval",
                    *added
                )
            );
        }

        [[nodiscard]]
        auto rollbackRuntimeArtifactUpgrade(
            sqlite3* database,
            RuntimeArtifactPin const& attempted,
            RuntimeArtifactPin const& predecessor,
            std::string_view reason
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY_VALUE(
                restoredGeneration,
                checkedSqlIncrement(
                    attempted.installedGeneration,
                    "rollback RuntimeArtifact generation"
                )
            );
            UF_TRY_VALUE(
                installationInsert,
                prepare(
                    database,
                    "INSERT INTO runtime_installations("
                    "installed_generation, artifact_root_hash) VALUES(?1, ?2)"
                )
            );
            UF_TRY(bindInteger(
                database,
                installationInsert.get(),
                1,
                restoredGeneration
            ));
            UF_TRY(bindText(
                database,
                installationInsert.get(),
                2,
                predecessor.artifactRootHash.hex()
            ));
            UF_TRY(expectDone(database, installationInsert.get()));

            UF_TRY_VALUE(
                stateUpdate,
                prepare(
                    database,
                    "UPDATE runtime_state SET installed_generation=?1, "
                    "active_runtime_artifact_root_hash=?2 WHERE singleton=1 "
                    "AND installed_generation=?3 "
                    "AND active_runtime_artifact_root_hash=?4"
                )
            );
            UF_TRY(bindInteger(database, stateUpdate.get(), 1, restoredGeneration));
            UF_TRY(bindText(
                database,
                stateUpdate.get(),
                2,
                predecessor.artifactRootHash.hex()
            ));
            UF_TRY(bindInteger(
                database,
                stateUpdate.get(),
                3,
                attempted.installedGeneration
            ));
            UF_TRY(bindText(
                database,
                stateUpdate.get(),
                4,
                attempted.artifactRootHash.hex()
            ));
            UF_TRY(expectDone(database, stateUpdate.get()));
            if (sqlite3_changes(database) != 1)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "RuntimeArtifact rollback lost its active-generation compare-and-swap"
                );
            }

            UF_TRY_VALUE(
                auditInsert,
                prepare(
                    database,
                    "INSERT INTO runtime_upgrade_failures("
                    "attempted_generation, attempted_artifact_root_hash, "
                    "restored_generation, restored_artifact_root_hash, reason) "
                    "VALUES(?1, ?2, ?3, ?4, ?5)"
                )
            );
            UF_TRY(bindInteger(
                database,
                auditInsert.get(),
                1,
                attempted.installedGeneration
            ));
            UF_TRY(bindText(
                database,
                auditInsert.get(),
                2,
                attempted.artifactRootHash.hex()
            ));
            UF_TRY(bindInteger(database, auditInsert.get(), 3, restoredGeneration));
            UF_TRY(bindText(
                database,
                auditInsert.get(),
                4,
                predecessor.artifactRootHash.hex()
            ));
            UF_TRY(bindText(database, auditInsert.get(), 5, reason));
            UF_TRY(expectDone(database, auditInsert.get()));
            return transaction.commit();
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
        auto isLocalReference(std::string_view value) -> bool
        {
            if (value.empty() || value.size() > 128U)
            {
                return false;
            }
            auto const alphanumeric = [](char character)
            {
                return (character >= 'A' && character <= 'Z')
                    || (character >= 'a' && character <= 'z')
                    || (character >= '0' && character <= '9');
            };
            if (!alphanumeric(value.front()))
            {
                return false;
            }
            return std::ranges::all_of(
                value,
                [alphanumeric](char character)
                {
                    return alphanumeric(character)
                        || character == '.'
                        || character == '_'
                        || character == ':'
                        || character == '-';
                }
            );
        }

        [[nodiscard]]
        auto isNamespacedIdentifier(std::string_view value) -> bool
        {
            auto const alphabetic = [](char character)
            {
                return (character >= 'A' && character <= 'Z')
                    || (character >= 'a' && character <= 'z');
            };
            auto const alphanumeric = [alphabetic](char character)
            {
                return alphabetic(character)
                    || (character >= '0' && character <= '9');
            };
            if (value.empty() || !alphabetic(value.front()))
            {
                return false;
            }
            auto hasSeparator  = false;
            auto startsSegment = false;
            for (auto const character : value)
            {
                if (character == '.')
                {
                    if (startsSegment)
                    {
                        return false;
                    }
                    hasSeparator  = true;
                    startsSegment = true;
                    continue;
                }
                if (startsSegment && !alphanumeric(character))
                {
                    return false;
                }
                if (
                    !alphanumeric(character)
                    && character != '_'
                    && character != '-'
                )
                {
                    return false;
                }
                startsSegment = false;
            }
            return hasSeparator && !startsSegment;
        }

        // A member the schema has already declared required, so its absence
        // would be a defect in this reader rather than in the document. This
        // is the reader for the Operator's OWN stored rows, whose bytes were
        // validated before they were written; a stored envelope missing a
        // member is an internal invariant, not an input to be refused.
        [[nodiscard]]
        auto member(
            json::Value const& object UF_LIFETIME_BOUND,
            std::string_view name
        ) -> json::Value const&
        {
            auto const* const p_member = object.find(name);
            UF_CHECK(p_member != nullptr);
            return *p_member;
        }

        // A member the proposal contract requires, read out of the parsed
        // derive output. The parsed value is authoritative for what was
        // validated: a project may pin a permissive observation schema, so a
        // member the contract requires can be absent from a document that
        // schema stamped. Refusing with the member's name is what keeps that
        // failure closed instead of terminating.
        [[nodiscard]]
        auto checkedMember(
            json::Value const& object UF_LIFETIME_BOUND,
            std::string_view name
        ) -> Result<json::Value const*>
        {
            auto const* const p_member = object.find(name);
            if (p_member == nullptr)
            {
                return fail(
                    ProjectObservationErrorCode::MalformedProposal,
                    "Derived observation output is missing member '"
                        + std::string{name} + "'"
                );
            }
            return p_member;
        }

        [[nodiscard]]
        auto parseProjectToolPreconditionStatus(
            json::Value const& statusValue
        ) -> Result<ProjectToolPreconditionStatus>
        {
            static constexpr auto k_statuses = std::array{
                std::pair{
                    std::string_view{"Known"},
                    ProjectToolPreconditionStatus::Known,
                },
                std::pair{
                    std::string_view{"Unknown"},
                    ProjectToolPreconditionStatus::Unknown,
                },
                std::pair{
                    std::string_view{"Stale"},
                    ProjectToolPreconditionStatus::Stale,
                },
                std::pair{
                    std::string_view{"Conflict"},
                    ProjectToolPreconditionStatus::Conflict,
                },
            };
            if (statusValue.kind() != json::ValueKind::String)
            {
                return fail(
                    ProjectObservationErrorCode::MalformedProposal,
                    "Project tool precondition status is not a string"
                );
            }
            auto const found = std::ranges::find(
                k_statuses,
                statusValue.string(),
                &std::pair<std::string_view, ProjectToolPreconditionStatus>::first
            );
            if (found == k_statuses.end())
            {
                return fail(
                    ProjectObservationErrorCode::MalformedProposal,
                    "Project tool precondition status is outside its wire domain"
                );
            }
            return found->second;
        }

        // The schema owner already parsed and validated the derive output once;
        // this maps the value it retained to the proposal the Operator mints
        // from, with no second parse of the bytes. A member missing from the
        // output or of the wrong kind is a MalformedProposal, the same code the
        // shape checks below report for a proposal that never fits the wire.
        [[nodiscard]]
        auto proposalFromDerived(json::Value const& document)
            -> Result<ProjectObservationProposal>
        {
            if (document.kind() != json::ValueKind::Object)
            {
                return fail(
                    ProjectObservationErrorCode::MalformedProposal,
                    "Derived observation output is not an object"
                );
            }
            auto const* const p_schema = document.find("schema");
            if (
                p_schema == nullptr
                || p_schema->kind() != json::ValueKind::String
                || p_schema->string() != "umbraflow-project-observation-proposal/v1"
            )
            {
                return fail(
                    ProjectObservationErrorCode::MalformedProposal,
                    "Derived observation output is not a project observation proposal"
                );
            }

            UF_TRY_VALUE(
                p_preconditions,
                checkedMember(document, "project_tool_preconditions")
            );
            auto const& preconditions = *p_preconditions;
            if (preconditions.kind() != json::ValueKind::Array)
            {
                return fail(
                    ProjectObservationErrorCode::MalformedProposal,
                    "Project observation proposal tool preconditions are not an array"
                );
            }
            auto projectToolPreconditions = std::vector<ProjectToolPrecondition>{};
            projectToolPreconditions.reserve(preconditions.items().size());
            for (auto const& precondition : preconditions.items())
            {
                if (precondition.kind() != json::ValueKind::Object)
                {
                    return fail(
                        ProjectObservationErrorCode::MalformedProposal,
                        "Project observation proposal tool precondition is not an object"
                    );
                }
                UF_TRY_VALUE(p_name, checkedMember(precondition, "name"));
                auto const& name = *p_name;
                if (name.kind() != json::ValueKind::String)
                {
                    return fail(
                        ProjectObservationErrorCode::MalformedProposal,
                        "Project tool precondition name is not a string"
                    );
                }
                UF_TRY_VALUE(
                    p_status,
                    checkedMember(precondition, "status")
                );
                UF_TRY_VALUE(
                    status,
                    parseProjectToolPreconditionStatus(*p_status)
                );
                projectToolPreconditions.emplace_back(
                    ProjectToolPrecondition{
                        .name   = std::string{name.string()},
                        .status = status,
                    }
                );
            }

            UF_TRY_VALUE(
                p_instances,
                checkedMember(document, "observed_instance_proposals")
            );
            auto const& instances = *p_instances;
            if (instances.kind() != json::ValueKind::Array)
            {
                return fail(
                    ProjectObservationErrorCode::MalformedProposal,
                    "Project observation proposal instances are not an array"
                );
            }
            auto observedInstanceProposals = std::vector<ObservedInstanceProposal>{};
            observedInstanceProposals.reserve(instances.items().size());
            for (auto const& instance : instances.items())
            {
                if (instance.kind() != json::ValueKind::Object)
                {
                    return fail(
                        ProjectObservationErrorCode::MalformedProposal,
                        "Project observation proposal instance is not an object"
                    );
                }
                auto proposal = ObservedInstanceProposal{};
                UF_TRY_VALUE(p_localRef, checkedMember(instance, "local_ref"));
                auto const& localRef = *p_localRef;
                if (localRef.kind() != json::ValueKind::String)
                {
                    return fail(
                        ProjectObservationErrorCode::MalformedProposal,
                        "Observed instance local_ref is not a string"
                    );
                }
                proposal.localRef = std::string{localRef.string()};
                auto const* const p_parent = instance.find("parent_local_ref");
                if (p_parent != nullptr)
                {
                    if (p_parent->kind() != json::ValueKind::String)
                    {
                        return fail(
                            ProjectObservationErrorCode::MalformedProposal,
                            "Observed instance parent_local_ref is not a string"
                        );
                    }
                    proposal.parentLocalRef = std::string{p_parent->string()};
                }
                UF_TRY_VALUE(p_kind, checkedMember(instance, "kind"));
                auto const& kind = *p_kind;
                if (kind.kind() != json::ValueKind::String)
                {
                    return fail(
                        ProjectObservationErrorCode::MalformedProposal,
                        "Observed instance kind is not a string"
                    );
                }
                proposal.kind = std::string{kind.string()};
                UF_TRY_VALUE(
                    p_schemaId,
                    checkedMember(instance, "identity_schema_id")
                );
                auto const& schemaId = *p_schemaId;
                if (schemaId.kind() != json::ValueKind::String)
                {
                    return fail(
                        ProjectObservationErrorCode::MalformedProposal,
                        "Observed instance identity_schema_id is not a string"
                    );
                }
                proposal.identitySchemaId = std::string{schemaId.string()};
                UF_TRY_VALUE(
                    p_basis,
                    checkedMember(instance, "semantic_identity_basis")
                );
                proposal.semanticIdentityBasis = *p_basis;
                UF_TRY_VALUE(
                    p_opaque,
                    checkedMember(instance, "opaque_project_payload")
                );
                proposal.opaqueProjectPayload = *p_opaque;
                observedInstanceProposals.emplace_back(std::move(proposal));
            }

            UF_TRY_VALUE(
                p_canonicalPayload,
                checkedMember(document, "canonical_opaque_payload")
            );
            return ProjectObservationProposal{
                .schema                    = "umbraflow-project-observation-proposal/v1",
                .canonicalOpaquePayload    = *p_canonicalPayload,
                .projectToolPreconditions  = std::move(projectToolPreconditions),
                .observedInstanceProposals = std::move(observedInstanceProposals),
            };
        }

        // Rebuilds the scope a session was pinned under from its three stored
        // columns. The DDL CHECK already guarantees the generation digits and
        // the kind/zero pairing, so a failure here is a defect in the stored
        // tuple rather than in a caller.
        [[nodiscard]]
        auto restoreWorldScopeKind(
            std::string_view wire
        ) -> std::optional<ObservedInstanceWorldScopeKind>
        {
            static constexpr auto k_kinds = std::array{
                std::pair{
                    std::string_view{"account"},
                    ObservedInstanceWorldScopeKind::Account,
                },
                std::pair{
                    std::string_view{"run"},
                    ObservedInstanceWorldScopeKind::Run,
                },
            };
            auto const found = std::ranges::find(
                k_kinds,
                wire,
                &std::pair<std::string_view, ObservedInstanceWorldScopeKind>::first
            );
            if (found == k_kinds.end())
            {
                return std::nullopt;
            }
            return found->second;
        }

        [[nodiscard]]
        auto restoreSessionWorldScope(
            std::string_view kindWire,
            std::string_view scopeId,
            std::string_view generationText
        ) -> Result<ObservedInstanceWorldScope>
        {
            auto const kind = restoreWorldScopeKind(kindWire);
            if (!kind)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "Session world scope kind is outside its stored domain"
                );
            }
            auto generation = uint64{};
            auto const* const begin = std::to_address(generationText.begin());
            auto const* const end   = std::to_address(generationText.end());
            auto const parsed       = std::from_chars(begin, end, generation);
            if (parsed.ec != std::errc{} || parsed.ptr != end)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "Session world scope generation is not a stored non-negative integer"
                );
            }
            auto scopeIdText = std::string{scopeId};
            switch (*kind)
            {
            case ObservedInstanceWorldScopeKind::Account:
                return ObservedInstanceWorldScope::account(
                    std::move(scopeIdText),
                    generation
                );
            case ObservedInstanceWorldScopeKind::Run:
                return ObservedInstanceWorldScope::run(
                    std::move(scopeIdText),
                    generation
                );
            }
            return fail(
                AutomationErrorKind::InternalInvariant,
                "Session world scope kind is outside its stored domain"
            );
        }

        [[nodiscard]]
        auto validateProjectObservationProposalShape(
            ProjectObservationProposal const& proposal
        ) -> Status
        {
            if (proposal.schema != "umbraflow-project-observation-proposal/v1")
            {
                return fail(
                    ProjectObservationErrorCode::MalformedProposal,
                    "Project observation proposal carries the wrong schema tag"
                );
            }
            // The empty name is forbidden, not merely odd: reserveDispatch
            // resolves a step's ui_target_id to the binding's local_ref and the
            // deliver check compares it with the receipt's own target, while
            // the migration sentinel for pre-local_ref bindings is exactly the
            // empty name -- a binding that names nothing could be mistaken for
            // one that names a target.
            for (auto const& instance : proposal.observedInstanceProposals)
            {
                if (
                    !isLocalReference(instance.localRef)
                    || (
                        instance.parentLocalRef.has_value()
                        && !isLocalReference(*instance.parentLocalRef)
                    )
                    || !isNamespacedIdentifier(instance.kind)
                    || instance.identitySchemaId.empty()
                    || instance.identitySchemaId.size() > 512U
                    || !isValidUtf8(instance.identitySchemaId)
                    || instance.semanticIdentityBasis.kind() != json::ValueKind::Object
                )
                {
                    return fail(
                        ProjectObservationErrorCode::MalformedProposal,
                        "Project observation proposal instance is outside its wire shape"
                    );
                }
            }
            for (auto const& precondition : proposal.projectToolPreconditions)
            {
                if (!isNamespacedIdentifier(precondition.name))
                {
                    return fail(
                        ProjectObservationErrorCode::PreconditionNameNotNamespaced,
                        "Project tool precondition name is not namespaced"
                    );
                }
            }
            for (auto const& precondition : proposal.projectToolPreconditions)
            {
                switch (precondition.status)
                {
                case ProjectToolPreconditionStatus::Known:
                case ProjectToolPreconditionStatus::Unknown:
                case ProjectToolPreconditionStatus::Stale:
                case ProjectToolPreconditionStatus::Conflict:
                    break;
                default:
                    return fail(
                        ProjectObservationErrorCode::PreconditionStatusOutsideFactDomain,
                        "Project tool precondition status is outside its four-value domain"
                    );
                }
            }
            return ok();
        }

        [[nodiscard]]
        auto validateProjectObservationProposalRelations(
            ProjectObservationProposal const& proposal
        ) -> Status
        {
            auto preconditionNames = std::set<std::string>{};
            for (auto const& precondition : proposal.projectToolPreconditions)
            {
                if (!preconditionNames.emplace(precondition.name).second)
                {
                    return fail(
                        ProjectObservationErrorCode::DuplicatePreconditionName,
                        "Project observation proposal repeats a precondition name"
                    );
                }
            }

            auto indexes = std::map<std::string, std::size_t>{};
            for (
                auto index = std::size_t{};
                index < proposal.observedInstanceProposals.size();
                ++index
            )
            {
                if (!indexes.emplace(
                    proposal.observedInstanceProposals[index].localRef,
                    index
                ).second)
                {
                    return fail(
                        ProjectObservationErrorCode::DuplicateObservedInstanceLocalRef,
                        "Project observation proposal repeats an instance local_ref"
                    );
                }
            }

            auto parentIndexes = std::vector<std::optional<std::size_t>>{};
            parentIndexes.reserve(proposal.observedInstanceProposals.size());
            for (auto const& instance : proposal.observedInstanceProposals)
            {
                if (!instance.parentLocalRef)
                {
                    parentIndexes.emplace_back(std::nullopt);
                    continue;
                }
                auto const found = indexes.find(*instance.parentLocalRef);
                if (found == indexes.end())
                {
                    return fail(
                        ProjectObservationErrorCode::ObservedInstanceParentMissing,
                        "Observed instance parent_local_ref is absent from its proposal"
                    );
                }
                parentIndexes.emplace_back(found->second);
            }

            enum class VisitState : uint8
            {
                Unvisited,
                Visiting,
                Visited,
            };
            auto states = std::vector<VisitState>(
                parentIndexes.size(),
                VisitState::Unvisited
            );
            auto const containsCycle = [
                &parentIndexes,
                &states
            ](auto const& visit, std::size_t index) -> bool
            {
                switch (states[index])
                {
                case VisitState::Visiting:  return true;
                case VisitState::Visited:   return false;
                case VisitState::Unvisited: break;
                }
                states[index] = VisitState::Visiting;
                if (
                    parentIndexes[index].has_value()
                    && visit(visit, *parentIndexes[index])
                )
                {
                    return true;
                }
                states[index] = VisitState::Visited;
                return false;
            };
            for (
                auto index = std::size_t{};
                index < parentIndexes.size();
                ++index
            )
            {
                if (containsCycle(containsCycle, index))
                {
                    return fail(
                        ProjectObservationErrorCode::ObservedInstanceParentCycle,
                        "Observed instance parent relation contains a cycle"
                    );
                }
            }
            return ok();
        }

        // Every observed instance id a command's canonical arguments spell,
        // across every nesting level. The id format is the ledger's own
        // ("oi1_" plus the random hex the mint drew), so a string carrying the
        // prefix IS an instance id wherever the project put it; the scan is
        // what lets the production entry gate resolve ids the framework does
        // not parse out of project-shaped arguments.
        auto collectObservedInstanceIds(
            json::Value const& value,
            std::vector<std::string>& ids
        ) -> void
        {
            switch (value.kind())
            {
            case json::ValueKind::Object:
                for (auto const& [name, member] : value.members())
                {
                    static_cast<void>(name);
                    collectObservedInstanceIds(member, ids);
                }
                break;
            case json::ValueKind::Array:
                for (auto const& item : value.items())
                {
                    collectObservedInstanceIds(item, ids);
                }
                break;
            case json::ValueKind::String:
                if (value.string().starts_with("oi1_"))
                {
                    ids.emplace_back(value.string());
                }
                break;
            case json::ValueKind::Null:
            case json::ValueKind::Boolean:
            case json::ValueKind::Number:
                break;
            }
        }

        [[nodiscard]]
        auto observedInstanceAuthorityBytes(
            ObservedInstanceContext const& context,
            ObservedInstanceWorldScope const& worldScope,
            ObservedInstanceProposal const& proposal
        ) -> std::string
        {
            auto output = std::string{"{\"identity_schema_id\":"};
            appendJsonString(output, proposal.identitySchemaId);
            output += ",\"kind\":";
            appendJsonString(output, proposal.kind);
            output += ",\"plugin_id\":";
            appendJsonString(output, context.pluginId);
            output += ",\"project_instance_key\":";
            appendJsonString(output, context.projectInstanceKey);
            output += ",\"project_registration_hash\":";
            appendJsonString(output, context.projectRegistrationHash.hex());
            output += ",\"schema\":\"umbraflow-observed-instance-authority-input/v1\"";
            output += ",\"semantic_identity_basis\":";
            output += json::canonicalBytes(proposal.semanticIdentityBasis);
            output += ",\"world_scope\":{\"generation\":";
            output += std::to_string(worldScope.generation());
            output += ",\"kind\":";
            appendJsonString(
                output,
                observedInstanceWorldScopeKindWireName(worldScope.kind())
            );
            output += ",\"scope_id\":";
            appendJsonString(output, worldScope.scopeId());
            output += "}}";
            return output;
        }

        [[nodiscard]]
        auto readObservedInstanceContext(
            sqlite3* database,
            uint64 currentSessionEpoch,
            ControlLease const& lease
        ) -> Result<ObservedInstanceContext>
        {
            UF_TRY_VALUE(
                query,
                prepare(
                    database,
                    "SELECT active_lease.lease_id, active_lease.session_id, "
                    "active_lease.controller_id, active_lease.session_epoch, "
                    "active_lease.fencing_token, active_lease.revision, "
                    "active_lease.capability_profile_hash, session.project_instance_key, "
                    "session.project_registration_hash, registration.plugin_id, "
                    "registration.plugin_hash FROM control_leases active_lease "
                    "JOIN sessions session ON session.session_id=active_lease.session_id "
                    "JOIN project_registrations registration ON "
                    "registration.registration_hash=session.project_registration_hash "
                    "WHERE active_lease.controlled_target_id=?1 AND session.active=1 "
                    "AND session.session_epoch=?2"
                )
            );
            UF_TRY(bindText(database, query.get(), 1, lease.controlledTargetId));
            UF_TRY(bindInteger(database, query.get(), 2, currentSessionEpoch));
            if (sqlite3_step(query.get()) != SQLITE_ROW)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Observed instance operation requires an active control lease"
                );
            }
            auto const matches = columnText(query.get(), 0) == lease.leaseId
                && columnText(query.get(), 1) == lease.sessionId
                && columnText(query.get(), 2) == lease.controllerId
                && static_cast<uint64>(sqlite3_column_int64(query.get(), 3))
                    == lease.sessionEpoch
                && static_cast<uint64>(sqlite3_column_int64(query.get(), 4))
                    == lease.fencingToken
                && static_cast<uint64>(sqlite3_column_int64(query.get(), 5))
                    == lease.revision
                && columnText(query.get(), 6) == lease.capabilityProfileHash.hex()
                && lease.sessionEpoch == currentSessionEpoch;
            if (!matches)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Observed instance control lease was superseded"
                );
            }
            UF_TRY_VALUE(registrationHash, parseHashColumn(columnText(query.get(), 8)));
            return ObservedInstanceContext{
                .pluginId                = columnText(query.get(), 9),
                .pluginHash              = columnText(query.get(), 10),
                .projectRegistrationHash = registrationHash,
                .projectInstanceKey      = columnText(query.get(), 7),
            };
        }

        [[nodiscard]]
        auto mintObservedInstanceBinding(
            sqlite3* database,
            ObservedInstanceContext const& context,
            ObservedInstanceWorldScope const& worldScope,
            std::string const& canonicalAuthority,
            std::string_view localRef
        ) -> Result<std::string>
        {
            UF_TRY_VALUE(
                existing,
                prepare(
                    database,
                    "SELECT observed_instance_id FROM observed_instance_bindings "
                    "WHERE canonical_authority=?1"
                )
            );
            UF_TRY(bindText(database, existing.get(), 1, canonicalAuthority));
            if (sqlite3_step(existing.get()) == SQLITE_ROW)
            {
                return columnText(existing.get(), 0);
            }

            for (;;)
            {
                UF_TRY_VALUE(randomBytes, randomToken(database));
                auto observedInstanceId = std::string{"oi1_"} + randomBytes;
                UF_TRY_VALUE(
                    collision,
                    prepare(
                        database,
                        "SELECT 1 FROM observed_instance_bindings "
                        "WHERE observed_instance_id=?1"
                    )
                );
                UF_TRY(bindText(database, collision.get(), 1, observedInstanceId));
                if (sqlite3_step(collision.get()) == SQLITE_ROW)
                {
                    continue;
                }

                UF_TRY_VALUE(
                    insert,
                    prepare(
                        database,
                        "INSERT INTO observed_instance_bindings("
                        "canonical_authority, observed_instance_id, plugin_id, "
                        "project_registration_hash, project_instance_key, "
                        "world_scope_kind, world_scope_id, world_scope_generation, "
                        "local_ref) "
                        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)"
                    )
                );
                UF_TRY(bindText(database, insert.get(), 1, canonicalAuthority));
                UF_TRY(bindText(database, insert.get(), 2, observedInstanceId));
                UF_TRY(bindText(database, insert.get(), 3, context.pluginId));
                UF_TRY(bindText(
                    database,
                    insert.get(),
                    4,
                    context.projectRegistrationHash.hex()
                ));
                UF_TRY(bindText(database, insert.get(), 5, context.projectInstanceKey));
                UF_TRY(bindText(
                    database,
                    insert.get(),
                    6,
                    observedInstanceWorldScopeKindWireName(worldScope.kind())
                ));
                UF_TRY(bindText(database, insert.get(), 7, worldScope.scopeId()));
                UF_TRY(bindText(
                    database,
                    insert.get(),
                    8,
                    std::to_string(worldScope.generation())
                ));
                UF_TRY(bindText(database, insert.get(), 9, localRef));
                UF_TRY(expectDone(database, insert.get()));
                return observedInstanceId;
            }
        }

        [[nodiscard]]
        auto finalProjectObservationValue(
            json::Value const& canonicalOpaquePayload,
            std::span<ProjectToolPrecondition const> preconditions,
            std::span<ObservedInstance const> instances
        ) -> json::Value
        {
            auto preconditionValues = std::vector<json::Value>{};
            preconditionValues.reserve(preconditions.size());
            for (auto const& precondition : preconditions)
            {
                preconditionValues.emplace_back(json::Value::ofObject({
                    json::Member{"name", json::Value::ofString(precondition.name)},
                    json::Member{
                        "status",
                        json::Value::ofString(std::string{
                            projectToolPreconditionStatusWireName(precondition.status)
                        }),
                    },
                }));
            }

            auto instanceValues = std::vector<json::Value>{};
            instanceValues.reserve(instances.size());
            for (auto const& instance : instances)
            {
                auto members = std::vector<json::Member>{};
                members.emplace_back(
                    "observed_instance_id",
                    json::Value::ofString(instance.observedInstanceId.value())
                );
                if (instance.parentObservedInstanceId)
                {
                    members.emplace_back(
                        "parent_observed_instance_id",
                        json::Value::ofString(
                            instance.parentObservedInstanceId->value()
                        )
                    );
                }
                members.emplace_back("kind", json::Value::ofString(instance.kind));
                members.emplace_back(
                    "opaque_project_payload",
                    instance.opaqueProjectPayload
                );
                instanceValues.emplace_back(json::Value::ofObject(std::move(members)));
            }

            return json::Value::ofObject({
                json::Member{
                    "canonical_opaque_payload",
                    canonicalOpaquePayload,
                },
                json::Member{
                    "observed_instances",
                    json::Value::ofArray(std::move(instanceValues)),
                },
                json::Member{
                    "project_tool_preconditions",
                    json::Value::ofArray(std::move(preconditionValues)),
                },
                json::Member{
                    "schema",
                    json::Value::ofString(std::string{ProjectObservation::schema()}),
                },
            });
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
                schemaObjectCount,
                readDatabaseInteger(
                    database,
                    "SELECT COUNT(*) FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%'"
                )
            );
            if (schemaObjectCount != 0U)
            {
                return upgradeOrVerifyExactDatabaseSchema(database);
            }

            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(execute(
                database,
                R"sql(
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
                        UNIQUE(installed_generation, artifact_root_hash)
                    ) STRICT;

)sql"
                R"sql(

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

)sql"
                R"sql(
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
                    CREATE TABLE IF NOT EXISTS snapshots(
                        -- token and snapshot_revision are deliberately outside
                        -- canonical_parts: they name the stored row rather than
                        -- the capture. observation_id remains inside and names
                        -- the capture, so recapturing an identical world moves
                        -- identity_hash while decision_basis_hash stays stable.
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
                // One statement sequence, three literals: MSVC caps a single
                // string literal and the schema outgrew it here. Adjacent
                // literals concatenate before anything reads them, so the SQL
                // text -- and therefore k_operatorDatabaseSchemaIdentity, which
                // covers the STORED DDL -- is byte-identical to the unsplit
                // block. The seam must add no character of its own: it sits
                // between a newline and a newline, and it moves to wherever the
                // cap requires without moving the fingerprint, because the two
                // halves concatenate to the same bytes wherever it sits.
                R"sql(

                    -- operation_id is the primary key, so a plan freezes once
                    -- and a second freeze is a constraint violation rather than
                    -- a policy check. There is no update path and none may be
                    -- added. required_approvals is the JCS array of approver
                    -- capabilities the pinned PolicyArtifact ruled must sign
                    -- this plan, and policy_hash names the artifact that ruled
                    -- them; issueApproval reads both, so neither is a column
                    -- nothing keeps true.
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
                        policy_hash TEXT NOT NULL,
                        required_approvals TEXT NOT NULL,
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
                        -- The capability the approver presented, matched
                        -- against the plan's own required_approvals. A hash of
                        -- an unnamed profile could not be matched against
                        -- anything, so an approval was recorded rather than
                        -- ruled.
                        approver_capability TEXT NOT NULL,
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
                    -- reason journal_events carries. contract-state-s06 binds
                    -- this column set to the schema's required list.
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

                )sql"
            ));
            UF_TRY(execute(database, k_observedInstanceBindingsDdl));
            UF_TRY(execute(database, k_sessionsDdl));
            UF_TRY(execute(database, k_oneActiveWriteSessionIndexDdl));
            UF_TRY(execute(database, k_projectRegistrationsDdl));
            UF_TRY(execute(database, k_projectInstancesDdl));
            UF_TRY(execute(database, k_ledgerEventsDdl));
            UF_TRY(execute(database, k_sessionPoliciesDdl));
            UF_TRY(execute(database, k_availabilityHeadsDdl));
            UF_TRY(execute(database, k_schemaIdentityTransitionsDdl));
            UF_TRY(execute(database, k_releaseCapabilityApprovalsDdl));
            UF_TRY(execute(database, k_runtimeUpgradeFailuresDdl));
            UF_TRY(verifyExactDatabaseSchema(database));
            return transaction.commit();
        }

        [[nodiscard]]
        auto pathToUtf8(std::filesystem::path const& path) -> std::string
        {
            auto const encoded = path.generic_u8string();
            return std::string{encoded.begin(), encoded.end()};
        }

        struct ReadOnlyOperatorLayout final
        {
            Database              database{};
            std::filesystem::path runtimeArtifactRoot{};
        };

        [[nodiscard]]
        auto openReadOnlyOperatorLayout(
            std::filesystem::path const& runtimeDirectory
        ) -> Result<ReadOnlyOperatorLayout>
        {
            if (runtimeDirectory.empty())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Operator runtime directory must not be empty"
                );
            }

            // A reader checks the existing layout and cannot bootstrap one.
            UF_TRY(requirePlainDirectory(runtimeDirectory, "Operator runtime root"));
            auto const runtimeArtifactRoot = runtimeDirectory / "runtime-artifacts";
            UF_TRY(requirePlainDirectory(
                runtimeArtifactRoot,
                "Production RuntimeArtifact root"
            ));

            auto const databasePath   = runtimeDirectory / "operator-runtime.sqlite";
            auto error                = std::error_code{};
            auto const databaseStatus = std::filesystem::symlink_status(
                databasePath,
                error
            );
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

            // The SQLite capability makes the separation executable: adding a
            // write to this path yields SQLITE_READONLY rather than changing
            // the ledger.
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

            // No busy timeout: a live Coordinator holds an exclusive lock, so
            // a read-only command refuses immediately instead of stalling.
            UF_TRY(execute(database.get(), "PRAGMA trusted_schema=OFF"));
            UF_TRY(verifyExactDatabaseSchema(database.get()));
            return ReadOnlyOperatorLayout{
                .database            = std::move(database),
                .runtimeArtifactRoot = runtimeArtifactRoot,
            };
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

        constexpr auto k_restartRecoveryReason = std::string_view{
            "operator restart found this dispatch unanswered"
        };

        [[nodiscard]]
        auto appendLedgerEvent(
            sqlite3* database,
            uint64 sessionEpoch,
            std::string_view controlledTargetId,
            LedgerEventKind kind,
            std::string_view subjectId,
            std::optional<std::string_view> detail = std::nullopt
        ) -> Status;

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
                "SELECT o.operation_id, o.revision, d.dispatch_sequence, "
                "session.session_epoch, o.controlled_target_id "
                "FROM operations o JOIN dispatches d ON d.operation_id=o.operation_id "
                "JOIN sessions session ON session.session_id=o.session_id "
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

            auto pending = std::vector<
                std::tuple<std::string, uint64, uint64, uint64, std::string>
            >{};
            auto queryStep = sqlite3_step(query.get());
            while (queryStep == SQLITE_ROW)
            {
                pending.emplace_back(
                    columnText(query.get(), 0),
                    static_cast<uint64>(sqlite3_column_int64(query.get(), 1)),
                    static_cast<uint64>(sqlite3_column_int64(query.get(), 2)),
                    static_cast<uint64>(sqlite3_column_int64(query.get(), 3)),
                    columnText(query.get(), 4)
                );
                queryStep = sqlite3_step(query.get());
            }
            if (queryStep != SQLITE_DONE)
            {
                return databaseFailure(database, "could not scan pending dispatches");
            }

            for (
                auto const& [
                    operationId,
                    revision,
                    dispatchSequence,
                    sessionEpoch,
                    targetId
                ] : pending
            )
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
                UF_TRY(appendLedgerEvent(
                    database,
                    sessionEpoch,
                    targetId,
                    LedgerEventKind::DeliveryOutcomeRecorded,
                    operationId,
                    deliveryOutcomeWireName(task::DeliveryOutcome::TransportUnknown)
                ));
                UF_TRY(appendLedgerEvent(
                    database,
                    sessionEpoch,
                    targetId,
                    LedgerEventKind::OperationStateChanged,
                    operationId,
                    operationStateWireName(OperationState::Reconciling)
                ));
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
            case LedgerEventKind::OperationStateChanged: return "operation_state_changed";
            case LedgerEventKind::DeliveryOutcomeRecorded:
                return "delivery_outcome_recorded";
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
                LedgerEventKind::OperationStateChanged,
                LedgerEventKind::DeliveryOutcomeRecorded,
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
        auto parseDeliveryOutcome(std::string_view value) -> Result<task::DeliveryOutcome>
        {
            constexpr auto outcomes = std::array{
                task::DeliveryOutcome::NotDelivered,
                task::DeliveryOutcome::Delivered,
                task::DeliveryOutcome::TransportUnknown,
            };
            auto const match = std::ranges::find_if(
                outcomes,
                [value](task::DeliveryOutcome candidate)
                {
                    return deliveryOutcomeWireName(candidate) == value;
                }
            );
            if (match == outcomes.end())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format("Unknown delivery outcome: {}", value)
                );
            }
            return *match;
        }

        [[nodiscard]]
        auto parseLedgerEventDetail(
            sqlite3_stmt* row,
            int column,
            LedgerEventKind kind
        ) -> Result<LedgerEventDetail>
        {
            switch (kind)
            {
            case LedgerEventKind::OperationStateChanged:
            {
                if (sqlite3_column_type(row, column) == SQLITE_NULL)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "Operation state event is missing its state"
                    );
                }
                UF_TRY_VALUE(state, parseOperationState(columnText(row, column)));
                return LedgerEventDetail{state};
            }
            case LedgerEventKind::DeliveryOutcomeRecorded:
            {
                if (sqlite3_column_type(row, column) == SQLITE_NULL)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "Delivery outcome event is missing its outcome"
                    );
                }
                UF_TRY_VALUE(outcome, parseDeliveryOutcome(columnText(row, column)));
                return LedgerEventDetail{outcome};
            }
            case LedgerEventKind::OperationCreated:
            case LedgerEventKind::ControlTransitioned:
            case LedgerEventKind::ExternalInputDetected:
                if (sqlite3_column_type(row, column) != SQLITE_NULL)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "Ledger event kind must not carry a detail"
                    );
                }
                return LedgerEventDetail{std::monostate{}};
            }

            UF_UNREACHABLE_MSG("Unknown LedgerEventKind value");
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

        // Keep a bounded working set without breaking the snapshot join. An
        // Operation makes its snapshot an audit dependency, and a retained
        // snapshot makes its ProjectObservation a composition dependency; both
        // exceptions are expressed by the NOT EXISTS clauses rather than by a
        // second lifetime flag that could disagree with the foreign keys.
        [[nodiscard]]
        auto pruneSnapshotHistory(
            sqlite3* database,
            std::string_view sessionId,
            std::string_view pluginId,
            std::string_view projectInstanceKey
        ) -> Status
        {
            UF_TRY_VALUE(
                snapshotPrune,
                prepare(
                    database,
                    "DELETE FROM snapshots WHERE session_id=?1 "
                    "AND token NOT IN (SELECT token FROM snapshots "
                    "WHERE session_id=?1 ORDER BY snapshot_revision DESC LIMIT ?2) "
                    "AND NOT EXISTS(SELECT 1 FROM operations operation "
                    "WHERE operation.snapshot_token=snapshots.token)"
                )
            );
            UF_TRY(bindText(database, snapshotPrune.get(), 1, sessionId));
            UF_TRY(bindInteger(
                database,
                snapshotPrune.get(),
                2,
                k_retainedSnapshotHeads
            ));
            UF_TRY(expectDone(database, snapshotPrune.get()));

            UF_TRY_VALUE(
                observationPrune,
                prepare(
                    database,
                    "DELETE FROM project_observations WHERE plugin_id=?1 "
                    "AND project_instance_key=?2 AND revision NOT IN ("
                    "SELECT revision FROM project_observations WHERE plugin_id=?1 "
                    "AND project_instance_key=?2 ORDER BY revision DESC LIMIT ?3) "
                    "AND NOT EXISTS(SELECT 1 FROM snapshots snapshot "
                    "WHERE snapshot.plugin_id=project_observations.plugin_id "
                    "AND snapshot.project_instance_key="
                    "project_observations.project_instance_key "
                    "AND snapshot.project_observation_revision="
                    "project_observations.revision)"
                )
            );
            UF_TRY(bindText(database, observationPrune.get(), 1, pluginId));
            UF_TRY(bindText(
                database,
                observationPrune.get(),
                2,
                projectInstanceKey
            ));
            UF_TRY(bindInteger(
                database,
                observationPrune.get(),
                3,
                k_retainedObservationHeads
            ));
            return expectDone(database, observationPrune.get());
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
            std::string_view subjectId,
            std::optional<std::string_view> detail
        ) -> Status
        {
            UF_TRY_VALUE(
                insert,
                prepare(
                    database,
                    "INSERT INTO ledger_events(session_epoch, controlled_target_id, "
                    "kind, subject_id, detail) VALUES(?1, ?2, ?3, ?4, ?5)"
                )
            );
            UF_TRY(bindInteger(database, insert.get(), 1, sessionEpoch));
            UF_TRY(bindText(database, insert.get(), 2, controlledTargetId));
            UF_TRY(bindText(database, insert.get(), 3, ledgerEventWireName(kind)));
            UF_TRY(bindText(database, insert.get(), 4, subjectId));
            if (detail)
            {
                UF_TRY(bindText(database, insert.get(), 5, *detail));
            }
            else if (sqlite3_bind_null(insert.get(), 5) != SQLITE_OK)
            {
                return databaseFailure(database, "could not bind ledger event detail");
            }
            UF_TRY(expectDone(database, insert.get()));
            UF_TRY_VALUE(
                prune,
                prepare(
                    database,
                    "DELETE FROM ledger_events WHERE sequence NOT IN ("
                    "SELECT sequence FROM ledger_events "
                    "ORDER BY sequence DESC LIMIT ?1)"
                )
            );
            UF_TRY(bindInteger(database, prune.get(), 1, k_retainedLedgerEvents));
            return expectDone(database, prune.get());
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
        // and resumeSession changes none of those pinned columns, so comparing
        // them back would compare a value against the column it came from. Only
        // bindController can mint a binding, so there is no forged one for such
        // a comparison to catch.
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

        // OP:`SnapshotParts`, all seventeen members in JCS order. It is the exact
        // text stored as snapshots.canonical_parts, so identity_hash is
        // recomputable from the row and a test can falsify the derivation
        // rather than only compare it against itself.
        struct SnapshotPartsInputs final
        {
            ContentHash      decisionBasisHash;
            ContentHash      projectObservationHash;
            ContentHash      projectStateHash;
            ContentHash      policyHash;
            ContentHash      sessionManifestHash;
            ContentHash      stateResolutionHash;
            std::string_view availableToolsJcs{};
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
            output += ",\"available_tools\":";
            output += parts.availableToolsJcs;
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
            output += ",\"policy_hash\":";
            appendHashMember(output, parts.policyHash);
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
                "INSERT INTO runtime_installations(installed_generation, artifact_root_hash) "
                "VALUES(?1, ?2)"
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

    auto OperatorCoordinator::activeRuntimeArtifactPin()
        -> Result<RuntimeArtifactPin>
    {
        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                "SELECT installed_generation, active_runtime_artifact_root_hash "
                "FROM runtime_state WHERE singleton=1 "
                "AND active_runtime_artifact_root_hash IS NOT NULL"
            )
        );
        if (sqlite3_step(query.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "No RuntimeArtifact release is active"
            );
        }
        auto const generation = static_cast<uint64>(
            sqlite3_column_int64(query.get(), 0)
        );
        auto encodedRoot = std::string{"sha256:"};
        encodedRoot += columnText(query.get(), 1);
        UF_TRY_VALUE(rootHash, ContentHash::parse(encodedRoot));
        return RuntimeArtifactPin{
            .installedGeneration = generation,
            .artifactRootHash    = rootHash,
        };
    }

    auto OperatorCoordinator::approveReleaseCapabilities(
        ReleaseCapabilityApproval const& approval
    ) -> Status
    {
        for (auto const& capability : approval.controllerCapabilities)
        {
            UF_TRY(requireName(capability, "approved release capability"));
        }
        auto const capabilities = canonicalNameArray(
            approval.controllerCapabilities
        );
        UF_TRY_VALUE(
            capabilityProfileHash,
            sha256(std::as_bytes(std::span{capabilities}))
        );
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            insert,
            prepare(
                m_impl->database.get(),
                "INSERT OR IGNORE INTO release_capability_approvals("
                "artifact_root_hash, capability_profile_hash, "
                "controller_capabilities, evidence_hash, session_epoch) "
                "VALUES(?1, ?2, ?3, ?4, ?5)"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            1,
            approval.artifactRootHash.hex()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            2,
            capabilityProfileHash.hex()
        ));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 3, capabilities));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            4,
            approval.evidenceHash.hex()
        ));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            insert.get(),
            5,
            m_impl->sessionEpoch
        ));
        UF_TRY(expectDone(m_impl->database.get(), insert.get()));

        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                "SELECT controller_capabilities, evidence_hash "
                "FROM release_capability_approvals "
                "WHERE artifact_root_hash=?1 AND capability_profile_hash=?2"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            query.get(),
            1,
            approval.artifactRootHash.hex()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            query.get(),
            2,
            capabilityProfileHash.hex()
        ));
        if (
            sqlite3_step(query.get()) != SQLITE_ROW
            || columnText(query.get(), 0) != capabilities
            || columnText(query.get(), 1) != approval.evidenceHash.hex()
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Release capability profile already names different approval evidence"
            );
        }
        return transaction.commit();
    }

    auto OperatorCoordinator::upgradeRuntimeArtifactAndPinSession(
        RuntimeArtifactInstallRequest const& installation,
        SessionPin const& pin,
        SessionManifest const& manifest,
        std::optional<AgentProfile> const& agentProfile
    ) -> Status
    {
        UF_TRY_VALUE(predecessor, activeRuntimeArtifactPin());
        auto installed = installRuntimeArtifact(installation);
        if (!installed)
        {
            return std::unexpected{std::move(installed).error()};
        }
        auto const attempted = RuntimeArtifactPin{
            .installedGeneration = installed->installedGeneration(),
            .artifactRootHash    = installed->rootHash(),
        };
        auto pinned = pinSession(pin, manifest, agentProfile);
        if (pinned)
        {
            return ok();
        }

        auto failure      = std::move(pinned).error();
        auto const reason = std::string{failure.message()};
        UF_TRY(rollbackRuntimeArtifactUpgrade(
            m_impl->database.get(),
            attempted,
            predecessor,
            reason
        ));
        return std::unexpected{std::move(failure)};
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
        UF_TRY_VALUE(layout, openReadOnlyOperatorLayout(runtimeDirectory));
        UF_TRY(requireInstalledArtifactPin(
            layout.database.get(),
            installedGeneration,
            artifactRootHash
        ));
        UF_TRY_VALUE(
            artifact,
            detail::openProductionRuntimeArtifact(
                layout.runtimeArtifactRoot,
                artifactRootHash
            )
        );
        return task::InstalledRuntimeArtifact{
            std::move(artifact),
            installedGeneration,
        };
    }

    auto OperatorCoordinator::readActiveInstalledRuntimeArtifact(
        std::filesystem::path const& runtimeDirectory,
        ContentHash const& compatibleArtifactRootHash
    ) -> Result<task::InstalledRuntimeArtifact>
    {
        UF_TRY_VALUE(layout, openReadOnlyOperatorLayout(runtimeDirectory));
        UF_TRY_VALUE(
            installedGeneration,
            activeInstalledGeneration(
                layout.database.get(),
                compatibleArtifactRootHash
            )
        );
        UF_TRY_VALUE(
            artifact,
            detail::openProductionRuntimeArtifact(
                layout.runtimeArtifactRoot,
                compatibleArtifactRootHash
            )
        );
        return task::InstalledRuntimeArtifact{
            std::move(artifact),
            installedGeneration,
        };
    }

    auto OperatorCoordinator::openActiveInstalledRuntimeArtifact(
        ContentHash const& compatibleArtifactRootHash
    ) -> Result<task::InstalledRuntimeArtifact>
    {
        UF_TRY_VALUE(
            installedGeneration,
            activeInstalledGeneration(
                m_impl->database.get(),
                compatibleArtifactRootHash
            )
        );
        return openInstalledRuntimeArtifact(
            installedGeneration,
            compatibleArtifactRootHash
        );
    }

    auto OperatorCoordinator::recoverUncertainDispatches() -> Result<uint64>
    {
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            resolved,
            resolveUnansweredDispatches(
                m_impl->database.get(),
                {},
                k_restartRecoveryReason
            )
        );
        UF_TRY(transaction.commit());
        return resolved;
    }

    auto OperatorCoordinator::recoveredUncertainDispatches()
        -> Result<std::vector<RecoveredUncertainDispatch>>
    {
        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                "SELECT operation.operation_id, operation.revision, state.revision, "
                "(SELECT dispatch.delivery_reason FROM dispatches dispatch "
                "WHERE dispatch.operation_id=operation.operation_id "
                "AND dispatch.delivery_outcome='transport_unknown' "
                "ORDER BY dispatch.dispatch_sequence DESC LIMIT 1) "
                "FROM operations operation "
                "JOIN project_state state "
                "ON state.plugin_id=operation.plugin_id "
                "AND state.project_instance_key=operation.project_instance_key "
                "WHERE operation.state='reconciling' AND EXISTS("
                "SELECT 1 FROM dispatches dispatch "
                "WHERE dispatch.operation_id=operation.operation_id "
                "AND dispatch.delivery_outcome='transport_unknown') "
                "ORDER BY operation.operation_id"
            )
        );
        auto recovered = std::vector<RecoveredUncertainDispatch>{};
        auto step      = sqlite3_step(query.get());
        while (step == SQLITE_ROW)
        {
            recovered.emplace_back(RecoveredUncertainDispatch{
                .operationId    = columnText(query.get(), 0),
                .deliveryReason = columnText(query.get(), 3),
                .expectedOperationRevision = static_cast<uint64>(
                    sqlite3_column_int64(query.get(), 1)
                ),
                .expectedProjectStateRevision = static_cast<uint64>(
                    sqlite3_column_int64(query.get(), 2)
                ),
            });
            step = sqlite3_step(query.get());
        }
        if (step != SQLITE_DONE)
        {
            return databaseFailure(
                m_impl->database.get(),
                "could not scan restart reconciliation work"
            );
        }
        return recovered;
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
                "(registration_hash, plugin_id, plugin_hash, canonical_manifest) "
                "VALUES(?1, ?2, ?3, ?4)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 1, registrationHash.hex()));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 2, pluginId));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 3, registration.pluginHash().hex()));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 4, canonicalManifest));
        UF_TRY(expectDone(m_impl->database.get(), insert.get()));

        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                "SELECT plugin_id, plugin_hash, canonical_manifest "
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
            || columnText(query.get(), 2) != canonicalManifest
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
        if (baseline.entry.has_value())
        {
            UF_TRY(requireName(baseline.eventId, "baseline event_id"));
            if (
                baseline.entry->projectRegistrationHash() != registration.hash()
                || baseline.entry->projectRegistrationHash()
                    != plugin.projectRegistrationHash()
            )
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Project baseline Journal data does not match the exact registration"
                );
            }
            if (
                baseline.entry->namespacedEventType()
                != registration.baselineEventType()
            )
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Project baseline event type does not match ProjectRegistration"
                );
            }
        }
        else if (!baseline.eventId.empty())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "A ProjectInstance with no baseline must not name a baseline event_id"
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
        // Initial state is always the reduction of the complete Journal prefix.
        // That prefix contains the declared baseline entry or is empty; no
        // synthetic Journal event stands in for absence.
        auto baselineEvents = std::vector<JournalAppend>{};
        if (baseline.entry.has_value())
        {
            baselineEvents.emplace_back(JournalAppend{
                .eventId = baseline.eventId,
                .entry   = *baseline.entry,
            });
        }
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
                "SELECT plugin_id, plugin_hash, canonical_manifest "
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
            || columnText(registrationQuery.get(), 2) != registration.canonicalJcs()
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
                "SELECT project_registration_hash, baseline_event_id "
                "FROM project_instances WHERE plugin_id=?1 "
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
            if (
                !baseline.entry.has_value()
                && columnText(existing.get(), 0) == registration.hash().hex()
                && sqlite3_column_type(existing.get(), 1) == SQLITE_NULL
            )
            {
                return transaction.commit();
            }
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
        if (baseline.entry.has_value())
        {
            UF_TRY(bindText(
                m_impl->database.get(),
                instanceInsert.get(),
                4,
                baseline.eventId
            ));
        }
        else if (sqlite3_bind_null(instanceInsert.get(), 4) != SQLITE_OK)
        {
            return databaseFailure(
                m_impl->database.get(),
                "could not bind absent ProjectInstance baseline"
            );
        }
        UF_TRY(expectDone(m_impl->database.get(), instanceInsert.get()));

        if (baseline.entry.has_value())
        {
            UF_TRY_VALUE(
                eventInsert,
                prepare(
                    m_impl->database.get(),
                    "INSERT INTO journal_events(event_id, plugin_id, project_instance_key, "
                    "sequence, prior_project_state_revision, session_manifest_hash, "
                    "operation_id, namespaced_event_type, payload_schema_hash, "
                    "opaque_project_payload, provenance) "
                    "VALUES(?1, ?2, ?3, 0, NULL, ?4, NULL, ?5, ?6, ?7, ?8)"
                )
            );
            UF_TRY(bindText(
                m_impl->database.get(),
                eventInsert.get(),
                1,
                baseline.eventId
            ));
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
                baseline.entry->namespacedEventType()
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                eventInsert.get(),
                6,
                baseline.entry->payloadSchemaHash().hex()
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                eventInsert.get(),
                7,
                baseline.entry->payload().bytes()
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                eventInsert.get(),
                8,
                baseline.entry->provenance().bytes()
            ));
            UF_TRY(expectDone(m_impl->database.get(), eventInsert.get()));
        }

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
        for (auto const& capability : pin.controllerCapabilities)
        {
            UF_TRY(requireName(capability, "controller capability"));
        }

        // The profile hash is the sha256 of the set, derived here and never
        // stated: a caller that could name it could pin a session whose
        // capability hash and capability set were about different things.
        auto const capabilities = canonicalNameArray(pin.controllerCapabilities);
        UF_TRY_VALUE(
            capabilityProfileHash,
            sha256(std::as_bytes(std::span{capabilities}))
        );

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
            releaseUpgrade,
            isReleaseUpgradeSessionPin(
                m_impl->database.get(),
                runtimeArtifactRootHash,
                pin.projectRegistrationHash,
                pin.projectInstanceKey
            )
        );
        if (releaseUpgrade)
        {
            UF_TRY(requireQuiescentSessionPin(m_impl->database.get()));
            UF_TRY(requireApprovedCapabilityExpansion(
                m_impl->database.get(),
                runtimeArtifactRootHash,
                pin.projectRegistrationHash,
                pin.projectInstanceKey,
                pin.controllerCapabilities,
                capabilityProfileHash
            ));
        }

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
                "project_registration_hash, controller_capabilities, "
                "capability_profile_hash, session_epoch, "
                "controlled_target_id, project_instance_key, mode, controller_kind, "
                "world_scope_kind, world_scope_id, world_scope_generation, "
                "active) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, "
                "?15, ?16, ?17, 1)"
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
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 8, capabilities));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            9,
            capabilityProfileHash.hex()
        ));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 10, m_impl->sessionEpoch));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 11, pin.controlledTargetId));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 12, pin.projectInstanceKey));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            13,
            sessionModeWireName(pin.mode)
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            14,
            controllerKindWireName(pin.kind)
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            15,
            observedInstanceWorldScopeKindWireName(pin.worldScope.kind())
        ));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 16, pin.worldScope.scopeId()));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            17,
            std::to_string(pin.worldScope.generation())
        ));
        UF_TRY(expectDone(m_impl->database.get(), insert.get()));

        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                "SELECT authenticated_controller_id, idempotency_namespace, manifest_hash, "
                "runtime_artifact_root_hash, installed_generation, "
                "project_registration_hash, controller_capabilities, "
                "capability_profile_hash, session_epoch, "
                "controlled_target_id, project_instance_key, mode, controller_kind, "
                "world_scope_kind, world_scope_id, world_scope_generation, "
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
            && columnText(query.get(), 6) == capabilities
            && columnText(query.get(), 7) == capabilityProfileHash.hex()
            && static_cast<uint64>(sqlite3_column_int64(query.get(), 8))
                == m_impl->sessionEpoch
            && columnText(query.get(), 9) == pin.controlledTargetId
            && columnText(query.get(), 10) == pin.projectInstanceKey
            && columnText(query.get(), 11) == sessionModeWireName(pin.mode)
            && columnText(query.get(), 12) == controllerKindWireName(pin.kind)
            && columnText(query.get(), 13)
                == observedInstanceWorldScopeKindWireName(pin.worldScope.kind())
            && columnText(query.get(), 14) == pin.worldScope.scopeId()
            && columnText(query.get(), 15) == std::to_string(pin.worldScope.generation())
            && sqlite3_column_int(query.get(), 16) == 1;
        if (!matches)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "session_id already names a different immutable session tuple"
            );
        }

        UF_TRY_VALUE(
            policyInsert,
            prepare(
                m_impl->database.get(),
                "INSERT OR IGNORE INTO session_policies(session_id, policy_hash) "
                "VALUES(?1, ?2)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), policyInsert.get(), 1, pin.sessionId));
        UF_TRY(bindText(
            m_impl->database.get(),
            policyInsert.get(),
            2,
            manifest.policyArtifactHash().hex()
        ));
        UF_TRY(expectDone(m_impl->database.get(), policyInsert.get()));
        UF_TRY_VALUE(
            policyQuery,
            prepare(
                m_impl->database.get(),
                "SELECT policy_hash FROM session_policies WHERE session_id=?1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), policyQuery.get(), 1, pin.sessionId));
        if (
            sqlite3_step(policyQuery.get()) != SQLITE_ROW
            || columnText(policyQuery.get(), 0) != manifest.policyArtifactHash().hex()
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "session_id already names a different policy artifact"
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

    auto OperatorCoordinator::resumeSession(
        SessionResume const& resume,
        SessionManifest const& manifest
    ) -> Result<ControllerBinding>
    {
        UF_TRY(requireName(
            resume.authenticatedControllerId,
            "authenticated_controller_id"
        ));
        UF_TRY(requireName(resume.controlledTargetId, "controlled_target_id"));
        if (controllerProfile(resume.kind).budgetsRequired)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "A budgeted Agent session cannot resume across a process epoch"
            );
        }

        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            installedGeneration,
            activeInstalledGeneration(
                m_impl->database.get(),
                manifest.runtimeModelArtifactRootHash()
            )
        );
        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                "SELECT session_id, session_epoch FROM sessions "
                "WHERE authenticated_controller_id=?1 AND manifest_hash=?2 "
                "AND runtime_artifact_root_hash=?3 "
                "AND project_registration_hash=?4 "
                "AND controlled_target_id=?5 AND mode=?6 "
                "AND controller_kind=?7 AND active=0 "
                "ORDER BY session_epoch DESC, session_id"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            query.get(),
            1,
            resume.authenticatedControllerId
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            query.get(),
            2,
            manifest.hash().hex()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            query.get(),
            3,
            manifest.runtimeModelArtifactRootHash().hex()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            query.get(),
            4,
            manifest.projectRegistrationHash().hex()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            query.get(),
            5,
            resume.controlledTargetId
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            query.get(),
            6,
            sessionModeWireName(resume.mode)
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            query.get(),
            7,
            controllerKindWireName(resume.kind)
        ));
        if (sqlite3_step(query.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "No compatible prior session is available to resume"
            );
        }
        auto const sessionId = columnText(query.get(), 0);
        auto const priorEpoch = static_cast<uint64>(
            sqlite3_column_int64(query.get(), 1)
        );
        auto const second = sqlite3_step(query.get());
        if (
            second == SQLITE_ROW
            && static_cast<uint64>(sqlite3_column_int64(query.get(), 1))
                == priorEpoch
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "More than one most-recent compatible prior session exists"
            );
        }
        if (second != SQLITE_ROW && second != SQLITE_DONE)
        {
            return databaseFailure(
                m_impl->database.get(),
                "could not select a prior session to resume"
            );
        }

        UF_TRY_VALUE(
            policyInsert,
            prepare(
                m_impl->database.get(),
                "INSERT OR IGNORE INTO session_policies(session_id, policy_hash) "
                "VALUES(?1, ?2)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), policyInsert.get(), 1, sessionId));
        UF_TRY(bindText(
            m_impl->database.get(),
            policyInsert.get(),
            2,
            manifest.policyArtifactHash().hex()
        ));
        UF_TRY(expectDone(m_impl->database.get(), policyInsert.get()));
        UF_TRY_VALUE(
            policyQuery,
            prepare(
                m_impl->database.get(),
                "SELECT policy_hash FROM session_policies WHERE session_id=?1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), policyQuery.get(), 1, sessionId));
        if (
            sqlite3_step(policyQuery.get()) != SQLITE_ROW
            || columnText(policyQuery.get(), 0) != manifest.policyArtifactHash().hex()
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Prior session policy does not match the supplied SessionManifest"
            );
        }

        UF_TRY_VALUE(
            update,
            prepare(
                m_impl->database.get(),
                "UPDATE sessions SET installed_generation=?1, "
                "session_epoch=?2, active=1 WHERE session_id=?3 "
                "AND session_epoch=?4 AND active=0"
            )
        );
        UF_TRY(bindInteger(
            m_impl->database.get(),
            update.get(),
            1,
            installedGeneration
        ));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            update.get(),
            2,
            m_impl->sessionEpoch
        ));
        UF_TRY(bindText(m_impl->database.get(), update.get(), 3, sessionId));
        UF_TRY(bindInteger(m_impl->database.get(), update.get(), 4, priorEpoch));
        UF_TRY(expectDone(m_impl->database.get(), update.get()));
        if (sqlite3_changes(m_impl->database.get()) != 1)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Prior session resume lost its compare-and-swap"
            );
        }

        UF_TRY_VALUE(controller, bindController(sessionId));
        UF_TRY(transaction.commit());
        return controller;
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
                "capability_profile_hash, session_epoch, controller_kind, mode "
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
        UF_TRY_VALUE(mode, parseSessionMode(columnText(query.get(), 5)));
        if (kind == ControllerKind::Agent && mode == SessionMode::Read)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "An Agent cannot bind a read-mode session"
            );
        }
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

        // Release is as final as takeover for the authority the Host may still
        // be using. Resolve first, under this transaction and while the live
        // lease still identifies the target, so no committed state can contain
        // neither the lease nor an answer for its dispatch.
        UF_TRY(resolveUnansweredDispatches(
            m_impl->database.get(),
            lease.controlledTargetId,
            "lease release found this dispatch unanswered"
        ));

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
        ProjectToolCatalogSchemaOwner const& catalog,
        ObservedInstanceIdentitySchemas const& identitySchemas,
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
                "session.project_registration_hash, session.controller_kind, "
                "session.controller_capabilities, policy.policy_hash, "
                "session.world_scope_kind, session.world_scope_id, "
                "session.world_scope_generation "
                "FROM sessions session JOIN project_registrations registration "
                "ON registration.registration_hash=session.project_registration_hash "
                "JOIN session_policies policy ON policy.session_id=session.session_id "
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
            || catalog.projectRegistrationHash().hex() != columnText(sessionQuery.get(), 6)
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Snapshot ProjectPlugin does not match the pinned session registration"
            );
        }

        // The scope this session was pinned under, rebuilt from its stored
        // tuple. It is the scope the observation's instances mint in: the
        // plugin output carries no scope and cannot name one.
        UF_TRY_VALUE(
            worldScope,
            restoreSessionWorldScope(
                columnText(sessionQuery.get(), 10),
                columnText(sessionQuery.get(), 11),
                columnText(sessionQuery.get(), 12)
            )
        );

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
            snapshotCapabilities,
            readNameArray(columnText(sessionQuery.get(), 8))
        );
        auto availableTools = catalog.offeredTools(
            controllerProfile(snapshotKind),
            snapshotCapabilities
        );
        auto availableToolNames = std::vector<std::string>{};
        availableToolNames.reserve(availableTools.size());
        for (auto const& tool : availableTools)
        {
            availableToolNames.emplace_back(tool.name);
        }
        auto const availableToolsJcs = canonicalNameArray(availableToolNames);
        UF_TRY_VALUE(policyHash, parseHashColumn(columnText(sessionQuery.get(), 9)));
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
                "project_registration_hash FROM project_observations "
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

        // The schema owner already parsed and validated the derive output; the
        // proposal is its retained value, not a second parse of its bytes.
        UF_TRY_VALUE(proposal, proposalFromDerived(derived.value()));

        // The context the mint re-checks its authorities against. The lease was
        // validated at the top of this function and the session row read above,
        // so the facts come from this same transaction's reads.
        UF_TRY_VALUE(registrationHash, parseHashColumn(columnText(sessionQuery.get(), 6)));
        UF_TRY_VALUE(
            finalObservation,
            mintProjectObservation(
                ObservedInstanceContext{
                    .pluginId                = pluginId,
                    .pluginHash              = columnText(sessionQuery.get(), 5),
                    .projectRegistrationHash = registrationHash,
                    .projectInstanceKey      = projectInstanceKey,
                },
                worldScope,
                identitySchemas,
                plugin,
                proposal
            )
        );

        // The fingerprint names the final closed observation the canonical
        // mint produced -- the bytes the row stores -- not the derive output
        // that proposed it.
        auto const observationHex     = finalObservation.hash().hex();
        auto const stateResolutionHex = observation.stateResolutionHash().hex();
        auto currentFingerprint       = stateResolutionHex;
        currentFingerprint += '\0';
        currentFingerprint += std::to_string(projectStateRevision);
        currentFingerprint += '\0';
        currentFingerprint += projectStateHex;
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
                    "revision, project_registration_hash, state_resolution_hash, "
                    "project_state_revision, project_state_hash, canonical_observation, "
                    "observation_hash) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)"
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
                stateResolutionHex
            ));
            UF_TRY(bindInteger(
                m_impl->database.get(),
                observationInsert.get(),
                6,
                projectStateRevision
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                observationInsert.get(),
                7,
                projectStateHex
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                observationInsert.get(),
                8,
                finalObservation.canonicalBytes()
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                observationInsert.get(),
                9,
                observationHex
            ));
            UF_TRY(expectDone(m_impl->database.get(), observationInsert.get()));
        }

        UF_TRY_VALUE(projectStateHash, parseHashColumn(projectStateHex));
        UF_TRY_VALUE(sessionManifestHash, parseHashColumn(sessionManifestHex));

        // StoredProjectObservation is minted here, in the member function body: its
        // single friend is OperatorCoordinator, which reaches its member
        // functions and neither Impl nor the file-local helpers above. Its
        // payload is the final closed envelope, the bytes the row stores.
        auto projectObservation = StoredProjectObservation{
            plugin.projectRegistrationHash(),
            plugin.pluginHash(),
            projectInstanceKey,
            observation.stateResolutionHash(),
            projectStateRevision,
            projectStateHash,
            observationRevision,
            std::move(finalObservation),
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

        UF_TRY_VALUE(
            availabilityQuery,
            prepare(
                m_impl->database.get(),
                "SELECT revision, policy_hash, available_tools "
                "FROM availability_heads WHERE controlled_target_id=?1"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            availabilityQuery.get(),
            1,
            controlledTargetId
        ));
        auto availabilityRevision = uint64{1};
        auto const availabilityStep = sqlite3_step(availabilityQuery.get());
        if (availabilityStep == SQLITE_ROW)
        {
            auto const priorAvailabilityRevision = static_cast<uint64>(
                sqlite3_column_int64(availabilityQuery.get(), 0)
            );
            availabilityRevision = priorAvailabilityRevision;
            if (
                columnText(availabilityQuery.get(), 1) != policyHash.hex()
                || columnText(availabilityQuery.get(), 2) != availableToolsJcs
            )
            {
                UF_TRY_VALUE(
                    nextRevision,
                    checkedSqlIncrement(
                        priorAvailabilityRevision,
                        "availability revision"
                    )
                );
                availabilityRevision = nextRevision;
                UF_TRY_VALUE(
                    availabilityUpdate,
                    prepare(
                        m_impl->database.get(),
                        "UPDATE availability_heads SET revision=?2, policy_hash=?3, "
                        "available_tools=?4 WHERE controlled_target_id=?1 AND revision=?5"
                    )
                );
                UF_TRY(bindText(
                    m_impl->database.get(),
                    availabilityUpdate.get(),
                    1,
                    controlledTargetId
                ));
                UF_TRY(bindInteger(
                    m_impl->database.get(),
                    availabilityUpdate.get(),
                    2,
                    availabilityRevision
                ));
                UF_TRY(bindText(
                    m_impl->database.get(),
                    availabilityUpdate.get(),
                    3,
                    policyHash.hex()
                ));
                UF_TRY(bindText(
                    m_impl->database.get(),
                    availabilityUpdate.get(),
                    4,
                    availableToolsJcs
                ));
                UF_TRY(bindInteger(
                    m_impl->database.get(),
                    availabilityUpdate.get(),
                    5,
                    priorAvailabilityRevision
                ));
                UF_TRY(expectDone(m_impl->database.get(), availabilityUpdate.get()));
                if (sqlite3_changes(m_impl->database.get()) != 1)
                {
                    return fail(
                        AutomationErrorKind::ActionRejected,
                        "Availability revision lost its compare-and-swap"
                    );
                }
            }
        }
        else if (availabilityStep == SQLITE_DONE)
        {
            UF_TRY_VALUE(
                availabilityInsert,
                prepare(
                    m_impl->database.get(),
                    "INSERT INTO availability_heads(controlled_target_id, revision, "
                    "policy_hash, available_tools) VALUES(?1, 1, ?2, ?3)"
                )
            );
            UF_TRY(bindText(
                m_impl->database.get(),
                availabilityInsert.get(),
                1,
                controlledTargetId
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                availabilityInsert.get(),
                2,
                policyHash.hex()
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                availabilityInsert.get(),
                3,
                availableToolsJcs
            ));
            UF_TRY(expectDone(m_impl->database.get(), availabilityInsert.get()));
        }
        else
        {
            return databaseFailure(
                m_impl->database.get(),
                "could not read the control availability revision"
            );
        }

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
            .policyHash                 = policyHash,
            .sessionManifestHash        = sessionManifestHash,
            .stateResolutionHash        = observation.stateResolutionHash(),
            .availableToolsJcs          = availableToolsJcs,
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

        UF_TRY(pruneSnapshotHistory(
            m_impl->database.get(),
            lease.sessionId,
            pluginId,
            projectInstanceKey
        ));

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
            .policyHash           = policyHash,
            .availableTools       = std::move(availableTools),
            .observation          = std::move(projectObservation),
            .eventCursor          = SubscriptionCursor{eventCursor},
        };
    }

    auto OperatorCoordinator::mintProjectObservation(
        ObservedInstanceContext const& context,
        ObservedInstanceWorldScope const& worldScope,
        ObservedInstanceIdentitySchemas const& identitySchemas,
        ProjectPluginHandle const& plugin,
        ProjectObservationProposal const& proposal
    ) -> Result<ProjectObservation>
    {
        // Shape-level codes precede every semantic relationship code in the
        // interface-lock registry.
        UF_TRY(validateProjectObservationProposalShape(proposal));
        UF_TRY(validateProjectObservationProposalRelations(proposal));

        // Registration closure membership is a whole-proposal pass and must
        // outrank every basis violation, collision and scope refusal,
        // irrespective of proposal order.
        for (auto const& instance : proposal.observedInstanceProposals)
        {
            if (!identitySchemas.contains(instance.identitySchemaId))
            {
                return fail(
                    ProjectObservationErrorCode::ObservedInstanceIdentitySchemaNotRegistered,
                    "Observed instance identity_schema_id is outside the registration closure"
                );
            }
        }
        for (auto const& instance : proposal.observedInstanceProposals)
        {
            UF_TRY(identitySchemas.validate(
                instance.identitySchemaId,
                instance.semanticIdentityBasis
            ));
        }

        auto minted = std::vector<ObservedInstanceId>{};
        minted.reserve(proposal.observedInstanceProposals.size());
        auto localRefById = std::map<std::string, std::string>{};
        for (auto const& instance : proposal.observedInstanceProposals)
        {
            auto const authority = observedInstanceAuthorityBytes(
                context,
                worldScope,
                instance
            );
            UF_TRY_VALUE(
                observedInstanceId,
                mintObservedInstanceBinding(
                    m_impl->database.get(),
                    context,
                    worldScope,
                    authority,
                    instance.localRef
                )
            );
            auto const [found, inserted] = localRefById.try_emplace(
                observedInstanceId,
                instance.localRef
            );
            if (!inserted && found->second != instance.localRef)
            {
                return fail(
                    ProjectObservationErrorCode::ObservedInstanceCollision,
                    "Different observed instance local_ref values minted one ID"
                );
            }
            minted.emplace_back(ObservedInstanceId{std::move(observedInstanceId)});
        }

        if (
            plugin.pluginId() != context.pluginId
            || plugin.pluginHash().hex() != context.pluginHash
            || plugin.projectRegistrationHash() != context.projectRegistrationHash
            || identitySchemas.projectRegistrationHash()
                != context.projectRegistrationHash
        )
        {
            return fail(
                ProjectObservationErrorCode::ObservedInstanceScopeMismatch,
                "Observed instance authorities do not match the active registration"
            );
        }

        auto indexes = std::map<std::string, std::size_t>{};
        for (
            auto index = std::size_t{};
            index < proposal.observedInstanceProposals.size();
            ++index
        )
        {
            indexes.emplace(
                proposal.observedInstanceProposals[index].localRef,
                index
            );
        }
        auto instances = std::vector<ObservedInstance>{};
        instances.reserve(proposal.observedInstanceProposals.size());
        for (
            auto index = std::size_t{};
            index < proposal.observedInstanceProposals.size();
            ++index
        )
        {
            auto parent = std::optional<ObservedInstanceId>{};
            auto const& proposed = proposal.observedInstanceProposals[index];
            if (proposed.parentLocalRef)
            {
                parent.emplace(minted[indexes.at(*proposed.parentLocalRef)]);
            }
            instances.emplace_back(ObservedInstance{
                .observedInstanceId       = minted[index],
                .parentObservedInstanceId = std::move(parent),
                .kind                     = proposed.kind,
                .opaqueProjectPayload     = proposed.opaqueProjectPayload,
            });
        }

        auto value = finalProjectObservationValue(
            proposal.canonicalOpaquePayload,
            proposal.projectToolPreconditions,
            instances
        );
        auto canonicalBytes = json::canonicalBytes(value);
        UF_TRY_VALUE(
            hash,
            sha256(std::as_bytes(std::span{canonicalBytes}))
        );
        return ProjectObservation{
            proposal.canonicalOpaquePayload,
            proposal.projectToolPreconditions,
            std::move(instances),
            std::move(canonicalBytes),
            hash,
        };
    }

    auto OperatorCoordinator::publishProjectObservation(
        ControlLease const& lease,
        ProjectPluginHandle const& plugin,
        ObservedInstanceWorldScope const& worldScope,
        ObservedInstanceIdentitySchemas const& identitySchemas,
        ProjectObservationProposal const& proposal
    ) -> Result<ProjectObservation>
    {
        UF_TRY_VALUE(
            derivedContext,
            readObservedInstanceContext(
                m_impl->database.get(),
                m_impl->sessionEpoch,
                lease
            )
        );
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            observation,
            mintProjectObservation(
                derivedContext,
                worldScope,
                identitySchemas,
                plugin,
                proposal
            )
        );
        UF_TRY(transaction.commit());
        return observation;
    }

    auto OperatorCoordinator::resolveObservedInstance(
        ControlLease const& lease,
        ObservedInstanceWorldScope const& worldScope,
        ProjectObservation const& freshObservation,
        std::string_view observedInstanceId
    ) -> Result<ObservedInstanceId>
    {
        UF_TRY_VALUE(
            context,
            readObservedInstanceContext(
                m_impl->database.get(),
                m_impl->sessionEpoch,
                lease
            )
        );
        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                "SELECT plugin_id, project_registration_hash, project_instance_key, "
                "world_scope_kind, world_scope_id, world_scope_generation "
                "FROM observed_instance_bindings WHERE observed_instance_id=?1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), query.get(), 1, observedInstanceId));
        if (sqlite3_step(query.get()) != SQLITE_ROW)
        {
            return fail(
                ProjectObservationErrorCode::ObservedInstanceStale,
                "Observed instance ID is not a known persistent binding"
            );
        }
        auto const scopeMatches = columnText(query.get(), 0) == context.pluginId
            && columnText(query.get(), 1) == context.projectRegistrationHash.hex()
            && columnText(query.get(), 2) == context.projectInstanceKey
            && columnText(query.get(), 3)
                == observedInstanceWorldScopeKindWireName(worldScope.kind())
            && columnText(query.get(), 4) == worldScope.scopeId()
            && columnText(query.get(), 5) == std::to_string(worldScope.generation());
        if (!scopeMatches)
        {
            return fail(
                ProjectObservationErrorCode::ObservedInstanceScopeMismatch,
                "Observed instance ID belongs to another registration, project or scope"
            );
        }
        auto const fresh = std::ranges::any_of(
            freshObservation.observedInstances(),
            [observedInstanceId](ObservedInstance const& instance)
            {
                return instance.observedInstanceId.value() == observedInstanceId;
            }
        );
        if (!fresh)
        {
            return fail(
                ProjectObservationErrorCode::ObservedInstanceStale,
                "Observed instance ID is absent from the fresh observation"
            );
        }
        return ObservedInstanceId{std::string{observedInstanceId}};
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

        // The accept side of p03 re-evaluates every offer predicate because an
        // invocation may be presented without first being offered.
        if (!toolSurfaceAllowed(controller.profile(), invocation.descriptor().surface))
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Controller restricted to semantic tools submitted a privileged tool"
            );
        }

        auto const& toolName = invocation.toolName();
        auto const& toolVersion = invocation.descriptor().toolVersion;
        auto const& canonicalArgs = invocation.canonicalArgs().bytes();
        auto const mutating =
            invocation.descriptor().mutability == ToolMutability::Mutating;

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
                "session.project_registration_hash, session.controller_capabilities, "
                "session.world_scope_kind, session.world_scope_id, "
                "session.world_scope_generation "
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
            heldCapabilities,
            readNameArray(columnText(sessionQuery.get(), 5))
        );
        auto const missingCapability = missingRequiredToolCapability(
            heldCapabilities,
            invocation.descriptor().requiredCapabilities
        );
        if (missingCapability)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Controller does not hold required capability '" + *missingCapability
                    + "' for tool " + toolName
            );
        }

        UF_TRY_VALUE(
            existingQuery,
            prepare(
                m_impl->database.get(),
                "SELECT operation_id, command_fingerprint, tool_name, tool_version, "
                "canonical_args, mutating, state, revision, "
                "EXISTS(SELECT 1 FROM dispatches d WHERE "
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
            if (columnText(existingQuery.get(), 9) != controller.sessionId())
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
            auto const dispatched = sqlite3_column_int(existingQuery.get(), 8) != 0;
            UF_TRY(transaction.commit());
            return AcceptedCommand{
                .operation = StoredOperation{
                    .operationId   = std::move(operationId),
                    .lookup        = CommandLookup::Existing,
                    .state         = state,
                    .revision      = revision,
                    .planFrozen    = dispatched,
                    .hasDispatched = dispatched,
                },
                .commandFingerprint = commandFingerprint,
            };
        }

        UF_TRY_VALUE(
            snapshotQuery,
            prepare(
                m_impl->database.get(),
                // The two project-state clauses together make the token a
                // reference to a COMPOSITION rather than to a lease: the
                // snapshot goes stale when ProjectState moves under it, not
                // only when control does. Their conjunction is the guarded
                // property; either clause is redundant by itself because both
                // revisions come from the same in-transaction state read and
                // that revision also participates in the derive fingerprint.
                //
                // The NOT EXISTS clause is what makes out-of-band human input
                // stop the automation: a finding records the snapshot revision
                // its target had reached, and every token at or below that
                // revision is refused afterwards. The controller has to look
                // again before it acts, which is the whole effect a finding is
                // allowed to have.
                "SELECT session.controlled_target_id, s.decision_basis_hash, "
                "obs.canonical_observation "
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

        // U2c production entry gate. Every observed instance id the command's
        // canonical arguments spell is resolved here, before the operation row
        // is created -- which is before a read-only operation can complete and
        // before plugin.plan can consume an id out of those arguments. A
        // stale or foreign id therefore never reaches either consumer, and a
        // mutating plan returning only Wait steps cannot carry one past the
        // gate either. The plugin can still name an id no argument carried,
        // which is why mintNextStep gates the step's ui_target_id again.
        UF_TRY_VALUE(
            sessionWorldScope,
            restoreSessionWorldScope(
                columnText(sessionQuery.get(), 6),
                columnText(sessionQuery.get(), 7),
                columnText(sessionQuery.get(), 8)
            )
        );
        auto targetIds = std::vector<std::string>{};
        {
            UF_TRY_VALUE(argumentsValue, json::parse(canonicalArgs));
            collectObservedInstanceIds(argumentsValue, targetIds);
        }
        if (!targetIds.empty())
        {
            std::ranges::sort(targetIds);
            targetIds.erase(
                std::unique(targetIds.begin(), targetIds.end()),
                targetIds.end()
            );
            UF_TRY_VALUE(
                freshObservation,
                restoreProjectObservation(columnText(snapshotQuery.get(), 2))
            );
            for (auto const& targetId : targetIds)
            {
                UF_TRY_VALUE(
                    bindingQuery,
                    prepare(
                        m_impl->database.get(),
                        "SELECT plugin_id, project_registration_hash, "
                        "project_instance_key, world_scope_kind, world_scope_id, "
                        "world_scope_generation FROM observed_instance_bindings "
                        "WHERE observed_instance_id=?1"
                    )
                );
                UF_TRY(bindText(
                    m_impl->database.get(),
                    bindingQuery.get(),
                    1,
                    targetId
                ));
                if (sqlite3_step(bindingQuery.get()) != SQLITE_ROW)
                {
                    return fail(
                        ProjectObservationErrorCode::ObservedInstanceStale,
                        "Observed instance ID is not a known persistent binding"
                    );
                }
                auto const scopeMatches =
                    columnText(bindingQuery.get(), 0) == pluginId
                    && columnText(bindingQuery.get(), 1)
                        == columnText(sessionQuery.get(), 4)
                    && columnText(bindingQuery.get(), 2) == projectInstanceKey
                    && columnText(bindingQuery.get(), 3)
                        == observedInstanceWorldScopeKindWireName(
                            sessionWorldScope.kind()
                        )
                    && columnText(bindingQuery.get(), 4)
                        == sessionWorldScope.scopeId()
                    && columnText(bindingQuery.get(), 5)
                        == std::to_string(sessionWorldScope.generation());
                if (!scopeMatches)
                {
                    return fail(
                        ProjectObservationErrorCode::ObservedInstanceScopeMismatch,
                        "Observed instance ID belongs to another registration, project or scope"
                    );
                }
                auto const fresh = std::ranges::any_of(
                    freshObservation.observedInstances(),
                    [&targetId](ObservedInstance const& instance)
                    {
                        return instance.observedInstanceId.value() == targetId;
                    }
                );
                if (!fresh)
                {
                    return fail(
                        ProjectObservationErrorCode::ObservedInstanceStale,
                        "Observed instance ID is absent from the fresh observation"
                    );
                }
            }
        }

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
                "SELECT operation_id, revision, state, EXISTS(SELECT 1 FROM dispatches d WHERE "
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
            auto const dispatched = sqlite3_column_int(pendingQuery.get(), 3) != 0;
            UF_TRY_VALUE(machine, OperationMachine::restore(state, dispatched, dispatched));
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
            UF_TRY(appendLedgerEvent(
                m_impl->database.get(),
                epoch,
                target,
                LedgerEventKind::OperationStateChanged,
                operationId,
                operationStateWireName(nextState)
            ));
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

        // Derived rather than assumed. Bounded retention moves this floor, and
        // MIN(sequence) - 1 is the last cursor that can still be answered
        // without hiding a deleted row.
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

        // Past the head means another database or epoch; below the floor means
        // retention deleted part of the requested stream. Neither is an empty
        // batch, because both require a fresh snapshot before continuing.
        if (after.value > currentCursor || after.value < oldestCursor)
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
                "SELECT sequence, kind, controlled_target_id, subject_id, detail "
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
            UF_TRY_VALUE(detail, parseLedgerEventDetail(events.get(), 4, kind));
            auto const sequence = static_cast<uint64>(
                sqlite3_column_int64(events.get(), 0)
            );
            batch.events.emplace_back(LedgerEvent{
                .sequence           = SubscriptionCursor{sequence},
                .kind               = kind,
                .controlledTargetId = columnText(events.get(), 2),
                .subjectId          = columnText(events.get(), 3),
                .detail             = detail,
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
                "SELECT o.state, o.revision, o.mutating, EXISTS(SELECT 1 FROM dispatches d "
                "WHERE d.operation_id=o.operation_id), o.controlled_target_id "
                "FROM operations o "
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
        auto const mutating   = sqlite3_column_int(query.get(), 2) != 0;
        auto const dispatched = sqlite3_column_int(query.get(), 3) != 0;
        if (event == OperationEvent::ReadCompleted && mutating)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Operation event contradicts the command mutability"
            );
        }
        UF_TRY_VALUE(machine, OperationMachine::restore(state, dispatched, dispatched));
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
        UF_TRY(appendLedgerEvent(
            m_impl->database.get(),
            m_impl->sessionEpoch,
            columnText(query.get(), 4),
            LedgerEventKind::OperationStateChanged,
            operationId,
            operationStateWireName(nextState)
        ));
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
        ProjectToolCatalogSchemaOwner const& catalog,
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
                "state.canonical_opaque_payload, session.controller_kind, "
                "session.controller_capabilities FROM operations o "
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
        if (catalog.projectRegistrationHash().hex() != registrationHex)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Plan Tool Catalog does not match the pinned session registration"
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
        UF_TRY_VALUE(descriptor, catalog.describe(toolName));
        if (descriptor.toolVersion != toolVersion)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "The Tool Catalog now declares a different version of the tool "
                "this Operation was created for"
            );
        }
        UF_TRY_VALUE(
            controllerCapabilities,
            readNameArray(columnText(query.get(), 16))
        );

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
                .descriptor              = descriptor,
                .controllerCapabilities  = controllerCapabilities,
                .operationId             = operationId,
                .toolName                = toolName,
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

        // The Operation's own edge is decided here, by what the policy ruled.
        // The caller has no signal for either of these two events for exactly
        // that reason.
        UF_TRY_VALUE(machine, OperationMachine::restore(state, false, false));
        UF_TRY_VALUE(
            nextState,
            machine.transition(
                plan.requiredApprovals().empty()
                    ? OperationEvent::ReadyWithoutApproval
                    : OperationEvent::ApprovalRequired
            )
        );

        auto const limits            = plan.limits();
        auto const requiredApprovals = canonicalNameArray(plan.requiredApprovals());
        UF_TRY_VALUE(
            insert,
            prepare(
                m_impl->database.get(),
                "INSERT INTO operation_plans(operation_id, plan_hash, "
                "command_fingerprint, decision_basis_hash, effect_envelope_hash, "
                "project_registration_hash, risk, policy_hash, required_approvals, "
                "maximum_steps, maximum_dispatches, maximum_observations, "
                "maximum_waits, maximum_elapsed_ms, canonical_plan) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, "
                "?14, ?15)"
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
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            8,
            planAuthority.policyHash().hex()
        ));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 9, requiredApprovals));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 10, limits.maximumSteps));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            insert.get(),
            11,
            limits.maximumDispatches
        ));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            insert.get(),
            12,
            limits.maximumObservations
        ));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 13, limits.maximumWaits));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            insert.get(),
            14,
            limits.maximumElapsedMillis
        ));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 15, plan.canonicalPlan()));
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
        UF_TRY(appendLedgerEvent(
            m_impl->database.get(),
            lease.sessionEpoch,
            lease.controlledTargetId,
            LedgerEventKind::OperationStateChanged,
            operationId,
            operationStateWireName(nextState)
        ));
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
            .policyHash         = planAuthority.policyHash(),
            .requiredApprovals  = plan.requiredApprovals(),
            .limits             = limits,
            .risk               = plan.risk(),
        };
    }

    auto OperatorCoordinator::restoreProjectObservation(std::string_view storedJcs)
        -> Result<ProjectObservation>
    {
        UF_TRY_VALUE(document, json::parse(storedJcs));
        if (document.kind() != json::ValueKind::Object)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "Stored observation is not an object"
            );
        }

        auto const& preconditionValues = member(document, "project_tool_preconditions");
        if (preconditionValues.kind() != json::ValueKind::Array)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "Stored observation tool preconditions are not an array"
            );
        }
        auto preconditions = std::vector<ProjectToolPrecondition>{};
        preconditions.reserve(preconditionValues.items().size());
        for (auto const& precondition : preconditionValues.items())
        {
            if (precondition.kind() != json::ValueKind::Object)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "Stored observation tool precondition is not an object"
                );
            }
            auto const& name = member(precondition, "name");
            if (name.kind() != json::ValueKind::String)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "Stored observation tool precondition name is not a string"
                );
            }
            UF_TRY_VALUE(
                status,
                parseProjectToolPreconditionStatus(member(precondition, "status"))
            );
            preconditions.emplace_back(ProjectToolPrecondition{
                .name   = std::string{name.string()},
                .status = status,
            });
        }

        auto const& instanceValues = member(document, "observed_instances");
        if (instanceValues.kind() != json::ValueKind::Array)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "Stored observation instances are not an array"
            );
        }
        auto instances = std::vector<ObservedInstance>{};
        instances.reserve(instanceValues.items().size());
        for (auto const& instance : instanceValues.items())
        {
            if (instance.kind() != json::ValueKind::Object)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "Stored observation instance is not an object"
                );
            }
            auto const& idValue = member(instance, "observed_instance_id");
            if (idValue.kind() != json::ValueKind::String)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "Stored observation instance id is not a string"
                );
            }
            auto parent = std::optional<ObservedInstanceId>{};
            auto const* const p_parent = instance.find("parent_observed_instance_id");
            if (p_parent != nullptr)
            {
                if (p_parent->kind() != json::ValueKind::String)
                {
                    return fail(
                        AutomationErrorKind::InternalInvariant,
                        "Stored observation instance parent id is not a string"
                    );
                }
                parent.emplace(ObservedInstanceId{std::string{p_parent->string()}});
            }
            auto const& kindValue = member(instance, "kind");
            if (kindValue.kind() != json::ValueKind::String)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "Stored observation instance kind is not a string"
                );
            }
            instances.emplace_back(ObservedInstance{
                .observedInstanceId = ObservedInstanceId{
                    std::string{idValue.string()}
                },
                .parentObservedInstanceId = std::move(parent),
                .kind                     = std::string{kindValue.string()},
                .opaqueProjectPayload     = member(instance, "opaque_project_payload"),
            });
        }

        // The stored bytes are the final observation's own canonical bytes, so
        // the restored hash is the sha256 of exactly what is stored.
        UF_TRY_VALUE(
            hash,
            sha256(std::as_bytes(std::span{storedJcs}))
        );
        return ProjectObservation{
            member(document, "canonical_opaque_payload"),
            std::move(preconditions),
            std::move(instances),
            std::string{storedJcs},
            hash,
        };
    }

    auto OperatorCoordinator::mintNextStep(
        std::string const& operationId,
        uint64 expectedRevision,
        ControlLease const& lease,
        ProjectPluginHandle const& plugin,
        ProjectToolCatalogSchemaOwner const& catalog,
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
                "EXISTS(SELECT 1 FROM dispatches d "
                "WHERE d.operation_id=o.operation_id), "
                "session.runtime_artifact_root_hash, o.tool_name, o.tool_version, "
                "session.world_scope_kind, session.world_scope_id, "
                "session.world_scope_generation "
                "FROM operations o "
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
        if (catalog.projectRegistrationHash().hex() != columnText(query.get(), 5))
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Step Tool Catalog does not match the pinned session registration"
            );
        }
        UF_TRY_VALUE(descriptor, catalog.describe(columnText(query.get(), 17)));
        if (descriptor.toolVersion != columnText(query.get(), 18))
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "The Tool Catalog now declares a different version of the tool "
                "this Operation was created for"
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
        auto const sessionArtifactRoot = columnText(query.get(), 16);
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
                .descriptor              = descriptor,
                .canonicalPlan           = canonicalPlan,
                .operationId             = operationId,
                .planHash                = planHash,
                .stepIndex               = stepIndex,
                .runtimeArtifactRootHash = sessionArtifactRoot,
            })
        );

        if (step.kind() == StepKind::UiAction)
        {
            // U2c: a UI action names the observed instance it acts on, so the
            // step is resolved through the same authorization gate as any
            // other observed_instance_id use -- the persistent binding's scope
            // is checked before fresh membership -- before the step row that
            // commits it is written. The id is read off the same member the
            // dispatch-side reader uses, so the step refused here is the step
            // that would have been delivered. The scope is the session's
            // pinned one, rebuilt from its stored tuple, which is the scope
            // the observation's instances were minted in.
            UF_TRY_VALUE(
                sessionWorldScope,
                restoreSessionWorldScope(
                    columnText(query.get(), 19),
                    columnText(query.get(), 20),
                    columnText(query.get(), 21)
                )
            );
            UF_TRY_VALUE(intentValue, json::parse(step.canonicalStep()));
            auto const* const p_action = intentValue.find("action");
            UF_CHECK(p_action != nullptr);
            auto const* const p_uiTargetId = p_action->find("ui_target_id");
            UF_CHECK(p_uiTargetId != nullptr);
            UF_TRY_VALUE(
                freshObservation,
                restoreProjectObservation(observationJcs)
            );
            UF_TRY(resolveObservedInstance(
                lease,
                sessionWorldScope,
                freshObservation,
                p_uiTargetId->string()
            ));
        }

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
        auto const dispatched = sqlite3_column_int(query.get(), 15) != 0;
        UF_TRY_VALUE(requiredApprovals, readNameArray(columnText(query.get(), 10)));
        auto const approvalNeeded = !requiredApprovals.empty();
        auto nextState      = state;
        auto nextFrozen     = dispatched;
        auto nextDispatched = dispatched;
        if (state == OperationState::Reconciling && step.kind() == StepKind::UiAction)
        {
            UF_TRY_VALUE(machine, OperationMachine::restore(state, dispatched, dispatched));
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
        if (nextState != state)
        {
            UF_TRY(appendLedgerEvent(
                m_impl->database.get(),
                lease.sessionEpoch,
                lease.controlledTargetId,
                LedgerEventKind::OperationStateChanged,
                operationId,
                operationStateWireName(nextState)
            ));
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
                "SELECT o.state, o.revision, "
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
                "snapshot.target_generation, step.canonical_step "
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
            columnText(query.get(), 4) != lease.sessionId
            || columnText(query.get(), 5) != lease.controlledTargetId
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Dispatch lease does not own the Operation target"
            );
        }
        if (sqlite3_column_int(query.get(), 6) != 1)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Read-only Operations cannot enter Host dispatch"
            );
        }
        UF_TRY(requireLiveLease(m_impl->database.get(), lease, "Dispatch lease is stale"));

        auto const planHashHex      = columnText(query.get(), 7);
        auto const decisionBasisHex = columnText(query.get(), 8);
        UF_TRY_VALUE(frozenPlanHash, parseHashColumn(planHashHex));
        UF_TRY_VALUE(decisionBasisHash, parseHashColumn(decisionBasisHex));
        auto const maximumDispatches = static_cast<uint64>(
            sqlite3_column_int64(query.get(), 9)
        );
        auto const stepIndex = static_cast<uint64>(sqlite3_column_int64(query.get(), 10));
        auto const stepIntentHex = columnText(query.get(), 11);
        UF_TRY_VALUE(stepIntentHash, parseHashColumn(stepIntentHex));
        auto const dispatchCount = static_cast<uint64>(
            sqlite3_column_int64(query.get(), 12)
        );
        auto const targetGeneration = TargetGeneration::fromValue(
            static_cast<uint64>(sqlite3_column_int64(query.get(), 13))
        );
        // The step's canonical intent names the observed instance it acts on,
        // and the deliver check needs the model target that instance was
        // observed at. The binding is the ledger's own row, so the resolution
        // cannot be handed to the authority: a step naming an instance with no
        // persistent binding, or a binding whose local_ref is the migrated
        // empty sentinel, is refused here rather than delivered. The id is
        // read off the same member the mint-side gate uses, so the step that
        // passed mintNextStep is exactly the step resolved here.
        UF_TRY_VALUE(intentValue, json::parse(columnText(query.get(), 14)));
        auto const* const p_action = intentValue.find("action");
        UF_CHECK(p_action != nullptr);
        auto const* const p_uiTargetId = p_action->find("ui_target_id");
        UF_CHECK(p_uiTargetId != nullptr);
        UF_TRY_VALUE(
            bindingQuery,
            prepare(
                m_impl->database.get(),
                "SELECT local_ref FROM observed_instance_bindings "
                "WHERE observed_instance_id=?1"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            bindingQuery.get(),
            1,
            p_uiTargetId->string()
        ));
        if (sqlite3_step(bindingQuery.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "The step names an observed instance with no persistent binding"
            );
        }
        auto bindingLocalRef = columnText(bindingQuery.get(), 0);
        if (bindingLocalRef.empty())
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "The step names a binding whose local_ref predates target resolution"
            );
        }
        // Two counters because a wait step consumes a step and no dispatch.
        if (dispatchCount >= maximumDispatches)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Workflow dispatch budget is exhausted for this frozen plan"
            );
        }

        auto const priorSequence = static_cast<uint64>(sqlite3_column_int64(query.get(), 2));
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
            if (sqlite3_column_type(query.get(), 3) == SQLITE_NULL)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "A new dispatch cannot overtake the prior Host outcome"
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
                "UPDATE operations SET state='running', revision=?1 "
                "WHERE operation_id=?2 AND revision=?3"
            )
        );
        UF_TRY(bindInteger(m_impl->database.get(), update.get(), 1, nextRevision));
        UF_TRY(bindText(m_impl->database.get(), update.get(), 2, operationId));
        UF_TRY(bindInteger(m_impl->database.get(), update.get(), 3, revision));
        UF_TRY(expectDone(m_impl->database.get(), update.get()));
        if (sqlite3_changes(m_impl->database.get()) != 1)
        {
            return fail(AutomationErrorKind::ActionRejected, "Operation revision lost its CAS");
        }
        if (state != OperationState::Running)
        {
            UF_TRY(appendLedgerEvent(
                m_impl->database.get(),
                lease.sessionEpoch,
                lease.controlledTargetId,
                LedgerEventKind::OperationStateChanged,
                operationId,
                operationStateWireName(OperationState::Running)
            ));
        }
        UF_TRY(transaction.commit());
        return DispatchReservation{
            .authority = task::DispatchAuthority{
                .controlledTargetId  = lease.controlledTargetId,
                .uiTarget            = std::move(bindingLocalRef),
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

        UF_TRY(appendLedgerEvent(
            m_impl->database.get(),
            lease.sessionEpoch,
            lease.controlledTargetId,
            LedgerEventKind::DeliveryOutcomeRecorded,
            operationId,
            deliveryOutcomeWireName(report.outcome())
        ));
        UF_TRY(appendLedgerEvent(
            m_impl->database.get(),
            lease.sessionEpoch,
            lease.controlledTargetId,
            LedgerEventKind::OperationStateChanged,
            operationId,
            operationStateWireName(nextState)
        ));

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
        UF_TRY(requireName(request.approverCapability, "approver_capability"));
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
                "o.command_fingerprint, plan.plan_hash, "
                "plan.decision_basis_hash, plan.effect_envelope_hash, "
                "step.step_intent_hash, plan.policy_hash, plan.required_approvals "
                "FROM operations o "
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
        auto const frozenPlanHex = columnText(operationQuery.get(), 4);
        UF_TRY(requireLiveLease(
            m_impl->database.get(),
            request.lease,
            "Approval lease is stale"
        ));

        // The plan's own ruling on who may approve it. An approver presenting
        // any other capability is refused here: required_approvals names the
        // approvers the policy ruled, so an approval by someone outside that
        // set is an approval the policy never authorised.
        UF_TRY_VALUE(
            requiredApprovals,
            readNameArray(columnText(operationQuery.get(), 9))
        );
        if (!std::ranges::contains(requiredApprovals, request.approverCapability))
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "The frozen plan's required_approvals does not name capability "
                    + request.approverCapability
            );
        }

        UF_TRY_VALUE(token, randomToken(m_impl->database.get()));
        UF_TRY_VALUE(
            insert,
            prepare(
                m_impl->database.get(),
                "INSERT INTO approvals(token, operation_id, session_id, controller_id, "
                "controlled_target_id, lease_id, session_epoch, fencing_token, "
                "command_fingerprint, frozen_plan_hash, step_intent_hash, decision_basis_hash, "
                "effect_envelope_hash, policy_hash, approver_principal, "
                "approver_capability, authority_decision_id, expires_at_unix_millis) "
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
            columnText(operationQuery.get(), 7)
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            12,
            columnText(operationQuery.get(), 5)
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            13,
            columnText(operationQuery.get(), 6)
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            14,
            columnText(operationQuery.get(), 8)
        ));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 15, request.approverPrincipal));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            16,
            request.approverCapability
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
                "registration.plugin_hash, session.project_registration_hash, "
                "o.controlled_target_id "
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
                "INSERT INTO reconciliations(operation_id, disposition, canonical_proposal) "
                "VALUES(?1, ?2, ?3)"
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

        UF_TRY(appendLedgerEvent(
            m_impl->database.get(),
            m_impl->sessionEpoch,
            columnText(operationQuery.get(), 7),
            LedgerEventKind::OperationStateChanged,
            commit.operationId,
            operationStateWireName(nextState)
        ));

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
