// What the Operator's own ledger owns: the production database, RuntimeArtifact
// installation and reclamation, and the exact reduce envelope its journal
// builds. The properties a project's registration decides -- catalog
// mutability, schema-owner binding, who owns a disposition -- are the exported
// conformance suite's, because a consuming repository proves them against its own
// project; see conformance/source/. No property is asserted in both places.

#include <operator/ledger.hpp>
#include <operator/manifest.hpp>

#include "project-fixture.hpp"
#include "unsafe/operator-database-probe.hpp"

#include <core/error/contracts.hpp>

#include <domain/content-hash.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        // The one plugin every case here registers. It comes from the shared
        // fixture because plan and next_step now answer with real operator
        // protocol documents, and a second spelling of them would be a second
        // plugin module-manifest hash for one project.
        inline auto const k_pluginSource = test_support::pluginSource("fixture.alpha");

        // The same plugin except that reduce answers with a document the
        // pinned ProjectState schema refuses -- but only once a prior state
        // exists, so provisioning still succeeds and the failure lands inside
        // the reconciliation transaction, which is where the no-write-on-failure
        // test needs it.
        [[nodiscard]]
        inline auto rejectedReducePluginSource() -> std::string
        {
            auto source        = test_support::pluginSource("fixture.alpha");
            auto const accepted = std::string{"return { revision = 1 }"};
            auto const at      = source.find(accepted);
            REQUIRE(at != std::string::npos);
            return source.replace(at, accepted.size(), "return { value = 99 }");
        }

        // The same plugin except that its OP:`UIActionIntent` names one
        // identifier the installed RuntimeModel does not define. Only the one
        // member moves, so a refusal is about that member and not about a
        // document the reader stopped understanding.
        [[nodiscard]]
        inline auto pluginNamingUndefinedUi(
            std::string_view spelled,
            std::string_view replacement
        ) -> std::string
        {
            auto source   = test_support::pluginSource("fixture.alpha");
            auto const at = source.find(spelled);
            REQUIRE(at != std::string::npos);
            REQUIRE(source.find(spelled, at + spelled.size()) == std::string::npos);
            return source.replace(at, spelled.size(), replacement);
        }

        class TemporaryDirectory final
        {
            std::filesystem::path m_path{};

        public:
            TemporaryDirectory()
            {
                static auto s_sequence = std::atomic<uint64>{1};
                m_path = std::filesystem::temp_directory_path()
                    / std::format(
                        "umbraflow-operator-ledger-{}-{}",
                        std::chrono::steady_clock::now().time_since_epoch().count(),
                        s_sequence.fetch_add(1, std::memory_order_relaxed)
                    );
                auto error = std::error_code{};
                auto const created = std::filesystem::create_directory(m_path, error);
                REQUIRE(created);
                REQUIRE_FALSE(error);
            }

            TemporaryDirectory(TemporaryDirectory const&) = delete;
            TemporaryDirectory(TemporaryDirectory&&) = delete;
            auto operator=(TemporaryDirectory const&) -> TemporaryDirectory& = delete;
            auto operator=(TemporaryDirectory&&) -> TemporaryDirectory& = delete;

            ~TemporaryDirectory() noexcept
            {
                auto error = std::error_code{};
                static_cast<void>(std::filesystem::remove_all(m_path, error));
            }

            [[nodiscard]] auto path() const -> std::filesystem::path const&
            {
                return m_path;
            }
        };

        [[nodiscard]]
        auto exactSchemaIdentity(
            test_support::OperatorDatabaseProbe const& database
        ) -> std::string
        {
            auto const rows = database.readRows(
                "SELECT type, name, tbl_name, coalesce(sql, '') FROM sqlite_schema "
                "WHERE name NOT LIKE 'sqlite_%' ORDER BY type, name"
            );
            auto canonical = std::string{};
            for (auto const& row : rows)
            {
                REQUIRE(row.size() == 4U);
                for (auto const& value : row)
                {
                    canonical += std::to_string(value.size());
                    canonical.push_back(':');
                    canonical += value;
                }
            }
            return std::format(
                "sha256:{}",
                test_support::hashOf(canonical).hex()
            );
        }

        // Where SQLite keeps PRAGMA user_version, and a value the Operator's
        // DDL never writes there.
        constexpr auto k_userVersionOffset      = std::streamoff{60};
        constexpr auto k_nonIdentityUserVersion = std::array<char, 4>{
            '\0',
            '\0',
            '\0',
            '\x2a',
        };

        auto writeNonIdentityUserVersion(
            std::filesystem::path const& databasePath
        ) -> void
        {
            auto database = std::fstream{
                databasePath,
                std::ios::binary | std::ios::in | std::ios::out,
            };
            REQUIRE(database.good());
            database.seekp(k_userVersionOffset);
            database.write(
                k_nonIdentityUserVersion.data(),
                std::ssize(k_nonIdentityUserVersion)
            );
            REQUIRE(database.good());
        }

        [[nodiscard]]
        auto storedUserVersion(
            std::filesystem::path const& databasePath
        ) -> std::array<char, 4>
        {
            auto database = std::ifstream{databasePath, std::ios::binary};
            REQUIRE(database.good());
            database.seekg(k_userVersionOffset);
            auto stored = std::array<char, 4>{};
            database.read(stored.data(), std::ssize(stored));
            REQUIRE(database.good());
            return stored;
        }

        // Rewrites the separator inside one stored CREATE statement, which
        // changes the exact DDL text without changing what the schema means.
        //
        // Every copy of that statement is rewritten, not the first. A b-tree
        // split leaves the pre-split cell bytes in the freed space of the page
        // it split, so one CREATE statement's text can appear more than once
        // and the live copy is not the earliest; rewriting only the first moves
        // a byte SQLite never reads and leaves the identity intact.
        [[nodiscard]]
        auto mutateStoredDdlSeparator(
            std::filesystem::path const& databasePath
        ) -> std::vector<std::streamoff>
        {
            auto database = std::fstream{
                databasePath,
                std::ios::binary | std::ios::in | std::ios::out,
            };
            REQUIRE(database.good());
            auto const bytes = std::string{
                std::istreambuf_iterator<char>{database},
                std::istreambuf_iterator<char>{},
            };
            auto constexpr opening = std::string_view{"CREATE TABLE runtime_artifacts("};

            auto offsets = std::vector<std::streamoff>{};
            auto at      = bytes.find(opening);
            while (at != std::string::npos)
            {
                auto const separator = at + std::string_view{"CREATE"}.size();
                REQUIRE(bytes[separator] == ' ');
                offsets.emplace_back(static_cast<std::streamoff>(separator));
                at = bytes.find(opening, at + opening.size());
            }
            REQUIRE_FALSE(offsets.empty());

            database.clear();
            for (auto const offset : offsets)
            {
                database.seekp(offset);
                database.put('\n');
            }
            REQUIRE(database.good());
            return offsets;
        }

        [[nodiscard]]
        auto storedDdlSeparators(
            std::filesystem::path const& databasePath,
            std::vector<std::streamoff> const& offsets
        ) -> std::string
        {
            auto database = std::ifstream{databasePath, std::ios::binary};
            REQUIRE(database.good());
            auto separators = std::string{};
            for (auto const offset : offsets)
            {
                database.seekg(offset);
                separators.push_back(static_cast<char>(database.get()));
            }
            REQUIRE(database.good());
            return separators;
        }

        auto restorePriorSnapshotIdentityComment(
            test_support::OperatorDatabaseProbe& database
        ) -> void
        {
            database.execute(R"sql(
                PRAGMA writable_schema=ON;
                UPDATE sqlite_schema SET sql=replace(
                    sql,
                    '                        -- token and snapshot_revision are deliberately outside
                        -- canonical_parts: they name the stored row rather than
                        -- the capture. observation_id remains inside and names
                        -- the capture, so recapturing an identical world moves
                        -- identity_hash while decision_basis_hash stays stable.
                        token TEXT PRIMARY KEY,',
                    '                        token TEXT PRIMARY KEY,'
                ) WHERE type='table' AND name='snapshots';
                PRAGMA writable_schema=OFF;
            )sql");
        }

        auto removeToolRuntimePersistence(
            test_support::OperatorDatabaseProbe& database
        ) -> void
        {
            database.execute("DROP TABLE IF EXISTS tool_approvals");
            database.execute("DROP TABLE IF EXISTS tool_admission_attempts");
            database.execute("DROP TABLE IF EXISTS tool_runs");
            database.execute("DROP TABLE IF EXISTS tool_call_history");
        }

        auto restorePriorToolAdmissionAuthority(
            test_support::OperatorDatabaseProbe& database
        ) -> void
        {
            database.execute(
                "PRAGMA foreign_keys=OFF;"
                "DROP TABLE IF EXISTS tool_approvals;"
                "ALTER TABLE tool_admission_attempts RENAME TO "
                "new_tool_admission_attempts;"
                "CREATE TABLE tool_admission_attempts("
                "call_identity TEXT NOT NULL REFERENCES tool_call_history(call_identity),"
                "attempt_number INTEGER NOT NULL CHECK(attempt_number > 0),"
                "root_identity TEXT NOT NULL REFERENCES tool_runs(root_identity),"
                "origin_principal_id TEXT NOT NULL,"
                "origin_principal_kind TEXT NOT NULL CHECK(origin_principal_kind IN "
                "('script','agent','human')),"
                "execution_principal_id TEXT NOT NULL,"
                "execution_principal_kind TEXT NOT NULL CHECK(execution_principal_kind IN "
                "('script','agent','human')),"
                "session_id TEXT NOT NULL,"
                "session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),"
                "controlled_target_id TEXT NOT NULL,"
                "project_registration_hash TEXT NOT NULL CHECK("
                "length(project_registration_hash)=64 AND "
                "project_registration_hash NOT GLOB '*[^0-9a-f]*'),"
                "policy_hash TEXT NOT NULL CHECK(length(policy_hash)=64 AND "
                "policy_hash NOT GLOB '*[^0-9a-f]*'),"
                "capability_profile_hash TEXT NOT NULL CHECK("
                "length(capability_profile_hash)=64 AND "
                "capability_profile_hash NOT GLOB '*[^0-9a-f]*'),"
                "lease_id TEXT NOT NULL,"
                "lease_revision INTEGER NOT NULL CHECK(lease_revision > 0),"
                "fencing_token INTEGER NOT NULL CHECK(fencing_token > 0),"
                "budget_snapshot TEXT NOT NULL,"
                "budget_snapshot_hash TEXT NOT NULL CHECK("
                "length(budget_snapshot_hash)=64 AND "
                "budget_snapshot_hash NOT GLOB '*[^0-9a-f]*'),"
                "PRIMARY KEY(call_identity, attempt_number)"
                ") STRICT;"
                "INSERT INTO tool_admission_attempts(call_identity, attempt_number, "
                "root_identity, origin_principal_id, origin_principal_kind, "
                "execution_principal_id, execution_principal_kind, session_id, "
                "session_epoch, controlled_target_id, project_registration_hash, "
                "policy_hash, capability_profile_hash, lease_id, lease_revision, "
                "fencing_token, budget_snapshot, budget_snapshot_hash) SELECT "
                "call_identity, attempt_number, root_identity, origin_principal_id, "
                "origin_principal_kind, execution_principal_id, "
                "execution_principal_kind, session_id, session_epoch, "
                "controlled_target_id, project_registration_hash, policy_hash, "
                "capability_profile_hash, lease_id, lease_revision, fencing_token, "
                "budget_snapshot, budget_snapshot_hash FROM "
                "new_tool_admission_attempts;"
                "DROP TABLE new_tool_admission_attempts;"
                "PRAGMA foreign_keys=ON;"
            );
        }

        auto restorePriorToolApprovalSchema(
            test_support::OperatorDatabaseProbe& database
        ) -> void
        {
            database.execute(
                "PRAGMA foreign_keys=OFF;"
                "DROP TABLE tool_approvals;"
                "ALTER TABLE tool_admission_attempts RENAME TO "
                "new_tool_admission_attempts;"
                "CREATE TABLE tool_admission_attempts("
                "call_identity TEXT NOT NULL REFERENCES tool_call_history(call_identity),"
                "attempt_number INTEGER NOT NULL CHECK(attempt_number > 0),"
                "root_identity TEXT NOT NULL REFERENCES tool_runs(root_identity),"
                "origin_principal_id TEXT NOT NULL,"
                "origin_principal_kind TEXT NOT NULL CHECK(origin_principal_kind IN "
                "('script','agent','human')),"
                "execution_principal_id TEXT NOT NULL,"
                "execution_principal_kind TEXT NOT NULL CHECK(execution_principal_kind IN "
                "('script','agent','human')),"
                "session_id TEXT NOT NULL,"
                "session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),"
                "controlled_target_id TEXT NOT NULL,"
                "project_registration_hash TEXT NOT NULL CHECK("
                "length(project_registration_hash)=64 AND "
                "project_registration_hash NOT GLOB '*[^0-9a-f]*'),"
                "policy_hash TEXT NOT NULL CHECK(length(policy_hash)=64 AND "
                "policy_hash NOT GLOB '*[^0-9a-f]*'),"
                "capability_profile_hash TEXT NOT NULL CHECK("
                "length(capability_profile_hash)=64 AND "
                "capability_profile_hash NOT GLOB '*[^0-9a-f]*'),"
                "lease_id TEXT NOT NULL,"
                "lease_revision INTEGER NOT NULL CHECK(lease_revision > 0),"
                "fencing_token INTEGER NOT NULL CHECK(fencing_token > 0),"
                "budget_snapshot TEXT NOT NULL,"
                "budget_snapshot_hash TEXT NOT NULL CHECK("
                "length(budget_snapshot_hash)=64 AND "
                "budget_snapshot_hash NOT GLOB '*[^0-9a-f]*'),"
                "effect_envelope TEXT,"
                "effect_envelope_hash TEXT,"
                "required_approvals TEXT,"
                "approval_token TEXT,"
                "CHECK((effect_envelope IS NULL AND effect_envelope_hash IS NULL "
                "AND required_approvals IS NULL AND approval_token IS NULL) OR "
                "(effect_envelope IS NOT NULL AND effect_envelope_hash IS NOT NULL "
                "AND length(effect_envelope_hash)=64 "
                "AND effect_envelope_hash NOT GLOB '*[^0-9a-f]*' "
                "AND required_approvals IS NOT NULL)),"
                "PRIMARY KEY(call_identity, attempt_number)"
                ") STRICT;"
                "INSERT INTO tool_admission_attempts(call_identity, attempt_number, "
                "root_identity, origin_principal_id, origin_principal_kind, "
                "execution_principal_id, execution_principal_kind, session_id, "
                "session_epoch, controlled_target_id, project_registration_hash, "
                "policy_hash, capability_profile_hash, lease_id, lease_revision, "
                "fencing_token, budget_snapshot, budget_snapshot_hash, "
                "effect_envelope, effect_envelope_hash, required_approvals, "
                "approval_token) SELECT call_identity, attempt_number, "
                "root_identity, origin_principal_id, origin_principal_kind, "
                "execution_principal_id, execution_principal_kind, session_id, "
                "session_epoch, controlled_target_id, project_registration_hash, "
                "policy_hash, capability_profile_hash, lease_id, lease_revision, "
                "fencing_token, budget_snapshot, budget_snapshot_hash, "
                "effect_envelope, effect_envelope_hash, required_approvals, NULL "
                "FROM new_tool_admission_attempts;"
                "DROP TABLE new_tool_admission_attempts;"
                "PRAGMA foreign_keys=ON;"
            );
        }

        auto removeToolIdentityPersistence(
            test_support::OperatorDatabaseProbe& database
        ) -> void
        {
            removeToolRuntimePersistence(database);
            database.execute("DROP INDEX IF EXISTS one_top_level_tool_call_position");
            database.execute("DROP TABLE IF EXISTS tool_call_positions");
            database.execute("DROP TABLE IF EXISTS tool_root_requests");
        }

        // Rebuilds only the registration table to the exact format-2 shape
        // that preceded the generation-neutral columns. Row keys and canonical
        // bytes are copied unchanged; module_manifest becomes the historical
        // single-source hash column solely to reproduce the old schema pair.
        auto restoreFormat2RegistrationIdentity(
            test_support::OperatorDatabaseProbe& database
        ) -> void
        {
            removeToolIdentityPersistence(database);
            database.execute(R"sql(
                PRAGMA foreign_keys=OFF;
                CREATE TABLE prior_project_registrations(
                    registration_hash TEXT,
                    plugin_id TEXT,
                    plugin_identity_hash TEXT,
                    canonical_manifest TEXT
                ) STRICT;
                INSERT INTO prior_project_registrations
                    SELECT registration_hash, plugin_id, plugin_identity_hash,
                        canonical_manifest FROM project_registrations;
                DROP TABLE project_registrations;
                CREATE TABLE project_registrations(registration_hash TEXT PRIMARY KEY,plugin_id TEXT NOT NULL,plugin_hash TEXT NOT NULL,canonical_manifest TEXT NOT NULL) STRICT;
                INSERT INTO project_registrations SELECT registration_hash,
                    plugin_id, plugin_identity_hash, canonical_manifest
                    FROM prior_project_registrations;
                DROP TABLE prior_project_registrations;
                PRAGMA foreign_keys=ON;
            )sql");
        }

        // project_registrations as it stood before project_state_schema_hash
        // was dropped. SQLite stores a CREATE statement verbatim apart from
        // IF NOT EXISTS, so the indentation below is part of the historical
        // identity and not formatting. Identity covers the DDL only, but the
        // column's values are recovered from the canonical manifest that always
        // carried them, so the restored row is the historical row entire.
        auto restorePriorRegistrationStateSchemaHash(
            test_support::OperatorDatabaseProbe& database
        ) -> void
        {
            database.execute(
                R"sql(
                PRAGMA foreign_keys=OFF;
                CREATE TABLE prior_project_registrations(
                    registration_hash TEXT,
                    plugin_id TEXT,
                    plugin_hash TEXT,
                    canonical_manifest TEXT
                ) STRICT;
                INSERT INTO prior_project_registrations
                    SELECT registration_hash, plugin_id, plugin_hash,
                        canonical_manifest FROM project_registrations;
                DROP TABLE project_registrations;
                )sql"
                "CREATE TABLE project_registrations(\n"
                "                        registration_hash TEXT PRIMARY KEY,\n"
                "                        plugin_id TEXT NOT NULL,\n"
                "                        plugin_hash TEXT NOT NULL,\n"
                "                        project_state_schema_hash TEXT NOT NULL,\n"
                "                        canonical_manifest TEXT NOT NULL\n"
                "                    ) STRICT;"
                R"sql(
                INSERT INTO project_registrations SELECT registration_hash,
                    plugin_id, plugin_hash,
                    substr(
                        canonical_manifest,
                        instr(canonical_manifest, '"project_state_schema_hash":"') + 29,
                        64
                    ),
                    canonical_manifest FROM prior_project_registrations;
                DROP TABLE prior_project_registrations;
                PRAGMA foreign_keys=ON;
                )sql"
            );
        }

        auto removeReleaseUpgradeEvidenceTables(
            test_support::OperatorDatabaseProbe& database
        ) -> void
        {
            database.execute("DROP TABLE runtime_upgrade_failures");
            database.execute("DROP TABLE release_capability_approvals");
        }

        // sessions as it stood before the observed-instance world scope became
        // part of the pinned tuple. SQLite stores a CREATE statement verbatim
        // apart from leading whitespace and IF NOT EXISTS, so the indentation
        // below is part of the historical identity and not formatting; the
        // three world_scope columns the batch added are removed together with
        // the comment that documents them.
        auto removeSessionWorldScopeColumns(
            test_support::OperatorDatabaseProbe& database
        ) -> void
        {
            database.execute(
                R"sql(
                PRAGMA foreign_keys=OFF;
                CREATE TABLE prior_sessions(
                    session_id TEXT PRIMARY KEY,
                    authenticated_controller_id TEXT NOT NULL,
                    idempotency_namespace TEXT NOT NULL,
                    manifest_hash TEXT NOT NULL,
                    runtime_artifact_root_hash TEXT NOT NULL,
                    installed_generation INTEGER NOT NULL,
                    project_registration_hash TEXT NOT NULL,
                    controller_capabilities TEXT NOT NULL,
                    capability_profile_hash TEXT NOT NULL,
                    session_epoch INTEGER NOT NULL,
                    controlled_target_id TEXT NOT NULL,
                    project_instance_key TEXT NOT NULL,
                    mode TEXT NOT NULL,
                    controller_kind TEXT NOT NULL,
                    active INTEGER NOT NULL
                ) STRICT;
                INSERT INTO prior_sessions SELECT session_id,
                    authenticated_controller_id, idempotency_namespace,
                    manifest_hash, runtime_artifact_root_hash,
                    installed_generation, project_registration_hash,
                    controller_capabilities, capability_profile_hash,
                    session_epoch, controlled_target_id, project_instance_key,
                    mode, controller_kind, active FROM sessions;
                DROP TABLE sessions;
                )sql"
                "CREATE TABLE sessions(\n"
                "                        session_id TEXT PRIMARY KEY,\n"
                "                        authenticated_controller_id TEXT NOT NULL,\n"
                "                        idempotency_namespace TEXT NOT NULL,\n"
                "                        manifest_hash TEXT NOT NULL,\n"
                "                        runtime_artifact_root_hash TEXT NOT NULL,\n"
                "                        installed_generation INTEGER NOT NULL\n"
                "                            CHECK(installed_generation > 0),\n"
                "                        project_registration_hash TEXT NOT NULL\n"
                "                            REFERENCES project_registrations(registration_hash),\n"
                "                        -- The capability set this session holds, as the exact\n"
                "                        -- JCS array capability_profile_hash is the sha256 of.\n"
                "                        -- The hash alone was a caller field with no content\n"
                "                        -- behind it, so a policy rule naming a required\n"
                "                        -- capability had nothing to be judged against.\n"
                "                        controller_capabilities TEXT NOT NULL,\n"
                "                        capability_profile_hash TEXT NOT NULL,\n"
                "                        session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),\n"
                "                        controlled_target_id TEXT NOT NULL,\n"
                "                        project_instance_key TEXT NOT NULL,\n"
                "                        mode TEXT NOT NULL CHECK(mode IN ('read', 'write')),\n"
                "                        -- Which of the three operators holds this session. It\n"
                "                        -- is part of the immutable pinned tuple, so a\n"
                "                        -- controller cannot become another kind between two\n"
                "                        -- commands, and bindController reads it here rather\n"
                "                        -- than accepting it.\n"
                "                        controller_kind TEXT NOT NULL\n"
                "                            CHECK(controller_kind IN ('script', 'agent', 'human')),\n"
                "                        active INTEGER NOT NULL CHECK(active IN (0, 1)),\n"
                "                        FOREIGN KEY(project_registration_hash, project_instance_key)\n"
                "                            REFERENCES project_instances(\n"
                "                                project_registration_hash,\n"
                "                                project_instance_key\n"
                "                            ),\n"
                "                        FOREIGN KEY(installed_generation, runtime_artifact_root_hash)\n"
                "                            REFERENCES runtime_installations(\n"
                "                                installed_generation,\n"
                "                                artifact_root_hash\n"
                "                            )\n"
                "                    ) STRICT;"
                R"sql(
                INSERT INTO sessions(session_id, authenticated_controller_id,
                    idempotency_namespace, manifest_hash,
                    runtime_artifact_root_hash, installed_generation,
                    project_registration_hash, controller_capabilities,
                    capability_profile_hash, session_epoch,
                    controlled_target_id, project_instance_key, mode,
                    controller_kind, active) SELECT session_id,
                    authenticated_controller_id, idempotency_namespace,
                    manifest_hash, runtime_artifact_root_hash,
                    installed_generation, project_registration_hash,
                    controller_capabilities, capability_profile_hash,
                    session_epoch, controlled_target_id, project_instance_key,
                    mode, controller_kind, active FROM prior_sessions;
                DROP TABLE prior_sessions;
                )sql"
                "CREATE UNIQUE INDEX IF NOT EXISTS "
                "one_active_write_session_per_instance\n"
                "                    ON sessions(project_registration_hash, "
                "project_instance_key)\n"
                "                    WHERE mode='write' AND active=1;"
                R"sql(
                PRAGMA foreign_keys=ON;
                )sql"
            );
        }

        // observed_instance_bindings as it stood before reserveDispatch could
        // resolve a step's ui_target_id to the model target the instance was
        // observed at. The local_ref column the batch added is NOT NULL with
        // no default, so the migration that adds it rebuilds the table, and
        // these historical rebuilds must do the same in reverse: the identity
        // below is over the stored DDL text, so a column the rebuild left in
        // place would fail the pinned source-hash check even though no row is
        // ever touched. The text is HEAD's, verbatim, comment included.
        auto removeObservedInstanceBindingLocalRefColumn(
            test_support::OperatorDatabaseProbe& database
        ) -> void
        {
            database.execute(
                R"sql(
                PRAGMA foreign_keys=OFF;
                CREATE TABLE prior_observed_instance_bindings(
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
                        )
                ) STRICT;
                INSERT INTO prior_observed_instance_bindings(
                    canonical_authority, observed_instance_id, plugin_id,
                    project_registration_hash, project_instance_key,
                    world_scope_kind, world_scope_id, world_scope_generation)
                    SELECT canonical_authority, observed_instance_id, plugin_id,
                        project_registration_hash, project_instance_key,
                        world_scope_kind, world_scope_id, world_scope_generation
                    FROM observed_instance_bindings;
                DROP TABLE observed_instance_bindings;
                )sql"
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
                R"sql(
                INSERT INTO observed_instance_bindings(
                    canonical_authority, observed_instance_id, plugin_id,
                    project_registration_hash, project_instance_key,
                    world_scope_kind, world_scope_id, world_scope_generation)
                    SELECT canonical_authority, observed_instance_id, plugin_id,
                        project_registration_hash, project_instance_key,
                        world_scope_kind, world_scope_id, world_scope_generation
                    FROM prior_observed_instance_bindings;
                DROP TABLE prior_observed_instance_bindings;
                PRAGMA foreign_keys=ON;
                )sql"
            );
        }

        using test_support::canonical;
        using test_support::hashOf;
        using test_support::journalEntry;
        using test_support::k_fixtureProvenance;
        using test_support::k_fixtureProvenanceViolations;
        using test_support::loadPlugin;
        using test_support::makeProject;
        using test_support::sessionManifest;
        using test_support::toolInvocation;

        // Re-adding any of these members would reopen the two P0 holes: a
        // reducer input beside the events lets the Journal say A while the
        // materialized state was reduced from B, and a request-owned tool or
        // mutability makes the mutation chain opt-out. The checks go through
        // concepts because a member lookup on a concrete type is an error
        // rather than a substitution failure.
        template <typename T>
        concept NamesReducerInput = requires(T value) { value.reducerInput; };

        template <typename T>
        concept NamesMutability = requires(T value) { value.mutating; };

        template <typename T>
        concept NamesTool = requires(T value) { value.toolName; };

        template <typename T>
        concept NamesCanonicalArgs = requires(T value) { value.canonicalArgs; };

        template <typename T>
        concept NamesSessionId = requires(T value) { value.sessionId; };

        template <typename T>
        concept NamesProjectInstanceKey = requires(T value) {
            value.projectInstanceKey;
        };

        template <typename T>
        concept NamesObservedInstanceId = requires(T value) {
            value.observedInstanceId;
        };

        static_assert(!NamesReducerInput<ReconciliationCommit>);
        static_assert(!NamesReducerInput<ProjectInstanceBaseline>);
        static_assert(!NamesMutability<CommandRequest>);
        static_assert(!NamesTool<CommandRequest>);
        static_assert(!NamesCanonicalArgs<CommandRequest>);
        static_assert(
            !NamesSessionId<SessionResume>,
            "SessionResume must not accept an internal session_id"
        );
        static_assert(
            !NamesProjectInstanceKey<SessionResume>,
            "SessionResume must not accept an internal project_instance_key"
        );
        static_assert(
            !NamesObservedInstanceId<ObservedInstanceProposal>,
            "ObservedInstanceProposal must not accept a final observed_instance_id"
        );

        // The same guard for every authority-bearing value, not just the one
        // that happened to get it: an aggregate could be brace-initialized past
        // its owner, and a public constructor would make the owner optional.
        static_assert(!std::is_aggregate_v<ValidatedJournalEntryData>);
        static_assert(!std::is_aggregate_v<ValidatedToolInvocation>);
        static_assert(!std::is_aggregate_v<ValidatedReconcileOutcome>);
        static_assert(!std::is_aggregate_v<ValidatedDocument>);
        static_assert(!std::is_aggregate_v<CanonicalJson>);
        static_assert(
            !std::is_constructible_v<
                ValidatedToolInvocation,
                ContentHash,
                ContentHash,
                std::string,
                std::string,
                CanonicalJson,
                ToolMutability
            >
        );
        static_assert(
            !std::is_constructible_v<
                ValidatedReconcileOutcome,
                ContentHash,
                ContentHash,
                ValidatedDocument,
                ReconcileDisposition
            >
        );
        static_assert(
            !std::is_constructible_v<
                ValidatedJournalEntryData,
                ContentHash,
                std::string,
                ContentHash,
                CanonicalJson,
                CanonicalJson
            >
        );

        struct PreparedStore final
        {
            OperatorCoordinator          store;
            ProjectPluginHandle          plugin;
            test_support::ProjectFixture project;
            SessionManifest              manifest;
            OperatorPlanAuthority        planAuthority;

            // The authenticated controller every entry point is reached
            // through. bindController is its only mint.
            ControllerBinding            controller;
            ControlLease                 lease;
            SnapshotRecord               snapshot;
            conformance::ObservationHost observation;

            // What a delivering Host is activated from. The observing Host above
            // cannot serve a second TaskContext, so a dispatch opens the same
            // installed artifact again rather than sharing it.
            ContentHash runtimeArtifactRootHash;
            uint64      installedGeneration{};
        };

        [[nodiscard]]
        auto prepareStore(
            std::filesystem::path const& path,
            std::string_view pluginSource = k_pluginSource,
            std::string_view preconditionSchema = test_support::k_toolPreconditionSchema
        ) -> PreparedStore
        {
            auto const release = test_support::runtimeRelease(path / "session-handoff");
            auto storeResult = OperatorCoordinator::open(path / "production");
            REQUIRE_MESSAGE(
                storeResult.has_value(),
                "the fixture Operator must open: ",
                storeResult.error().message()
            );
            auto store = *std::move(storeResult);
            auto installed = store.installRuntimeArtifact(
                RuntimeArtifactInstallRequest{
                    .handoffRoot                 = release.handoffRoot,
                    .expectedReleaseManifestHash = release.releaseManifestHash,
                    .expectedInstalledGeneration = 0U,
                }
            );
            auto const installedMessage = installed.has_value()
                                              ? std::string{}
                                              : std::string{installed.error().message()};
            REQUIRE_MESSAGE(installed.has_value(), installedMessage);
            auto const artifactRootHash    = installed->rootHash();
            auto const installedGeneration = installed->installedGeneration();
            auto const project = makeProject(
                "fixture.alpha",
                pluginSource,
                test_support::k_projectObservationSchema,
                preconditionSchema
            );
            auto const manifest = sessionManifest(
                project.registration,
                installed->rootHash(),
                hashOf("agent"),
                test_support::policyArtifactBytes()
            );
            auto const projectPlugin = loadPlugin(project, pluginSource);
            REQUIRE(store.registerProject(project.registration).has_value());
            REQUIRE(store.provisionProjectInstance(
                project.registration,
                projectPlugin,
                ProjectInstanceBaseline{
                    .projectInstanceKey  = "instance-1",
                    .eventId             = "baseline-1",
                    .sessionManifestHash = manifest.hash(),
                    .entry = journalEntry(
                        project,
                        project.registration.baselineEventType(),
                        "{\"kind\":\"baseline\"}"
                    ),
                }
            ).has_value());
            auto const sessionWorldScope = ObservedInstanceWorldScope::run(
                "target-1",
                1
            );
            REQUIRE(sessionWorldScope.has_value());
            REQUIRE(store.pinSession(
                SessionPin{
                    .sessionId                 = "session-1",
                    .authenticatedControllerId = "controller-1",
                    .idempotencyNamespace      = "controller-1",
                    .projectRegistrationHash   = project.registration.hash(),
                    .controllerCapabilities    = {std::string{conformance::k_operateCapability}},
                    .controlledTargetId        = "target-1",
                    .projectInstanceKey        = "instance-1",
                    .mode                      = SessionMode::Write,
                    .kind                      = ControllerKind::Script,
                    .worldScope                = *sessionWorldScope,
                },
                manifest,
                std::nullopt
            ).has_value());
            auto controller = store.bindController("session-1");
            REQUIRE(controller.has_value());
            auto lease = store.acquireLease(*controller);
            REQUIRE(lease.has_value());
            auto observation = conformance::activateObservationHost(
                *std::move(installed),
                test_support::umbraflowProbeFrame(),
                FrameId{201}
            );
            auto snapshot = store.createSnapshot(
                *lease,
                projectPlugin,
                project.toolCatalogSchemaOwner,
                project.observedInstanceIdentitySchemas,
                conformance::observeOnce(observation)
            );
            REQUIRE(snapshot.has_value());
            auto runtimeModel = observation.host->runtimeModelBinding(
                observation.generation
            );
            REQUIRE(runtimeModel.has_value());
            auto planAuthority = conformance::planAuthority(
                project.registration,
                manifest,
                *runtimeModel,
                "operator",
                test_support::policyArtifactBytes(),
                test_support::k_fixtureUiAction
            );
            REQUIRE(planAuthority.has_value());
            return PreparedStore{
                .store                   = std::move(store),
                .plugin                  = projectPlugin,
                .project                 = project,
                .manifest                = manifest,
                .planAuthority           = *std::move(planAuthority),
                .controller              = *controller,
                .lease                   = *lease,
                .snapshot                = *std::move(snapshot),
                .observation             = std::move(observation),
                .runtimeArtifactRootHash = artifactRootHash,
                .installedGeneration     = installedGeneration,
            };
        }

        // A Host that can act under this store's current lease.
        [[nodiscard]]
        auto deliveringHost(PreparedStore& prepared)
            -> std::unique_ptr<conformance::DeliveringHost>
        {
            return conformance::deliveringHostFor(
                prepared.store,
                prepared.lease,
                prepared.installedGeneration,
                prepared.runtimeArtifactRootHash,
                test_support::k_fixtureUiAction,
                test_support::umbraflowProbeFrame()
            );
        }

        [[nodiscard]]
        auto additionalSessionPin(
            PreparedStore const& prepared,
            std::string sessionId
        ) -> SessionPin
        {
            auto const worldScope = ObservedInstanceWorldScope::run(
                "target-1",
                1
            );
            REQUIRE(worldScope.has_value());
            return SessionPin{
                .sessionId                 = std::move(sessionId),
                .authenticatedControllerId = "upgrade-controller",
                .idempotencyNamespace      = "upgrade-controller",
                .projectRegistrationHash   = prepared.project.registration.hash(),
                .controllerCapabilities = {
                    std::string{conformance::k_operateCapability},
                },
                .controlledTargetId = "target-1",
                .projectInstanceKey = "instance-1",
                .mode               = SessionMode::Read,
                .kind               = ControllerKind::Script,
                .worldScope         = *worldScope,
            };
        }

        [[nodiscard]]
        auto reconciliationOutcome(
            PreparedStore const& prepared,
            std::string operationId,
            std::string document
        ) -> ValidatedReconcileOutcome
        {
            return test_support::reconcileOutcome(
                prepared.project,
                prepared.plugin,
                std::move(operationId),
                std::move(document)
            );
        }

        [[nodiscard]]
        auto command(
            SnapshotRecord const& snapshot,
            std::string clientRequestId,
            std::string idempotencyNamespace
        ) -> CommandRequest
        {
            return CommandRequest{
                .snapshotToken        = snapshot.token,
                .idempotencyNamespace = std::move(idempotencyNamespace),
                .clientRequestId      = std::move(clientRequestId),
            };
        }

        // A catalog owner over one tool entry whose argument validator accepts
        // any canonical arguments. The fixture's tool argument schema admits
        // exactly {"value": 1..8}, so no Operation whose canonical arguments
        // name an observed instance id can be created through the prepared
        // catalog -- and such arguments are the whole subject of the
        // submitCommand gate cases. Entry name and descriptor source are both
        // the case's choice, so a case can present an entry the fixture
        // catalog never declared (the plan canary) under a descriptor the
        // fixture catalog did declare.
        [[nodiscard]]
        auto catalogAcceptingAnyArguments(
            PreparedStore const& prepared,
            std::string entryName,
            std::string descriptorSource
        ) -> Result<ProjectToolCatalogSchemaOwner>
        {
            UF_TRY_VALUE(
                descriptor,
                prepared.project.toolCatalogSchemaOwner.describe(descriptorSource)
            );
            return ProjectToolCatalogSchemaOwner::create(
                prepared.project.registration,
                prepared.project.toolCatalogBytes,
                [descriptor, entryName = std::move(entryName)]()
                    -> Result<std::vector<ToolCatalogEntry>>
                {
                    return std::vector<ToolCatalogEntry>{
                        ToolCatalogEntry{
                            .name       = entryName,
                            .descriptor = descriptor,
                        },
                    };
                },
                [](std::string_view, std::string_view) -> Status { return ok(); }
            );
        }

        // An observed instance id minted under a second session on another
        // target of the SAME registration: a real persistent binding, but one
        // whose world scope no command of the prepared session may name. The
        // mint reads the scope out of the pinned tuple, so the second
        // createSnapshot derives a different canonical authority and mints a
        // different id than the prepared snapshot's.
        [[nodiscard]]
        auto foreignObservedInstanceId(
            PreparedStore& prepared
        ) -> std::string
        {
            REQUIRE(prepared.store.provisionProjectInstance(
                prepared.project.registration,
                prepared.plugin,
                ProjectInstanceBaseline{
                    .projectInstanceKey  = "instance-2",
                    .eventId             = "baseline-instance-2",
                    .sessionManifestHash = prepared.manifest.hash(),
                    .entry               = journalEntry(
                        prepared.project,
                        prepared.project.registration.baselineEventType(),
                        "{\"kind\":\"baseline\"}"
                    ),
                }
            ).has_value());
            auto const secondScope = ObservedInstanceWorldScope::run("target-2", 1);
            REQUIRE(secondScope.has_value());
            REQUIRE(prepared.store.pinSession(
                SessionPin{
                    .sessionId                 = "session-2",
                    .authenticatedControllerId = "controller-1",
                    .idempotencyNamespace      = "controller-1",
                    .projectRegistrationHash   = prepared.project.registration.hash(),
                    .controllerCapabilities    = {std::string{conformance::k_operateCapability}},
                    .controlledTargetId        = "target-2",
                    .projectInstanceKey        = "instance-2",
                    .mode                      = SessionMode::Write,
                    .kind                      = ControllerKind::Script,
                    .worldScope                = *secondScope,
                },
                prepared.manifest,
                std::nullopt
            ).has_value());
            auto second = prepared.store.bindController("session-2");
            REQUIRE(second.has_value());
            auto lease = prepared.store.acquireLease(*second);
            REQUIRE(lease.has_value());
            auto snapshot = prepared.store.createSnapshot(
                *lease,
                prepared.plugin,
                prepared.project.toolCatalogSchemaOwner,
                prepared.project.observedInstanceIdentitySchemas,
                conformance::observeOnce(prepared.observation)
            );
            REQUIRE(snapshot.has_value());
            return snapshot->observation.payload()
                .observedInstances()[0].observedInstanceId.value();
        }

        [[nodiscard]]
        auto proposedOperation(
            PreparedStore& prepared,
            std::string clientRequestId,
            std::string_view toolName
        ) -> StoredOperation
        {
            auto operation = prepared.store.submitCommand(
                prepared.controller,
                command(prepared.snapshot, std::move(clientRequestId), "controller-1"),
                toolInvocation(prepared.project, std::string{toolName})
            );
            REQUIRE(operation.has_value());
            return operation->operation;
        }

        [[nodiscard]]
        auto freezePlanFor(
            PreparedStore& prepared,
            StoredOperation const& operation
        ) -> Result<FrozenPlan>
        {
            return prepared.store.freezePlan(
                operation.operationId,
                operation.revision,
                prepared.lease,
                prepared.plugin,
                prepared.project.toolCatalogSchemaOwner,
                prepared.planAuthority
            );
        }

        [[nodiscard]]
        auto mintStepFor(
            PreparedStore& prepared,
            StoredOperation const& operation
        ) -> Result<PlannedStep>
        {
            return prepared.store.mintNextStep(
                operation.operationId,
                operation.revision,
                prepared.lease,
                prepared.plugin,
                prepared.project.toolCatalogSchemaOwner,
                prepared.planAuthority
            );
        }

        // A plan authority carrying nothing but the Operator's own protocol
        // readers, which is what a production deployment builds.
        // conformance::planAuthority wraps the step reader in a check that the
        // step names the run's one agreed UI action, and that check would
        // answer the cases below before the Operator did.
        [[nodiscard]]
        auto deploymentAuthority(
            PreparedStore& prepared,
            ContentHash const& runtimeArtifactRootHash
        ) -> Result<OperatorPlanAuthority>
        {
            auto runtimeModel = prepared.observation.host->runtimeModelBinding(
                prepared.observation.generation
            );
            REQUIRE(runtimeModel.has_value());
            return OperatorPlanAuthority::create(
                prepared.project.registration,
                sessionManifest(
                    prepared.project.registration,
                    runtimeArtifactRootHash,
                    hashOf("agent"),
                    test_support::policyArtifactBytes()
                ),
                *runtimeModel,
                "operator",
                test_support::policyArtifactBytes(),
                deployment::readPlanProposal,
                deployment::readStepIntent
            );
        }

        [[nodiscard]]
        auto mintStepUnder(
            PreparedStore& prepared,
            OperatorPlanAuthority const& authority
        ) -> Result<PlannedStep>
        {
            auto const proposed = proposedOperation(prepared, "request-1", "command-1");
            auto const frozen   = prepared.store.freezePlan(
                proposed.operationId,
                proposed.revision,
                prepared.lease,
                prepared.plugin,
                prepared.project.toolCatalogSchemaOwner,
                authority
            );
            REQUIRE(frozen.has_value());
            return prepared.store.mintNextStep(
                frozen->operation.operationId,
                frozen->operation.revision,
                prepared.lease,
                prepared.plugin,
                prepared.project.toolCatalogSchemaOwner,
                authority
            );
        }

        // Proposed, plan frozen by the Operator, first step minted from the
        // plugin's own next_step: everything a dispatch may be reserved from.
        [[nodiscard]]
        auto createReadyOperation(
            PreparedStore& prepared,
            std::string clientRequestId,
            std::string_view toolName
        ) -> StoredOperation
        {
            auto const proposed = proposedOperation(
                prepared,
                std::move(clientRequestId),
                toolName
            );
            auto const frozen = freezePlanFor(prepared, proposed);
            REQUIRE(frozen.has_value());
            auto const step = mintStepFor(prepared, frozen->operation);
            REQUIRE(step.has_value());
            return step->operation;
        }

        // test_support::runtimeRelease always writes the same page model, so
        // every release it builds has the same content hash and shares one
        // production directory. Reclamation needs two that do not.
        // Builds a handoff whose release manifest declares the two generations
        // given, so a case can move exactly one of them off the number this
        // deployment principal reads.
        [[nodiscard]]
        auto releaseWithFormats(
            std::filesystem::path const& root,
            uint64 annotationWorkspaceFormat,
            uint64 workspaceSqliteRevision
        ) -> conformance::ObservationRelease
        {
            auto const handoff  = root / "release";
            auto const artifact = handoff / "runtime-artifact";
            auto const model    = std::string_view{"a page model\r\n"};
            test_support::writeFile(artifact / task::k_runtimeModelFileName, model);
            auto const manifest = std::format(
                "{{\"assets\":[],"
                "\"page_model\":{{\"path\":\"runtime-model.toml\",\"sha256\":\"{}\","
                "\"size\":{}}},\"runtime_artifact_format\":{},"
                "\"runtime_model_format\":{}}}",
                hashOf(model).hex(),
                model.size(),
                task::k_runtimeArtifactFormat,
                task::k_runtimeModelFormat
            );
            test_support::writeFile(
                artifact / task::k_runtimeArtifactManifestFileName,
                manifest
            );
            auto const artifactRootHash = hashOf(manifest);
            auto const releaseManifest = std::format(
                "{{\"annotation_workspace_format\":{},"
                "\"candidate_id\":\"candidate-1\",\"candidate_revision\":1,"
                "\"generation\":1,\"predecessor_publication_id\":null,"
                "\"replay_gate_hash\":\"{}\",\"runtime_artifact_root_hash\":\"{}\","
                "\"workspace_sqlite_revision\":{}}}",
                annotationWorkspaceFormat,
                hashOf("replay-gate").hex(),
                artifactRootHash.hex(),
                workspaceSqliteRevision
            );
            test_support::writeFile(handoff / "release.manifest.json", releaseManifest);
            return conformance::ObservationRelease{
                .handoffRoot         = handoff,
                .releaseManifestHash = hashOf(releaseManifest),
                .artifactRootHash    = artifactRootHash,
            };
        }

        [[nodiscard]]
        auto releaseWithModel(
            std::filesystem::path const& root,
            std::string_view model
        ) -> conformance::ObservationRelease
        {
            auto const handoff  = root / "release";
            auto const artifact = handoff / "runtime-artifact";
            test_support::writeFile(artifact / task::k_runtimeModelFileName, model);
            auto const manifest = std::format(
                "{{\"assets\":[],"
                "\"page_model\":{{\"path\":\"runtime-model.toml\",\"sha256\":\"{}\","
                "\"size\":{}}},\"runtime_artifact_format\":{},"
                "\"runtime_model_format\":{}}}",
                hashOf(model).hex(),
                model.size(),
                task::k_runtimeArtifactFormat,
                task::k_runtimeModelFormat
            );
            test_support::writeFile(
                artifact / task::k_runtimeArtifactManifestFileName,
                manifest
            );
            auto const artifactRootHash = hashOf(manifest);
            auto const releaseManifest = std::format(
                "{{\"annotation_workspace_format\":{},"
                "\"candidate_id\":\"candidate-1\",\"candidate_revision\":1,"
                "\"generation\":1,\"predecessor_publication_id\":null,"
                "\"replay_gate_hash\":\"{}\",\"runtime_artifact_root_hash\":\"{}\","
                "\"workspace_sqlite_revision\":{}}}",
                detail::k_annotationWorkspaceFormat,
                hashOf("replay-gate").hex(),
                artifactRootHash.hex(),
                detail::k_workspaceSqliteRevision
            );
            test_support::writeFile(handoff / "release.manifest.json", releaseManifest);
            return conformance::ObservationRelease{
                .handoffRoot         = handoff,
                .releaseManifestHash = hashOf(releaseManifest),
                .artifactRootHash    = artifactRootHash,
            };
        }

        [[nodiscard]]
        auto installRequest(
            conformance::ObservationRelease const& release,
            uint64 expectedInstalledGeneration
        ) -> RuntimeArtifactInstallRequest
        {
            return RuntimeArtifactInstallRequest{
                .handoffRoot                 = release.handoffRoot,
                .expectedReleaseManifestHash = release.releaseManifestHash,
                .expectedInstalledGeneration = expectedInstalledGeneration,
            };
        }

        // The whole ledger file. Compared rather than any one column, because
        // "wrote nothing" is a claim about every table at once and a case that
        // named one would absorb the next write silently. Reading it after the
        // coordinator was destroyed is what makes it complete: WAL frames are
        // checkpointed into this file on close.
        [[nodiscard]]
        auto ledgerBytes(std::filesystem::path const& databasePath) -> std::string
        {
            auto stream = std::ifstream{databasePath, std::ios::binary};
            REQUIRE(stream.good());
            return std::string{
                std::istreambuf_iterator<char>{stream},
                std::istreambuf_iterator<char>{},
            };
        }

        // A directory link that needs no privilege on Windows and that the
        // portable inspection functions report as a plain directory, which is
        // what makes it the shape worth planting.
        [[nodiscard]]
        auto linkDirectory(
            std::filesystem::path const& link,
            std::filesystem::path const& target
        ) -> bool
        {
#if defined(_WIN32)
            auto const command = std::format(
                "cmd /c mklink /J \"{}\" \"{}\" >nul 2>&1",
                link.string(),
                target.string()
            );
            return std::system(command.c_str()) == 0;
#else
            auto error = std::error_code{};
            std::filesystem::create_directory_symlink(target, link, error);
            return !error;
#endif
        }

        // Neither pinSession refusal case below needs a registered project or
        // a provisioned instance: the registration-disagreement refusal fires
        // by comparing the pin against the manifest, before any table is
        // read; the missing-instance refusal fires on a query that finds no
        // row, which a project that was never registered also produces.
        // Naming only what pinSession touches keeps each case pinned to the
        // one check under test.
        [[nodiscard]]
        auto storeWithInstalledArtifact(std::filesystem::path const& path)
            -> std::pair<OperatorCoordinator, ContentHash>
        {
            auto const release =
                test_support::runtimeRelease(path / "session-handoff");
            auto storeResult = OperatorCoordinator::open(path / "production");
            REQUIRE(storeResult.has_value());
            auto store = *std::move(storeResult);
            auto installed = store.installRuntimeArtifact(
                RuntimeArtifactInstallRequest{
                    .handoffRoot                 = release.handoffRoot,
                    .expectedReleaseManifestHash = release.releaseManifestHash,
                    .expectedInstalledGeneration = 0U,
                }
            );
            REQUIRE(installed.has_value());
            return std::pair{std::move(store), installed->rootHash()};
        }

        [[nodiscard]]
        auto manifestNamingRegistration(
            ContentHash runtimeArtifactRootHash,
            ContentHash projectRegistrationHash
        ) -> SessionManifest
        {
            auto manifest = SessionManifest::create(
                SessionManifestSpec{
                    .runtimeModelArtifactRootHash = runtimeArtifactRootHash,
                    .operatorProtocolSchemaHash   = hashOf("operator"),
                    .projectRegistrationHash      = projectRegistrationHash,
                    .policyArtifactHash           = hashOf("policy"),
                    .agentProfileHash             = hashOf("agent"),
                }
            );
            REQUIRE(manifest.has_value());
            return *std::move(manifest);
        }

        [[nodiscard]]
        auto semanticBasis(
            std::string nativeId,
            double surfaceEpoch,
            bool reversed = false
        ) -> json::Value
        {
            if (reversed)
            {
                return json::Value::ofObject({
                    json::Member{
                        "surface_epoch",
                        json::Value::ofNumber(surfaceEpoch),
                    },
                    json::Member{
                        "native_id",
                        json::Value::ofString(std::move(nativeId)),
                    },
                });
            }
            return json::Value::ofObject({
                json::Member{
                    "native_id",
                    json::Value::ofString(std::move(nativeId)),
                },
                json::Member{
                    "surface_epoch",
                    json::Value::ofNumber(surfaceEpoch),
                },
            });
        }

        [[nodiscard]]
        auto observedInstanceProposal(
            std::string localRef,
            std::string nativeId,
            std::optional<std::string> parentLocalRef = std::nullopt,
            double surfaceEpoch = 4.0,
            bool reversedBasis = false
        ) -> ObservedInstanceProposal
        {
            return ObservedInstanceProposal{
                .localRef         = std::move(localRef),
                .parentLocalRef   = std::move(parentLocalRef),
                .kind             = "fixture.overlay",
                .identitySchemaId = "https://fixture.example/identity/overlay/v1",
                .semanticIdentityBasis = semanticBasis(
                    std::move(nativeId),
                    surfaceEpoch,
                    reversedBasis
                ),
                .opaqueProjectPayload = json::Value::ofObject({
                    json::Member{
                        "visible",
                        json::Value::ofBoolean(true),
                    },
                }),
            };
        }

        [[nodiscard]]
        auto observationProposal(
            std::vector<ObservedInstanceProposal> instances
        ) -> ProjectObservationProposal
        {
            return ProjectObservationProposal{
                .schema                 = "umbraflow-project-observation-proposal/v1",
                .canonicalOpaquePayload = json::Value::ofObject({
                    json::Member{
                        "surface",
                        json::Value::ofString("fixture.surface"),
                    },
                }),
                .projectToolPreconditions = {
                    ProjectToolPrecondition{
                        .name   = "fixture.overlay_clear",
                        .status = ProjectToolPreconditionStatus::Known,
                    },
                },
                .observedInstanceProposals = std::move(instances),
            };
        }

        [[nodiscard]]
        auto runScope(
            std::string scopeId = "run-7",
            uint64 generation = 7U
        ) -> ObservedInstanceWorldScope
        {
            auto scope = ObservedInstanceWorldScope::run(
                std::move(scopeId),
                generation
            );
            REQUIRE(scope.has_value());
            return *std::move(scope);
        }

        [[nodiscard]]
        auto normativeProjectObservationErrorWireName(
            ProjectObservationErrorCode code
        ) -> std::string_view
        {
            switch (code)
            {
            case ProjectObservationErrorCode::MalformedAuthorityInput:
                return "MalformedAuthorityInput";
            case ProjectObservationErrorCode::MalformedProposal:
                return "MalformedProposal";
            case ProjectObservationErrorCode::PreconditionNameNotNamespaced:
                return "PreconditionNameNotNamespaced";
            case ProjectObservationErrorCode::PreconditionStatusOutsideFactDomain:
                return "PreconditionStatusOutsideFactDomain";
            case ProjectObservationErrorCode::InvalidWorldScopeGeneration:
                return "InvalidWorldScopeGeneration";
            case ProjectObservationErrorCode::DuplicatePreconditionName:
                return "DuplicatePreconditionName";
            case ProjectObservationErrorCode::DuplicateObservedInstanceLocalRef:
                return "DuplicateObservedInstanceLocalRef";
            case ProjectObservationErrorCode::ObservedInstanceParentMissing:
                return "ObservedInstanceParentMissing";
            case ProjectObservationErrorCode::ObservedInstanceParentCycle:
                return "ObservedInstanceParentCycle";
            case ProjectObservationErrorCode::ObservedInstanceIdentitySchemaNotRegistered:
                return "ObservedInstanceIdentitySchemaNotRegistered";
            case ProjectObservationErrorCode::SemanticIdentityBasisSchemaViolation:
                return "SemanticIdentityBasisSchemaViolation";
            case ProjectObservationErrorCode::ObservedInstanceCollision:
                return "ObservedInstanceCollision";
            case ProjectObservationErrorCode::ObservedInstanceScopeMismatch:
                return "ObservedInstanceScopeMismatch";
            case ProjectObservationErrorCode::ObservedInstanceStale:
                return "ObservedInstanceStale";
            }
            UF_UNREACHABLE_MSG("Unknown ProjectObservationErrorCode test value");
        }

        template <typename Value>
        auto expectProjectObservationError(
            Result<Value> const& result,
            ProjectObservationErrorCode expected
        ) -> void
        {
            auto const expectedWireName = normativeProjectObservationErrorWireName(
                expected
            );
            CAPTURE(expectedWireName);
            REQUIRE_MESSAGE(
                !result.has_value(),
                std::format(
                    "{} must be a refusal",
                    expectedWireName
                )
            );
            CHECK_MESSAGE(
                projectObservationErrorCode(result.error()) == expected,
                std::format(
                    "the refusal must name the exact normative code {}",
                    expectedWireName
                )
            );
            CHECK_MESSAGE(
                projectObservationErrorWireName(expected) == expectedWireName,
                std::format(
                    "the public wire code must be exactly {}",
                    expectedWireName
                )
            );
            CHECK_MESSAGE(
                result.error().detailCode().message() == expectedWireName,
                std::format(
                    "the emitted wire code must be exactly {}",
                    expectedWireName
                )
            );
        }
    }

    // The two operator protocol readers, on documents this registration's
    // schema owner stamped. They are read here rather than in tests/deployment
    // because each takes a ValidatedDocument and only a ProjectSchemaOwner can
    // mint one, so reaching a reader at all needs a plugin.
    TEST_CASE("the operator protocol readers read a stamped document")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const project = prepared.project;
        auto const plugin  = prepared.plugin;

        auto const proposal = plugin.plan(canonical(
            project.schemaOwner,
            "{\"canonical_args\":{\"value\":1},\"project_observation\":"
                + prepared.snapshot.observation.payload().canonicalBytes()
                + ",\"project_state\":{\"revision\":0},\"tool_name\":\"command-1\","
                  "\"tool_version\":\"1\"}"
        ));
        REQUIRE(proposal.has_value());
        // The stored final observation, exactly as mintNextStep hands it to the
        // plugin: the default next_step names the minted id back out of it.
        auto const intent = plugin.nextStep(canonical(
            project.schemaOwner,
            "{\"frozen_plan_hash\":\"" + hashOf("plan").hex()
                + "\",\"project_observation\":"
                + prepared.snapshot.observation.payload().canonicalBytes()
                + ",\"project_state\":{\"revision\":0},\"step_index\":1}"
        ));
        REQUIRE(intent.has_value());

        auto const claims = deployment::readPlanProposal(*proposal);
        REQUIRE(claims.has_value());
        CHECK(claims->toolName == "command-1");
        CHECK(claims->toolVersion == "1");
        CHECK(claims->canonicalArgs == "{\"value\":1}");
        REQUIRE(claims->allowedUiActions.size() == 1U);
        CHECK(claims->allowedUiActions.front() == "fixture.step");
        REQUIRE(claims->effects.size() == 2U);
        CHECK(claims->effects.front().namespacedType == "fixture.write");
        CHECK(claims->effects.front().risk == Risk::Low);
        CHECK(claims->effects.front().scopeKind == "instance");
        CHECK(claims->effects.front().scopeKey == "alpha");
        CHECK(claims->effects.front().opaqueProjectPayload == "{\"value\":1}");
        CHECK(claims->effects.back().risk == Risk::Medium);
        CHECK(claims->effects.back().scopeKey == "beta");
        CHECK(claims->limits.maximumSteps == 8U);
        CHECK(claims->limits.maximumDispatches == 8U);
        CHECK(claims->limits.maximumObservations == 16U);
        CHECK(claims->limits.maximumWaits == 4U);
        CHECK(claims->limits.maximumElapsedMillis == 60000U);

        auto const step = deployment::readStepIntent(*intent);
        REQUIRE(step.has_value());
        CHECK(step->kind == StepKind::UiAction);
        CHECK(step->stepKey == "fixture.step");
        CHECK(step->surfaceId == "fixture.surface");
        CHECK(
            step->uiTargetId
            == prepared.snapshot.observation.payload().observedInstances()[0]
                   .observedInstanceId
                   .value()
        );
        CHECK(step->actionId == "fixture.press");
        CHECK(step->canonicalParameters == "{\"value\":1}");

        // The one claim a ValidatedDocument does not carry, and the whole of
        // what each reader still refuses. Both documents are exact JCS this
        // owner's schema accepted, so neither refusal is about canonical form
        // or about the definition -- only about which function stamped it.
        CHECK_FALSE(deployment::readPlanProposal(*intent).has_value());
        CHECK_FALSE(deployment::readStepIntent(*proposal).has_value());
    }

    TEST_CASE("OperatorCoordinator creates only the production database name")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        CHECK(prepared.store.databasePath().filename() == "operator-runtime.sqlite");
        CHECK(std::filesystem::is_regular_file(prepared.store.databasePath()));
    }

    TEST_CASE("the proposal cannot state an observed instance ID or authority binding")
    {
        static_assert(std::is_aggregate_v<ProjectObservationProposal>);
        static_assert(std::is_aggregate_v<ObservedInstanceProposal>);
        static_assert(!std::is_aggregate_v<ProjectObservation>);
        static_assert(
            !std::is_constructible_v<ObservedInstanceId, std::string>,
            "Only OperatorCoordinator may construct a final observed-instance ID"
        );
        static_assert(
            !std::is_constructible_v<ObservedInstanceId, std::string_view>,
            "A wire spelling must not construct observed-instance authority"
        );
        static_assert(
            !std::is_constructible_v<
                ProjectObservation,
                json::Value,
                std::vector<ProjectToolPrecondition>,
                std::vector<ObservedInstance>,
                std::string,
                ContentHash
            >,
            "Only OperatorCoordinator may construct the final observation"
        );
        CHECK(ProjectObservation::schema() == "umbraflow-project-observation/v1");
    }

    TEST_CASE("observed instance mint is opaque stable scoped and projects parents")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto schemas   = prepared.project.observedInstanceIdentitySchemas;
        auto const scope = runScope();
        auto first = prepared.store.publishProjectObservation(
            prepared.lease,
            prepared.plugin,
            scope,
            schemas,
            observationProposal({
                observedInstanceProposal("event", "overlay.event"),
                observedInstanceProposal(
                    "choice",
                    "overlay.choice",
                    std::string{"event"}
                ),
            })
        );
        REQUIRE(first.has_value());
        REQUIRE(first->observedInstances().size() == 2U);
        auto const eventId  = first->observedInstances()[0].observedInstanceId.value();
        auto const choiceId = first->observedInstances()[1].observedInstanceId.value();
        CHECK(choiceId != eventId);
        CHECK(eventId.size() == 68U);
        CHECK(eventId.starts_with("oi1_"));
        CHECK(std::ranges::all_of(
            eventId.substr(4),
            [](char character)
            {
                return (character >= '0' && character <= '9')
                    || (character >= 'a' && character <= 'f');
            }
        ));
        REQUIRE(first->observedInstances()[1].parentObservedInstanceId.has_value());
        CHECK(
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): REQUIRE above proved engagement.
            first->observedInstances()[1].parentObservedInstanceId->value()
            == eventId
        );
        CHECK(first->projectToolPreconditions().size() == 1U);
        CHECK(
            first->projectToolPreconditions()[0].status
            == ProjectToolPreconditionStatus::Known
        );
        CHECK(first->canonicalBytes().find("semantic_identity_basis") == std::string::npos);
        CHECK(first->canonicalBytes().find("identity_schema_id") == std::string::npos);
        CHECK(first->canonicalBytes().find("local_ref") == std::string::npos);
        CHECK(first->canonicalBytes().find(eventId) != std::string::npos);
        CHECK(first->canonicalBytes().find(choiceId) != std::string::npos);

        auto equivalentProposal = observationProposal({
            observedInstanceProposal(
                "renamed-event",
                "overlay.event",
                std::nullopt,
                4.0,
                true
            ),
            observedInstanceProposal(
                "renamed-choice",
                "overlay.choice",
                std::string{"renamed-event"},
                4.0,
                true
            ),
        });
        equivalentProposal.canonicalOpaquePayload = json::Value::ofObject({
            json::Member{
                "surface",
                json::Value::ofString("different opaque envelope payload"),
            },
        });
        for (auto& instance : equivalentProposal.observedInstanceProposals)
        {
            instance.opaqueProjectPayload = json::Value::ofObject({
                json::Member{
                    "visible",
                    json::Value::ofBoolean(false),
                },
            });
        }
        auto equivalent = prepared.store.publishProjectObservation(
            prepared.lease,
            prepared.plugin,
            scope,
            schemas,
            equivalentProposal
        );
        REQUIRE(equivalent.has_value());
        CHECK(
            equivalent->observedInstances()[0].observedInstanceId.value()
            == eventId
        );
        CHECK(
            equivalent->observedInstances()[1].observedInstanceId.value()
            == choiceId
        );

        auto changedBasis = prepared.store.publishProjectObservation(
            prepared.lease,
            prepared.plugin,
            scope,
            schemas,
            observationProposal({
                observedInstanceProposal("event", "overlay.other"),
            })
        );
        REQUIRE(changedBasis.has_value());
        CHECK(
            changedBasis->observedInstances()[0].observedInstanceId.value()
            != eventId
        );

        auto changedBasisMember = prepared.store.publishProjectObservation(
            prepared.lease,
            prepared.plugin,
            scope,
            schemas,
            observationProposal({
                observedInstanceProposal(
                    "event",
                    "overlay.event",
                    std::nullopt,
                    5.0
                ),
            })
        );
        REQUIRE(changedBasisMember.has_value());
        CHECK(
            changedBasisMember->observedInstances()[0].observedInstanceId.value()
            != eventId
        );

        auto changedKindProposal = observationProposal({
            observedInstanceProposal("event", "overlay.event"),
        });
        changedKindProposal.observedInstanceProposals.front().kind =
            "fixture.other-overlay";
        auto changedKind = prepared.store.publishProjectObservation(
            prepared.lease,
            prepared.plugin,
            scope,
            schemas,
            changedKindProposal
        );
        REQUIRE(changedKind.has_value());
        CHECK(
            changedKind->observedInstances()[0].observedInstanceId.value()
            != eventId
        );

        auto changedScope = prepared.store.publishProjectObservation(
            prepared.lease,
            prepared.plugin,
            runScope("run-8", 8U),
            schemas,
            observationProposal({
                observedInstanceProposal("event", "overlay.event"),
            })
        );
        REQUIRE(changedScope.has_value());
        CHECK_MESSAGE(
            changedScope->observedInstances()[0].observedInstanceId.value()
                != eventId,
            "changing only the run scope must mint an isolated ID"
        );
    }

    TEST_CASE("observed instance proposal refusals follow the normative precedence")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto schemas   = prepared.project.observedInstanceIdentitySchemas;
        auto const scope = runScope();
        auto invalidStatusAndName = observationProposal({});
        invalidStatusAndName.projectToolPreconditions.front().status =
            static_cast<ProjectToolPreconditionStatus>(0xFFU);
        invalidStatusAndName.projectToolPreconditions.emplace_back(
            ProjectToolPrecondition{
                .name   = "not_namespaced",
                .status = ProjectToolPreconditionStatus::Known,
            }
        );
        expectProjectObservationError(
            prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.plugin,
                scope,
                schemas,
                invalidStatusAndName
            ),
            ProjectObservationErrorCode::PreconditionNameNotNamespaced
        );

        auto invalidStatus = observationProposal({});
        invalidStatus.projectToolPreconditions.front().status =
            static_cast<ProjectToolPreconditionStatus>(0xFFU);
        expectProjectObservationError(
            prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.plugin,
                scope,
                schemas,
                invalidStatus
            ),
            ProjectObservationErrorCode::PreconditionStatusOutsideFactDomain
        );

        auto duplicatePrecondition = observationProposal({
            observedInstanceProposal("repeat", "overlay.a"),
            observedInstanceProposal("repeat", "overlay.b"),
        });
        duplicatePrecondition.projectToolPreconditions.emplace_back(
            duplicatePrecondition.projectToolPreconditions.front()
        );
        expectProjectObservationError(
            prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.plugin,
                scope,
                schemas,
                duplicatePrecondition
            ),
            ProjectObservationErrorCode::DuplicatePreconditionName
        );

        expectProjectObservationError(
            prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.plugin,
                scope,
                schemas,
                observationProposal({
                    observedInstanceProposal("repeat", "overlay.a"),
                    observedInstanceProposal("repeat", "overlay.b"),
                    observedInstanceProposal(
                        "orphan",
                        "overlay.orphan",
                        std::string{"missing"}
                    ),
                })
            ),
            ProjectObservationErrorCode::DuplicateObservedInstanceLocalRef
        );

        expectProjectObservationError(
            prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.plugin,
                scope,
                schemas,
                observationProposal({
                    observedInstanceProposal(
                        "cycle-a",
                        "overlay.same",
                        std::string{"cycle-b"}
                    ),
                    observedInstanceProposal(
                        "cycle-b",
                        "overlay.same",
                        std::string{"cycle-a"}
                    ),
                    observedInstanceProposal(
                        "orphan",
                        "overlay.orphan",
                        std::string{"missing"}
                    ),
                })
            ),
            ProjectObservationErrorCode::ObservedInstanceParentMissing
        );

        auto cycleBeforeRegistration = observationProposal({
            observedInstanceProposal(
                "cycle-a",
                "overlay.a",
                std::string{"cycle-b"}
            ),
            observedInstanceProposal(
                "cycle-b",
                "overlay.b",
                std::string{"cycle-a"}
            ),
            observedInstanceProposal("stray", "overlay.stray"),
        });
        cycleBeforeRegistration.observedInstanceProposals.back().identitySchemaId =
            "https://fixture.example/identity/unregistered/v1";
        expectProjectObservationError(
            prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.plugin,
                scope,
                schemas,
                cycleBeforeRegistration
            ),
            ProjectObservationErrorCode::ObservedInstanceParentCycle
        );

        auto unregisteredBeforeBasis = observationProposal({
            observedInstanceProposal(
                "invalid",
                "overlay.invalid",
                std::nullopt,
                -1.0
            ),
            observedInstanceProposal("stray", "overlay.stray"),
        });
        unregisteredBeforeBasis.observedInstanceProposals.back().identitySchemaId =
            "https://fixture.example/identity/unregistered/v1";
        expectProjectObservationError(
            prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.plugin,
                scope,
                schemas,
                unregisteredBeforeBasis
            ),
            ProjectObservationErrorCode::ObservedInstanceIdentitySchemaNotRegistered
        );

        expectProjectObservationError(
            prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.plugin,
                scope,
                schemas,
                observationProposal({
                    observedInstanceProposal("duplicate-a", "overlay.same"),
                    observedInstanceProposal("duplicate-b", "overlay.same"),
                    observedInstanceProposal(
                        "invalid",
                        "overlay.invalid",
                        std::nullopt,
                        -1.0
                    ),
                })
            ),
            ProjectObservationErrorCode::SemanticIdentityBasisSchemaViolation
        );
    }

    TEST_CASE("missing observed instance parent emits ObservedInstanceParentMissing")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto schemas   = prepared.project.observedInstanceIdentitySchemas;
        expectProjectObservationError(
            prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.plugin,
                runScope(),
                schemas,
                observationProposal({
                    observedInstanceProposal(
                        "orphan",
                        "overlay.orphan",
                        std::string{"missing"}
                    ),
                })
            ),
            ProjectObservationErrorCode::ObservedInstanceParentMissing
        );
    }

    TEST_CASE("duplicate observed instance authority emits ObservedInstanceCollision")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto schemas   = prepared.project.observedInstanceIdentitySchemas;
        expectProjectObservationError(
            prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.plugin,
                runScope(),
                schemas,
                observationProposal({
                    observedInstanceProposal("duplicate-a", "overlay.same"),
                    observedInstanceProposal("duplicate-b", "overlay.same"),
                })
            ),
            ProjectObservationErrorCode::ObservedInstanceCollision
        );
    }

    TEST_CASE("collision precedes registration scope mismatch")
    {
        auto temporary     = TemporaryDirectory{};
        auto prepared      = test_support::prepareStore(temporary.path());
        auto foreignSource = k_pluginSource;
        foreignSource += "\n-- exact registration variant\n";
        auto const foreign = makeProject("fixture.alpha", foreignSource);
        auto foreignSchemas = foreign.observedInstanceIdentitySchemas;
        REQUIRE(
            foreign.registration.hash()
            != prepared.project.registration.hash()
        );

        expectProjectObservationError(
            prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.plugin,
                runScope(),
                foreignSchemas,
                observationProposal({
                    observedInstanceProposal("foreign", "overlay.foreign"),
                })
            ),
            ProjectObservationErrorCode::ObservedInstanceScopeMismatch
        );
        expectProjectObservationError(
            prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.plugin,
                runScope(),
                foreignSchemas,
                observationProposal({
                    observedInstanceProposal("duplicate-a", "overlay.same"),
                    observedInstanceProposal("duplicate-b", "overlay.same"),
                })
            ),
            ProjectObservationErrorCode::ObservedInstanceCollision
        );
    }

    TEST_CASE("fresh observed instance membership is accepted")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto schemas   = prepared.project.observedInstanceIdentitySchemas;
        auto const scope = runScope();
        auto first = prepared.store.publishProjectObservation(
            prepared.lease,
            prepared.plugin,
            scope,
            schemas,
            observationProposal({
                observedInstanceProposal("first", "overlay.first"),
            })
        );
        REQUIRE(first.has_value());
        auto const id = first->observedInstances()[0].observedInstanceId.value();
        auto const allowed = prepared.store.resolveObservedInstance(
            prepared.lease,
            scope,
            *first,
            id
        );
        REQUIRE(allowed.has_value());
        CHECK(allowed->value() == id);
    }

    TEST_CASE("cross-run observed instance use emits ObservedInstanceScopeMismatch")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto schemas   = prepared.project.observedInstanceIdentitySchemas;
        auto const scope = runScope();
        auto first = prepared.store.publishProjectObservation(
            prepared.lease,
            prepared.plugin,
            scope,
            schemas,
            observationProposal({
                observedInstanceProposal("first", "overlay.first"),
            })
        );
        REQUIRE(first.has_value());
        auto const id = first->observedInstances()[0].observedInstanceId.value();
        expectProjectObservationError(
            prepared.store.resolveObservedInstance(
                prepared.lease,
                runScope("run-8", 8U),
                *first,
                id
            ),
            ProjectObservationErrorCode::ObservedInstanceScopeMismatch
        );
    }

    TEST_CASE("fault matrix stale instance expires without emitting input")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto schemas   = prepared.project.observedInstanceIdentitySchemas;
        auto const scope = runScope();
        auto first = prepared.store.publishProjectObservation(
            prepared.lease,
            prepared.plugin,
            scope,
            schemas,
            observationProposal({
                observedInstanceProposal("first", "overlay.first"),
            })
        );
        REQUIRE(first.has_value());
        auto const id = first->observedInstances()[0].observedInstanceId.value();
        auto second = prepared.store.publishProjectObservation(
            prepared.lease,
            prepared.plugin,
            scope,
            schemas,
            observationProposal({
                observedInstanceProposal("second", "overlay.second"),
            })
        );
        REQUIRE(second.has_value());
        auto host = test_support::deliveringHost(prepared);
        auto const operation = test_support::createReadyOperation(
            prepared,
            "request-stale-instance",
            "command-1"
        );
        auto const stale = prepared.store.resolveObservedInstance(
            prepared.lease,
            scope,
            *second,
            id
        );
        if (stale.has_value())
        {
            auto const reserved = prepared.store.reserveDispatch(
                operation.operationId,
                operation.revision,
                prepared.lease,
                host->generation(),
                AuthorityDecisionId{"authority-stale-instance"},
                std::nullopt
            );
            REQUIRE(reserved.has_value());
            static_cast<void>(host->deliverReport(reserved->authority));
        }
        CHECK_MESSAGE(
            host->clicks() == 0U,
            "an expired observed instance must emit no input"
        );
        expectProjectObservationError(
            stale,
            ProjectObservationErrorCode::ObservedInstanceStale
        );
    }

    TEST_CASE("scope mismatch precedes stale observed instance membership")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto schemas   = prepared.project.observedInstanceIdentitySchemas;
        auto const scope = runScope();
        auto first = prepared.store.publishProjectObservation(
            prepared.lease,
            prepared.plugin,
            scope,
            schemas,
            observationProposal({
                observedInstanceProposal("first", "overlay.first"),
            })
        );
        REQUIRE(first.has_value());
        auto const id = first->observedInstances()[0].observedInstanceId.value();
        auto second = prepared.store.publishProjectObservation(
            prepared.lease,
            prepared.plugin,
            scope,
            schemas,
            observationProposal({
                observedInstanceProposal("second", "overlay.second"),
            })
        );
        REQUIRE(second.has_value());
        expectProjectObservationError(
            prepared.store.resolveObservedInstance(
                prepared.lease,
                runScope("run-8", 8U),
                *second,
                id
            ),
            ProjectObservationErrorCode::ObservedInstanceScopeMismatch
        );
    }

    TEST_CASE("observed instance binding survives a Coordinator reopen")
    {
        auto temporary = TemporaryDirectory{};
        auto retained = [&temporary]()
        {
            auto prepared = test_support::prepareStore(temporary.path());
            auto schemas  = prepared.project.observedInstanceIdentitySchemas;
            auto first = prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.plugin,
                runScope(),
                schemas,
                observationProposal({
                    observedInstanceProposal("first", "overlay.stable"),
                })
            );
            REQUIRE(first.has_value());
            return std::tuple{
                prepared.project,
                prepared.plugin,
                prepared.manifest,
                std::string{
                    first->observedInstances()[0].observedInstanceId.value()
                }
            };
        }();

        auto reopenedResult = OperatorCoordinator::open(
            temporary.path() / "production"
        );
        REQUIRE(reopenedResult.has_value());
        auto reopened = *std::move(reopenedResult);
        auto const& [project, plugin, manifest, firstId] = retained;
        auto const reopenedScope = ObservedInstanceWorldScope::run(
            "target-after-reopen",
            1
        );
        REQUIRE(reopenedScope.has_value());
        REQUIRE(reopened.pinSession(
            SessionPin{
                .sessionId                 = "session-after-reopen",
                .authenticatedControllerId = "controller-after-reopen",
                .idempotencyNamespace      = "controller-after-reopen",
                .projectRegistrationHash   = project.registration.hash(),
                .controllerCapabilities    = {
                    std::string{conformance::k_operateCapability},
                },
                .controlledTargetId = "target-after-reopen",
                .projectInstanceKey = "instance-1",
                .mode               = SessionMode::Write,
                .kind               = ControllerKind::Script,
                .worldScope         = *reopenedScope,
            },
            manifest,
            std::nullopt
        ).has_value());
        auto controller = reopened.bindController("session-after-reopen");
        REQUIRE(controller.has_value());
        auto lease = reopened.acquireLease(*controller);
        REQUIRE(lease.has_value());
        auto schemas = project.observedInstanceIdentitySchemas;
        auto afterReopen = reopened.publishProjectObservation(
            *lease,
            plugin,
            runScope(),
            schemas,
            observationProposal({
                observedInstanceProposal(
                    "different-local-ref",
                    "overlay.stable",
                    std::nullopt,
                    4.0,
                    true
                ),
            })
        );
        REQUIRE(afterReopen.has_value());
        CHECK(
            afterReopen->observedInstances()[0].observedInstanceId.value()
            == firstId
        );
    }

    TEST_CASE("observed instance authority isolates exact registrations")
    {
        auto temporary     = TemporaryDirectory{};
        auto variantSource = k_pluginSource;
        variantSource += "\n-- exact registration variant\n";
        auto const observeRegistration = [](
            std::filesystem::path const& path,
            std::string_view source
        )
        {
            auto prepared = prepareStore(path, source);
            auto schemas  = prepared.project.observedInstanceIdentitySchemas;
            auto observation = prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.plugin,
                runScope(),
                schemas,
                observationProposal({
                    observedInstanceProposal("same", "overlay.registration"),
                })
            );
            REQUIRE(observation.has_value());
            return std::tuple{
                prepared.project.registration.pluginId(),
                prepared.project.registration.hash(),
                std::string{
                    observation->observedInstances()[0].observedInstanceId.value()
                },
                prepared.store.databasePath(),
            };
        };
        auto const [
            firstPluginId,
            firstRegistrationHash,
            firstObservedInstanceId,
            firstDatabasePath
        ] = observeRegistration(
            temporary.path() / "registration-a",
            k_pluginSource
        );
        auto const [
            secondPluginId,
            secondRegistrationHash,
            secondObservedInstanceId,
            secondDatabasePath
        ] = observeRegistration(
            temporary.path() / "registration-b",
            variantSource
        );
        REQUIRE(firstPluginId == secondPluginId);
        REQUIRE(firstRegistrationHash != secondRegistrationHash);

        auto firstProbe = test_support::OperatorDatabaseProbe{
            firstDatabasePath,
        };
        auto secondProbe = test_support::OperatorDatabaseProbe{
            secondDatabasePath,
        };
        // prepareStore itself mints the fixture's snapshot observation, so the
        // store already holds one binding; the row under test is the one the
        // explicit publish minted, named by its minted id.
        auto const firstRows = firstProbe.readRows(
            "SELECT canonical_authority FROM observed_instance_bindings "
            "WHERE observed_instance_id='" + firstObservedInstanceId + "'"
        );
        auto const secondRows = secondProbe.readRows(
            "SELECT canonical_authority FROM observed_instance_bindings "
            "WHERE observed_instance_id='" + secondObservedInstanceId + "'"
        );
        REQUIRE(firstRows.size() == 1U);
        REQUIRE(firstRows.front().size() == 1U);
        REQUIRE(secondRows.size() == 1U);
        REQUIRE(secondRows.front().size() == 1U);
        auto const& firstAuthority  = firstRows.front().front();
        auto const& secondAuthority = secondRows.front().front();
        CHECK_MESSAGE(
            firstAuthority.contains(
                "\"project_registration_hash\":\""
                + firstRegistrationHash.hex()
                + "\""
            ),
            "the first canonical authority must bind its exact registration"
        );
        CHECK_MESSAGE(
            secondAuthority.contains(
                "\"project_registration_hash\":\""
                + secondRegistrationHash.hex()
                + "\""
            ),
            "the second canonical authority must bind its exact registration"
        );
        CHECK_MESSAGE(
            firstAuthority != secondAuthority,
            "changing only the exact registration must change canonical authority"
        );
        CHECK_MESSAGE(
            firstObservedInstanceId != secondObservedInstanceId,
            "different exact registrations must mint isolated IDs"
        );
    }

    TEST_CASE("observed instance identity schemas refuse a set the registration never pinned")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto const& registration = prepared.project.registration;

        // A validator whose bytes the registration never pinned -- the schema
        // hash of some other document -- must be refused when the authority is
        // built, not when an observation is judged. Both hash sets are named,
        // because which supplied validator is unpinned and which pinned
        // document has no validator are the whole of the diagnosis.
        auto wrong = ObservedInstanceIdentitySchemas::create(
            registration,
            {
                ObservedInstanceIdentitySchema{
                    .schemaId   = "https://fixture.example/identity/overlay/v1",
                    .schemaHash = test_support::schemaHash(
                        test_support::k_projectObservationSchema
                    ),
                    .validate   = [](json::Value const&) -> Status
                    {
                        return ok();
                    },
                },
            }
        );
        REQUIRE_FALSE(wrong.has_value());
        auto const wrongMessage = std::string{wrong.error().message()};
        CHECK(
            wrongMessage.find(
                test_support::schemaHashHex(test_support::k_projectObservationSchema)
            )
            != std::string::npos
        );
        CHECK(
            wrongMessage.find(
                test_support::schemaHashHex(test_support::k_observedIdentitySchema)
            )
            != std::string::npos
        );

        // The other direction: a pinned document with no validator supplied is
        // a registration the authority would answer for without being able to
        // apply, and is equally refused.
        auto missing = ObservedInstanceIdentitySchemas::create(
            registration,
            {}
        );
        REQUIRE_FALSE(missing.has_value());
        CHECK(
            std::string{missing.error().message()}.find(
                test_support::schemaHashHex(test_support::k_observedIdentitySchema)
            ) != std::string::npos
        );

        // The evasion that rule closes: two bindings with distinct IDs that
        // claim the same pinned hash. A creator that collapsed the supplied
        // set before comparing would pass this registration while leaving a
        // second validator usable that no pinned document establishes.
        auto duplicate = ObservedInstanceIdentitySchemas::create(
            registration,
            {
                ObservedInstanceIdentitySchema{
                    .schemaId   = "https://fixture.example/identity/overlay/v1",
                    .schemaHash = test_support::schemaHash(
                        test_support::k_observedIdentitySchema
                    ),
                    .validate   = [](json::Value const&) -> Status
                    {
                        return ok();
                    },
                },
                ObservedInstanceIdentitySchema{
                    .schemaId   = "https://fixture.example/identity/overlay/v2",
                    .schemaHash = test_support::schemaHash(
                        test_support::k_observedIdentitySchema
                    ),
                    .validate   = [](json::Value const&) -> Status
                    {
                        return ok();
                    },
                },
            }
        );
        REQUIRE_FALSE(duplicate.has_value());
        CHECK(
            std::string{duplicate.error().message()}.find("unique and sorted")
            != std::string::npos
        );
    }

    TEST_CASE("observed instance mint separates project and registration domains")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto const scope = runScope();
        auto schemas = prepared.project.observedInstanceIdentitySchemas;
        auto first = prepared.store.publishProjectObservation(
            prepared.lease,
            prepared.plugin,
            scope,
            schemas,
            observationProposal({
                observedInstanceProposal("first", "overlay.domain"),
            })
        );
        REQUIRE(first.has_value());
        auto const firstId = first->observedInstances()[0].observedInstanceId.value();

        REQUIRE(prepared.store.provisionProjectInstance(
            prepared.project.registration,
            prepared.plugin,
            ProjectInstanceBaseline{
                .projectInstanceKey  = "instance-2",
                .eventId             = "baseline-2",
                .sessionManifestHash = prepared.manifest.hash(),
                .entry = journalEntry(
                    prepared.project,
                    prepared.project.registration.baselineEventType(),
                    "{\"kind\":\"baseline\"}"
                ),
            }
        ).has_value());
        auto const projectScope = ObservedInstanceWorldScope::run(
            "target-project-2",
            1
        );
        REQUIRE(projectScope.has_value());
        REQUIRE(prepared.store.pinSession(
            SessionPin{
                .sessionId                 = "session-project-2",
                .authenticatedControllerId = "controller-project-2",
                .idempotencyNamespace      = "controller-project-2",
                .projectRegistrationHash =
                    prepared.project.registration.hash(),
                .controllerCapabilities = {
                    std::string{conformance::k_operateCapability},
                },
                .controlledTargetId = "target-project-2",
                .projectInstanceKey = "instance-2",
                .mode               = SessionMode::Write,
                .kind               = ControllerKind::Script,
                .worldScope         = *projectScope,
            },
            prepared.manifest,
            std::nullopt
        ).has_value());
        auto projectController = prepared.store.bindController("session-project-2");
        REQUIRE(projectController.has_value());
        auto projectLease = prepared.store.acquireLease(*projectController);
        REQUIRE(projectLease.has_value());
        auto otherProject = prepared.store.publishProjectObservation(
            *projectLease,
            prepared.plugin,
            scope,
            schemas,
            observationProposal({
                observedInstanceProposal("first", "overlay.domain"),
            })
        );
        REQUIRE(otherProject.has_value());
        CHECK_MESSAGE(
            otherProject->observedInstances()[0].observedInstanceId.value()
                != firstId,
            "changing only the project instance must mint an isolated ID"
        );
        auto const crossProject = prepared.store.resolveObservedInstance(
            *projectLease,
            scope,
            *otherProject,
            firstId
        );
        expectProjectObservationError(
            crossProject,
            ProjectObservationErrorCode::ObservedInstanceScopeMismatch
        );

        auto const foreignSource = test_support::pluginSource("fixture.foreign");
        auto foreignProject = makeProject("fixture.foreign", foreignSource);
        auto foreignPlugin  = loadPlugin(foreignProject, foreignSource);
        auto foreignManifest = test_support::sessionManifest(
            foreignProject.registration,
            prepared.runtimeArtifactRootHash,
            hashOf("agent"),
            test_support::policyArtifactBytes()
        );
        REQUIRE(prepared.store.registerProject(
            foreignProject.registration
        ).has_value());
        REQUIRE(prepared.store.provisionProjectInstance(
            foreignProject.registration,
            foreignPlugin,
            ProjectInstanceBaseline{
                .projectInstanceKey  = "instance-1",
                .eventId             = "baseline-foreign",
                .sessionManifestHash = foreignManifest.hash(),
                .entry = journalEntry(
                    foreignProject,
                    foreignProject.registration.baselineEventType(),
                    "{\"kind\":\"baseline\"}"
                ),
            }
        ).has_value());
        auto const foreignScope = ObservedInstanceWorldScope::run(
            "target-foreign",
            1
        );
        REQUIRE(foreignScope.has_value());
        REQUIRE(prepared.store.pinSession(
            SessionPin{
                .sessionId                 = "session-foreign",
                .authenticatedControllerId = "controller-foreign",
                .idempotencyNamespace      = "controller-foreign",
                .projectRegistrationHash   = foreignProject.registration.hash(),
                .controllerCapabilities    = {
                    std::string{conformance::k_operateCapability},
                },
                .controlledTargetId = "target-foreign",
                .projectInstanceKey = "instance-1",
                .mode               = SessionMode::Write,
                .kind               = ControllerKind::Script,
                .worldScope         = *foreignScope,
            },
            foreignManifest,
            std::nullopt
        ).has_value());
        auto foreignController = prepared.store.bindController("session-foreign");
        REQUIRE(foreignController.has_value());
        auto foreignLease = prepared.store.acquireLease(*foreignController);
        REQUIRE(foreignLease.has_value());
        auto foreignSchemas = foreignProject.observedInstanceIdentitySchemas;
        auto foreignObservation = prepared.store.publishProjectObservation(
            *foreignLease,
            foreignPlugin,
            scope,
            foreignSchemas,
            observationProposal({
                observedInstanceProposal("first", "overlay.domain"),
            })
        );
        REQUIRE(foreignObservation.has_value());
        CHECK(
            foreignObservation->observedInstances()[0].observedInstanceId.value()
            != firstId
        );

        auto const crossRegistration = prepared.store.resolveObservedInstance(
            *foreignLease,
            scope,
            *foreignObservation,
            firstId
        );
        expectProjectObservationError(
            crossRegistration,
            ProjectObservationErrorCode::ObservedInstanceScopeMismatch
        );
    }

    // The refusals below drive createSnapshot themselves instead of going
    // through prepareStore's own snapshot REQUIRE: the case under test is the
    // mint refusing, so a fixture that demands the first mint succeed cannot
    // host it. The world is assembled up to the pinned lease and nothing more.
    struct PinnedSourceStore final
    {
        OperatorCoordinator          store;
        ProjectPluginHandle          plugin;
        test_support::ProjectFixture project;
        ControlLease                 lease;
        conformance::ObservationHost observation;
    };

    [[nodiscard]]
    auto pinSourceStore(
        std::filesystem::path const& path,
        std::string pluginId,
        std::string_view source,
        std::string_view observationSchema = test_support::k_projectObservationSchema
    ) -> PinnedSourceStore
    {
        auto const release = test_support::runtimeRelease(path / "session-handoff");
        auto storeResult = OperatorCoordinator::open(path / "production");
        REQUIRE(storeResult.has_value());
        auto store = *std::move(storeResult);
        auto installed = store.installRuntimeArtifact(
            RuntimeArtifactInstallRequest{
                .handoffRoot                 = release.handoffRoot,
                .expectedReleaseManifestHash = release.releaseManifestHash,
                .expectedInstalledGeneration = 0U,
            }
        );
        REQUIRE(installed.has_value());
        auto const project = makeProject(
            std::move(pluginId),
            source,
            observationSchema
        );
        auto const manifest = sessionManifest(
            project.registration,
            installed->rootHash(),
            hashOf("agent"),
            test_support::policyArtifactBytes()
        );
        auto const plugin = loadPlugin(project, source);
        REQUIRE(store.registerProject(project.registration).has_value());
        REQUIRE(store.provisionProjectInstance(
            project.registration,
            plugin,
            ProjectInstanceBaseline{
                .projectInstanceKey  = "instance-1",
                .eventId             = "baseline-1",
                .sessionManifestHash = manifest.hash(),
                .entry = journalEntry(
                    project,
                    project.registration.baselineEventType(),
                    "{\"kind\":\"baseline\"}"
                ),
            }
        ).has_value());
        auto const worldScope = ObservedInstanceWorldScope::run(
            "target-1",
            1
        );
        REQUIRE(worldScope.has_value());
        REQUIRE(store.pinSession(
            SessionPin{
                .sessionId                 = "session-1",
                .authenticatedControllerId = "controller-1",
                .idempotencyNamespace      = "controller-1",
                .projectRegistrationHash   = project.registration.hash(),
                .controllerCapabilities    = {std::string{conformance::k_operateCapability}},
                .controlledTargetId        = "target-1",
                .projectInstanceKey        = "instance-1",
                .mode                      = SessionMode::Write,
                .kind                      = ControllerKind::Script,
                .worldScope                = *worldScope,
            },
            manifest,
            std::nullopt
        ).has_value());
        auto controller = store.bindController("session-1");
        REQUIRE(controller.has_value());
        auto lease = store.acquireLease(*controller);
        REQUIRE(lease.has_value());
        auto observation = conformance::activateObservationHost(
            *std::move(installed),
            test_support::umbraflowProbeFrame(),
            FrameId{211}
        );
        auto const reading = conformance::observeOnce(observation);
        conformance::requireResolvedSurface(reading, test_support::k_fixtureUiAction.surface);
        return PinnedSourceStore{
            .store       = std::move(store),
            .plugin      = std::move(plugin),
            .project     = std::move(project),
            .lease       = *lease,
            .observation = std::move(observation),
        };
    }

    TEST_CASE("a derive envelope missing a required member is MalformedProposal, not a termination")
    {
        auto temporary = TemporaryDirectory{};
        // The registration pins a permissive observation schema, so the derive
        // output below is stamped -- a member the proposal contract requires is
        // absent from a document that schema accepted. Only the proposal
        // reader's defensive member check can refuse it; a contract check
        // there would abort this test's process instead of returning.
        auto const permissiveObservationSchema = std::string_view{
            R"json({
            "$schema": "https://json-schema.org/draft/2020-12/schema",
            "$id": "https://umbraflow.dev/schema/project/observation",
            "type": "object"
        })json"
        };
        auto const malformedDerive = std::string{
            "{ schema = \"umbraflow-project-observation-proposal/v1\","
            " project_tool_preconditions = {}, observed_instance_proposals = {} }"
        };
        auto const source = test_support::pluginSource(
            "fixture.malformed",
            test_support::k_fixtureUiActionIntent,
            malformedDerive
        );
        auto pinned = pinSourceStore(
            temporary.path(),
            "fixture.malformed",
            source,
            permissiveObservationSchema
        );
        auto const refused = pinned.store.createSnapshot(
            pinned.lease,
            pinned.plugin,
            pinned.project.toolCatalogSchemaOwner,
            pinned.project.observedInstanceIdentitySchemas,
            conformance::observeOnce(pinned.observation)
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(
            projectObservationErrorCode(refused.error())
            == ProjectObservationErrorCode::MalformedProposal
        );
        // The refusal names the missing member, and the case reached this line
        // at all -- the refusal was a Result rather than an abort.
        CHECK(refused.error().message().contains("canonical_opaque_payload"));
    }

    TEST_CASE("production mint is stable across an identical re-observation")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const firstId = prepared.snapshot.observation.payload()
            .observedInstances()[0].observedInstanceId.value();
        CHECK(firstId.starts_with("oi1_"));

        // The identical world through the identical derive: the mint answers
        // with the same instance id because the canonical authority -- not a
        // fresh random draw -- is what the id is minted from. Without the
        // dedup a second observation would mint a second id and this fails.
        auto const again = prepared.store.createSnapshot(
            prepared.lease,
            prepared.plugin,
            prepared.project.toolCatalogSchemaOwner,
            prepared.project.observedInstanceIdentitySchemas,
            conformance::observeOnce(prepared.observation)
        );
        REQUIRE(again.has_value());
        REQUIRE(again->observation.payload().observedInstances().size() == 1U);
        CHECK(
            again->observation.payload().observedInstances()[0]
                .observedInstanceId.value()
            == firstId
        );
    }

    TEST_CASE("production mint refuses a collision between two proposals")
    {
        auto temporary = TemporaryDirectory{};
        auto const shared = std::string{test_support::k_fixtureIdentitySchemaId};
        auto const source = test_support::pluginSource(
            "fixture.collision",
            test_support::k_fixtureUiActionIntent,
            "{ schema = \"umbraflow-project-observation-proposal/v1\","
            " canonical_opaque_payload = {}, project_tool_preconditions = {},"
            " observed_instance_proposals = {"
            " { local_ref = \"first\", kind = \"fixture.control\","
            " identity_schema_id = \"" + shared + "\","
            " semantic_identity_basis = { native_id = \"same\", surface_epoch = 1 },"
            " opaque_project_payload = {} },"
            " { local_ref = \"second\", kind = \"fixture.control\","
            " identity_schema_id = \"" + shared + "\","
            " semantic_identity_basis = { native_id = \"same\", surface_epoch = 1 },"
            " opaque_project_payload = {} } } }"
        );
        auto pinned = pinSourceStore(temporary.path(), "fixture.collision", source);
        auto const refused = pinned.store.createSnapshot(
            pinned.lease,
            pinned.plugin,
            pinned.project.toolCatalogSchemaOwner,
            pinned.project.observedInstanceIdentitySchemas,
            conformance::observeOnce(pinned.observation)
        );
        expectProjectObservationError(
            refused,
            ProjectObservationErrorCode::ObservedInstanceCollision
        );
    }

    TEST_CASE("production mint refuses an identity schema outside the registration closure")
    {
        auto temporary = TemporaryDirectory{};
        auto const source = test_support::pluginSource(
            "fixture.outside",
            test_support::k_fixtureUiActionIntent,
            "{ schema = \"umbraflow-project-observation-proposal/v1\","
            " canonical_opaque_payload = {}, project_tool_preconditions = {},"
            " observed_instance_proposals = {"
            " { local_ref = \"outside\", kind = \"fixture.control\","
            " identity_schema_id = \"https://other.example/identity/v1\","
            " semantic_identity_basis = { native_id = \"outside\", surface_epoch = 1 },"
            " opaque_project_payload = {} } } }"
        );
        auto pinned = pinSourceStore(temporary.path(), "fixture.outside", source);
        auto const refused = pinned.store.createSnapshot(
            pinned.lease,
            pinned.plugin,
            pinned.project.toolCatalogSchemaOwner,
            pinned.project.observedInstanceIdentitySchemas,
            conformance::observeOnce(pinned.observation)
        );
        expectProjectObservationError(
            refused,
            ProjectObservationErrorCode::ObservedInstanceIdentitySchemaNotRegistered
        );
    }

    TEST_CASE("production mint refuses a proposal whose parents cycle")
    {
        auto temporary = TemporaryDirectory{};
        auto const shared = std::string{test_support::k_fixtureIdentitySchemaId};
        auto const source = test_support::pluginSource(
            "fixture.cycle",
            test_support::k_fixtureUiActionIntent,
            "{ schema = \"umbraflow-project-observation-proposal/v1\","
            " canonical_opaque_payload = {}, project_tool_preconditions = {},"
            " observed_instance_proposals = {"
            " { local_ref = \"first\", parent_local_ref = \"second\","
            " kind = \"fixture.control\", identity_schema_id = \"" + shared + "\","
            " semantic_identity_basis = { native_id = \"first\", surface_epoch = 1 },"
            " opaque_project_payload = {} },"
            " { local_ref = \"second\", parent_local_ref = \"first\","
            " kind = \"fixture.control\", identity_schema_id = \"" + shared + "\","
            " semantic_identity_basis = { native_id = \"second\", surface_epoch = 1 },"
            " opaque_project_payload = {} } } }"
        );
        auto pinned = pinSourceStore(temporary.path(), "fixture.cycle", source);
        auto const refused = pinned.store.createSnapshot(
            pinned.lease,
            pinned.plugin,
            pinned.project.toolCatalogSchemaOwner,
            pinned.project.observedInstanceIdentitySchemas,
            conformance::observeOnce(pinned.observation)
        );
        expectProjectObservationError(
            refused,
            ProjectObservationErrorCode::ObservedInstanceParentCycle
        );
    }

    TEST_CASE("a step naming an instance minted in another scope is refused at the gate")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const firstId = prepared.snapshot.observation.payload()
            .observedInstances()[0].observedInstanceId.value();

        // A second registration whose next_step names the FIRST session's
        // minted id back to the gate: the only way the production path can
        // name a foreign id at all is a plugin that spells it, so the plugin
        // differs from the fixture's and the registration with it.
        auto const crossScopeStep = std::string{
            "{ action = { action_id = \"fixture.press\","
            " canonical_parameters = { value = 1 },"
            " surface_id = \"fixture.surface\", ui_target_id = \""
            + firstId
            + "\" }, binding_variant_constraints = {}, delivery_class = \"delivery_safe\","
              " expected_ui_postconditions = {}, required_ui_preconditions = {},"
              " step_key = \"fixture.step\","
              " timeout_policy = { maximum_elapsed_ms = 5000, on_timeout = \"reobserve\" } }"
        };
        auto const foreignSource = test_support::pluginSource(
            "fixture.other",
            crossScopeStep
        );
        auto foreignProject = makeProject("fixture.other", foreignSource);
        auto foreignPlugin  = loadPlugin(foreignProject, foreignSource);
        auto foreignManifest = test_support::sessionManifest(
            foreignProject.registration,
            prepared.runtimeArtifactRootHash,
            hashOf("agent"),
            test_support::policyArtifactBytes()
        );
        REQUIRE(prepared.store.registerProject(
            foreignProject.registration
        ).has_value());
        REQUIRE(prepared.store.provisionProjectInstance(
            foreignProject.registration,
            foreignPlugin,
            ProjectInstanceBaseline{
                .projectInstanceKey  = "instance-other",
                .eventId             = "baseline-other",
                .sessionManifestHash = foreignManifest.hash(),
                .entry = journalEntry(
                    foreignProject,
                    foreignProject.registration.baselineEventType(),
                    "{\"kind\":\"baseline\"}"
                ),
            }
        ).has_value());
        auto const otherScope = ObservedInstanceWorldScope::run(
            "target-other",
            1
        );
        REQUIRE(otherScope.has_value());
        REQUIRE(prepared.store.pinSession(
            SessionPin{
                .sessionId                 = "session-other",
                .authenticatedControllerId = "controller-other",
                .idempotencyNamespace      = "controller-other",
                .projectRegistrationHash   = foreignProject.registration.hash(),
                .controllerCapabilities    = {
                    std::string{conformance::k_operateCapability},
                },
                .controlledTargetId = "target-other",
                .projectInstanceKey = "instance-other",
                .mode               = SessionMode::Write,
                .kind               = ControllerKind::Script,
                .worldScope         = *otherScope,
            },
            foreignManifest,
            std::nullopt
        ).has_value());
        auto otherController = prepared.store.bindController("session-other");
        REQUIRE(otherController.has_value());
        auto otherLease = prepared.store.acquireLease(*otherController);
        REQUIRE(otherLease.has_value());

        // The other session mints its own world first, so the refusal below
        // is about the id the step names and not about the observation.
        auto otherSnapshot = prepared.store.createSnapshot(
            *otherLease,
            foreignPlugin,
            foreignProject.toolCatalogSchemaOwner,
            foreignProject.observedInstanceIdentitySchemas,
            conformance::observeOnce(prepared.observation)
        );
        REQUIRE(otherSnapshot.has_value());

        auto const runtimeModel = prepared.observation.host->runtimeModelBinding(
            prepared.observation.generation
        );
        REQUIRE(runtimeModel.has_value());
        auto foreignAuthority = conformance::planAuthority(
            foreignProject.registration,
            foreignManifest,
            *runtimeModel,
            "operator",
            test_support::policyArtifactBytes(),
            test_support::k_fixtureUiAction
        );
        REQUIRE(foreignAuthority.has_value());

        // A mutating tool: the scope check runs in mintNextStep, which a
        // read-only Operation never reaches because it carries no frozen plan.
        auto operation = prepared.store.submitCommand(
            *otherController,
            command(*otherSnapshot, "request-other", "controller-other"),
            toolInvocation(foreignProject, "command-1")
        );
        REQUIRE(operation.has_value());
        auto frozen = prepared.store.freezePlan(
            operation->operation.operationId,
            operation->operation.revision,
            *otherLease,
            foreignPlugin,
            foreignProject.toolCatalogSchemaOwner,
            *foreignAuthority
        );
        REQUIRE(frozen.has_value());
        auto const refused = prepared.store.mintNextStep(
            frozen->operation.operationId,
            frozen->operation.revision,
            *otherLease,
            foreignPlugin,
            foreignProject.toolCatalogSchemaOwner,
            *foreignAuthority
        );
        expectProjectObservationError(
            refused,
            ProjectObservationErrorCode::ObservedInstanceScopeMismatch
        );
    }

    // A read-only Operation never reaches mintNextStep, so the submitCommand
    // gate is the ONLY place its canonical arguments' observed instance ids
    // can be resolved. An id minted in another scope of the same registration
    // must therefore be refused at submitCommand itself, before the read-only
    // command is accepted at all.
    TEST_CASE("a read-only command naming an instance minted in another scope is refused at submitCommand")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(
            temporary.path(),
            k_pluginSource,
            test_support::k_toolPreconditionSchemaWithInstanceIds
        );
        auto const otherId = foreignObservedInstanceId(prepared);

        auto catalog = catalogAcceptingAnyArguments(
            prepared,
            "observe-1",
            "observe-1"
        );
        REQUIRE(catalog.has_value());
        auto invocation = catalog->validate(
            "observe-1",
            canonical(
                prepared.project.schemaOwner,
                "{\"observed_instance_id\":\"" + otherId + "\",\"value\":1}"
            )
        );
        REQUIRE(invocation.has_value());
        auto const refused = prepared.store.submitCommand(
            prepared.controller,
            command(prepared.snapshot, "request-observe-foreign", "controller-1"),
            *invocation
        );
        expectProjectObservationError(
            refused,
            ProjectObservationErrorCode::ObservedInstanceScopeMismatch
        );
    }

    // The fixture plugin has a proposal for command-1 but none for command-9,
    // so "command-9" is the canary entry: if the resolution ever moved out of
    // submitCommand into the plan path, plugin.plan would be called for a tool
    // it has no proposal for and fail with its own error. This case is green
    // only because the gate refused the command before plan was ever invoked,
    // and the mutating descriptor makes it the path the plan would actually
    // run on.
    TEST_CASE("a mutating command naming an instance minted in another scope is refused before plugin.plan")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(
            temporary.path(),
            k_pluginSource,
            test_support::k_toolPreconditionSchemaWithInstanceIds
        );
        auto const otherId = foreignObservedInstanceId(prepared);

        auto catalog = catalogAcceptingAnyArguments(
            prepared,
            "command-9",
            "command-1"
        );
        REQUIRE(catalog.has_value());
        auto invocation = catalog->validate(
            "command-9",
            canonical(
                prepared.project.schemaOwner,
                "{\"observed_instance_id\":\"" + otherId + "\",\"value\":1}"
            )
        );
        REQUIRE(invocation.has_value());
        auto const refused = prepared.store.submitCommand(
            prepared.controller,
            command(prepared.snapshot, "request-foreign-command", "controller-1"),
            *invocation
        );
        expectProjectObservationError(
            refused,
            ProjectObservationErrorCode::ObservedInstanceScopeMismatch
        );
    }

    // The deliver check compares the reserved step's resolved model target --
    // the binding's local_ref -- with the target the Host's own runtime
    // resolved for the receipt it mints. A plugin that observes two instances
    // under one session drives both halves of the check on identical binding
    // tables: the step naming the instance at the model's declared target
    // delivers, and the step naming the other instance is refused at
    // TaskHost::deliver, before the linearization point consumes anything.
    TEST_CASE("deliver refuses a receipt whose model target the reserved step's instance lacks")
    {
        auto const twoInstanceDerive = std::string{
            "{ schema = \"umbraflow-project-observation-proposal/v1\","
            " canonical_opaque_payload = {}, project_tool_preconditions = {},"
            " observed_instance_proposals = {"
            " { local_ref = \"fixture.target\", kind = \"fixture.control\","
            " identity_schema_id = \""
            + std::string{test_support::k_fixtureIdentitySchemaId}
            + "\", semantic_identity_basis = { native_id = \"fixture.target\","
            " surface_epoch = 1 }, opaque_project_payload = {} },"
            " { local_ref = \"target.B\", kind = \"fixture.control\","
            " identity_schema_id = \""
            + std::string{test_support::k_fixtureIdentitySchemaId}
            + "\", semantic_identity_basis = { native_id = \"target.B\","
            " surface_epoch = 1 }, opaque_project_payload = {} } } }"
        };
        auto const secondInstanceIntent = std::string{
            "{\n        action = { action_id = \"fixture.press\","
            " canonical_parameters = { value = 1 },"
            " surface_id = \"fixture.surface\","
            " ui_target_id = input.project_observation.observed_instances[2]"
            ".observed_instance_id },"
            "\n        binding_variant_constraints = {}, delivery_class = \"delivery_safe\","
            "\n        expected_ui_postconditions = {}, required_ui_preconditions = {},"
            "\n        step_key = \"fixture.step\","
            "\n        timeout_policy = { maximum_elapsed_ms = 5000, on_timeout = \"reobserve\" },"
            "\n    }"
        };

        // Positive control: the step names the instance whose local_ref is the
        // model's declared target, so the reserved authority carries that
        // target and the receipt the Host mints names it too.
        {
            auto temporary = TemporaryDirectory{};
            auto prepared = prepareStore(
                temporary.path(),
                test_support::pluginSource(
                    "fixture.alpha",
                    test_support::k_fixtureUiActionIntent,
                    twoInstanceDerive
                )
            );
            auto host = deliveringHost(prepared);
            auto const operation = createReadyOperation(
                prepared,
                "request-target-a",
                "command-1"
            );
            auto const reserved = prepared.store.reserveDispatch(
                operation.operationId,
                operation.revision,
                prepared.lease,
                host->generation(),
                AuthorityDecisionId{"authority-target-a"},
                std::nullopt
            );
            REQUIRE(reserved.has_value());
            CHECK(reserved->authority.uiTarget == "fixture.target");
            REQUIRE(host->deliver(reserved->authority).has_value());
        }

        // The negative half: the step names the other instance, whose
        // local_ref no receipt this model can mint carries. reserveDispatch
        // resolves it all the way to a reservation, and the deliver check --
        // not the ledger -- is the refusal that stops the Host from acting.
        {
            auto temporary = TemporaryDirectory{};
            auto prepared = prepareStore(
                temporary.path(),
                test_support::pluginSource(
                    "fixture.alpha",
                    secondInstanceIntent,
                    twoInstanceDerive
                )
            );
            auto host = deliveringHost(prepared);
            auto const operation = createReadyOperation(
                prepared,
                "request-target-b",
                "command-1"
            );
            auto const reserved = prepared.store.reserveDispatch(
                operation.operationId,
                operation.revision,
                prepared.lease,
                host->generation(),
                AuthorityDecisionId{"authority-target-b"},
                std::nullopt
            );
            REQUIRE(reserved.has_value());
            CHECK(reserved->authority.uiTarget == "target.B");
            auto const refused = host->deliver(reserved->authority);
            REQUIRE_FALSE(refused.has_value());
            CHECK_MESSAGE(
                automationErrorKind(refused.error()) == AutomationErrorKind::InvalidResource,
                "a wrong model target is a Host-authority disagreement, not a "
                "stale or foreign observation"
            );
            CHECK_MESSAGE(
                refused.error().message().contains(
                    "delivery receipt names a model target the reserved step's "
                    "observed instance does not"
                ),
                "the refusal must name the target disagreement"
            );
        }
    }

    TEST_CASE("the pinned world scope is immutable and survives a restart")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const firstId = prepared.snapshot.observation.payload()
            .observedInstances()[0].observedInstanceId.value();

        // The scope is part of the immutable session tuple: the same session,
        // the same manifest, a different generation on the same target is
        // refused while the tuple stands. Without the stored columns this
        // would be a silent re-pin.
        auto const movedScope = ObservedInstanceWorldScope::run(
            "target-1",
            2
        );
        REQUIRE(movedScope.has_value());
        auto const sameTuplePin = SessionPin{
            .sessionId                 = "session-1",
            .authenticatedControllerId = "controller-1",
            .idempotencyNamespace      = "controller-1",
            .projectRegistrationHash   = prepared.project.registration.hash(),
            .controllerCapabilities    = {std::string{conformance::k_operateCapability}},
            .controlledTargetId        = "target-1",
            .projectInstanceKey        = "instance-1",
            .mode                      = SessionMode::Write,
            .kind                      = ControllerKind::Script,
            .worldScope                = *movedScope,
        };
        auto const refused = prepared.store.pinSession(
            sameTuplePin,
            prepared.manifest,
            std::nullopt
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(
            refused.error().message().contains(
                "already names a different immutable session tuple"
            )
        );

        // Across a restart the columns the createSnapshot path restores still
        // name the same scope: a session pinned on the same target with the
        // same generation mints the same canonical id, because the mint reads
        // the scope back out of the stored tuple and not out of the caller.
        // The first coordinator holds the runtime directory exclusively, so
        // it is released before the reopened door can take it.
        {
            auto releasedStore = std::move(prepared.store);
        }
        auto restarted = OperatorCoordinator::open(temporary.path() / "production");
        REQUIRE(restarted.has_value());
        auto const restoredScope = ObservedInstanceWorldScope::run(
            "target-1",
            1
        );
        REQUIRE(restoredScope.has_value());
        REQUIRE(restarted->pinSession(
            SessionPin{
                .sessionId                 = "session-after-restart",
                .authenticatedControllerId = "controller-after-restart",
                .idempotencyNamespace      = "controller-after-restart",
                .projectRegistrationHash   = prepared.project.registration.hash(),
                .controllerCapabilities    = {
                    std::string{conformance::k_operateCapability},
                },
                .controlledTargetId = "target-1",
                .projectInstanceKey = "instance-1",
                .mode               = SessionMode::Write,
                .kind               = ControllerKind::Script,
                .worldScope         = *restoredScope,
            },
            prepared.manifest,
            std::nullopt
        ).has_value());
        auto afterRestartController = restarted->bindController("session-after-restart");
        REQUIRE(afterRestartController.has_value());
        auto afterRestartLease = restarted->acquireLease(*afterRestartController);
        REQUIRE(afterRestartLease.has_value());
        auto afterRestartSnapshot = restarted->createSnapshot(
            *afterRestartLease,
            prepared.plugin,
            prepared.project.toolCatalogSchemaOwner,
            prepared.project.observedInstanceIdentitySchemas,
            conformance::observeOnce(prepared.observation)
        );
        REQUIRE(afterRestartSnapshot.has_value());
        REQUIRE(afterRestartSnapshot->observation.payload().observedInstances().size() == 1U);
        CHECK(
            afterRestartSnapshot->observation.payload().observedInstances()[0]
                .observedInstanceId.value()
            == firstId
        );
    }

    TEST_CASE("PRAGMA user_version is no part of Operator schema identity")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        {
            auto created = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(
                created.has_value(),
                "a created schema must equal the pinned exact DDL schema identity"
            );
        }

        writeNonIdentityUserVersion(databasePath);
        {
            auto reopened = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(
                reopened.has_value(),
                "a user_version the Operator never writes must not refuse the open"
            );
        }

        // Without this the case would also pass against an open that reset the
        // header, which is a second identity mechanism rather than none.
        CHECK_MESSAGE(
            storedUserVersion(databasePath) == k_nonIdentityUserVersion,
            "the open must neither read nor write user_version"
        );
    }

    TEST_CASE("Tool root and call identities rejoin exact positions across restart")
    {
        auto temporary        = TemporaryDirectory{};
        auto const production = temporary.path() / "production";

        auto preimage = CanonicalJson::parseExact(
            R"({"objective":"drive"})"
        );
        REQUIRE(preimage.has_value());
        auto root = ToolRootRequestIdentity::create(
            "script:fixture",
            "request-1",
            std::move(*preimage)
        );
        REQUIRE(root.has_value());

        auto frameworkCatalog = FrameworkToolCatalogOwner::create();
        REQUIRE(frameworkCatalog.has_value());
        auto observeArguments = CanonicalJson::parseExact("{}");
        auto waitArguments = CanonicalJson::parseExact(
            R"({"duration_ms":250})"
        );
        REQUIRE(observeArguments.has_value());
        REQUIRE(waitArguments.has_value());
        auto observe = frameworkCatalog->validate(
            "framework.screen.observe",
            std::move(*observeArguments)
        );
        auto wait = frameworkCatalog->validate(
            "framework.workflow.wait",
            std::move(*waitArguments)
        );
        REQUIRE(observe.has_value());
        REQUIRE(wait.has_value());

        auto const execution = ToolExecutionIdentity{
            .runIdentity                 = hashOf("run-1"),
            .frameworkReleaseIdentity    = hashOf("framework-release-1"),
            .toolRuntimeProtocolIdentity = hashOf("tool-runtime-protocol-1"),
            .environmentIdentity         = hashOf("environment-1"),
        };
        auto first = ToolCallPositionIdentity::create(
            *root,
            std::nullopt,
            1U,
            execution,
            *observe
        );
        auto changedAtFirstPosition = ToolCallPositionIdentity::create(
            *root,
            std::nullopt,
            1U,
            execution,
            *wait
        );
        auto parent = ToolCallPositionIdentity::create(
            *root,
            std::nullopt,
            2U,
            execution,
            *observe
        );
        REQUIRE(first.has_value());
        REQUIRE(changedAtFirstPosition.has_value());
        REQUIRE(parent.has_value());
        auto child = ToolCallPositionIdentity::create(
            *root,
            parent->asParent(),
            1U,
            execution,
            *wait
        );
        REQUIRE(child.has_value());

        {
            auto store = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(store.has_value(), store.error().message());

            auto createdRoot = store->persistToolRootRequest(*root);
            REQUIRE(createdRoot.has_value());
            CHECK(createdRoot->lookup == ToolIdentityLookup::Created);
            CHECK(createdRoot->rootIdentity == root->identity());

            auto repeatedRoot = store->persistToolRootRequest(*root);
            REQUIRE(repeatedRoot.has_value());
            CHECK(repeatedRoot->lookup == ToolIdentityLookup::Existing);

            auto conflictingPreimage = CanonicalJson::parseExact(
                R"({"objective":"different"})"
            );
            REQUIRE(conflictingPreimage.has_value());
            auto conflictingRoot = ToolRootRequestIdentity::create(
                "script:fixture",
                "request-1",
                std::move(*conflictingPreimage)
            );
            REQUIRE(conflictingRoot.has_value());
            auto refusedRoot = store->persistToolRootRequest(*conflictingRoot);
            REQUIRE_FALSE(refusedRoot.has_value());
            CHECK(refusedRoot.error().message().contains(
                "different canonical material"
            ));

            auto createdCall = store->persistToolCallPosition(*root, *first);
            REQUIRE(createdCall.has_value());
            CHECK(createdCall->lookup == ToolIdentityLookup::Created);
            CHECK(createdCall->callIdentity == first->identity());

            auto repeatedCall = store->persistToolCallPosition(*root, *first);
            REQUIRE(repeatedCall.has_value());
            CHECK(repeatedCall->lookup == ToolIdentityLookup::Existing);

            auto nondeterministic =
                store->persistToolCallPosition(*root, *changedAtFirstPosition);
            REQUIRE_FALSE(nondeterministic.has_value());
            CHECK(nondeterministic.error().message().contains(
                "different caller-fixed material"
            ));

            auto missingParent = store->persistToolCallPosition(*root, *child);
            REQUIRE_FALSE(missingParent.has_value());
            CHECK(missingParent.error().message().contains(
                "parent call to be persisted first"
            ));
            REQUIRE(store->persistToolCallPosition(*root, *parent).has_value());
            REQUIRE(store->persistToolCallPosition(*root, *child).has_value());

            auto foreignPreimage = CanonicalJson::parseExact("{}");
            REQUIRE(foreignPreimage.has_value());
            auto foreignRoot = ToolRootRequestIdentity::create(
                "script:fixture",
                "request-2",
                std::move(*foreignPreimage)
            );
            REQUIRE(foreignRoot.has_value());
            auto foreignCall = ToolCallPositionIdentity::create(
                *foreignRoot,
                std::nullopt,
                1U,
                execution,
                *observe
            );
            REQUIRE(foreignCall.has_value());
            auto crossRoot = store->persistToolCallPosition(*root, *foreignCall);
            REQUIRE_FALSE(crossRoot.has_value());
            CHECK(crossRoot.error().message().contains("different root request"));
        }

        {
            auto database = test_support::OperatorDatabaseProbe{
                production / "operator-runtime.sqlite",
            };
            CHECK(
                database.readRows(
                    "SELECT root_identity, caller_namespace, request_key, "
                    "request_preimage, request_preimage_hash "
                    "FROM tool_root_requests"
                )
                == std::vector<std::vector<std::string>>{
                    {
                        root->identity().hex(),
                        "script:fixture",
                        "request-1",
                        root->requestPreimage().bytes(),
                        root->requestPreimage().contentHash().hex(),
                    },
                }
            );
            CHECK(
                database.readRows(
                    "SELECT root_identity, coalesce(parent_call_identity, ''), "
                    "call_sequence, canonical_args FROM tool_call_positions "
                    "WHERE call_identity='" + child->identity().hex() + "'"
                )
                == std::vector<std::vector<std::string>>{
                    {
                        root->identity().hex(),
                        parent->identity().hex(),
                        "1",
                        child->canonicalArgs(),
                    },
                }
            );
            CHECK(
                database.readRows("SELECT count(*) FROM tool_call_positions")
                == std::vector<std::vector<std::string>>{{"3"}}
            );
        }

        auto restarted = OperatorCoordinator::open(production);
        REQUIRE_MESSAGE(restarted.has_value(), restarted.error().message());
        auto repeatedRoot = restarted->persistToolRootRequest(*root);
        auto repeatedCall = restarted->persistToolCallPosition(*root, *first);
        REQUIRE(repeatedRoot.has_value());
        REQUIRE(repeatedCall.has_value());
        CHECK(repeatedRoot->lookup == ToolIdentityLookup::Existing);
        CHECK(repeatedCall->lookup == ToolIdentityLookup::Existing);
    }

    TEST_CASE("Tool identity replay refuses stored canonical-byte tampering")
    {
        auto temporary        = TemporaryDirectory{};
        auto const production = temporary.path() / "production";
        auto preimage         = CanonicalJson::parseExact(R"({"request":1})");
        REQUIRE(preimage.has_value());
        auto root = ToolRootRequestIdentity::create(
            "script:tamper",
            "request-1",
            std::move(*preimage)
        );
        REQUIRE(root.has_value());
        auto frameworkCatalog = FrameworkToolCatalogOwner::create();
        auto arguments        = CanonicalJson::parseExact("{}");
        REQUIRE(frameworkCatalog.has_value());
        REQUIRE(arguments.has_value());
        auto invocation = frameworkCatalog->validate(
            "framework.screen.observe",
            std::move(*arguments)
        );
        REQUIRE(invocation.has_value());
        auto call = ToolCallPositionIdentity::create(
            *root,
            std::nullopt,
            1U,
            ToolExecutionIdentity{
                .runIdentity                 = hashOf("tamper-run"),
                .frameworkReleaseIdentity    = hashOf("tamper-framework"),
                .toolRuntimeProtocolIdentity = hashOf("tamper-protocol"),
                .environmentIdentity         = hashOf("tamper-environment"),
            },
            *invocation
        );
        REQUIRE(call.has_value());
        {
            auto store = OperatorCoordinator::open(production);
            REQUIRE(store.has_value());
            REQUIRE(store->persistToolRootRequest(*root).has_value());
            REQUIRE(store->persistToolCallPosition(*root, *call).has_value());
        }
        {
            auto database = test_support::OperatorDatabaseProbe{
                production / "operator-runtime.sqlite",
            };
            database.execute(
                "UPDATE tool_call_positions SET canonical_args='[]' "
                "WHERE call_identity='" + call->identity().hex() + "'"
            );
        }
        {
            auto reopened = OperatorCoordinator::open(production);
            REQUIRE(reopened.has_value());
            auto refused = reopened->persistToolCallPosition(*root, *call);
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "different caller-fixed material"
            ));
        }
        {
            auto database = test_support::OperatorDatabaseProbe{
                production / "operator-runtime.sqlite",
            };
            database.execute(
                "UPDATE tool_root_requests SET request_preimage='{}' "
                "WHERE root_identity='" + root->identity().hex() + "'"
            );
        }
        auto reopened = OperatorCoordinator::open(production);
        REQUIRE(reopened.has_value());
        auto refused = reopened->persistToolRootRequest(*root);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains("different canonical material"));
    }

    TEST_CASE("Tool replay refuses a corrupted state and outcome combination")
    {
        auto temporary        = TemporaryDirectory{};
        auto const production = temporary.path() / "production";
        auto preimage         = CanonicalJson::parseExact("{}");
        REQUIRE(preimage.has_value());
        auto root = ToolRootRequestIdentity::create(
            "state-tamper",
            "request-1",
            std::move(*preimage)
        );
        REQUIRE(root.has_value());
        auto catalog   = FrameworkToolCatalogOwner::create();
        auto arguments = CanonicalJson::parseExact("{}");
        REQUIRE(catalog.has_value());
        REQUIRE(arguments.has_value());
        auto invocation = catalog->validate(
            "framework.screen.observe",
            std::move(*arguments)
        );
        REQUIRE(invocation.has_value());
        auto call = ToolCallPositionIdentity::create(
            *root,
            std::nullopt,
            1U,
            ToolExecutionIdentity{
                .runIdentity                 = hashOf("state-tamper-run"),
                .frameworkReleaseIdentity    = hashOf("state-tamper-framework"),
                .toolRuntimeProtocolIdentity = hashOf("state-tamper-protocol"),
                .environmentIdentity         = hashOf("state-tamper-environment"),
            },
            *invocation
        );
        REQUIRE(call.has_value());
        {
            auto store = OperatorCoordinator::open(production);
            REQUIRE(store.has_value());
            REQUIRE(store->persistToolRootRequest(*root).has_value());
            REQUIRE(store->persistToolCallPosition(*root, *call).has_value());
        }
        {
            auto database = test_support::OperatorDatabaseProbe{
                production / "operator-runtime.sqlite",
            };
            CHECK(database.refuses(
                "UPDATE tool_call_history SET state='confirmed', "
                "active_admission_attempt=1"
            ));
            database.execute("PRAGMA ignore_check_constraints=ON");
            database.execute(
                "UPDATE tool_call_history SET state='confirmed', "
                "active_admission_attempt=1"
            );
        }
        auto reopened = OperatorCoordinator::open(production);
        REQUIRE(reopened.has_value());
        auto refused = reopened->replayToolCall(*root, *call);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains(
            "state and outcome shape disagree"
        ));
    }

    TEST_CASE("the immediate-prior Tool identity schema migrates audit rows exactly")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        auto operationId        = std::string{};
        {
            auto prepared = prepareStore(temporary.path());
            operationId = proposedOperation(
                prepared,
                "tool-identity-migration-request",
                "command-1"
            ).operationId;
        }

        auto sourceIdentity = std::string{};
        auto auditRows      = std::vector<std::vector<std::string>>{};
        {
            auto prior = test_support::OperatorDatabaseProbe{databasePath};
            auditRows = prior.readRows(
                "SELECT operation_id, client_request_id, tool_name, state "
                "FROM operations WHERE operation_id='" + operationId + "'"
            );
            removeToolIdentityPersistence(prior);
            sourceIdentity = exactSchemaIdentity(prior);
        }
        CHECK(
            sourceIdentity
            == "sha256:d26b0e12be915009587a72312d4b46f4afc88509df5432f967eb15b016c24257"
        );

        {
            auto migrated = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(migrated.has_value(), migrated.error().message());
        }
        auto target = test_support::OperatorDatabaseProbe{databasePath};
        auto const targetIdentity = exactSchemaIdentity(target);
        CHECK(
            targetIdentity
            == "sha256:53c56cce2064c47a07bd29529320aac7e7f8f4e8c01a74dc54da936159dd44f8"
        );
        CHECK(
            target.readRows(
                "SELECT operation_id, client_request_id, tool_name, state "
                "FROM operations WHERE operation_id='" + operationId + "'"
            ) == auditRows
        );
        CHECK(
            target.readRows(
                "SELECT source_identity, target_identity "
                "FROM schema_identity_transitions"
            )
            == std::vector<std::vector<std::string>>{
                {sourceIdentity, targetIdentity},
            }
        );
        CHECK(
            target.readRows("SELECT count(*) FROM tool_root_requests")
            == std::vector<std::vector<std::string>>{{"0"}}
        );
        CHECK(
            target.readRows("SELECT count(*) FROM tool_call_positions")
            == std::vector<std::vector<std::string>>{{"0"}}
        );
    }

    TEST_CASE("read-only Tool dispatch records and replays one exact terminal outcome")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto preimage  = CanonicalJson::parseExact(
            R"({"objective":"observe-once"})"
        );
        REQUIRE(preimage.has_value());
        auto root = ToolRootRequestIdentity::create(
            "controller-1",
            "tool-runtime-request-1",
            std::move(*preimage)
        );
        REQUIRE(root.has_value());
        auto catalog   = FrameworkToolCatalogOwner::create();
        auto arguments = CanonicalJson::parseExact("{}");
        REQUIRE(catalog.has_value());
        REQUIRE(arguments.has_value());
        auto invocation = catalog->validate(
            "framework.screen.observe",
            std::move(*arguments)
        );
        REQUIRE(invocation.has_value());
        auto call = ToolCallPositionIdentity::create(
            *root,
            std::nullopt,
            1U,
            ToolExecutionIdentity{
                .runIdentity                 = hashOf("read-only-run"),
                .frameworkReleaseIdentity    = hashOf("read-only-framework"),
                .toolRuntimeProtocolIdentity = hashOf("read-only-protocol"),
                .environmentIdentity         = hashOf("read-only-environment"),
            },
            *invocation
        );
        REQUIRE(call.has_value());
        auto nextCall = ToolCallPositionIdentity::create(
            *root,
            std::nullopt,
            2U,
            call->executionIdentity(),
            *invocation
        );
        auto childCall = ToolCallPositionIdentity::create(
            *root,
            call->asParent(),
            1U,
            call->executionIdentity(),
            *invocation
        );
        REQUIRE(nextCall.has_value());
        REQUIRE(childCall.has_value());
        REQUIRE(prepared.store.persistToolRootRequest(*root).has_value());
        REQUIRE(prepared.store.persistToolCallPosition(*root, *call).has_value());
        auto proposed = prepared.store.replayToolCall(*root, *call);
        REQUIRE(proposed.has_value());
        CHECK(proposed->state == ToolCallState::Proposed);
        CHECK(proposed->revision == 1U);

        auto admission = prepared.store.admitReadOnlyToolCall(
            prepared.controller,
            prepared.lease,
            *root,
            *call
        );
        REQUIRE(admission.has_value());
        CHECK(admission->attemptNumber() == 1U);
        CHECK(admission->historyRevision() == 2U);
        auto repeatedAdmission = prepared.store.admitReadOnlyToolCall(
            prepared.controller,
            prepared.lease,
            *root,
            *call
        );
        REQUIRE(repeatedAdmission.has_value());
        CHECK(repeatedAdmission->attemptNumber() == 1U);
        CHECK(repeatedAdmission->historyRevision() == 2U);
        auto refusedNext = prepared.store.admitReadOnlyToolCall(
            prepared.controller,
            prepared.lease,
            *root,
            *nextCall
        );
        REQUIRE_FALSE(refusedNext.has_value());
        CHECK(refusedNext.error().message().contains(
            "no deterministic terminal outcome"
        ));
        auto refusedChild = prepared.store.admitReadOnlyToolCall(
            prepared.controller,
            prepared.lease,
            *root,
            *childCall
        );
        REQUIRE_FALSE(refusedChild.has_value());
        CHECK(refusedChild.error().message().contains(
            "nested delegation grant"
        ));

        auto dispatch = prepared.store.beginToolCallDispatch(*admission);
        REQUIRE(dispatch.has_value());
        CHECK(dispatch->historyRevision() == 3U);
        auto dispatching = prepared.store.replayToolCall(*root, *call);
        REQUIRE(dispatching.has_value());
        CHECK(dispatching->state == ToolCallState::Dispatching);

        auto result = CanonicalJson::parseExact(
            R"({"frame_id":"frame-1"})"
        );
        auto evidence = CanonicalJson::parseExact(
            R"({"capture_hash":"capture-1"})"
        );
        REQUIRE(result.has_value());
        REQUIRE(evidence.has_value());
        auto completion = ToolCallCompletion::confirmed(
            *result,
            std::optional{*evidence}
        );
        auto completed = prepared.store.completeToolCallDispatch(
            *dispatch,
            completion
        );
        REQUIRE(completed.has_value());
        CHECK(completed->lookup == ToolOutcomeLookup::Created);
        CHECK(completed->state == ToolCallState::Confirmed);
        CHECK(completed->revision == 4U);
        auto admittedNext = prepared.store.admitReadOnlyToolCall(
            prepared.controller,
            prepared.lease,
            *root,
            *nextCall
        );
        REQUIRE(admittedNext.has_value());
        CHECK(admittedNext->attemptNumber() == 1U);

        auto repeated = prepared.store.completeToolCallDispatch(
            *dispatch,
            completion
        );
        REQUIRE(repeated.has_value());
        CHECK(repeated->lookup == ToolOutcomeLookup::Existing);
        CHECK(repeated->revision == 4U);
        auto changedResult = CanonicalJson::parseExact(
            R"({"frame_id":"frame-2"})"
        );
        REQUIRE(changedResult.has_value());
        auto changedCompletion = ToolCallCompletion::confirmed(
            *changedResult,
            std::optional{*evidence}
        );
        auto changed = prepared.store.completeToolCallDispatch(
            *dispatch,
            changedCompletion
        );
        REQUIRE_FALSE(changed.has_value());
        CHECK(changed.error().message().contains("immutable"));

        auto replay = prepared.store.replayToolCall(*root, *call);
        REQUIRE(replay.has_value());
        REQUIRE(replay->payload.has_value());
        REQUIRE(replay->evidence.has_value());
        CHECK(replay->state == ToolCallState::Confirmed);
        CHECK(replay->payload->bytes() == result->bytes());
        CHECK(replay->evidence->bytes() == evidence->bytes());

        {
            auto released = std::move(prepared.store);
        }
        auto restarted = OperatorCoordinator::open(
            temporary.path() / "production"
        );
        REQUIRE(restarted.has_value());
        auto replayAfterRestart = restarted->replayToolCall(*root, *call);
        REQUIRE(replayAfterRestart.has_value());
        REQUIRE(replayAfterRestart->payload.has_value());
        CHECK(replayAfterRestart->state == ToolCallState::Confirmed);
        CHECK(replayAfterRestart->payload->bytes() == result->bytes());
    }

    TEST_CASE("restart classifies an unanswered Tool dispatch possible and never redispatches")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto preimage  = CanonicalJson::parseExact(
            R"({"objective":"crash-during-observe"})"
        );
        REQUIRE(preimage.has_value());
        auto root = ToolRootRequestIdentity::create(
            "controller-1",
            "tool-runtime-crash-request",
            std::move(*preimage)
        );
        REQUIRE(root.has_value());
        auto catalog   = FrameworkToolCatalogOwner::create();
        auto arguments = CanonicalJson::parseExact("{}");
        REQUIRE(catalog.has_value());
        REQUIRE(arguments.has_value());
        auto invocation = catalog->validate(
            "framework.screen.observe",
            std::move(*arguments)
        );
        REQUIRE(invocation.has_value());
        auto call = ToolCallPositionIdentity::create(
            *root,
            std::nullopt,
            1U,
            ToolExecutionIdentity{
                .runIdentity                 = hashOf("crash-run"),
                .frameworkReleaseIdentity    = hashOf("crash-framework"),
                .toolRuntimeProtocolIdentity = hashOf("crash-protocol"),
                .environmentIdentity         = hashOf("crash-environment"),
            },
            *invocation
        );
        REQUIRE(call.has_value());
        auto admission = prepared.store.admitReadOnlyToolCall(
            prepared.controller,
            prepared.lease,
            *root,
            *call
        );
        REQUIRE(admission.has_value());
        auto dispatch = prepared.store.beginToolCallDispatch(*admission);
        REQUIRE(dispatch.has_value());
        auto continuationPreimage = CanonicalJson::parseExact(
            R"({"objective":"admitted-before-restart"})"
        );
        REQUIRE(continuationPreimage.has_value());
        auto continuationRoot = ToolRootRequestIdentity::create(
            "controller-1",
            "tool-runtime-admitted-restart",
            std::move(*continuationPreimage)
        );
        REQUIRE(continuationRoot.has_value());
        auto continuationCall = ToolCallPositionIdentity::create(
            *continuationRoot,
            std::nullopt,
            1U,
            call->executionIdentity(),
            *invocation
        );
        REQUIRE(continuationCall.has_value());
        auto priorAdmission = prepared.store.admitReadOnlyToolCall(
            prepared.controller,
            prepared.lease,
            *continuationRoot,
            *continuationCall
        );
        REQUIRE(priorAdmission.has_value());
        auto manifest = prepared.manifest;
        {
            auto released = std::move(prepared.store);
        }

        auto restarted = OperatorCoordinator::open(
            temporary.path() / "production"
        );
        REQUIRE(restarted.has_value());
        auto replay = restarted->replayToolCall(*root, *call);
        REQUIRE(replay.has_value());
        REQUIRE(replay->payload.has_value());
        CHECK(replay->state == ToolCallState::Possible);
        CHECK(
            replay->payload->bytes()
            == R"({"reason":"operator_restart_after_dispatch_started"})"
        );
        CHECK(replay->revision == 4U);

        auto lateResult = CanonicalJson::parseExact(R"({"late":true})");
        REQUIRE(lateResult.has_value());
        auto lateCompletion = ToolCallCompletion::confirmed(*lateResult);
        auto refusedLate = restarted->completeToolCallDispatch(
            *dispatch,
            lateCompletion
        );
        REQUIRE_FALSE(refusedLate.has_value());
        CHECK(refusedLate.error().message().contains(
            "does not match the active dispatch"
        ));

        auto resumed = restarted->resumeSession(
            SessionResume{
                .authenticatedControllerId = "controller-1",
                .controlledTargetId        = "target-1",
                .mode                      = SessionMode::Write,
                .kind                      = ControllerKind::Script,
            },
            manifest
        );
        REQUIRE(resumed.has_value());
        auto lease = restarted->acquireLease(*resumed);
        REQUIRE(lease.has_value());
        auto continuedAdmission = restarted->admitReadOnlyToolCall(
            *resumed,
            *lease,
            *continuationRoot,
            *continuationCall
        );
        REQUIRE(continuedAdmission.has_value());
        CHECK(continuedAdmission->attemptNumber() == 2U);
        CHECK(continuedAdmission->historyRevision() == 3U);
        auto staleAdmissionDispatch = restarted->beginToolCallDispatch(
            *priorAdmission
        );
        REQUIRE_FALSE(staleAdmissionDispatch.has_value());
        CHECK(staleAdmissionDispatch.error().message().contains(
            "no longer live"
        ));
        auto continuedDispatch = restarted->beginToolCallDispatch(
            *continuedAdmission
        );
        REQUIRE(continuedDispatch.has_value());
        auto continuedResult = CanonicalJson::parseExact(
            R"({"continued":true})"
        );
        REQUIRE(continuedResult.has_value());
        REQUIRE(restarted->completeToolCallDispatch(
            *continuedDispatch,
            ToolCallCompletion::confirmed(*continuedResult)
        ).has_value());
        auto refusedReadmission = restarted->admitReadOnlyToolCall(
            *resumed,
            *lease,
            *root,
            *call
        );
        REQUIRE_FALSE(refusedReadmission.has_value());
        CHECK(refusedReadmission.error().message().contains(
            "cannot enter admission"
        ));
    }

    TEST_CASE(
        "restart makes an unanswered mutating Tool a target-wide barrier until reconciliation"
    )
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto preimage  = CanonicalJson::parseExact(
            R"({"objective":"crash-during-mutation"})"
        );
        REQUIRE(preimage.has_value());
        auto root = ToolRootRequestIdentity::create(
            "controller-1",
            "mutating-crash-request",
            std::move(*preimage)
        );
        REQUIRE(root.has_value());
        auto invocation = toolInvocation(prepared.project, "command-1");
        auto effects = std::array{
            test_support::routineToolEffect(prepared.project),
        };
        auto const execution = ToolExecutionIdentity{
            .runIdentity                 = hashOf("mutating-crash-run"),
            .frameworkReleaseIdentity    = hashOf("mutating-crash-framework"),
            .toolRuntimeProtocolIdentity = hashOf("mutating-crash-protocol"),
            .environmentIdentity         = hashOf("mutating-crash-environment"),
        };
        auto call = ToolCallPositionIdentity::create(
            *root,
            std::nullopt,
            1U,
            execution,
            invocation
        );
        REQUIRE(call.has_value());
        auto admission = prepared.store.admitMutatingToolCall(
            prepared.controller,
            prepared.lease,
            *root,
            *call,
            prepared.planAuthority,
            effects,
            {}
        );
        REQUIRE(admission.has_value());
        auto dispatch = prepared.store.beginToolCallDispatch(*admission);
        REQUIRE(dispatch.has_value());
        auto const manifest = prepared.manifest;
        {
            auto released = std::move(prepared.store);
        }

        auto restarted = OperatorCoordinator::open(
            temporary.path() / "production"
        );
        REQUIRE(restarted.has_value());
        auto replay = restarted->replayToolCall(*root, *call);
        REQUIRE(replay.has_value());
        CHECK(replay->state == ToolCallState::Possible);

        auto resumed = restarted->resumeSession(
            SessionResume{
                .authenticatedControllerId = "controller-1",
                .controlledTargetId        = "target-1",
                .mode                      = SessionMode::Write,
                .kind                      = ControllerKind::Script,
            },
            manifest
        );
        REQUIRE(resumed.has_value());
        auto lease = restarted->acquireLease(*resumed);
        REQUIRE(lease.has_value());

        auto secondPreimage = CanonicalJson::parseExact(
            R"({"objective":"mutation-after-restart"})"
        );
        REQUIRE(secondPreimage.has_value());
        auto secondRoot = ToolRootRequestIdentity::create(
            "controller-1",
            "mutation-after-restart",
            std::move(*secondPreimage)
        );
        REQUIRE(secondRoot.has_value());
        auto secondCall = ToolCallPositionIdentity::create(
            *secondRoot,
            std::nullopt,
            1U,
            execution,
            invocation
        );
        REQUIRE(secondCall.has_value());
        auto blocked = restarted->admitMutatingToolCall(
            *resumed,
            *lease,
            *secondRoot,
            *secondCall,
            prepared.planAuthority,
            effects,
            {}
        );
        REQUIRE_FALSE(blocked.has_value());
        CHECK(blocked.error().message().contains("state possible"));

        auto result = CanonicalJson::parseExact(R"({"delivered":true})");
        auto evidence = CanonicalJson::parseExact(
            R"({"snapshot_ref":"post-restart-evidence"})"
        );
        REQUIRE(result.has_value());
        REQUIRE(evidence.has_value());
        auto reconciled = restarted->reconcileMutatingToolCall(
            *resumed,
            *lease,
            *root,
            *call,
            ToolCallReconciliation::confirmed(*result, *evidence)
        );
        REQUIRE(reconciled.has_value());
        CHECK(reconciled->state == ToolCallState::Confirmed);

        auto unblocked = restarted->admitMutatingToolCall(
            *resumed,
            *lease,
            *secondRoot,
            *secondCall,
            prepared.planAuthority,
            effects,
            {}
        );
        REQUIRE(unblocked.has_value());
    }

    TEST_CASE("Tool admission charges an Agent once and rechecks lease at dispatch")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto agent = test_support::addController(
            prepared,
            ControllerKind::Agent,
            SessionMode::Write,
            "agent-tool-runtime",
            "agent-tool-instance",
            "agent-tool-target",
            AgentBudget{
                .maximumToolCalls    = 1U,
                .maximumMutations    = 1U,
                .maximumObservations = 1U,
                .maximumElapsedMillis = 60'000U,
                .maximumRiskUnits    = 1U,
            }
        );
        auto lease = prepared.store.acquireLease(agent);
        REQUIRE(lease.has_value());
        auto preimage = CanonicalJson::parseExact(
            R"({"objective":"agent-observe"})"
        );
        REQUIRE(preimage.has_value());
        auto root = ToolRootRequestIdentity::create(
            "controller-1",
            "agent-tool-request",
            std::move(*preimage)
        );
        REQUIRE(root.has_value());
        auto catalog   = FrameworkToolCatalogOwner::create();
        auto arguments = CanonicalJson::parseExact("{}");
        REQUIRE(catalog.has_value());
        REQUIRE(arguments.has_value());
        auto invocation = catalog->validate(
            "framework.screen.observe",
            std::move(*arguments)
        );
        REQUIRE(invocation.has_value());
        auto const execution = ToolExecutionIdentity{
            .runIdentity                 = hashOf("agent-tool-run"),
            .frameworkReleaseIdentity    = hashOf("agent-tool-framework"),
            .toolRuntimeProtocolIdentity = hashOf("agent-tool-protocol"),
            .environmentIdentity         = hashOf("agent-tool-environment"),
        };
        auto first = ToolCallPositionIdentity::create(
            *root,
            std::nullopt,
            1U,
            execution,
            *invocation
        );
        auto secondPreimage = CanonicalJson::parseExact(
            R"({"objective":"agent-observe-second-root"})"
        );
        REQUIRE(secondPreimage.has_value());
        auto secondRoot = ToolRootRequestIdentity::create(
            "controller-1",
            "agent-tool-request-2",
            std::move(*secondPreimage)
        );
        REQUIRE(secondRoot.has_value());
        auto second = ToolCallPositionIdentity::create(
            *secondRoot,
            std::nullopt,
            1U,
            execution,
            *invocation
        );
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());

        auto before = prepared.store.remainingBudget(agent);
        REQUIRE(before.has_value());
        CHECK(before->toolCalls == 1U);
        auto admission = prepared.store.admitReadOnlyToolCall(
            agent,
            *lease,
            *root,
            *first
        );
        REQUIRE(admission.has_value());
        auto after = prepared.store.remainingBudget(agent);
        REQUIRE(after.has_value());
        CHECK(after->toolCalls == 0U);
        CHECK(after->observations == 0U);
        CHECK(after->mutations == 1U);

        auto repeated = prepared.store.admitReadOnlyToolCall(
            agent,
            *lease,
            *root,
            *first
        );
        REQUIRE(repeated.has_value());
        CHECK(repeated->attemptNumber() == admission->attemptNumber());
        auto afterRepeated = prepared.store.remainingBudget(agent);
        REQUIRE(afterRepeated.has_value());
        CHECK(afterRepeated->toolCalls == 0U);
        CHECK(afterRepeated->observations == 0U);

        REQUIRE(prepared.store.releaseLease(*lease).has_value());
        auto staleDispatch = prepared.store.beginToolCallDispatch(*admission);
        REQUIRE_FALSE(staleDispatch.has_value());
        CHECK(staleDispatch.error().message().contains("no longer live"));

        auto replacementLease = prepared.store.acquireLease(agent);
        REQUIRE(replacementLease.has_value());
        auto exhausted = prepared.store.admitReadOnlyToolCall(
            agent,
            *replacementLease,
            *secondRoot,
            *second
        );
        REQUIRE_FALSE(exhausted.has_value());
        CHECK(exhausted.error().message().contains(
            "tool-call budget is exhausted"
        ));
        auto secondReplay = prepared.store.replayToolCall(*secondRoot, *second);
        REQUIRE(secondReplay.has_value());
        CHECK(secondReplay->state == ToolCallState::Proposed);
    }

    TEST_CASE("read-only Tool admission refuses a mutating Project descriptor")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto preimage  = CanonicalJson::parseExact("{}");
        REQUIRE(preimage.has_value());
        auto root = ToolRootRequestIdentity::create(
            "controller-1",
            "mutating-project-tool",
            std::move(*preimage)
        );
        REQUIRE(root.has_value());
        auto invocation = toolInvocation(prepared.project, "command-1");
        REQUIRE(invocation.descriptor().mutability == ToolMutability::Mutating);
        auto call = ToolCallPositionIdentity::create(
            *root,
            std::nullopt,
            1U,
            ToolExecutionIdentity{
                .runIdentity                 = hashOf("mutating-run"),
                .frameworkReleaseIdentity    = hashOf("mutating-framework"),
                .toolRuntimeProtocolIdentity = hashOf("mutating-protocol"),
                .environmentIdentity         = hashOf("mutating-environment"),
            },
            invocation
        );
        REQUIRE(call.has_value());
        auto refused = prepared.store.admitReadOnlyToolCall(
            prepared.controller,
            prepared.lease,
            *root,
            *call
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains("mutating descriptor"));
    }

    TEST_CASE("mutating Tool admission charges Agent tool and mutation budgets once")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto agent = test_support::addController(
            prepared,
            ControllerKind::Agent,
            SessionMode::Write,
            "agent-mutating-runtime",
            "agent-mutating-instance",
            "agent-mutating-target",
            AgentBudget{
                .maximumToolCalls    = 2U,
                .maximumMutations    = 1U,
                .maximumObservations = 2U,
                .maximumElapsedMillis = 60'000U,
                .maximumRiskUnits     = 2U,
            }
        );
        auto lease = prepared.store.acquireLease(agent);
        REQUIRE(lease.has_value());
        auto invocation = toolInvocation(prepared.project, "command-1");
        auto effects = std::array{
            test_support::routineToolEffect(prepared.project),
        };
        auto const execution = ToolExecutionIdentity{
            .runIdentity                 = hashOf("agent-mutating-run"),
            .frameworkReleaseIdentity    = hashOf("agent-mutating-framework"),
            .toolRuntimeProtocolIdentity = hashOf("agent-mutating-protocol"),
            .environmentIdentity         = hashOf("agent-mutating-environment"),
        };
        auto firstPreimage = CanonicalJson::parseExact(
            R"({"objective":"agent mutation one"})"
        );
        REQUIRE(firstPreimage.has_value());
        auto firstRoot = ToolRootRequestIdentity::create(
            "controller-1",
            "agent-mutation-one",
            std::move(*firstPreimage)
        );
        REQUIRE(firstRoot.has_value());
        auto firstCall = ToolCallPositionIdentity::create(
            *firstRoot,
            std::nullopt,
            1U,
            execution,
            invocation
        );
        REQUIRE(firstCall.has_value());
        auto admitted = prepared.store.admitMutatingToolCall(
            agent,
            *lease,
            *firstRoot,
            *firstCall,
            prepared.planAuthority,
            effects,
            {}
        );
        REQUIRE(admitted.has_value());
        auto afterFirst = prepared.store.remainingBudget(agent);
        REQUIRE(afterFirst.has_value());
        CHECK(afterFirst->toolCalls == 1U);
        CHECK(afterFirst->mutations == 0U);
        CHECK(afterFirst->observations == 2U);

        auto repeated = prepared.store.admitMutatingToolCall(
            agent,
            *lease,
            *firstRoot,
            *firstCall,
            prepared.planAuthority,
            effects,
            {}
        );
        REQUIRE(repeated.has_value());
        CHECK(repeated->attemptNumber() == admitted->attemptNumber());
        auto afterRepeat = prepared.store.remainingBudget(agent);
        REQUIRE(afterRepeat.has_value());
        CHECK(afterRepeat->toolCalls == 1U);
        CHECK(afterRepeat->mutations == 0U);

        auto dispatch = prepared.store.beginToolCallDispatch(*admitted);
        REQUIRE(dispatch.has_value());
        auto terminalError = CanonicalJson::parseExact(
            R"({"error":"provider returned after dispatch"})"
        );
        REQUIRE(terminalError.has_value());
        auto unsafeTerminal = prepared.store.completeToolCallDispatch(
            *dispatch,
            ToolCallCompletion::terminalFailure(*terminalError)
        );
        REQUIRE_FALSE(unsafeTerminal.has_value());
        CHECK(unsafeTerminal.error().message().contains(
            "must report possible"
        ));
        auto result = CanonicalJson::parseExact(R"({"delivered":true})");
        REQUIRE(result.has_value());
        REQUIRE(prepared.store.completeToolCallDispatch(
            *dispatch,
            ToolCallCompletion::confirmed(*result)
        ).has_value());

        auto secondPreimage = CanonicalJson::parseExact(
            R"({"objective":"agent mutation two"})"
        );
        REQUIRE(secondPreimage.has_value());
        auto secondRoot = ToolRootRequestIdentity::create(
            "controller-1",
            "agent-mutation-two",
            std::move(*secondPreimage)
        );
        REQUIRE(secondRoot.has_value());
        auto secondCall = ToolCallPositionIdentity::create(
            *secondRoot,
            std::nullopt,
            1U,
            execution,
            invocation
        );
        REQUIRE(secondCall.has_value());
        auto exhausted = prepared.store.admitMutatingToolCall(
            agent,
            *lease,
            *secondRoot,
            *secondCall,
            prepared.planAuthority,
            effects,
            {}
        );
        REQUIRE_FALSE(exhausted.has_value());
        CHECK(exhausted.error().message().contains(
            "mutation budget is exhausted"
        ));
        auto afterExhaustion = prepared.store.remainingBudget(agent);
        REQUIRE(afterExhaustion.has_value());
        CHECK(afterExhaustion->toolCalls == 1U);
        CHECK(afterExhaustion->mutations == 0U);
    }

    TEST_CASE("mutating Tool admission persists one immutable policy effect envelope")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto preimage = CanonicalJson::parseExact(
            R"({"objective":"effect-authority"})"
        );
        REQUIRE(preimage.has_value());
        auto root = ToolRootRequestIdentity::create(
            "controller-1",
            "effect-authority",
            std::move(*preimage)
        );
        REQUIRE(root.has_value());
        auto invocation = toolInvocation(prepared.project, "command-1");
        auto call = ToolCallPositionIdentity::create(
            *root,
            std::nullopt,
            1U,
            ToolExecutionIdentity{
                .runIdentity                 = hashOf("effect-authority-run"),
                .frameworkReleaseIdentity    = hashOf("effect-authority-framework"),
                .toolRuntimeProtocolIdentity = hashOf("effect-authority-protocol"),
                .environmentIdentity         = hashOf("effect-authority-environment"),
            },
            invocation
        );
        REQUIRE(call.has_value());
        auto effects = std::array{
            test_support::routineToolEffect(prepared.project),
        };
        auto expected = deriveEffectiveEffectEnvelope(
            std::vector<ProposedEffect>{effects.begin(), effects.end()}
        );
        REQUIRE(expected.has_value());
        auto admitted = prepared.store.admitMutatingToolCall(
            prepared.controller,
            prepared.lease,
            *root,
            *call,
            prepared.planAuthority,
            effects,
            {}
        );
        REQUIRE(admitted.has_value());

        auto changedEffects = effects;
        changedEffects.front().scopeKey = "another-fixture-instance";
        auto changed = prepared.store.admitMutatingToolCall(
            prepared.controller,
            prepared.lease,
            *root,
            *call,
            prepared.planAuthority,
            changedEffects,
            {}
        );
        REQUIRE_FALSE(changed.has_value());
        CHECK(changed.error().message().contains("durable effect authority"));
        { auto released = std::move(prepared.store); }

        auto database = test_support::OperatorDatabaseProbe{
            temporary.path() / "production" / "operator-runtime.sqlite",
        };
        CHECK(
            database.readRows(
                "SELECT effect_envelope, effect_envelope_hash, "
                "required_approvals, approval_tokens FROM "
                "tool_admission_attempts WHERE call_identity='"
                + call->identity().hex() + "'"
            ) == std::vector<std::vector<std::string>>{
                {
                    expected->canonicalJcs,
                    expected->hash.hex(),
                    "[]",
                    "[]",
                },
            }
        );
    }

    TEST_CASE("mutating Tool admission stops at the policy approval boundary")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto preimage  = CanonicalJson::parseExact(R"({"objective":"high-risk"})");
        REQUIRE(preimage.has_value());
        auto root = ToolRootRequestIdentity::create(
            "controller-1",
            "high-risk-effect",
            std::move(*preimage)
        );
        REQUIRE(root.has_value());
        auto invocation = toolInvocation(prepared.project, "command-1");
        auto call = ToolCallPositionIdentity::create(
            *root,
            std::nullopt,
            1U,
            ToolExecutionIdentity{
                .runIdentity                 = hashOf("high-risk-run"),
                .frameworkReleaseIdentity    = hashOf("high-risk-framework"),
                .toolRuntimeProtocolIdentity = hashOf("high-risk-protocol"),
                .environmentIdentity         = hashOf("high-risk-environment"),
            },
            invocation
        );
        REQUIRE(call.has_value());
        auto effect  = test_support::routineToolEffect(prepared.project);
        effect.risk  = Risk::High;
        auto effects = std::array{effect};
        auto approver = test_support::addController(
            prepared,
            ControllerKind::Human,
            SessionMode::Read,
            "high-risk-approver-session",
            "high-risk-approver-instance",
            prepared.controller.controlledTargetId(),
            std::nullopt,
            "human-approver",
            {"approve"}
        );
        auto refused = prepared.store.admitMutatingToolCall(
            prepared.controller,
            prepared.lease,
            *root,
            *call,
            prepared.planAuthority,
            effects,
            {}
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains("requires approval"));
        auto replay = prepared.store.replayToolCall(*root, *call);
        REQUIRE(replay.has_value());
        CHECK(replay->state == ToolCallState::Proposed);

        auto selfApproved = prepared.store.issueToolApproval(
            prepared.controller,
            prepared.lease,
            prepared.controller,
            *root,
            *call,
            prepared.planAuthority,
            effects,
            ToolApprovalRequest{
                .approverCapability = "approve",
                .expiresAtUnixMillis = static_cast<uint64>(
                    std::numeric_limits<int64>::max()
                ),
            },
            AuthorityDecisionId{"self-approval-decision"}
        );
        REQUIRE_FALSE(selfApproved.has_value());
        CHECK(selfApproved.error().message().contains("human binding"));

        auto unprivilegedApprover = test_support::addController(
            prepared,
            ControllerKind::Human,
            SessionMode::Read,
            "unprivileged-approver-session",
            "unprivileged-approver-instance",
            prepared.controller.controlledTargetId(),
            std::nullopt,
            "unprivileged-approver",
            {"operate"}
        );
        auto inventedCapability = prepared.store.issueToolApproval(
            prepared.controller,
            prepared.lease,
            unprivilegedApprover,
            *root,
            *call,
            prepared.planAuthority,
            effects,
            ToolApprovalRequest{
                .approverCapability = "approve",
                .expiresAtUnixMillis = static_cast<uint64>(
                    std::numeric_limits<int64>::max()
                ),
            },
            AuthorityDecisionId{"invented-capability-decision"}
        );
        REQUIRE_FALSE(inventedCapability.has_value());
        CHECK(inventedCapability.error().message().contains(
            "does not hold capability"
        ));

        auto wrongCapability = prepared.store.issueToolApproval(
            prepared.controller,
            prepared.lease,
            approver,
            *root,
            *call,
            prepared.planAuthority,
            effects,
            ToolApprovalRequest{
                .approverCapability = "not-an-approver",
                .expiresAtUnixMillis = static_cast<uint64>(
                    std::numeric_limits<int64>::max()
                ),
            },
            AuthorityDecisionId{"wrong-capability-decision"}
        );
        REQUIRE_FALSE(wrongCapability.has_value());
        CHECK_MESSAGE(
            wrongCapability.error().message().contains(
                "does not require approver capability"
            ),
            wrongCapability.error().message()
        );

        auto approval = prepared.store.issueToolApproval(
            prepared.controller,
            prepared.lease,
            approver,
            *root,
            *call,
            prepared.planAuthority,
            effects,
            ToolApprovalRequest{
                .approverCapability = "approve",
                .expiresAtUnixMillis = static_cast<uint64>(
                    std::numeric_limits<int64>::max()
                ),
            },
            AuthorityDecisionId{"high-risk-decision"}
        );
        auto const approvalWhy = approval.has_value()
            ? std::string{}
            : approval.error().message();
        REQUIRE_MESSAGE(approval.has_value(), approvalWhy);
        auto forged = std::array{
            ToolApprovalGrant{
                .token = approval->token,
                .authorityDecisionId = AuthorityDecisionId{"another-decision"},
            },
        };
        auto mismatched = prepared.store.admitMutatingToolCall(
            prepared.controller,
            prepared.lease,
            *root,
            *call,
            prepared.planAuthority,
            effects,
            forged
        );
        REQUIRE_FALSE(mismatched.has_value());
        CHECK(mismatched.error().message().contains(
            "stale, expired, mismatched, or already consumed"
        ));

        auto approvals = std::array{*approval};
        auto admitted = prepared.store.admitMutatingToolCall(
            prepared.controller,
            prepared.lease,
            *root,
            *call,
            prepared.planAuthority,
            effects,
            approvals
        );
        REQUIRE(admitted.has_value());
        auto repeated = prepared.store.admitMutatingToolCall(
            prepared.controller,
            prepared.lease,
            *root,
            *call,
            prepared.planAuthority,
            effects,
            approvals
        );
        REQUIRE(repeated.has_value());
        CHECK(repeated->attemptNumber() == admitted->attemptNumber());

        auto dispatch = prepared.store.beginToolCallDispatch(*admitted);
        REQUIRE(dispatch.has_value());
        auto delivered = CanonicalJson::parseExact(R"({"delivered":true})");
        REQUIRE(delivered.has_value());
        REQUIRE(prepared.store.completeToolCallDispatch(
            *dispatch,
            ToolCallCompletion::confirmed(*delivered)
        ).has_value());
        auto secondPreimage = CanonicalJson::parseExact(
            R"({"objective":"high-risk-second-call"})"
        );
        REQUIRE(secondPreimage.has_value());
        auto secondRoot = ToolRootRequestIdentity::create(
            "controller-1",
            "high-risk-second-call",
            std::move(*secondPreimage)
        );
        REQUIRE(secondRoot.has_value());
        auto secondCall = ToolCallPositionIdentity::create(
            *secondRoot,
            std::nullopt,
            1U,
            call->executionIdentity(),
            invocation
        );
        REQUIRE(secondCall.has_value());
        auto reused = prepared.store.admitMutatingToolCall(
            prepared.controller,
            prepared.lease,
            *secondRoot,
            *secondCall,
            prepared.planAuthority,
            effects,
            approvals
        );
        REQUIRE_FALSE(reused.has_value());
        CHECK(reused.error().message().contains(
            "stale, expired, mismatched, or already consumed"
        ));

        auto leaseBoundApproval = prepared.store.issueToolApproval(
            prepared.controller,
            prepared.lease,
            approver,
            *secondRoot,
            *secondCall,
            prepared.planAuthority,
            effects,
            ToolApprovalRequest{
                .approverCapability = "approve",
                .expiresAtUnixMillis = static_cast<uint64>(
                    std::numeric_limits<int64>::max()
                ),
            },
            AuthorityDecisionId{"lease-bound-decision"}
        );
        REQUIRE(leaseBoundApproval.has_value());
        REQUIRE(prepared.store.releaseLease(prepared.lease).has_value());
        auto replacementLease = prepared.store.acquireLease(prepared.controller);
        REQUIRE(replacementLease.has_value());
        auto leaseBoundApprovals = std::array{*leaseBoundApproval};
        auto staleLeaseApproval = prepared.store.admitMutatingToolCall(
            prepared.controller,
            *replacementLease,
            *secondRoot,
            *secondCall,
            prepared.planAuthority,
            effects,
            leaseBoundApprovals
        );
        REQUIRE_FALSE(staleLeaseApproval.has_value());
        CHECK(staleLeaseApproval.error().message().contains(
            "stale, expired, mismatched, or already consumed"
        ));
    }

    TEST_CASE("the immediate-prior Tool admission schema migrates attempts exactly")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        {
            auto prepared = prepareStore(temporary.path());
            auto preimage = CanonicalJson::parseExact(
                R"({"objective":"migration-observe"})"
            );
            REQUIRE(preimage.has_value());
            auto root = ToolRootRequestIdentity::create(
                "controller-1",
                "migration-observe",
                std::move(*preimage)
            );
            REQUIRE(root.has_value());
            auto catalog   = FrameworkToolCatalogOwner::create();
            auto arguments = CanonicalJson::parseExact("{}");
            REQUIRE(catalog.has_value());
            REQUIRE(arguments.has_value());
            auto invocation = catalog->validate(
                "framework.screen.observe",
                std::move(*arguments)
            );
            REQUIRE(invocation.has_value());
            auto call = ToolCallPositionIdentity::create(
                *root,
                std::nullopt,
                1U,
                ToolExecutionIdentity{
                    .runIdentity = hashOf("authority-migration-run"),
                    .frameworkReleaseIdentity =
                        hashOf("authority-migration-framework"),
                    .toolRuntimeProtocolIdentity =
                        hashOf("authority-migration-protocol"),
                    .environmentIdentity =
                        hashOf("authority-migration-environment"),
                },
                *invocation
            );
            REQUIRE(call.has_value());
            auto admitted = prepared.store.admitReadOnlyToolCall(
                prepared.controller,
                prepared.lease,
                *root,
                *call
            );
            REQUIRE(admitted.has_value());
        }

        auto oldRows        = std::vector<std::vector<std::string>>{};
        auto sourceIdentity = std::string{};
        {
            auto prior = test_support::OperatorDatabaseProbe{databasePath};
            oldRows = prior.readRows(
                "SELECT call_identity, attempt_number, root_identity, "
                "policy_hash, budget_snapshot_hash FROM tool_admission_attempts"
            );
            restorePriorToolAdmissionAuthority(prior);
            sourceIdentity = exactSchemaIdentity(prior);
        }
        CHECK(
            sourceIdentity
            == "sha256:64d3396e51680ec12cb91d944357f965e2136065fbb7b7fccc009d168fc4ac80"
        );

        {
            auto migrated = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(migrated.has_value(), migrated.error().message());
        }
        auto target = test_support::OperatorDatabaseProbe{databasePath};
        CHECK(
            exactSchemaIdentity(target)
            == "sha256:53c56cce2064c47a07bd29529320aac7e7f8f4e8c01a74dc54da936159dd44f8"
        );
        CHECK(
            target.readRows(
                "SELECT call_identity, attempt_number, root_identity, "
                "policy_hash, budget_snapshot_hash FROM tool_admission_attempts"
            ) == oldRows
        );
        CHECK(
            target.readRows(
                "SELECT count(*) FROM tool_admission_attempts WHERE "
                "effect_envelope IS NOT NULL OR effect_envelope_hash IS NOT NULL "
                "OR required_approvals IS NOT NULL OR approval_tokens IS NOT NULL"
            ) == std::vector<std::vector<std::string>>{{"0"}}
        );
        CHECK(
            target.readRows(
                "SELECT source_identity, target_identity FROM "
                "schema_identity_transitions WHERE source_identity='"
                + sourceIdentity + "'"
            ) == std::vector<std::vector<std::string>>{
                {
                    sourceIdentity,
                    "sha256:53c56cce2064c47a07bd29529320aac7e7f8f4e8c01a74dc54da936159dd44f8",
                },
            }
        );
    }

    TEST_CASE("Tool approval must remain live until the dispatch boundary")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto preimage  = CanonicalJson::parseExact(R"({"objective":"expiry"})");
        REQUIRE(preimage.has_value());
        auto root = ToolRootRequestIdentity::create(
            "controller-1",
            "approval-expiry",
            std::move(*preimage)
        );
        REQUIRE(root.has_value());
        auto invocation = toolInvocation(prepared.project, "command-1");
        auto call = ToolCallPositionIdentity::create(
            *root,
            std::nullopt,
            1U,
            ToolExecutionIdentity{
                .runIdentity                 = hashOf("approval-expiry-run"),
                .frameworkReleaseIdentity    = hashOf("approval-expiry-framework"),
                .toolRuntimeProtocolIdentity = hashOf("approval-expiry-protocol"),
                .environmentIdentity         = hashOf("approval-expiry-environment"),
            },
            invocation
        );
        REQUIRE(call.has_value());
        auto effect  = test_support::routineToolEffect(prepared.project);
        effect.risk  = Risk::High;
        auto effects = std::array{effect};
        auto approver = test_support::addController(
            prepared,
            ControllerKind::Human,
            SessionMode::Read,
            "expiry-approver-session",
            "expiry-approver-instance",
            prepared.controller.controlledTargetId(),
            std::nullopt,
            "expiry-approver",
            {"approve"}
        );
        auto const now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        REQUIRE(now > 0);
        auto const expiresAt = static_cast<uint64>(now) + 1'000U;
        auto approval = prepared.store.issueToolApproval(
            prepared.controller,
            prepared.lease,
            approver,
            *root,
            *call,
            prepared.planAuthority,
            effects,
            ToolApprovalRequest{
                .approverCapability  = "approve",
                .expiresAtUnixMillis = expiresAt,
            },
            AuthorityDecisionId{"expiry-decision"}
        );
        REQUIRE(approval.has_value());
        auto approvals = std::array{*approval};
        auto admitted = prepared.store.admitMutatingToolCall(
            prepared.controller,
            prepared.lease,
            *root,
            *call,
            prepared.planAuthority,
            effects,
            approvals
        );
        REQUIRE(admitted.has_value());

        std::this_thread::sleep_until(std::chrono::system_clock::time_point{
            std::chrono::milliseconds{static_cast<int64>(expiresAt + 100U)},
        });
        auto expired = prepared.store.beginToolCallDispatch(*admitted);
        REQUIRE_FALSE(expired.has_value());
        CHECK(expired.error().message().contains(
            "approval expired after admission"
        ));
    }

    TEST_CASE("the immediate-prior Tool approval schema migrates effect authority")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        {
            auto prepared = prepareStore(temporary.path());
            auto preimage = CanonicalJson::parseExact(
                R"({"objective":"approval-schema-migration"})"
            );
            REQUIRE(preimage.has_value());
            auto root = ToolRootRequestIdentity::create(
                "controller-1",
                "approval-schema-migration",
                std::move(*preimage)
            );
            REQUIRE(root.has_value());
            auto invocation = toolInvocation(prepared.project, "command-1");
            auto call = ToolCallPositionIdentity::create(
                *root,
                std::nullopt,
                1U,
                ToolExecutionIdentity{
                    .runIdentity = hashOf("approval-migration-run"),
                    .frameworkReleaseIdentity =
                        hashOf("approval-migration-framework"),
                    .toolRuntimeProtocolIdentity =
                        hashOf("approval-migration-protocol"),
                    .environmentIdentity =
                        hashOf("approval-migration-environment"),
                },
                invocation
            );
            REQUIRE(call.has_value());
            auto effects = std::array{
                test_support::routineToolEffect(prepared.project),
            };
            auto admitted = prepared.store.admitMutatingToolCall(
                prepared.controller,
                prepared.lease,
                *root,
                *call,
                prepared.planAuthority,
                effects,
                {}
            );
            REQUIRE(admitted.has_value());
        }

        auto authorityRows  = std::vector<std::vector<std::string>>{};
        auto sourceIdentity = std::string{};
        {
            auto prior = test_support::OperatorDatabaseProbe{databasePath};
            authorityRows = prior.readRows(
                "SELECT call_identity, attempt_number, effect_envelope, "
                "effect_envelope_hash, required_approvals FROM "
                "tool_admission_attempts"
            );
            restorePriorToolApprovalSchema(prior);
            sourceIdentity = exactSchemaIdentity(prior);
        }
        CHECK(
            sourceIdentity
            == "sha256:14fbb87b8e84ce4c9f977d423a1b6e981e0425e06ef17f9a7822e6d32a8e87a4"
        );

        {
            auto migrated = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(migrated.has_value(), migrated.error().message());
        }
        auto target = test_support::OperatorDatabaseProbe{databasePath};
        auto migratedAuthorityRows = target.readRows(
            "SELECT call_identity, attempt_number, effect_envelope, "
            "effect_envelope_hash, required_approvals, approval_tokens FROM "
            "tool_admission_attempts"
        );
        REQUIRE(migratedAuthorityRows.size() == authorityRows.size());
        for (auto index = std::size_t{}; index < authorityRows.size(); ++index)
        {
            REQUIRE(migratedAuthorityRows[index].size() == 6U);
            CHECK(std::vector<std::string>{
                migratedAuthorityRows[index].begin(),
                migratedAuthorityRows[index].begin() + 5,
            } == authorityRows[index]);
            CHECK(migratedAuthorityRows[index][5] == "[]");
        }
        CHECK(
            target.readRows("SELECT count(*) FROM tool_approvals")
            == std::vector<std::vector<std::string>>{{"0"}}
        );
        CHECK(
            target.readRows(
                "SELECT source_identity, target_identity FROM "
                "schema_identity_transitions WHERE source_identity='"
                + sourceIdentity + "'"
            ).size() == 1U
        );
    }

    TEST_CASE("the immediate-prior Tool runtime schema migrates identity rows exactly")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        auto preimage           = CanonicalJson::parseExact("{}");
        REQUIRE(preimage.has_value());
        auto root = ToolRootRequestIdentity::create(
            "migration-principal",
            "migration-request",
            std::move(*preimage)
        );
        REQUIRE(root.has_value());
        auto catalog   = FrameworkToolCatalogOwner::create();
        auto arguments = CanonicalJson::parseExact("{}");
        REQUIRE(catalog.has_value());
        REQUIRE(arguments.has_value());
        auto invocation = catalog->validate(
            "framework.screen.observe",
            std::move(*arguments)
        );
        REQUIRE(invocation.has_value());
        auto call = ToolCallPositionIdentity::create(
            *root,
            std::nullopt,
            1U,
            ToolExecutionIdentity{
                .runIdentity                 = hashOf("migration-run"),
                .frameworkReleaseIdentity    = hashOf("migration-framework"),
                .toolRuntimeProtocolIdentity = hashOf("migration-protocol"),
                .environmentIdentity         = hashOf("migration-environment"),
            },
            *invocation
        );
        REQUIRE(call.has_value());
        {
            auto store = OperatorCoordinator::open(production);
            REQUIRE(store.has_value());
            REQUIRE(store->persistToolRootRequest(*root).has_value());
            REQUIRE(store->persistToolCallPosition(*root, *call).has_value());
        }
        auto identityRows   = std::vector<std::vector<std::string>>{};
        auto sourceIdentity = std::string{};
        {
            auto prior = test_support::OperatorDatabaseProbe{databasePath};
            identityRows = prior.readRows(
                "SELECT call_identity, root_identity, canonical_args "
                "FROM tool_call_positions"
            );
            removeToolRuntimePersistence(prior);
            sourceIdentity = exactSchemaIdentity(prior);
        }
        CHECK(
            sourceIdentity
            == "sha256:50375791a22d12ab8b03f83eb48afc2183091e0d95b07fc5e4be47bb9aa07062"
        );

        {
            auto migrated = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(migrated.has_value(), migrated.error().message());
        }
        {
            auto target = test_support::OperatorDatabaseProbe{databasePath};
            CHECK(
                exactSchemaIdentity(target)
                == "sha256:53c56cce2064c47a07bd29529320aac7e7f8f4e8c01a74dc54da936159dd44f8"
            );
            CHECK(
                target.readRows(
                    "SELECT call_identity, root_identity, canonical_args "
                    "FROM tool_call_positions"
                ) == identityRows
            );
            CHECK(
                target.readRows("SELECT count(*) FROM tool_call_history")
                == std::vector<std::vector<std::string>>{{"0"}}
            );
            CHECK(
                target.readRows(
                    "SELECT source_identity, target_identity "
                    "FROM schema_identity_transitions"
                )
                == std::vector<std::vector<std::string>>{
                    {
                        sourceIdentity,
                        "sha256:53c56cce2064c47a07bd29529320aac7e7f8f4e8c01a74dc54da936159dd44f8",
                    },
                }
            );
        }
        auto migrated = OperatorCoordinator::open(production);
        REQUIRE(migrated.has_value());
        auto restored = migrated->persistToolCallPosition(*root, *call);
        REQUIRE(restored.has_value());
        auto replay = migrated->replayToolCall(*root, *call);
        REQUIRE(replay.has_value());
        CHECK(replay->state == ToolCallState::Proposed);
    }

    TEST_CASE("format-2 registrations migrate byte-identical and become audit-only")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        auto prepared = prepareStore(temporary.path());
        auto const operation = createReadyOperation(
            prepared,
            "request-format-2-recovery",
            "command-1"
        );
        auto host = deliveringHost(prepared);
        auto const reserved = prepared.store.reserveDispatch(
            operation.operationId,
            operation.revision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"authority-format-2-recovery"},
            std::nullopt
        );
        REQUIRE(reserved.has_value());
        { auto releasedStore = std::move(prepared.store); }

        auto sourceIdentity = std::string{};
        auto historicalRows = std::vector<std::vector<std::string>>{};
        {
            auto prior = test_support::OperatorDatabaseProbe{databasePath};
            restoreFormat2RegistrationIdentity(prior);
            sourceIdentity = exactSchemaIdentity(prior);
            historicalRows = prior.readRows(
                "SELECT registration_hash, plugin_id, plugin_hash, canonical_manifest "
                "FROM project_registrations ORDER BY registration_hash"
            );
        }
        CHECK(sourceIdentity
              == "sha256:b26344e031574f95020ed445e16e9de396f76442d98c5a3b758a91d84660237e");

        auto const legacyExecutionRows = test_support::OperatorDatabaseProbe{
            databasePath
        }.readRows(
            "SELECT o.state, o.revision, coalesce(d.delivery_outcome, ''), "
            "coalesce(d.delivery_reason, ''), "
            "(SELECT count(*) FROM ledger_events e WHERE e.subject_id=o.operation_id) "
            "FROM operations o JOIN dispatches d ON d.operation_id=o.operation_id "
            "WHERE o.operation_id='" + operation.operationId + "'"
        );
        REQUIRE(legacyExecutionRows.size() == 1U);

        {
            auto migrated = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(migrated.has_value(), migrated.error().message());
        }
        CHECK(test_support::OperatorDatabaseProbe{databasePath}.readRows(
            "SELECT o.state, o.revision, coalesce(d.delivery_outcome, ''), "
            "coalesce(d.delivery_reason, ''), "
            "(SELECT count(*) FROM ledger_events e WHERE e.subject_id=o.operation_id) "
            "FROM operations o JOIN dispatches d ON d.operation_id=o.operation_id "
            "WHERE o.operation_id='" + operation.operationId + "'"
        ) == legacyExecutionRows);
        auto auditRows = test_support::OperatorDatabaseProbe{databasePath}.readRows(
            "SELECT registration_hash, plugin_id, plugin_identity_hash, "
            "canonical_manifest, registration_format, plugin_identity_kind "
            "FROM project_registrations ORDER BY registration_hash"
        );
        REQUIRE(auditRows.size() == historicalRows.size());
        for (auto index = std::size_t{0}; index < historicalRows.size(); ++index)
        {
            CHECK(std::vector<std::string>{
                auditRows[index][0],
                auditRows[index][1],
                auditRows[index][2],
                auditRows[index][3],
            } == historicalRows[index]);
            CHECK(auditRows[index][4] == "2");
            CHECK(auditRows[index][5] == "single_source");
        }

        auto migrated = OperatorCoordinator::open(production);
        REQUIRE_MESSAGE(migrated.has_value(), migrated.error().message());
        auto const refused = migrated->resumeSession(
            SessionResume{
                .authenticatedControllerId = "controller-1",
                .controlledTargetId        = "target-1",
                .mode                      = SessionMode::Write,
                .kind                      = ControllerKind::Script,
            },
            prepared.manifest
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains("legacy registration is audit-only"));
    }

    TEST_CASE("registration format and plugin identity kind remain an exact pair")
    {
        auto temporary          = TemporaryDirectory{};
        auto const databasePath = temporary.path()
            / "production"
            / "operator-runtime.sqlite";
        auto prepared = prepareStore(temporary.path());
        { auto releasedStore = std::move(prepared.store); }

        {
            auto database = test_support::OperatorDatabaseProbe{databasePath};
            CHECK(database.refuses(
                "UPDATE project_registrations SET plugin_identity_kind='single_source' "
                "WHERE registration_format=3"
            ));

            // Simulate storage corruption after proving the exact DDL rejects
            // it normally. Runtime admission must still check both columns.
            database.execute("PRAGMA ignore_check_constraints=ON");
            database.execute(
                "UPDATE project_registrations SET plugin_identity_kind='single_source' "
                "WHERE registration_format=3"
            );
        }

        auto reopened = OperatorCoordinator::open(temporary.path() / "production");
        REQUIRE(reopened.has_value());
        auto const refused = reopened->resumeSession(
            SessionResume{
                .authenticatedControllerId = "controller-1",
                .controlledTargetId        = "target-1",
                .mode                      = SessionMode::Write,
                .kind                      = ControllerKind::Script,
            },
            prepared.manifest
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains("legacy registration is audit-only"));
    }

    TEST_CASE("release upgrade evidence migrates by exact identity with empty replay difference")
    {
        auto temporary          = TemporaryDirectory{};
        auto const databasePath = temporary.path()
            / "production"
            / "operator-runtime.sqlite";
        {
            auto prepared = prepareStore(temporary.path());
            static_cast<void>(prepared);
        }

        auto sourceIdentity = std::string{};
        auto replayBefore   = std::vector<std::vector<std::string>>{};
        {
            auto source = test_support::OperatorDatabaseProbe{databasePath};
            restoreFormat2RegistrationIdentity(source);
            removeReleaseUpgradeEvidenceTables(source);
            restorePriorRegistrationStateSchemaHash(source);
            removeSessionWorldScopeColumns(source);
            removeObservedInstanceBindingLocalRefColumn(source);
            sourceIdentity = exactSchemaIdentity(source);
            replayBefore = source.readRows(
                "SELECT sequence, event_id, namespaced_event_type, "
                "opaque_project_payload, provenance FROM journal_events "
                "ORDER BY sequence"
            );
        }
        CHECK_MESSAGE(
            sourceIdentity
                == "sha256:d96860862dc25fb6efb21d09f59dcc99e3eed9508a5b6a6766937a15b3186eb9",
            "the release migration fixture must reproduce its exact source identity"
        );

        {
            auto migrated = OperatorCoordinator::open(
                temporary.path() / "production"
            );
            REQUIRE_MESSAGE(
                migrated.has_value(),
                "the registered release-evidence identity pair must migrate: ",
                migrated.error().message()
            );
        }

        auto target = test_support::OperatorDatabaseProbe{databasePath};
        auto const targetIdentity = exactSchemaIdentity(target);
        auto const expectedTransition = std::vector<std::vector<std::string>>{
            {sourceIdentity, targetIdentity},
        };
        CHECK(
            target.readRows(
                "SELECT source_identity, target_identity "
                "FROM schema_identity_transitions "
                "WHERE source_identity='sha256:"
                "d96860862dc25fb6efb21d09f59dcc99e3eed9508a5b6a6766937a15b3186eb9'"
            ) == expectedTransition
        );
        auto const replayAfter = target.readRows(
            "SELECT sequence, event_id, namespaced_event_type, "
            "opaque_project_payload, provenance FROM journal_events "
            "ORDER BY sequence"
        );
        CHECK_MESSAGE(
            replayAfter == replayBefore,
            "replaying the pre-upgrade Journal must produce an empty domain-history difference"
        );
    }

    // The pair for the observed-instance world scope joining the pinned
    // session tuple. Pre-scope sessions cannot claim a world scope -- the
    // ruling forbids inferring one -- so the backfill is the empty-account
    // sentinel: the column CHECKs accept it and restoreSessionWorldScope
    // refuses it, leaving the session unable to observe rather than minting
    // under a scope it never claimed. Every other byte of the row survives.
    // The refusal is exercised below through the resumed production observe
    // path, not only asserted on the stored row.
    TEST_CASE("the session world scope pair migrates pre-scope sessions fail-closed")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        auto prepared = prepareStore(temporary.path());
        // The fixture store holds the runtime directory exclusively, so the
        // reopened door below can only take it after this one is released.
        { auto releasedStore = std::move(prepared.store); }

        auto sourceIdentity = std::string{};
        auto sessionRows    = std::vector<std::vector<std::string>>{};
        {
            auto prior = test_support::OperatorDatabaseProbe{databasePath};
            restoreFormat2RegistrationIdentity(prior);
            removeSessionWorldScopeColumns(prior);
            removeObservedInstanceBindingLocalRefColumn(prior);
            sourceIdentity = exactSchemaIdentity(prior);
            sessionRows = prior.readRows(
                "SELECT session_id, controlled_target_id, active FROM sessions "
                "ORDER BY session_id"
            );
        }
        REQUIRE_FALSE(sessionRows.empty());
        CHECK_MESSAGE(
            sourceIdentity
                == "sha256:035e04f2e066eb90c457a0af7440356274551be4abd6496b620879e9d4e3b133",
            "the fixture must reproduce the exact identity this pair migrates from"
        );

        {
            auto migrated = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(
                migrated.has_value(),
                "the registered session-world-scope identity pair must migrate: ",
                migrated.error().message()
            );

            // The registration row survives but format 2 is audit-only. Resume
            // must refuse before any legacy VM or world-scope interpretation.
            auto resumed = migrated->resumeSession(
                SessionResume{
                    .authenticatedControllerId = "controller-1",
                    .controlledTargetId        = "target-1",
                    .mode                      = SessionMode::Write,
                    .kind                      = ControllerKind::Script,
                },
                prepared.manifest
            );
            REQUIRE_FALSE(resumed.has_value());
            CHECK_MESSAGE(
                resumed.error().message().contains("legacy registration is audit-only"),
                "resume must refuse the historical registration explicitly"
            );
        }

        auto target = test_support::OperatorDatabaseProbe{databasePath};
        auto const targetIdentity = exactSchemaIdentity(target);
        CHECK(sourceIdentity != targetIdentity);
        CHECK(
            target.readRows(
                "SELECT source_identity, target_identity "
                "FROM schema_identity_transitions"
            )
            == std::vector<std::vector<std::string>>{
                {sourceIdentity, targetIdentity},
            }
        );
        // The row remains deactivated and its sentinel survives byte-for-byte;
        // the audit-only refusal above prevents interpreting it.
        CHECK(
            target.readRows(
                "SELECT session_id, controlled_target_id, active, "
                "world_scope_kind, world_scope_id, world_scope_generation "
                "FROM sessions ORDER BY session_id"
            )
            == std::vector<std::vector<std::string>>{
                {"session-1", "target-1", "0", "account", "", "0"},
            }
        );
    }

    // The pair for the local_ref the binding gained this batch, resolving a
    // step's ui_target_id to the model target the instance was observed at.
    // Pre-local_ref bindings cannot have their model target reconstructed --
    // the ruling forbids inferring one -- so the backfill is the empty-string
    // sentinel: the NOT NULL column accepts it and reserveDispatch refuses it,
    // leaving the instance undeliverable rather than dispatching under a
    // model target it never had. Every other byte of the row survives. The
    // refusal is exercised below through the reserved production dispatch
    // path, not only asserted on the stored row.
    TEST_CASE("the binding local_ref column migrates pre-target bindings fail-closed")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        auto prepared = prepareStore(temporary.path());
        // The fixture store holds the runtime directory exclusively, so the
        // reopened door below can only take it after this one is released.
        { auto releasedStore = std::move(prepared.store); }

        auto sourceIdentity = std::string{};
        auto bindingRows    = std::vector<std::vector<std::string>>{};
        {
            auto prior = test_support::OperatorDatabaseProbe{databasePath};
            restoreFormat2RegistrationIdentity(prior);
            removeObservedInstanceBindingLocalRefColumn(prior);
            sourceIdentity = exactSchemaIdentity(prior);
            bindingRows = prior.readRows(
                "SELECT observed_instance_id FROM observed_instance_bindings "
                "ORDER BY observed_instance_id"
            );
        }
        REQUIRE_FALSE(bindingRows.empty());
        CHECK_MESSAGE(
            sourceIdentity
                == "sha256:26a38c2fd4357f538a99cb1b54573f6c2998e19e9a09252e7e9792c45745cec9",
            "the fixture must reproduce the exact identity this pair migrates from"
        );

        {
            auto migrated = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(
                migrated.has_value(),
                "the registered binding-local-ref identity pair must migrate: ",
                migrated.error().message()
            );

            // No legacy execution reaches the binding. The old row remains
            // available for audit, but resume refuses before constructing a VM.
            auto resumed = migrated->resumeSession(
                SessionResume{
                    .authenticatedControllerId = "controller-1",
                    .controlledTargetId        = "target-1",
                    .mode                      = SessionMode::Write,
                    .kind                      = ControllerKind::Script,
                },
                prepared.manifest
            );
            REQUIRE_FALSE(resumed.has_value());
            CHECK_MESSAGE(
                resumed.error().message().contains("legacy registration is audit-only"),
                "resume must refuse the historical registration explicitly"
            );
        }

        auto target = test_support::OperatorDatabaseProbe{databasePath};
        auto const targetIdentity = exactSchemaIdentity(target);
        CHECK(sourceIdentity != targetIdentity);
        CHECK(
            target.readRows(
                "SELECT source_identity, target_identity "
                "FROM schema_identity_transitions"
            )
            == std::vector<std::vector<std::string>>{
                {sourceIdentity, targetIdentity},
            }
        );
        // The walk above minted a step against the migrated binding, so the
        // row still exists; the backfill wrote the empty sentinel, which is
        // the only local_ref a pre-target binding can honestly carry. The
        // successful step mint itself proves the rest of the row survived:
        // mintNextStep resolves the very binding reserveDispatch refused.
        auto const migratedBindings = target.readRows(
            "SELECT observed_instance_id, local_ref FROM observed_instance_bindings "
            "ORDER BY observed_instance_id"
        );
        REQUIRE(migratedBindings.size() == bindingRows.size());
        for (std::size_t index = 0; index < migratedBindings.size(); ++index)
        {
            CHECK(migratedBindings[index][0] == bindingRows[index][0]);
            CHECK(migratedBindings[index][1].empty());
        }
    }

    // The pair for dropping project_registrations.project_state_schema_hash.
    // The registration rows must survive it: the column was a copy of a member
    // their canonical_manifest carries, and losing the row would lose the
    // original rather than the copy.
    TEST_CASE("dropping the registration state schema column migrates under its exact pair")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        {
            auto prepared = prepareStore(temporary.path());
            static_cast<void>(prepared);
        }

        auto sourceIdentity   = std::string{};
        auto registrationRows = std::vector<std::vector<std::string>>{};
        {
            auto prior = test_support::OperatorDatabaseProbe{databasePath};
            restoreFormat2RegistrationIdentity(prior);
            restorePriorRegistrationStateSchemaHash(prior);
            removeSessionWorldScopeColumns(prior);
            removeObservedInstanceBindingLocalRefColumn(prior);
            sourceIdentity   = exactSchemaIdentity(prior);
            registrationRows = prior.readRows(
                "SELECT registration_hash, plugin_id, plugin_hash, "
                "canonical_manifest FROM project_registrations "
                "ORDER BY registration_hash"
            );
        }
        REQUIRE_FALSE(registrationRows.empty());
        CHECK_MESSAGE(
            sourceIdentity
                == "sha256:869fb0a128df4a0026bb429449fae03d6b43244c9cef4e794dfdd648421bcc19",
            "the fixture must reproduce the exact identity this pair migrates from"
        );

        {
            auto migrated = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(
                migrated.has_value(),
                "the registered registration-column identity pair must migrate: ",
                migrated.error().message()
            );
        }

        auto target = test_support::OperatorDatabaseProbe{databasePath};
        auto const targetIdentity = exactSchemaIdentity(target);
        CHECK(sourceIdentity != targetIdentity);
        CHECK(
            target.readRows(
                "SELECT registration_hash, plugin_id, plugin_identity_hash, "
                "canonical_manifest FROM project_registrations "
                "ORDER BY registration_hash"
            ) == registrationRows
        );
        CHECK(
            target.readRows(
                "SELECT source_identity, target_identity "
                "FROM schema_identity_transitions"
            )
            == std::vector<std::vector<std::string>>{
                {sourceIdentity, targetIdentity},
            }
        );
    }

    TEST_CASE("the corrected snapshot claim migrates under its exact identity pair")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        {
            auto created = OperatorCoordinator::open(production);
            REQUIRE(created.has_value());
        }

        auto sourceIdentity = std::string{};
        {
            auto prior = test_support::OperatorDatabaseProbe{databasePath};
            restoreFormat2RegistrationIdentity(prior);
            removeReleaseUpgradeEvidenceTables(prior);
            restorePriorSnapshotIdentityComment(prior);
            restorePriorRegistrationStateSchemaHash(prior);
            removeSessionWorldScopeColumns(prior);
            removeObservedInstanceBindingLocalRefColumn(prior);
            sourceIdentity = exactSchemaIdentity(prior);
        }
        CHECK_MESSAGE(
            sourceIdentity
                == "sha256:1b70212548858e70daf7f120a0245d0af93fd3ff1e9cbab48d7dfa271b57f302",
            "the fixture must reproduce the exact comment-only identity this pair migrates from"
        );

        {
            auto migrated = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(
                migrated.has_value(),
                "the registered comment-only identity pair must migrate: ",
                migrated.error().message()
            );
        }

        auto target = test_support::OperatorDatabaseProbe{databasePath};
        auto const targetIdentity = exactSchemaIdentity(target);
        CHECK(sourceIdentity != targetIdentity);
        CHECK(
            target.readRows(
                "SELECT source_identity, target_identity "
                "FROM schema_identity_transitions"
            )
            == std::vector<std::vector<std::string>>{
                {sourceIdentity, targetIdentity},
            }
        );
    }

    TEST_CASE("an unregistered exact identity pair is refused byte-identical")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        {
            auto created = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(
                created.has_value(),
                "a created schema must equal the pinned exact DDL schema identity"
            );
        }

        auto const mutated = mutateStoredDdlSeparator(databasePath);
        auto const beforeRefusal = ledgerBytes(databasePath);
        auto const refused = OperatorCoordinator::open(production);
        REQUIRE_FALSE_MESSAGE(
            refused.has_value(),
            "an unregistered exact identity pair must not be upgraded or replaced"
        );

        // Names the guard so another open refusal cannot stand in for identity.
        CHECK_MESSAGE(
            refused.error().message().contains(
                "no registered audit-preserving disposition"
            ),
            "the refusal must come from the unregistered identity-pair gate"
        );
        CHECK_MESSAGE(
            ledgerBytes(databasePath) == beforeRefusal,
            "an unregistered identity refusal must leave the whole ledger byte-identical"
        );
        CHECK_MESSAGE(
            storedDdlSeparators(databasePath, mutated)
                == std::string(mutated.size(), '\n'),
            "a refused database keeps the exact bytes it was refused for"
        );
    }

    TEST_CASE("a registered exact identity pair upgrades a populated audit chain")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        auto operationId         = std::string{};
        auto artifactRootHash    = std::optional<ContentHash>{};
        auto installedGeneration = uint64{};
        {
            auto prepared = prepareStore(temporary.path());
            auto const operation = proposedOperation(
                prepared,
                "migration-request",
                "command-1"
            );
            operationId         = operation.operationId;
            artifactRootHash    = prepared.runtimeArtifactRootHash;
            installedGeneration = prepared.installedGeneration;
        }

        auto sourceIdentity = std::string{};
        {
            auto priorSchema = test_support::OperatorDatabaseProbe{databasePath};
            restoreFormat2RegistrationIdentity(priorSchema);
            priorSchema.execute("DROP TABLE availability_heads");
            priorSchema.execute("DROP TABLE session_policies");
            priorSchema.execute(
                "ALTER TABLE ledger_events RENAME TO prior_ledger_events"
            );
            priorSchema.execute(R"sql(
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
                )sql");
            priorSchema.execute(
                "INSERT INTO ledger_events(sequence, session_epoch, "
                "controlled_target_id, kind, subject_id) SELECT sequence, "
                "session_epoch, controlled_target_id, kind, subject_id "
                "FROM prior_ledger_events"
            );
            priorSchema.execute("DROP TABLE prior_ledger_events");
            removeReleaseUpgradeEvidenceTables(priorSchema);
            restorePriorSnapshotIdentityComment(priorSchema);
            restorePriorRegistrationStateSchemaHash(priorSchema);
            removeSessionWorldScopeColumns(priorSchema);
            removeObservedInstanceBindingLocalRefColumn(priorSchema);
            sourceIdentity = exactSchemaIdentity(priorSchema);
        }
        CHECK_MESSAGE(
            sourceIdentity
                == "sha256:2a8fdd44c39346f1ee7d380b0c1cf0f51fa07b68db396a593446e3029421a23b",
            "the migration fixture must reproduce the exact U9 source identity"
        );

        REQUIRE(artifactRootHash.has_value());
        auto const beforeReadOnlyRefusal = ledgerBytes(databasePath);
        auto const readOnlyRefusal = OperatorCoordinator::readInstalledRuntimeArtifact(
            production,
            installedGeneration,
            *artifactRootHash
        );
        REQUIRE_FALSE_MESSAGE(
            readOnlyRefusal.has_value(),
            "the read-only door must not apply a registered schema migration"
        );
        CHECK_MESSAGE(
            readOnlyRefusal.error().message().contains("schema identity"),
            "a registered source must reach the read-only schema-identity gate"
        );
        CHECK_MESSAGE(
            ledgerBytes(databasePath) == beforeReadOnlyRefusal,
            "read-only refusal of a registered source must preserve the whole ledger"
        );

        {
            auto upgraded = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(
                upgraded.has_value(),
                "a registered exact source-target pair must produce the pinned target identity: ",
                upgraded.error().message()
            );
        }
        {
            auto verifiedTarget = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(
                verifiedTarget.has_value(),
                "a committed schema migration must reopen as the pinned target identity"
            );
        }

        auto migrated = test_support::OperatorDatabaseProbe{databasePath};
        auto const targetIdentity = exactSchemaIdentity(migrated);
        REQUIRE(sourceIdentity != targetIdentity);

        auto const expectedTransition = std::vector<std::vector<std::string>>{
            {sourceIdentity, targetIdentity},
        };
        CHECK_MESSAGE(
            migrated.readRows(
                "SELECT source_identity, target_identity "
                "FROM schema_identity_transitions"
            ) == expectedTransition,
            "the schema upgrade must record its exact source-target identity pair"
        );

        auto const expectedJournal = std::vector<std::vector<std::string>>{
            {
                "baseline-1",
                "fixture.baseline",
                "{\"kind\":\"baseline\"}",
                std::string{k_fixtureProvenance},
            },
        };
        CHECK_MESSAGE(
            migrated.readRows(
                "SELECT event_id, namespaced_event_type, opaque_project_payload, provenance "
                "FROM journal_events WHERE event_id='baseline-1'"
            ) == expectedJournal,
            "the populated Journal entry must remain readable with its exact content"
        );

        auto const expectedOperation = std::vector<std::vector<std::string>>{
            {
                operationId,
                "migration-request",
                "command-1",
                "proposed",
            },
        };
        CHECK_MESSAGE(
            migrated.readRows(
                "SELECT operation_id, client_request_id, tool_name, state "
                "FROM operations WHERE client_request_id='migration-request'"
            ) == expectedOperation,
            "the populated Operation ledger row must remain readable with its exact content"
        );

        auto const expectedAuditTrace = std::vector<std::vector<std::string>>{
            {
                "operation_created",
                "target-1",
                operationId,
            },
        };
        CHECK_MESSAGE(
            migrated.readRows(
                "SELECT kind, controlled_target_id, subject_id FROM ledger_events "
                "WHERE kind='operation_created' AND controlled_target_id='target-1'"
            ) == expectedAuditTrace,
            "the populated audit trace must remain readable with its exact content"
        );
    }

    TEST_CASE("pinSession names both registration hashes when the pin and manifest disagree")
    {
        auto temporary = TemporaryDirectory{};
        auto [store, artifactRootHash] = storeWithInstalledArtifact(temporary.path());

        auto const manifestRegistration = hashOf("registration-manifest-names");
        auto const pinRegistration      = hashOf("registration-pin-selects");
        auto const manifest =
            manifestNamingRegistration(artifactRootHash, manifestRegistration);
        auto const mismatchScope = ObservedInstanceWorldScope::run(
            "target-mismatch",
            1
        );
        REQUIRE(mismatchScope.has_value());

        auto const disagreeing = store.pinSession(
            SessionPin{
                .sessionId                 = "session-mismatch",
                .authenticatedControllerId = "controller-mismatch",
                .idempotencyNamespace      = "controller-mismatch",
                .projectRegistrationHash   = pinRegistration,
                .controllerCapabilities    = {std::string{conformance::k_operateCapability}},
                .controlledTargetId        = "target-mismatch",
                .projectInstanceKey        = "instance-mismatch",
                .mode                      = SessionMode::Write,
                .kind                      = ControllerKind::Script,
                .worldScope                = *mismatchScope,
            },
            manifest,
            std::nullopt
        );
        REQUIRE_FALSE(disagreeing.has_value());
        CHECK(
            disagreeing.error().message().contains("does not bind the selected")
        );
        CHECK(disagreeing.error().message().contains(manifestRegistration.hex()));
        CHECK(disagreeing.error().message().contains(pinRegistration.hex()));
    }

    TEST_CASE("session pin refuses an unterminated mutation and names its Operation")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const operation = proposedOperation(
            prepared,
            "upgrade-mutation",
            "command-1"
        );
        auto const pin = additionalSessionPin(
            prepared,
            "session-after-mutation"
        );
        auto const candidate = releaseWithModel(
            temporary.path() / "mutation-quiescence-upgrade",
            "mutation quiescence candidate runtime model\n"
        );
        REQUIRE(prepared.store.installRuntimeArtifact(
            installRequest(candidate, prepared.installedGeneration)
        ).has_value());
        auto const manifest = sessionManifest(
            prepared.project.registration,
            candidate.artifactRootHash,
            hashOf("agent"),
            test_support::policyArtifactBytes()
        );
        auto const refused = prepared.store.pinSession(
            pin,
            manifest,
            std::nullopt
        );

        REQUIRE_FALSE_MESSAGE(
            refused.has_value(),
            "an unterminated mutating Operation must refuse a new session pin"
        );
        CHECK_MESSAGE(
            refused.error().message().contains("unterminated mutating Operation"),
            "the refusal must come from the mutation quiescence guard"
        );
        CHECK_MESSAGE(
            refused.error().message().contains(operation.operationId),
            "the mutation refusal must name the Operation blocking the pin"
        );

        auto const terminated = prepared.store.transitionOperation(
            operation.operationId,
            operation.revision,
            OperationSignal::Invalidated
        );
        REQUIRE(terminated.has_value());
        auto const accepted = prepared.store.pinSession(
            pin,
            manifest,
            std::nullopt
        );
        CHECK_MESSAGE(
            accepted.has_value(),
            "a pin must succeed after its mutation terminates: ",
            accepted.error().message()
        );
    }

    TEST_CASE("session pin refuses an in-flight dispatch until it is answered")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const operation = createReadyOperation(
            prepared,
            "upgrade-dispatch",
            "command-1"
        );
        auto host           = deliveringHost(prepared);
        auto const dispatch = prepared.store.reserveDispatch(
            operation.operationId,
            operation.revision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"upgrade-dispatch-authority"},
            std::nullopt
        );
        REQUIRE(dispatch.has_value());

        auto const candidate = releaseWithModel(
            temporary.path() / "dispatch-quiescence-upgrade",
            "dispatch quiescence candidate runtime model\n"
        );
        REQUIRE(prepared.store.installRuntimeArtifact(
            installRequest(candidate, prepared.installedGeneration)
        ).has_value());
        auto const manifest = sessionManifest(
            prepared.project.registration,
            candidate.artifactRootHash,
            hashOf("agent"),
            test_support::policyArtifactBytes()
        );
        auto const pin = additionalSessionPin(
            prepared,
            "session-after-dispatch"
        );
        auto const refused = prepared.store.pinSession(
            pin,
            manifest,
            std::nullopt
        );
        REQUIRE_FALSE_MESSAGE(
            refused.has_value(),
            "an unanswered dispatch must refuse a new session pin"
        );
        CHECK_MESSAGE(
            refused.error().message().contains("dispatch in flight"),
            "the refusal must come from the dispatch quiescence guard"
        );
        CHECK(refused.error().message().contains(operation.operationId));

        auto const answered = prepared.store.recordDeliveryOutcome(
            prepared.lease,
            dispatch->operationRevision,
            host->deliverReport(dispatch->authority)
        );
        REQUIRE(answered.has_value());
        auto const accepted = prepared.store.pinSession(
            pin,
            manifest,
            std::nullopt
        );
        CHECK_MESSAGE(
            accepted.has_value(),
            "the same pin must succeed after its dispatch is answered: ",
            accepted.error().message()
        );
    }

    TEST_CASE("a compatible release upgrade freezes once and pins only the new session")
    {
        auto temporary     = TemporaryDirectory{};
        auto oldRoot       = std::optional<ContentHash>{};
        auto candidateRoot = std::optional<ContentHash>{};
        {
            auto prepared = prepareStore(temporary.path());
            auto const candidate = releaseWithModel(
                temporary.path() / "compatible-upgrade",
                "compatible candidate runtime model\n"
            );
            auto const manifest = sessionManifest(
                prepared.project.registration,
                candidate.artifactRootHash,
                hashOf("agent"),
                test_support::policyArtifactBytes()
            );
            auto const pin = additionalSessionPin(
                prepared,
                "session-compatible-upgrade"
            );
            oldRoot       = prepared.runtimeArtifactRootHash;
            candidateRoot = candidate.artifactRootHash;

            REQUIRE(prepared.store.upgradeRuntimeArtifactAndPinSession(
                installRequest(candidate, prepared.installedGeneration),
                pin,
                manifest,
                std::nullopt
            ).has_value());
            auto const active = prepared.store.activeRuntimeArtifactPin();
            REQUIRE(active.has_value());
            CHECK(active->installedGeneration == prepared.installedGeneration + 1U);
            CHECK(active->artifactRootHash == candidate.artifactRootHash);
        }

        REQUIRE(oldRoot.has_value());
        REQUIRE(candidateRoot.has_value());
        auto database = test_support::OperatorDatabaseProbe{
            temporary.path() / "production" / "operator-runtime.sqlite"
        };
        auto const expectedSessions = std::vector<std::vector<std::string>>{
            {"session-1", oldRoot->hex()},
            {"session-compatible-upgrade", candidateRoot->hex()},
        };
        CHECK_MESSAGE(
            database.readRows(
                "SELECT session_id, runtime_artifact_root_hash FROM sessions "
                "WHERE session_id IN ('session-1', 'session-compatible-upgrade') "
                "ORDER BY session_id"
            ) == expectedSessions,
            "the existing session must retain its old release while the new "
            "session pins the candidate"
        );
    }

    TEST_CASE("fault matrix migration failure rolls back the active release")
    {
        auto temporary     = TemporaryDirectory{};
        auto oldRoot       = std::optional<ContentHash>{};
        auto candidateRoot = std::optional<ContentHash>{};
        {
            auto prepared = prepareStore(temporary.path());
            auto const candidate = releaseWithModel(
                temporary.path() / "rollback-upgrade",
                "rollback candidate runtime model\n"
            );
            auto const pin = additionalSessionPin(
                prepared,
                "session-rollback-upgrade"
            );
            oldRoot       = prepared.runtimeArtifactRootHash;
            candidateRoot = candidate.artifactRootHash;

            auto const failed = prepared.store.upgradeRuntimeArtifactAndPinSession(
                installRequest(candidate, prepared.installedGeneration),
                pin,
                prepared.manifest,
                std::nullopt
            );
            REQUIRE_FALSE_MESSAGE(
                failed.has_value(),
                "the manifest mismatch must inject a failure after publication and before pin"
            );
            auto const active = prepared.store.activeRuntimeArtifactPin();
            REQUIRE_MESSAGE(
                active.has_value(),
                "failed migration must leave a decidable active release"
            );
            CHECK(active->installedGeneration == prepared.installedGeneration + 2U);
            CHECK_MESSAGE(
                active->artifactRootHash == prepared.runtimeArtifactRootHash,
                "failed migration must roll back to the previously pinned release"
            );
        }

        REQUIRE(oldRoot.has_value());
        REQUIRE(candidateRoot.has_value());
        auto database = test_support::OperatorDatabaseProbe{
            temporary.path() / "production" / "operator-runtime.sqlite"
        };
        CHECK(database.readRows(
            "SELECT session_id FROM sessions "
            "WHERE session_id='session-rollback-upgrade'"
        ).empty());
        auto const expectedFailure = std::vector<std::vector<std::string>>{
            {"2", candidateRoot->hex(), "3", oldRoot->hex()},
        };
        CHECK_MESSAGE(
            database.readRows(
                "SELECT attempted_generation, attempted_artifact_root_hash, "
                "restored_generation, restored_artifact_root_hash FROM runtime_upgrade_failures"
            ) == expectedFailure,
            "the audit chain must name the failed candidate and restored predecessor"
        );
    }

    TEST_CASE("capability expansion cannot pin before its approval evidence is recorded")
    {
        auto temporary     = TemporaryDirectory{};
        auto candidateRoot = std::optional<ContentHash>{};
        auto const evidenceHash = hashOf("human capability expansion approval");
        {
            auto prepared = prepareStore(temporary.path());
            auto const candidate = releaseWithModel(
                temporary.path() / "capability-upgrade",
                "capability candidate runtime model\n"
            );
            auto const manifest = sessionManifest(
                prepared.project.registration,
                candidate.artifactRootHash,
                hashOf("agent"),
                test_support::policyArtifactBytes()
            );
            auto pin = additionalSessionPin(
                prepared,
                "session-capability-upgrade"
            );
            pin.controllerCapabilities.emplace_back("release.expanded");
            candidateRoot = candidate.artifactRootHash;

            auto const refused = prepared.store.upgradeRuntimeArtifactAndPinSession(
                installRequest(candidate, prepared.installedGeneration),
                pin,
                manifest,
                std::nullopt
            );
            REQUIRE_FALSE_MESSAGE(
                refused.has_value(),
                "a capability expansion must be refused before approval"
            );
            CHECK_MESSAGE(
                refused.error().message().contains("capability expansion"),
                "the refusal must come from the capability-expansion approval guard"
            );
            CHECK(refused.error().message().contains("release.expanded"));

            REQUIRE(prepared.store.approveReleaseCapabilities(
                ReleaseCapabilityApproval{
                    .artifactRootHash       = candidate.artifactRootHash,
                    .controllerCapabilities = pin.controllerCapabilities,
                    .evidenceHash           = evidenceHash,
                }
            ).has_value());
            auto const rolledBack = prepared.store.activeRuntimeArtifactPin();
            REQUIRE(rolledBack.has_value());
            REQUIRE(prepared.store.upgradeRuntimeArtifactAndPinSession(
                installRequest(candidate, rolledBack->installedGeneration),
                pin,
                manifest,
                std::nullopt
            ).has_value());
        }

        REQUIRE(candidateRoot.has_value());
        auto database = test_support::OperatorDatabaseProbe{
            temporary.path() / "production" / "operator-runtime.sqlite"
        };
        auto const expectedApproval = std::vector<std::vector<std::string>>{
            {candidateRoot->hex(), evidenceHash.hex()},
        };
        CHECK_MESSAGE(
            database.readRows(
                "SELECT artifact_root_hash, evidence_hash "
                "FROM release_capability_approvals"
            ) == expectedApproval,
            "the accepted release must retain the exact approval evidence"
        );
    }

    // What the SessionManifest pin buys. A session row stores the manifest hash
    // it was pinned under, and a later pin of the same session is refused
    // unless it presents the same one. The manifest binds the plugin
    // environment (contract-state-s05), so a framework whose Luau bridge or
    // global whitelist moved mints a different hash for the same spec and every
    // session stored under the old one stops being re-pinnable -- which is the
    // whole reason the environment is in the manifest at all.
    TEST_CASE("a session stored under one manifest is refused under another")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // The stored session pins worldScope run("target-1", 1), so the
        // re-pin below must name the same immutable tuple column for the
        // manifest to be the only variable.
        auto const storedScope = ObservedInstanceWorldScope::run(
            "target-1",
            1
        );
        REQUIRE(storedScope.has_value());
        auto const samePin = SessionPin{
            .sessionId                 = "session-1",
            .authenticatedControllerId = "controller-1",
            .idempotencyNamespace      = "controller-1",
            .projectRegistrationHash   = prepared.project.registration.hash(),
            .controllerCapabilities    = {std::string{conformance::k_operateCapability}},
            .controlledTargetId        = "target-1",
            .projectInstanceKey        = "instance-1",
            .mode                      = SessionMode::Write,
            .kind                      = ControllerKind::Script,
            .worldScope                = *storedScope,
        };

        // The positive control: the stored session accepts its own manifest,
        // so the refusal below is about the manifest and not about re-pinning.
        auto const stored = sessionManifest(
            prepared.project.registration,
            prepared.runtimeArtifactRootHash,
            hashOf("agent"),
            test_support::policyArtifactBytes()
        );
        REQUIRE(prepared.store.pinSession(samePin, stored, std::nullopt).has_value());

        auto movedResult = SessionManifest::create(
            SessionManifestSpec{
                .runtimeModelArtifactRootHash = prepared.runtimeArtifactRootHash,
                .operatorProtocolSchemaHash   = hashOf("operator"),
                .projectRegistrationHash      = prepared.project.registration.hash(),
                .policyArtifactHash           = hashOf("a policy this session was not pinned to"),
                .agentProfileHash             = hashOf("agent"),
            }
        );
        REQUIRE(movedResult.has_value());
        auto const moved = *std::move(movedResult);
        REQUIRE(moved.hash() != stored.hash());
        auto const refused = prepared.store.pinSession(samePin, moved, std::nullopt);
        REQUIRE_FALSE(refused.has_value());
        CHECK(
            refused.error().message().contains(
                "already names a different immutable session tuple"
            )
        );
    }

    TEST_CASE(
        "pinSession names the registration and instance key it required when "
        "no ProjectInstance exists"
    )
    {
        auto temporary = TemporaryDirectory{};
        auto [store, artifactRootHash] = storeWithInstalledArtifact(temporary.path());

        auto const registrationHash = hashOf("registration-never-provisioned");
        auto const manifest =
            manifestNamingRegistration(artifactRootHash, registrationHash);
        auto const missingScope = ObservedInstanceWorldScope::run(
            "target-no-instance",
            1
        );
        REQUIRE(missingScope.has_value());

        auto const missingInstance = store.pinSession(
            SessionPin{
                .sessionId                 = "session-no-instance",
                .authenticatedControllerId = "controller-no-instance",
                .idempotencyNamespace      = "controller-no-instance",
                .projectRegistrationHash   = registrationHash,
                .controllerCapabilities    = {std::string{conformance::k_operateCapability}},
                .controlledTargetId        = "target-no-instance",
                .projectInstanceKey        = "instance-never-provisioned",
                .mode                      = SessionMode::Write,
                .kind                      = ControllerKind::Script,
                .worldScope                = *missingScope,
            },
            manifest,
            std::nullopt
        );
        REQUIRE_FALSE(missingInstance.has_value());
        CHECK(
            missingInstance.error().message().contains(
                "requires an existing ProjectInstance"
            )
        );
        CHECK(missingInstance.error().message().contains(registrationHash.hex()));
        CHECK(
            missingInstance.error().message().contains(
                "instance-never-provisioned"
            )
        );
    }

    TEST_CASE("Journal schema owner prevents caller-attached payload and provenance labels")
    {
        auto const project    = makeProject("fixture.alpha", k_pluginSource);
        auto const provenance = std::string{k_fixtureProvenance};
        auto const accepted = project.journalSchemaOwner.validate(
            "fixture.progress",
            canonical(project.schemaOwner, "{\"value\":1}"),
            canonical(project.schemaOwner, provenance)
        );
        REQUIRE(accepted.has_value());
        CHECK(accepted->projectRegistrationHash() == project.registration.hash());
        // The sha256 of the schema that judged the payload, which is what the
        // journal event schema manifest names for this event type. It used to
        // be hashOf("progress-schema") -- a hash of the word, from when no
        // schema stood behind it.
        CHECK(
            accepted->payloadSchemaHash() == hashOf(test_support::k_progressPayloadSchema)
        );

        CHECK_FALSE(project.journalSchemaOwner.validate(
            "fixture.progress",
            canonical(project.schemaOwner, "{\"value\":2}"),
            canonical(project.schemaOwner, provenance)
        ).has_value());
        CHECK_FALSE(project.journalSchemaOwner.validate(
            "fixture.unknown",
            canonical(project.schemaOwner, "{\"value\":99}"),
            canonical(project.schemaOwner, provenance)
        ).has_value());
        CHECK_FALSE(project.journalSchemaOwner.validate(
            "fixture.progress",
            canonical(project.schemaOwner, "{\"value\":1}"),
            canonical(
                project.schemaOwner,
                std::string{k_fixtureProvenanceViolations.front()}
            )
        ).has_value());
    }

    TEST_CASE("installation refuses a release manifest from a generation it cannot read")
    {
        // Both numbers are the deployment principal's half of a cross-boundary
        // agreement: the authoring side declares which generation of the
        // annotation contract and of the workspace database produced the
        // release, and this side decides whether it reads them. The refusal
        // has to name both, because a publisher told only "unsupported" cannot
        // tell which generation to move to -- and because a message naming
        // neither would let a comparison against the wrong constant pass.
        auto temporary = TemporaryDirectory{};
        auto const production = temporary.path() / "production";
        auto coordinator = OperatorCoordinator::open(production);
        REQUIRE(coordinator.has_value());

        SUBCASE("the workspace SQLite revision must be the one this build reads")
        {
            auto const supplied = detail::k_workspaceSqliteRevision + 1U;
            auto const release  = releaseWithFormats(
                temporary.path() / "wrong-sqlite",
                detail::k_annotationWorkspaceFormat,
                supplied
            );
            auto const refused =
                coordinator->installRuntimeArtifact(installRequest(release, 0U));
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                std::format("states workspace SQLite revision {}", supplied)
            ));
            CHECK(refused.error().message().contains(
                std::format(
                    "this Host reads revision {}",
                    detail::k_workspaceSqliteRevision
                )
            ));
        }

        SUBCASE("the annotation workspace format must be too")
        {
            auto const supplied = detail::k_annotationWorkspaceFormat + 1U;
            auto const release  = releaseWithFormats(
                temporary.path() / "wrong-annotation",
                supplied,
                detail::k_workspaceSqliteRevision
            );
            auto const refused =
                coordinator->installRuntimeArtifact(installRequest(release, 0U));
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                std::format("states annotation workspace format {}", supplied)
            ));
            CHECK(refused.error().message().contains(
                std::format(
                    "this Host reads format {}",
                    detail::k_annotationWorkspaceFormat
                )
            ));
        }

        SUBCASE("both at the generations this build reads install")
        {
            auto const release = releaseWithFormats(
                temporary.path() / "correct",
                detail::k_annotationWorkspaceFormat,
                detail::k_workspaceSqliteRevision
            );
            CHECK(
                coordinator->installRuntimeArtifact(installRequest(release, 0U))
                    .has_value()
            );
        }
    }

    TEST_CASE("a second coordinator is refused while the first holds the directory")
    {
        auto temporary = TemporaryDirectory{};
        auto const production = temporary.path() / "production";

        // Opening clears every control lease, deactivates every session and
        // drops every publication claim, on the reading that whatever those
        // rows describe died with its process. A second open against a live
        // coordinator would perform those three clears against state that is
        // still in use, so it has to be refused rather than serialized.
        auto first = OperatorCoordinator::open(production);
        REQUIRE(first.has_value());

        auto const second = OperatorCoordinator::open(production);
        CHECK_FALSE(second.has_value());

        // The refusal is ownership, not a permanent property of the directory:
        // closing the first coordinator releases it.
        first = fail(AutomationErrorKind::Cancelled, "closed");
        auto const reopened = OperatorCoordinator::open(production);
        CHECK(reopened.has_value());
    }

    TEST_CASE("ProjectInstance provisioning rejects Journal data from another registration")
    {
        auto temporary = TemporaryDirectory{};
        auto const release = test_support::runtimeRelease(temporary.path());
        auto coordinator = OperatorCoordinator::open(temporary.path() / "production");
        REQUIRE(coordinator.has_value());
        auto const installed = coordinator->installRuntimeArtifact(
            RuntimeArtifactInstallRequest{
                .handoffRoot                 = release.handoffRoot,
                .expectedReleaseManifestHash = release.releaseManifestHash,
                .expectedInstalledGeneration = 0U,
            }
        );
        REQUIRE(installed.has_value());

        auto const project = makeProject("fixture.alpha", k_pluginSource);
        auto const foreign = makeProject("fixture.foreign", "foreign-plugin-bytes");
        auto const plugin = loadPlugin(project, k_pluginSource);
        auto const manifest = sessionManifest(
            project.registration,
            installed->rootHash(),
            hashOf("agent"),
            test_support::policyArtifactBytes()
        );
        REQUIRE(coordinator->registerProject(project.registration).has_value());
        CHECK_FALSE(coordinator->provisionProjectInstance(
            project.registration,
            plugin,
            ProjectInstanceBaseline{
                .projectInstanceKey  = "instance-1",
                .eventId             = "baseline-1",
                .sessionManifestHash = manifest.hash(),
                .entry = journalEntry(
                    foreign,
                    foreign.registration.baselineEventType(),
                    "{\"kind\":\"baseline\"}"
                ),
            }
        ).has_value());
    }

    TEST_CASE("production RuntimeArtifact installation owns activation CAS")
    {
        auto temporary = TemporaryDirectory{};
        auto const release = test_support::runtimeRelease(temporary.path());
        auto coordinator = OperatorCoordinator::open(temporary.path() / "production");
        REQUIRE(coordinator.has_value());

        auto installed = coordinator->installRuntimeArtifact(
            RuntimeArtifactInstallRequest{
                .handoffRoot                 = release.handoffRoot,
                .expectedReleaseManifestHash = release.releaseManifestHash,
                .expectedInstalledGeneration = 0U,
            }
        );
        REQUIRE(installed.has_value());
        CHECK(installed->installedGeneration() == 1U);
        CHECK(installed->rootHash() == release.artifactRootHash);

        CHECK_FALSE(coordinator->installRuntimeArtifact(
            RuntimeArtifactInstallRequest{
                .handoffRoot                 = release.handoffRoot,
                .expectedReleaseManifestHash = release.releaseManifestHash,
                .expectedInstalledGeneration = 0U,
            }
        ).has_value());

        test_support::writeFile(
            release.handoffRoot / "runtime-artifact" / task::k_runtimeModelFileName,
            "authoring handoff changed"
        );
        auto reopened = coordinator->openInstalledRuntimeArtifact(
            1U,
            release.artifactRootHash
        );
        REQUIRE(reopened.has_value());
        CHECK(reopened->rootHash() == release.artifactRootHash);
    }

    // The three properties readInstalledRuntimeArtifact's declaration states, one
    // case each. They are here rather than beside the verb that calls it because
    // the guarantee belongs to the door: any second caller inherits it.
    TEST_CASE("the read-only door answers for a pin without writing a byte")
    {
        auto temporary = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        auto const release = test_support::runtimeRelease(temporary.path());

        // Scoped, so the coordinator's connection is closed and its WAL
        // checkpointed before the ledger is measured.
        {
            auto coordinator = OperatorCoordinator::open(production);
            REQUIRE(coordinator.has_value());
            REQUIRE(
                coordinator->installRuntimeArtifact(installRequest(release, 0U))
                    .has_value()
            );
        }
        auto const installed = ledgerBytes(databasePath);

        auto const artifact = OperatorCoordinator::readInstalledRuntimeArtifact(
            production,
            1U,
            release.artifactRootHash
        );
        REQUIRE(artifact.has_value());
        CHECK(artifact->installedGeneration() == 1U);
        CHECK(artifact->rootHash() == release.artifactRootHash);
        CHECK_MESSAGE(
            ledgerBytes(databasePath) == installed,
            "the read-only door wrote to the ledger its declaration says it only reads"
        );

        // A wrong pin is refused by the same query the coordinator's door uses,
        // and a refusal writes nothing either.
        CHECK_FALSE(OperatorCoordinator::readInstalledRuntimeArtifact(
            production,
            2U,
            release.artifactRootHash
        ).has_value());
        CHECK_MESSAGE(
            ledgerBytes(databasePath) == installed,
            "a refused read wrote to the ledger"
        );
    }

    TEST_CASE("the active read-only door derives the generation without writing")
    {
        auto temporary = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        auto const first = test_support::runtimeRelease(temporary.path() / "first");
        auto const second = releaseWithModel(
            temporary.path() / "second",
            "a different active page model\r\n"
        );
        {
            auto coordinator = OperatorCoordinator::open(production);
            REQUIRE(coordinator.has_value());
            REQUIRE(
                coordinator->installRuntimeArtifact(installRequest(first, 0U))
                    .has_value()
            );
            REQUIRE(
                coordinator->installRuntimeArtifact(installRequest(second, 1U))
                    .has_value()
            );
        }
        auto const installed = ledgerBytes(databasePath);

        auto const active = OperatorCoordinator::readActiveInstalledRuntimeArtifact(
            production,
            second.artifactRootHash
        );
        REQUIRE(active.has_value());
        CHECK(active->installedGeneration() == 2U);
        CHECK(active->rootHash() == second.artifactRootHash);
        CHECK_FALSE(OperatorCoordinator::readActiveInstalledRuntimeArtifact(
            production,
            first.artifactRootHash
        ).has_value());
        CHECK_MESSAGE(
            ledgerBytes(databasePath) == installed,
            "active installation selection must not mutate the Operator ledger"
        );
    }

    TEST_CASE("an open Coordinator selects its active compatible release internally")
    {
        auto temporary       = TemporaryDirectory{};
        auto const production = temporary.path() / "production";
        auto const release    = test_support::runtimeRelease(
            temporary.path() / "active"
        );
        auto coordinator = OperatorCoordinator::open(production);
        REQUIRE(coordinator.has_value());
        REQUIRE(
            coordinator->installRuntimeArtifact(installRequest(release, 0U))
                .has_value()
        );

        auto const active = coordinator->openActiveInstalledRuntimeArtifact(
            release.artifactRootHash
        );
        REQUIRE(active.has_value());
        CHECK(active->installedGeneration() == 1U);
        CHECK(active->rootHash() == release.artifactRootHash);
    }

    TEST_CASE("the read-only door bootstraps no Operator layout")
    {
        auto temporary = TemporaryDirectory{};
        auto const production = temporary.path() / "production";
        auto const release = test_support::runtimeRelease(temporary.path());

        auto const artifact = OperatorCoordinator::readInstalledRuntimeArtifact(
            production,
            1U,
            release.artifactRootHash
        );
        CHECK_FALSE(artifact.has_value());
        CHECK_MESSAGE(
            !std::filesystem::exists(production),
            "reading an Operator root that does not exist created one"
        );
    }

    TEST_CASE("the read-only door is refused while a coordinator holds the directory")
    {
        auto temporary = TemporaryDirectory{};
        auto const production = temporary.path() / "production";
        auto const release = test_support::runtimeRelease(temporary.path());

        auto coordinator = OperatorCoordinator::open(production);
        REQUIRE(coordinator.has_value());
        REQUIRE(
            coordinator->installRuntimeArtifact(installRequest(release, 0U)).has_value()
        );

        // claimExclusiveOwnership holds SQLite's lock for the connection's
        // lifetime, so this read cannot proceed beside a live coordinator. That
        // is the refusal the declaration promises, and it is the reason a
        // read-only door needs no lock of its own.
        CHECK_FALSE(OperatorCoordinator::readInstalledRuntimeArtifact(
            production,
            1U,
            release.artifactRootHash
        ).has_value());
    }

    TEST_CASE("reclamation removes a RuntimeArtifact directory nothing references")
    {
        auto temporary = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const artifactRoot = production / "runtime-artifacts";
        auto const installed = test_support::runtimeRelease(temporary.path() / "first");
        auto const orphan = releaseWithModel(
            temporary.path() / "second",
            "a different page model\r\n"
        );
        REQUIRE(installed.artifactRootHash != orphan.artifactRootHash);

        auto coordinator = OperatorCoordinator::open(production);
        REQUIRE(coordinator.has_value());
        REQUIRE(coordinator->installRuntimeArtifact(installRequest(installed, 0U)).has_value());

        // A-F8: the directory is published before the transaction, so losing
        // the generation CAS leaves it behind with nothing pointing at it.
        CHECK_FALSE(coordinator->installRuntimeArtifact(installRequest(orphan, 0U)).has_value());
        auto const orphanPath = artifactRoot / orphan.artifactRootHash.hex();
        REQUIRE(std::filesystem::is_directory(orphanPath));

        auto const reclaimed = coordinator->reclaimUnreferencedRuntimeArtifacts();
        REQUIRE(reclaimed.has_value());
        CHECK(reclaimed->artifactDirectories == 1U);
        CHECK_FALSE(std::filesystem::exists(orphanPath));
        CHECK(std::filesystem::is_directory(
            artifactRoot / installed.artifactRootHash.hex()
        ));
        CHECK(coordinator->openInstalledRuntimeArtifact(
            1U,
            installed.artifactRootHash
        ).has_value());

        // The reference set is the database's, so a second pass has nothing
        // left to decide about.
        auto const again = coordinator->reclaimUnreferencedRuntimeArtifacts();
        REQUIRE(again.has_value());
        CHECK(again->artifactDirectories == 0U);
    }

    TEST_CASE("reclamation keeps a RuntimeArtifact another publisher installed")
    {
        auto temporary = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const artifactRoot = production / "runtime-artifacts";
        auto const first = test_support::runtimeRelease(temporary.path() / "first");
        auto const second = releaseWithModel(
            temporary.path() / "second",
            "a different page model\r\n"
        );

        auto coordinator = OperatorCoordinator::open(production);
        REQUIRE(coordinator.has_value());
        REQUIRE(coordinator->installRuntimeArtifact(installRequest(first, 0U)).has_value());
        REQUIRE(coordinator->installRuntimeArtifact(installRequest(second, 1U)).has_value());

        // The A-F8 case: these bytes are already in place because another
        // publisher's installation won, so our own failed attempt is not
        // permission to remove their directory. The generation it belongs to is
        // no longer the active one, which is what keeps this case from being
        // decided by the active-root clause instead.
        CHECK_FALSE(coordinator->installRuntimeArtifact(installRequest(first, 0U)).has_value());

        auto const reclaimed = coordinator->reclaimUnreferencedRuntimeArtifacts();
        REQUIRE(reclaimed.has_value());
        CHECK(reclaimed->artifactDirectories == 0U);
        CHECK(std::filesystem::is_directory(artifactRoot / first.artifactRootHash.hex()));
        CHECK(std::filesystem::is_directory(artifactRoot / second.artifactRootHash.hex()));
        CHECK(coordinator->openInstalledRuntimeArtifact(1U, first.artifactRootHash).has_value());
        CHECK(coordinator->openInstalledRuntimeArtifact(2U, second.artifactRootHash).has_value());
    }

    TEST_CASE("reclamation removes staging directories no publication claims")
    {
        auto temporary = TemporaryDirectory{};
        auto const production = temporary.path() / "production";
        auto coordinator = OperatorCoordinator::open(production);
        REQUIRE(coordinator.has_value());

        // What a publisher that died between create_directory and rename leaves
        // behind. Nothing ever removed it before, because the staging token is
        // in no row and the filesystem cannot say whose it is.
        auto const staging = production / "runtime-artifacts" / ".staging" / "0123abcd";
        test_support::writeFile(staging / "runtime-model.toml", "half a deployment");

        auto const reclaimed = coordinator->reclaimUnreferencedRuntimeArtifacts();
        REQUIRE(reclaimed.has_value());
        CHECK(reclaimed->stagingDirectories == 1U);
        CHECK_FALSE(std::filesystem::exists(staging));
        CHECK(std::filesystem::is_directory(
            production / "runtime-artifacts" / ".staging"
        ));
    }

    TEST_CASE("reclamation refuses a tree with a link planted in it")
    {
        auto temporary = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const artifactRoot = production / "runtime-artifacts";
        auto const installed = test_support::runtimeRelease(temporary.path() / "first");
        auto const orphan = releaseWithModel(
            temporary.path() / "second",
            "a different page model\r\n"
        );

        auto coordinator = OperatorCoordinator::open(production);
        REQUIRE(coordinator.has_value());
        REQUIRE(coordinator->installRuntimeArtifact(installRequest(installed, 0U)).has_value());
        CHECK_FALSE(coordinator->installRuntimeArtifact(installRequest(orphan, 0U)).has_value());

        auto const outside = temporary.path() / "outside";
        std::filesystem::create_directories(outside);
        test_support::writeFile(outside / "canary.txt", "must survive");

        auto const orphanPath = artifactRoot / orphan.artifactRootHash.hex();
        auto const linked = linkDirectory(orphanPath / "assets", outside);
#if defined(_WIN32)
        // A junction needs no privilege here, so a failure is a broken test
        // rather than an unavailable feature.
        REQUIRE(linked);
#else
        if (!linked)
        {
            MESSAGE("this account cannot create a directory symlink");
            return;
        }
#endif

        // Everything about the row still says reclaimable; only the walk
        // refuses, and it refuses rather than unlinking through the link.
        CHECK_FALSE(coordinator->reclaimUnreferencedRuntimeArtifacts().has_value());
        CHECK(std::filesystem::is_regular_file(outside / "canary.txt"));
    }

    TEST_CASE("lease takeover advances fencing and invalidates stale snapshot creation")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const takeover = prepared.store.takeoverLease(prepared.controller, "human takeover");
        REQUIRE(takeover.has_value());
        CHECK(takeover->lease.fencingToken > prepared.lease.fencingToken);
        CHECK(takeover->resolvedDispatches == 0U);
        CHECK_FALSE(prepared.store.createSnapshot(
            prepared.lease,
            prepared.plugin,
            prepared.project.toolCatalogSchemaOwner,
            prepared.project.observedInstanceIdentitySchemas,
            conformance::observeOnce(prepared.observation)
        ).has_value());
    }

    TEST_CASE("commands are durable-idempotent and mutation chains are exclusive")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const request = command(prepared.snapshot, "request-1", "controller-1");
        auto first = prepared.store.submitCommand(
            prepared.controller,
            request,
            toolInvocation(prepared.project, "command-1")
        );
        REQUIRE(first.has_value());
        CHECK(first->operation.lookup == CommandLookup::Created);

        auto const repeated = prepared.store.submitCommand(
            prepared.controller,
            request,
            toolInvocation(prepared.project, "command-1")
        );
        REQUIRE(repeated.has_value());
        CHECK(repeated->operation.lookup == CommandLookup::Existing);
        CHECK(repeated->operation.operationId == first->operation.operationId);

        // Idempotency is by request identity and the fingerprint is over the
        // catalog's bytes, so one command submitted twice is one fingerprint.
        CHECK(repeated->commandFingerprint == first->commandFingerprint);

        // Same client_request_id, different tool: the stored fingerprint is
        // what decides, and it covers the tool the catalog named.
        CHECK_FALSE(prepared.store.submitCommand(
            prepared.controller,
            request,
            toolInvocation(prepared.project, "different-command")
        ).has_value());
        CHECK_FALSE(prepared.store.submitCommand(
            prepared.controller,
            command(prepared.snapshot, "request-2", "controller-1"),
            toolInvocation(prepared.project, "command-2")
        ).has_value());

        // A read-only tool takes no mutation chain, so it is admitted while the
        // mutating Operation above is still live.
        CHECK(prepared.store.submitCommand(
            prepared.controller,
            command(prepared.snapshot, "request-3", "controller-1"),
            toolInvocation(prepared.project, "observe-1")
        ).has_value());

        auto const cancelled = prepared.store.transitionOperation(
            first->operation.operationId,
            first->operation.revision,
            OperationSignal::Cancelled
        );
        REQUIRE(cancelled.has_value());
        CHECK(cancelled->state == OperationState::Cancelled);
        CHECK(prepared.store.submitCommand(
            prepared.controller,
            command(prepared.snapshot, "request-2", "controller-1"),
            toolInvocation(prepared.project, "command-2")
        ).has_value());
    }

    TEST_CASE("dispatch freezes once and every Host outcome enters reconciliation")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const proposed = proposedOperation(prepared, "request-1", "command-1");
        auto const frozen   = freezePlanFor(prepared, proposed);
        REQUIRE(frozen.has_value());
        auto const step = mintStepFor(prepared, frozen->operation);
        REQUIRE(step.has_value());
        auto const operation = step->operation;

        auto host           = deliveringHost(prepared);
        auto const dispatch = prepared.store.reserveDispatch(
            operation.operationId,
            operation.revision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"authority-1"},
            std::nullopt
        );
        REQUIRE(dispatch.has_value());
        CHECK(dispatch->authority.frozenPlanHash == frozen->planHash);

        // The one pending step is now linked to that dispatch, so a second
        // reservation finds none and refuses rather than freezing again.
        CHECK_FALSE(prepared.store.reserveDispatch(
            operation.operationId,
            dispatch->operationRevision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"authority-2"},
            std::nullopt
        ).has_value());

        // The engine exposes one Result for the whole delivery path, not a
        // phase result. A refused sink therefore stays transport_unknown: the
        // same result also covers a failure after the click reached the target,
        // so narrowing an engine error to not_delivered would claim too much.
        host->refuseClicks();
        auto const unknown = host->deliverReport(dispatch->authority);
        REQUIRE(unknown.outcome() == task::DeliveryOutcome::TransportUnknown);
        auto const reconciles = prepared.store.recordDeliveryOutcome(
            prepared.lease,
            dispatch->operationRevision,
            unknown
        );
        REQUIRE(reconciles.has_value());
        CHECK(reconciles->state == OperationState::Reconciling);
        CHECK(reconciles->planFrozen);
        auto const recovery = prepared.store.recoveredUncertainDispatches();
        REQUIRE(recovery.has_value());
        REQUIRE(recovery->size() == 1U);
        CHECK(recovery->front().operationId == operation.operationId);
        CHECK(recovery->front().deliveryReason == unknown.reason());
        CHECK_FALSE(prepared.store.recordDeliveryOutcome(
            prepared.lease,
            reconciles->revision,
            host->deliverReport(dispatch->authority)
        ).has_value());

        auto const foreign = makeProject(
            "fixture.foreign",
            "foreign-plugin-bytes"
        );
        CHECK_FALSE(prepared.store.commitReconciliation(
            prepared.plugin,
            ReconciliationCommit{
                .operationId = operation.operationId,
                .expectedOperationRevision = reconciles->revision,
                .expectedProjectStateRevision = 0U,
                .outcome                      = reconciliationOutcome(prepared, operation.operationId, "{\"disposition\":\"confirmed\"}"),
                .journalEvents = {
                    JournalAppend{
                        .eventId = "event-foreign",
                        .entry = journalEntry(
                            foreign,
                            "fixture.confirmed",
                            "{\"value\":1}"
                        ),
                    },
                },
            }
        ).has_value());

        auto const committed = prepared.store.commitReconciliation(
            prepared.plugin,
            ReconciliationCommit{
                .operationId = operation.operationId,
                .expectedOperationRevision = reconciles->revision,
                .expectedProjectStateRevision = 0U,
                .outcome                      = reconciliationOutcome(prepared, operation.operationId, "{\"disposition\":\"confirmed\"}"),
                .journalEvents = {
                    JournalAppend{
                        .eventId = "event-1",
                        .entry = journalEntry(
                            prepared.project,
                            "fixture.confirmed",
                            "{\"value\":1}"
                        ),
                    },
                },
            }
        );
        REQUIRE(committed.has_value());
        CHECK(committed->state == OperationState::Confirmed);
    }

    TEST_CASE("release resolves its unanswered dispatch before dropping authority")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const operation = createReadyOperation(
            prepared,
            "request-1",
            "command-1"
        );
        auto host           = deliveringHost(prepared);
        auto const reserved = prepared.store.reserveDispatch(
            operation.operationId,
            operation.revision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"authority-1"},
            std::nullopt
        );
        REQUIRE(reserved.has_value());

        auto const releasedFence = prepared.store.releaseLease(prepared.lease);
        REQUIRE(releasedFence.has_value());
        CHECK(*releasedFence > prepared.lease.fencingToken);

        auto const recovery = prepared.store.recoveredUncertainDispatches();
        REQUIRE(recovery.has_value());
        REQUIRE(recovery->size() == 1U);
        CHECK(recovery->front().operationId == operation.operationId);
        CHECK(
            recovery->front().deliveryReason
            == "lease release found this dispatch unanswered"
        );

        // The Host may still return after release, but the in-transaction
        // resolution already consumed the outcome CAS and the lease is gone.
        CHECK_FALSE(prepared.store.recordDeliveryOutcome(
            prepared.lease,
            reserved->operationRevision,
            host->deliverReport(reserved->authority)
        ).has_value());
    }

    TEST_CASE("fault matrix timeout never records an unreturned action as success")
    {
        auto temporary   = TemporaryDirectory{};
        auto operationId = std::string{};
        {
            auto prepared = prepareStore(temporary.path());
            auto const operation = createReadyOperation(
                prepared,
                "request-timeout",
                "command-1"
            );
            operationId = operation.operationId;
            auto host   = deliveringHost(prepared);
            auto const reserved = prepared.store.reserveDispatch(
                operation.operationId,
                operation.revision,
                prepared.lease,
                host->generation(),
                AuthorityDecisionId{"authority-timeout"},
                std::nullopt
            );
            REQUIRE(reserved.has_value());

            // The Host action lands, but the injected transport fault
            // suppresses its return before the coordinator can record it.
            auto const suppressed = host->deliverReport(reserved->authority);
            REQUIRE(suppressed.outcome() == task::DeliveryOutcome::Delivered);
            REQUIRE(host->clicks() == 1U);
            REQUIRE(prepared.store.releaseLease(prepared.lease).has_value());
        }

        auto const rows = test_support::OperatorDatabaseProbe{
            temporary.path() / "production" / "operator-runtime.sqlite"
        }.readRows(
            "SELECT delivery_outcome, coalesce(delivery_reason, '') FROM dispatches "
            "WHERE operation_id='" + operationId + "'"
        );
        REQUIRE(rows.size() == 1U);
        REQUIRE(rows.front().size() == 2U);
        CHECK_MESSAGE(
            rows.front()[0] == "transport_unknown",
            "an unreturned Host action must be delivery-uncertain, never success"
        );
        CHECK(rows.front()[1] == "lease release found this dispatch unanswered");
    }

    TEST_CASE("fault matrix tamper names the altered signed evidence file")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const original = test_support::agentProfileBytes(
            test_support::k_unconstrainedAgentBudget
        );
        auto const evidencePath = temporary.path() / "agent-profile.json";
        test_support::writeFile(evidencePath, original);

        auto changed = original;
        REQUIRE_FALSE(changed.empty());
        changed.back() = changed.back() == '}' ? ']' : '}';
        test_support::writeFile(evidencePath, changed);
        auto stream = std::ifstream{evidencePath, std::ios::binary};
        REQUIRE(stream.good());
        auto const alteredBytes = std::string{
            std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{}
        };
        auto const manifest = sessionManifest(
            prepared.project.registration,
            prepared.runtimeArtifactRootHash,
            hashOf(original),
            test_support::policyArtifactBytes()
        );
        auto const verified = AgentProfile::verifyExact(
            manifest,
            evidencePath,
            alteredBytes,
            test_support::agentProfileValidator()
        );
        REQUIRE_FALSE_MESSAGE(
            verified.has_value(),
            "verification must refuse after one signed-evidence byte changes"
        );
        CHECK_MESSAGE(
            verified.error().message().contains(evidencePath.string()),
            "signed-evidence refusal must name the altered file"
        );
    }

    TEST_CASE("fault matrix crash recovers a sent unrecorded mutation")
    {
        auto const root = std::filesystem::current_path();
        if (!std::filesystem::is_regular_file(root / "fault-root"))
        {
            return;
        }
        auto const verify = std::filesystem::is_regular_file(
            root / "verify-mode"
        );

        if (!verify)
        {
            auto prepared = prepareStore(root);
            auto const operation = createReadyOperation(
                prepared,
                "request-crash",
                "command-1"
            );
            auto host           = deliveringHost(prepared);
            auto const reserved = prepared.store.reserveDispatch(
                operation.operationId,
                operation.revision,
                prepared.lease,
                host->generation(),
                AuthorityDecisionId{"authority-crash"},
                std::nullopt
            );
            REQUIRE(reserved.has_value());
            auto const unrecorded = host->deliverReport(reserved->authority);
            REQUIRE(unrecorded.outcome() == task::DeliveryOutcome::Delivered);
            REQUIRE(host->clicks() == 1U);
            test_support::writeFile(root / "operation-id", operation.operationId);
            test_support::writeFile(root / "action-sent", "ready\n");

            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::hours{1});
            }
        }

        auto operationStream = std::ifstream{root / "operation-id", std::ios::binary};
        REQUIRE(operationStream.good());
        auto const operationId = std::string{
            std::istreambuf_iterator<char>{operationStream},
            std::istreambuf_iterator<char>{}
        };
        {
            auto reopened = OperatorCoordinator::open(root / "production");
            REQUIRE(reopened.has_value());
            auto const recovered = reopened->recoveredUncertainDispatches();
            REQUIRE(recovered.has_value());
            REQUIRE_MESSAGE(
                recovered->size() == 1U,
                "a sent but unrecorded mutation must restart explicitly uncertain"
            );
            CHECK(recovered->front().operationId == operationId);
            CHECK_MESSAGE(
                recovered->front().deliveryReason
                    == "operator restart found this dispatch unanswered",
                "a sent but unrecorded mutation must restart explicitly uncertain"
            );
        }

        auto const audit = test_support::OperatorDatabaseProbe{
            root / "production" / "operator-runtime.sqlite"
        }.readRows(
            "SELECT kind, coalesce(detail, '') FROM ledger_events "
            "WHERE subject_id='" + operationId + "' ORDER BY sequence"
        );
        CHECK(std::ranges::contains(
            audit,
            std::vector<std::string>{
                "delivery_outcome_recorded",
                "transport_unknown",
            }
        ));
        CHECK_MESSAGE(
            std::ranges::contains(
                audit,
                std::vector<std::string>{
                    "operation_state_changed",
                    "reconciling",
                }
            ),
            "restart recovery must preserve the audit sequence through reconciliation"
        );
        CHECK_FALSE(std::ranges::contains(
            audit,
            std::vector<std::string>{
                "delivery_outcome_recorded",
                "delivered",
            }
        ));
    }

    TEST_CASE("a restart preserves the uncertain dispatch release resolved")
    {
        auto temporary = TemporaryDirectory{};
        {
            auto prepared = prepareStore(temporary.path());
            auto const operation = createReadyOperation(
                prepared,
                "request-1",
                "command-1"
            );
            auto host           = deliveringHost(prepared);
            auto const reserved = prepared.store.reserveDispatch(
                operation.operationId,
                operation.revision,
                prepared.lease,
                host->generation(),
                AuthorityDecisionId{"authority-1"},
                std::nullopt
            );
            REQUIRE(reserved.has_value());
            REQUIRE(host->deliver(reserved->authority).has_value());
            REQUIRE(prepared.store.releaseLease(prepared.lease).has_value());
        }

        // Dropping the coordinator closes the database. The reopen sweep sees
        // the release-resolved row as answered and leaves its reconciliation
        // work intact.
        {
            auto restarted = OperatorCoordinator::open(
                temporary.path() / "production"
            );
            REQUIRE(restarted.has_value());
        }

        // Second restart: the sweep is idempotent because the dispatch it
        // resolved is no longer unanswered.
        auto again = OperatorCoordinator::open(temporary.path() / "production");
        REQUIRE(again.has_value());
    }

    TEST_CASE("ledger retention makes both subscription resync directions reachable")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto lease     = prepared.lease;

        // Each complete lease cycle appends release and acquire facts. The
        // retained stream holds 128 rows, so 65 cycles move its floor without
        // fabricating database rows outside the production write path.
        for (auto cycle = uint32{}; cycle < 65U; ++cycle)
        {
            REQUIRE(prepared.store.releaseLease(lease).has_value());
            auto acquired = prepared.store.acquireLease(prepared.controller);
            REQUIRE(acquired.has_value());
            lease = *std::move(acquired);
        }

        auto const behind = prepared.store.subscribe(
            prepared.controller,
            SubscriptionCursor{0U},
            1U
        );
        REQUIRE(behind.has_value());
        auto const* p_retainedGap = std::get_if<ResyncRequired>(&*behind);
        REQUIRE(p_retainedGap != nullptr);
        CHECK(p_retainedGap->oldestAvailableCursor.value > 0U);
        CHECK(p_retainedGap->requestedCursor.value == 0U);

        auto const aheadCursor = SubscriptionCursor{
            p_retainedGap->currentCursor.value + 1U,
        };
        auto const ahead = prepared.store.subscribe(
            prepared.controller,
            aheadCursor,
            1U
        );
        REQUIRE(ahead.has_value());
        auto const* p_foreignGap = std::get_if<ResyncRequired>(&*ahead);
        REQUIRE(p_foreignGap != nullptr);
        CHECK(p_foreignGap->requestedCursor == aheadCursor);
        CHECK(
            p_foreignGap->oldestAvailableCursor
            == p_retainedGap->oldestAvailableCursor
        );
    }

    TEST_CASE("snapshot retention preserves every retained observation join")
    {
        auto temporary        = TemporaryDirectory{};
        auto const production = temporary.path() / "production";
        auto const database   = production / "operator-runtime.sqlite";
        auto retained = [&temporary]()
        {
            auto prepared = prepareStore(temporary.path());
            return std::tuple{
                prepared.project,
                prepared.plugin,
                prepared.manifest,
                prepared.runtimeArtifactRootHash,
                prepared.installedGeneration,
            };
        }();
        auto const& [project, plugin, manifest, artifactRootHash, generation] = retained;

        {
            auto probe = test_support::OperatorDatabaseProbe{database};
            probe.execute(R"sql(
                WITH RECURSIVE revisions(value) AS (
                    SELECT 2
                    UNION ALL
                    SELECT value + 1 FROM revisions WHERE value < 41
                )
                INSERT INTO project_observations(
                    plugin_id, project_instance_key, revision,
                    project_registration_hash, state_resolution_hash,
                    project_state_revision, project_state_hash,
                    canonical_observation, observation_hash
                )
                SELECT observation.plugin_id, observation.project_instance_key,
                    revisions.value, observation.project_registration_hash,
                    observation.state_resolution_hash,
                    observation.project_state_revision,
                    observation.project_state_hash,
                    observation.canonical_observation,
                    observation.observation_hash
                FROM project_observations observation CROSS JOIN revisions
                WHERE observation.revision=1;
            )sql");
        }

        {
            auto reopened = OperatorCoordinator::open(production);
            REQUIRE(reopened.has_value());
            auto controller = reopened->resumeSession(
                SessionResume{
                    .authenticatedControllerId = "controller-1",
                    .controlledTargetId        = "target-1",
                    .mode                      = SessionMode::Write,
                    .kind                      = ControllerKind::Script,
                },
                manifest
            );
            REQUIRE(controller.has_value());
            auto lease = reopened->acquireLease(*controller);
            REQUIRE(lease.has_value());
            auto installed = reopened->openInstalledRuntimeArtifact(
                generation,
                artifactRootHash
            );
            REQUIRE(installed.has_value());
            auto observationHost = conformance::activateObservationHost(
                *std::move(installed),
                test_support::umbraflowProbeFrame(),
                FrameId{708}
            );
            auto snapshot = reopened->createSnapshot(
                *lease,
                plugin,
                project.toolCatalogSchemaOwner,
                project.observedInstanceIdentitySchemas,
                conformance::observeOnce(observationHost)
            );
            REQUIRE(snapshot.has_value());
        }

        auto probe = test_support::OperatorDatabaseProbe{database};
        auto const rows = probe.readRows(
            "SELECT (SELECT COUNT(*) FROM snapshots), "
            "(SELECT COUNT(*) FROM project_observations), "
            "(SELECT COUNT(*) FROM snapshots snapshot "
            "LEFT JOIN project_observations observation "
            "ON observation.plugin_id=snapshot.plugin_id "
            "AND observation.project_instance_key=snapshot.project_instance_key "
            "AND observation.revision=snapshot.project_observation_revision "
            "WHERE observation.revision IS NULL)"
        );
        REQUIRE(rows.size() == 1U);
        REQUIRE(rows.front().size() == 3U);
        CHECK(rows.front()[0] == "2");
        CHECK(rows.front()[1] == "33");
        CHECK(rows.front()[2] == "0");
    }

    TEST_CASE(
        "restart recovery reports actionable revisions after automatic session resume"
    )
    {
        auto temporary = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        auto retained = [&temporary]()
        {
            auto prepared = prepareStore(temporary.path());
            auto const operation = createReadyOperation(
                prepared,
                "request-restart-action",
                "command-1"
            );
            auto host = deliveringHost(prepared);
            auto const reserved = prepared.store.reserveDispatch(
                operation.operationId,
                operation.revision,
                prepared.lease,
                host->generation(),
                AuthorityDecisionId{"authority-restart-action"},
                std::nullopt
            );
            REQUIRE(reserved.has_value());
            REQUIRE(host->deliver(reserved->authority).has_value());
            REQUIRE(prepared.store.releaseLease(prepared.lease).has_value());
            return std::tuple{
                prepared.project,
                prepared.plugin,
                prepared.runtimeArtifactRootHash,
                operation.operationId,
            };
        }();
        auto const& [project, plugin, artifactRootHash, operationId] = retained;
        auto const beforeRead = ledgerBytes(databasePath);

        auto const artifact = OperatorCoordinator::readActiveInstalledRuntimeArtifact(
            production,
            artifactRootHash
        );
        REQUIRE(artifact.has_value());
        CHECK_MESSAGE(
            ledgerBytes(databasePath) == beforeRead,
            "read-only active selection must not consume restart recovery"
        );

        auto restarted = OperatorCoordinator::open(production);
        REQUIRE(restarted.has_value());
        auto recovered = restarted->recoveredUncertainDispatches();
        REQUIRE(recovered.has_value());
        REQUIRE(recovered->size() == 1U);
        CHECK(recovered->front().operationId == operationId);
        CHECK(recovered->front().expectedOperationRevision > 1U);
        CHECK(recovered->front().expectedProjectStateRevision == 0U);

        auto const manifest = sessionManifest(
            project.registration,
            artifactRootHash,
            hashOf("agent"),
            test_support::policyArtifactBytes()
        );
        auto const budgeted = restarted->resumeSession(
            SessionResume{
                .authenticatedControllerId = "controller-1",
                .controlledTargetId        = "target-1",
                .mode                      = SessionMode::Write,
                .kind                      = ControllerKind::Agent,
            },
            manifest
        );
        REQUIRE_FALSE(budgeted.has_value());
        CHECK_MESSAGE(
            budgeted.error().message().contains(
                "cannot resume across a process epoch"
            ),
            "budgeted Agent sessions must not resume across a process epoch"
        );

        auto controller = restarted->resumeSession(
            SessionResume{
                .authenticatedControllerId = "controller-1",
                .controlledTargetId        = "target-1",
                .mode                      = SessionMode::Write,
                .kind                      = ControllerKind::Script,
            },
            manifest
        );
        REQUIRE(controller.has_value());
        CHECK(controller->sessionId() == "session-1");
        auto lease = restarted->acquireLease(*controller);
        REQUIRE(lease.has_value());

        auto stillRecovered = restarted->recoveredUncertainDispatches();
        REQUIRE(stillRecovered.has_value());
        REQUIRE(stillRecovered->size() == 1U);
        CHECK(stillRecovered->front().operationId == operationId);
        auto const committed = restarted->commitReconciliation(
            plugin,
            ReconciliationCommit{
                .operationId = operationId,
                .expectedOperationRevision =
                    stillRecovered->front().expectedOperationRevision,
                .expectedProjectStateRevision =
                    stillRecovered->front().expectedProjectStateRevision,
                .outcome = test_support::reconcileOutcome(
                    project,
                    plugin,
                    operationId,
                    "{\"disposition\":\"ambiguous\"}"
                ),
                .journalEvents = {},
            }
        );
        REQUIRE(committed.has_value());
        CHECK(committed->state == OperationState::Ambiguous);
        auto const completed = restarted->recoveredUncertainDispatches();
        REQUIRE(completed.has_value());
        CHECK(completed->empty());
    }

    TEST_CASE("ambiguous prior sessions refuse automatic resume and remain readable")
    {
        auto temporary = TemporaryDirectory{};
        auto const production = temporary.path() / "production";
        auto retained = [&temporary]()
        {
            auto prepared = prepareStore(temporary.path());
            auto const manifest = sessionManifest(
                prepared.project.registration,
                prepared.runtimeArtifactRootHash,
                hashOf("agent"),
                test_support::policyArtifactBytes()
            );
            REQUIRE(prepared.store.provisionProjectInstance(
                prepared.project.registration,
                prepared.plugin,
                ProjectInstanceBaseline{
                    .projectInstanceKey  = "instance-ambiguous",
                    .eventId             = "baseline-ambiguous",
                    .sessionManifestHash = manifest.hash(),
                    .entry = journalEntry(
                        prepared.project,
                        prepared.project.registration.baselineEventType(),
                        "{\"kind\":\"baseline\"}"
                    ),
                }
            ).has_value());
            auto const ambiguousScope = ObservedInstanceWorldScope::run(
                "target-1",
                1
            );
            REQUIRE(ambiguousScope.has_value());
            REQUIRE(prepared.store.pinSession(
                SessionPin{
                    .sessionId                 = "session-ambiguous",
                    .authenticatedControllerId = "controller-1",
                    .idempotencyNamespace      = "controller-ambiguous",
                    .projectRegistrationHash =
                        prepared.project.registration.hash(),
                    .controllerCapabilities = {
                        std::string{conformance::k_operateCapability},
                    },
                    .controlledTargetId = "target-1",
                    .projectInstanceKey = "instance-ambiguous",
                    .mode               = SessionMode::Write,
                    .kind               = ControllerKind::Script,
                    .worldScope         = *ambiguousScope,
                },
                manifest,
                std::nullopt
            ).has_value());
            return std::pair{manifest, prepared.runtimeArtifactRootHash};
        }();
        auto const& [manifest, artifactRootHash] = retained;
        {
            auto restarted = OperatorCoordinator::open(production);
            REQUIRE(restarted.has_value());
            auto const resumed = restarted->resumeSession(
                SessionResume{
                    .authenticatedControllerId = "controller-1",
                    .controlledTargetId        = "target-1",
                    .mode                      = SessionMode::Write,
                    .kind                      = ControllerKind::Script,
                },
                manifest
            );
            REQUIRE_FALSE(resumed.has_value());
            CHECK_MESSAGE(
                resumed.error().message().contains("More than one most-recent"),
                "automatic resume must refuse equally recent prior sessions"
            );
        }

        CHECK(OperatorCoordinator::readActiveInstalledRuntimeArtifact(
            production,
            artifactRootHash
        ).has_value());
    }

    // Only not_delivered proves an external effect absent, and only a Host that
    // consumed its authorization and never called into the delivery path can
    // produce it. transport_unknown deliberately under-claims, so it must not
    // unlock the same conclusion.
    TEST_CASE("only a proven absence unlocks Rejected")
    {
        auto const rejectedFor = [](task::DeliveryOutcome outcome)
        {
            auto temporary = TemporaryDirectory{};
            auto prepared  = prepareStore(temporary.path());
            auto const operation = createReadyOperation(
                prepared,
                "request-1",
                "command-1"
            );
            auto host           = deliveringHost(prepared);
            auto const dispatch = prepared.store.reserveDispatch(
                operation.operationId,
                operation.revision,
                prepared.lease,
                host->generation(),
                AuthorityDecisionId{"authority-1"},
                std::nullopt
            );
            REQUIRE(dispatch.has_value());
            if (outcome == task::DeliveryOutcome::TransportUnknown)
            {
                host->refuseClicks();
            }
            auto const report = outcome == task::DeliveryOutcome::NotDelivered
                ? host->deliverIntoAnotherCycle(dispatch->authority)
                : host->deliverReport(dispatch->authority);
            REQUIRE(report.outcome() == outcome);
            auto const reconciling = prepared.store.recordDeliveryOutcome(
                prepared.lease,
                dispatch->operationRevision,
                report
            );
            REQUIRE(reconciling.has_value());
            CHECK(host->clicks() == 0U);

            return prepared.store.commitReconciliation(
                prepared.plugin,
                ReconciliationCommit{
                    .operationId                  = reconciling->operationId,
                    .expectedOperationRevision    = reconciling->revision,
                    .expectedProjectStateRevision = 0U,
                    .outcome                      = reconciliationOutcome(
                        prepared,
                        reconciling->operationId,
                        "{\"disposition\":\"rejected\"}"
                    ),
                    .journalEvents                = {},
                }
            ).has_value();
        };

        CHECK(rejectedFor(task::DeliveryOutcome::NotDelivered));
        CHECK_FALSE(rejectedFor(task::DeliveryOutcome::TransportUnknown));
    }

    namespace
    {
        // Drives one command all the way to Reconciling, which is the only
        // state commitReconciliation accepts.
        [[nodiscard]]
        auto reconcilingOperation(
            PreparedStore& prepared,
            std::string_view clientRequestId,
            std::string_view toolName
        ) -> StoredOperation
        {
            // Every dispatch needs its own authority decision id, so it is
            // derived from the request rather than fixed.
            auto const authority = AuthorityDecisionId{
                std::format("authority-{}", clientRequestId),
            };
            auto const operation = createReadyOperation(
                prepared,
                std::string{clientRequestId},
                toolName
            );
            auto host           = deliveringHost(prepared);
            auto const dispatch = prepared.store.reserveDispatch(
                operation.operationId,
                operation.revision,
                prepared.lease,
                host->generation(),
                authority,
                std::nullopt
            );
            REQUIRE(dispatch.has_value());
            auto reconciles = prepared.store.recordDeliveryOutcome(
                prepared.lease,
                dispatch->operationRevision,
                host->deliverReport(dispatch->authority)
            );
            REQUIRE(reconciles.has_value());
            REQUIRE(reconciles->state == OperationState::Reconciling);
            return *reconciles;
        }

        [[nodiscard]]
        auto confirmedCommit(
            PreparedStore const& prepared,
            StoredOperation const& operation,
            uint64 expectedProjectStateRevision,
            std::string eventId,
            std::string payload
        ) -> ReconciliationCommit
        {
            return ReconciliationCommit{
                .operationId                  = operation.operationId,
                .expectedOperationRevision    = operation.revision,
                .expectedProjectStateRevision = expectedProjectStateRevision,
                .outcome                      = reconciliationOutcome(prepared, operation.operationId, "{\"disposition\":\"confirmed\"}"),
                .journalEvents = {
                    JournalAppend{
                        .eventId = std::move(eventId),
                        .entry   = journalEntry(
                            prepared.project,
                            "fixture.confirmed",
                            std::move(payload)
                        ),
                    },
                },
            };
        }
    }

    TEST_CASE("the reducer is handed exactly the Journal prefix that is appended")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // The baseline reduces its own creation event against no prior state.
        CHECK(
            prepared.project.documentInputLog->lastReduceInput()
            == "{\"journal_events\":[{\"namespaced_event_type\":\"fixture.baseline\","
               "\"opaque_project_payload\":{\"kind\":\"baseline\"},"
               "\"provenance\":" + std::string{k_fixtureProvenance} + "}],"
               "\"prior_project_state\":null}"
        );

        auto const operation = reconcilingOperation(prepared, "request-1", "command-1");
        REQUIRE(prepared.store.commitReconciliation(
            prepared.plugin,
            confirmedCommit(prepared, operation, 0U, "event-1", "{\"value\":1}")
        ).has_value());

        // The envelope is a function of the appended events and the stored
        // state, so a caller that wanted the reducer to see something else has
        // nowhere to put it: the payload below is the one the Journal recorded.
        CHECK(
            prepared.project.documentInputLog->lastReduceInput()
            == "{\"journal_events\":[{\"namespaced_event_type\":\"fixture.confirmed\","
               "\"opaque_project_payload\":{\"value\":1},"
               "\"provenance\":" + std::string{k_fixtureProvenance} + "}],"
               "\"prior_project_state\":{\"revision\":0}}"
        );
    }

    TEST_CASE("the snapshot project-state conjunction rejects a stale composition")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const operation = reconcilingOperation(
            prepared,
            "request-1",
            "command-1"
        );
        REQUIRE(prepared.store.commitReconciliation(
            prepared.plugin,
            confirmedCommit(
                prepared,
                operation,
                0U,
                "event-1",
                "{\"value\":1}"
            )
        ).has_value());

        CHECK_FALSE_MESSAGE(
            prepared.store.submitCommand(
                prepared.controller,
                command(prepared.snapshot, "request-stale-snapshot", "controller-1"),
                toolInvocation(prepared.project, "command-2")
            ).has_value(),
            "both project-state clauses together must reject the stale snapshot"
        );
    }

    TEST_CASE("a reconciliation that fails after opening its transaction writes nothing")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path(), rejectedReducePluginSource());
        auto const operation = reconcilingOperation(prepared, "request-1", "command-1");

        // The reducer runs inside the transaction, so this fails after the
        // Journal insert would already have been prepared.
        CHECK_FALSE(prepared.store.commitReconciliation(
            prepared.plugin,
            confirmedCommit(prepared, operation, 0U, "event-1", "{\"value\":1}")
        ).has_value());

        // Both halves of "nothing was written" are observable: the same
        // event_id is still free, and the ProjectState is still at revision 0.
        auto const retried = prepared.store.commitReconciliation(
            prepared.plugin,
            confirmedCommit(prepared, operation, 0U, "event-1", "{\"value\":1}")
        );
        CHECK_FALSE(retried.has_value());
        CHECK(prepared.store.commitReconciliation(
            prepared.plugin,
            confirmedCommit(prepared, operation, 1U, "event-2", "{\"value\":1}")
        ).has_value() == false);
    }

    TEST_CASE("Rejected and Ambiguous reconciliations cannot append Journal events")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const operation = reconcilingOperation(prepared, "request-1", "command-1");

        for (auto const document : {
                 std::string_view{"{\"disposition\":\"rejected\"}"},
                 std::string_view{"{\"disposition\":\"ambiguous\"}"},
             })
        {
            auto commit    = confirmedCommit(prepared, operation, 0U, "event-1", "{\"value\":1}");
            commit.outcome = reconciliationOutcome(
                prepared,
                operation.operationId,
                std::string{document}
            );
            CHECK_FALSE(
                prepared.store.commitReconciliation(prepared.plugin, commit).has_value()
            );
        }

        // Diverged is the mirror image: it may not claim a correction without
        // recording one.
        auto empty          = confirmedCommit(prepared, operation, 0U, "event-1", "{\"value\":1}");
        empty.outcome = reconciliationOutcome(
            prepared,
            operation.operationId,
            "{\"disposition\":\"diverged\"}"
        );
        empty.journalEvents = {};
        CHECK_FALSE(prepared.store.commitReconciliation(prepared.plugin, empty).has_value());
    }

    TEST_CASE("ApprovalToken is operation-bound and single-use")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // The awaiting state is reached by freezing a plan whose derived risk
        // requires an approval; no caller can ask for it.
        auto const proposed = proposedOperation(prepared, "request-1", "approval-plan");
        auto const frozen   = freezePlanFor(prepared, proposed);
        REQUIRE(frozen.has_value());
        REQUIRE_FALSE(frozen->requiredApprovals.empty());
        auto const step = mintStepFor(prepared, frozen->operation);
        REQUIRE(step.has_value());

        auto const request = ApprovalRequest{
            .operationId       = proposed.operationId,
            .lease             = prepared.lease,
            .approverPrincipal = "human-1",
            .approverCapability  = frozen->requiredApprovals.front(),
            .expiresAtUnixMillis = 4'000'000'000'000U,
        };
        auto const approval = prepared.store.issueApproval(
            request,
            AuthorityDecisionId{"human-decision-1"}
        );
        REQUIRE(approval.has_value());
        auto host           = deliveringHost(prepared);
        auto const dispatch = prepared.store.reserveDispatch(
            proposed.operationId,
            step->operation.revision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"dispatch-authority-1"},
            *approval
        );
        REQUIRE(dispatch.has_value());

        // Recording an outcome moves no fence, which is what leaves the token
        // below differing from a usable one in the step it names and nothing
        // else. Resolving through a takeover would refuse it for its fence
        // instead, and the case would pass with the step binding removed.
        auto reconciles = prepared.store.recordDeliveryOutcome(
            prepared.lease,
            dispatch->operationRevision,
            host->deliverIntoAnotherCycle(dispatch->authority)
        );
        REQUIRE(reconciles.has_value());
        CHECK(host->clicks() == 0U);
        auto const waiting = mintStepFor(prepared, *reconciles);
        REQUIRE(waiting.has_value());
        REQUIRE(waiting->operation.state == OperationState::AwaitingApproval);

        // Single use: the token the first dispatch consumed does not answer for
        // the step that replaced it.
        CHECK_FALSE(prepared.store.reserveDispatch(
            proposed.operationId,
            waiting->operation.revision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"dispatch-authority-2"},
            *approval
        ).has_value());
    }
    TEST_CASE("plan authority is bound to its registration")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // A second registration of the same shape, complete enough to build an
        // authority of its own. Authority is per registration, so this one must
        // not be able to freeze the first one's Operation.
        auto const foreignSource   = test_support::pluginSource("fixture.foreign");
        auto const foreign         = makeProject("fixture.foreign", foreignSource);

        // The same RuntimeArtifact the prepared session pinned. An authority can
        // no longer be built against an artifact root nobody installed, so the
        // only difference left between the two authorities is the registration
        // -- which is the one this case is about.
        auto const foreignManifest = sessionManifest(
            foreign.registration,
            prepared.runtimeArtifactRootHash,
            hashOf("agent"),
            test_support::policyArtifactBytes()
        );
        auto runtimeModel = prepared.observation.host->runtimeModelBinding(
            prepared.observation.generation
        );
        REQUIRE(runtimeModel.has_value());
        auto foreignAuthority = conformance::planAuthority(
            foreign.registration,
            foreignManifest,
            *runtimeModel,
            "operator",
            test_support::policyArtifactBytes(),
            test_support::k_fixtureUiAction
        );
        REQUIRE(foreignAuthority.has_value());

        auto const proposed = proposedOperation(prepared, "request-1", "command-1");
        CHECK_FALSE(prepared.store.freezePlan(
            proposed.operationId,
            proposed.revision,
            prepared.lease,
            prepared.plugin,
            prepared.project.toolCatalogSchemaOwner,
            *foreignAuthority
        ).has_value());
        CHECK(freezePlanFor(prepared, proposed).has_value());
    }

    // A plan's surface_id and action_id reach no Host: the Receipt's intent is
    // minted by the trusted chunk out of the model, and task::DispatchAuthority
    // carries no UI identifier. Until the step check below existed they were
    // decoration, and a plan could name UI that exists in no RuntimeModel and
    // still be dispatched. ui_target_id is deliberately absent from the pair:
    // since U2b it is the minted observed instance id, whose judge is the
    // observation gate in mintNextStep and not the model vocabulary.
    TEST_CASE("a step naming UI the installed RuntimeModel does not define is refused")
    {
        struct UndefinedUi final
        {
            std::string_view member{};
            std::string_view spelled{};
            std::string_view replacement{};
        };
        auto const undefined = std::array{
            UndefinedUi{
                "surface_id",
                R"(surface_id = "fixture.surface")",
                R"(surface_id = "fixture.absent")",
            },
            UndefinedUi{
                "action_id",
                R"(action_id = "fixture.press")",
                R"(action_id = "fixture.absent")",
            },
        };
        for (auto const& named : undefined)
        {
            CAPTURE(named.member);
            auto temporary = TemporaryDirectory{};
            auto const source = pluginNamingUndefinedUi(
                named.spelled,
                named.replacement
            );
            auto prepared  = prepareStore(temporary.path(), source);
            auto authority = deploymentAuthority(
                prepared,
                prepared.runtimeArtifactRootHash
            );
            REQUIRE(authority.has_value());
            CHECK_FALSE(mintStepUnder(prepared, *authority).has_value());
        }
    }

    // The positive control the two refusals above are worthless without: the
    // identical route, the identical authority, and the fixture's own plan,
    // whose surface and action this project's model does define.
    TEST_CASE("a step naming UI the installed RuntimeModel defines is minted")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto authority = deploymentAuthority(
            prepared,
            prepared.runtimeArtifactRootHash
        );
        REQUIRE(authority.has_value());

        auto const step = mintStepUnder(prepared, *authority);
        REQUIRE(step.has_value());
        CHECK(step->kind == StepKind::UiAction);
    }

    TEST_CASE("a plan authority answers only for the pinned RuntimeArtifact")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // The Host parsed the installed artifact; this manifest pins another.
        // Without the refusal an authority could carry one project's declared
        // vocabulary into a session pinned to a different model.
        CHECK_FALSE(
            deploymentAuthority(prepared, hashOf("another-artifact")).has_value()
        );
        CHECK(
            deploymentAuthority(prepared, prepared.runtimeArtifactRootHash)
                .has_value()
        );
    }

    // What binding the authority at creation does not close: a manifest is not a
    // session, and two manifests of one registration may pin two RuntimeArtifacts.
    // The authority below is honestly built -- its manifest and its model agree --
    // and still answers for a model this Operation's session row does not name,
    // so only the ledger's own column can refuse it.
    TEST_CASE("a plan authority for another installed artifact cannot mint a step")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // A second model differing only by one further scene, so its declared
        // vocabulary still contains every identifier the plan names and the
        // artifact root is the only thing left that can decide.
        auto const second = conformance::observationRelease(
            temporary.path() / "second",
            conformance::ProjectRuntimeArtifact{
                .model  = test_support::ambiguousRuntimeModel(),
                .assets = test_support::umbraflowRuntimeAssets(),
            }
        );
        auto installed = prepared.store.installRuntimeArtifact(
            installRequest(second, 1U)
        );
        REQUIRE(installed.has_value());
        auto const secondRootHash = installed->rootHash();
        REQUIRE(secondRootHash != prepared.runtimeArtifactRootHash);

        auto secondHost = conformance::activateObservationHost(
            *std::move(installed),
            test_support::umbraflowProbeFrame(),
            FrameId{909}
        );
        auto secondModel = secondHost.host->runtimeModelBinding(
            secondHost.generation
        );
        REQUIRE(secondModel.has_value());

        auto authority = OperatorPlanAuthority::create(
            prepared.project.registration,
            sessionManifest(
                prepared.project.registration,
                secondRootHash,
                hashOf("agent"),
                test_support::policyArtifactBytes()
            ),
            *secondModel,
            "operator",
            test_support::policyArtifactBytes(),
            deployment::readPlanProposal,
            deployment::readStepIntent
        );
        REQUIRE(authority.has_value());
        CHECK_FALSE(mintStepUnder(prepared, *authority).has_value());
    }

    TEST_CASE("the plugin cannot widen the workflow bound")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // The bound is this tool's own descriptor and not a number compiled in
        // beside it, so the case reads the catalog rather than a constant: a
        // catalog that raised the tool's ceiling would raise this expectation
        // with it, which is what makes tool_catalog_hash the single authority.
        auto const declared =
            prepared.project.toolCatalogSchemaOwner.describe("oversized-plan");
        REQUIRE(declared.has_value());

        auto const proposed = proposedOperation(prepared, "request-1", "oversized-plan");
        auto const frozen   = freezePlanFor(prepared, proposed);
        REQUIRE(frozen.has_value());

        // Every bound is a minimum against the descriptor, so widening is
        // arithmetically impossible rather than policy-checked. The proposal
        // asks for far more than the descriptor allows on the first two.
        CHECK(frozen->limits.maximumSteps == declared->limits.maximumSteps);
        CHECK(frozen->limits.maximumDispatches == declared->limits.maximumDispatches);
        CHECK(frozen->limits.maximumObservations <= declared->limits.maximumObservations);
        CHECK(frozen->limits.maximumWaits <= declared->limits.maximumWaits);
        CHECK(frozen->limits.maximumElapsedMillis <= declared->limits.maximumElapsedMillis);
    }

    TEST_CASE("a step cannot be replayed at another index")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // The plugin answers next_step with the identical document every time,
        // so the two steps differ in nothing except the position they were
        // minted at.
        auto const proposed = proposedOperation(prepared, "request-1", "command-1");
        auto const frozen   = freezePlanFor(prepared, proposed);
        REQUIRE(frozen.has_value());
        auto const first = mintStepFor(prepared, frozen->operation);
        REQUIRE(first.has_value());

        auto host           = deliveringHost(prepared);
        auto const dispatch = prepared.store.reserveDispatch(
            proposed.operationId,
            first->operation.revision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"authority-1"},
            std::nullopt
        );
        REQUIRE(dispatch.has_value());
        auto const reconciling = prepared.store.recordDeliveryOutcome(
            prepared.lease,
            dispatch->operationRevision,
            host->deliverReport(dispatch->authority)
        );
        REQUIRE(reconciling.has_value());

        auto const second = mintStepFor(prepared, *reconciling);
        REQUIRE(second.has_value());
        CHECK(second->stepKey == first->stepKey);
        CHECK(second->stepIndex == first->stepIndex + 1U);
        CHECK(second->stepIntentHash != first->stepIntentHash);
    }

    TEST_CASE("only one step may await dispatch")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        auto const proposed = proposedOperation(prepared, "request-1", "command-1");
        auto const frozen   = freezePlanFor(prepared, proposed);
        REQUIRE(frozen.has_value());
        auto const first = mintStepFor(prepared, frozen->operation);
        REQUIRE(first.has_value());

        // The check lives in mintNextStep alone. A partial unique index saying
        // the same thing would keep this green after the check was deleted.
        CHECK_FALSE(mintStepFor(prepared, first->operation).has_value());
    }

    TEST_CASE("a plan freezes once")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        auto const proposed = proposedOperation(prepared, "request-1", "command-1");
        auto const frozen   = freezePlanFor(prepared, proposed);
        REQUIRE(frozen.has_value());
        CHECK_FALSE(freezePlanFor(prepared, frozen->operation).has_value());

        // The stored plan is the first one: the dispatch still reports its
        // hash, so a second freeze did not replace the row underneath it.
        auto const step = mintStepFor(prepared, frozen->operation);
        REQUIRE(step.has_value());
        auto host           = deliveringHost(prepared);
        auto const dispatch = prepared.store.reserveDispatch(
            proposed.operationId,
            step->operation.revision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"authority-1"},
            std::nullopt
        );
        REQUIRE(dispatch.has_value());
        CHECK(dispatch->authority.frozenPlanHash == frozen->planHash);
    }

    TEST_CASE("read-only Operations get no plan")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        auto const readOnly = proposedOperation(prepared, "request-1", "observe-1");
        CHECK_FALSE(freezePlanFor(prepared, readOnly).has_value());
    }

    TEST_CASE("the effect envelope is order-independent")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        auto const first = proposedOperation(prepared, "request-1", "command-1");
        auto const one   = freezePlanFor(prepared, first);
        REQUIRE(one.has_value());

        // The mutation chain is per target, so the first Operation is retired
        // before the second is opened. `reordered-effects` declares the same
        // effect set in the opposite order and nothing else different.
        REQUIRE(prepared.store.transitionOperation(
            first.operationId,
            one->operation.revision,
            OperationSignal::Cancelled
        ).has_value());

        auto const second = proposedOperation(prepared, "request-2", "reordered-effects");
        auto const other  = freezePlanFor(prepared, second);
        REQUIRE(other.has_value());

        CHECK(other->effectEnvelopeHash == one->effectEnvelopeHash);
        CHECK(other->risk == one->risk);

        // The plans themselves still differ: the command is part of the plan.
        CHECK(other->planHash != one->planHash);
    }

    TEST_CASE("the dispatch records the frozen basis")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        auto const proposed = proposedOperation(prepared, "request-1", "command-1");
        auto const frozen   = freezePlanFor(prepared, proposed);
        REQUIRE(frozen.has_value());
        auto const step = mintStepFor(prepared, frozen->operation);
        REQUIRE(step.has_value());

        auto host           = deliveringHost(prepared);
        auto const dispatch = prepared.store.reserveDispatch(
            proposed.operationId,
            step->operation.revision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"authority-1"},
            std::nullopt
        );
        REQUIRE(dispatch.has_value());

        // None of the three was the caller's to say, and each is the value the
        // ledger derived rather than any other hash it holds.
        CHECK(dispatch->decisionBasisHash == frozen->decisionBasisHash);
        CHECK(dispatch->authority.frozenPlanHash == frozen->planHash);
        CHECK(dispatch->stepIntentHash == step->stepIntentHash);
        CHECK(dispatch->decisionBasisHash != frozen->planHash);
    }

    TEST_CASE("the caller cannot choose approval")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        auto const proposed = proposedOperation(prepared, "request-1", "approval-plan");
        auto const frozen   = freezePlanFor(prepared, proposed);
        REQUIRE(frozen.has_value());

        // The derived risk decided the edge. OperationSignal carries no
        // ReadyWithoutApproval, so no caller could have taken the other one.
        CHECK(frozen->risk == Risk::High);
        CHECK(
            frozen->requiredApprovals
            == std::vector<std::string>{
                std::string{conformance::k_approveCapability},
            }
        );
        CHECK(frozen->operation.state == OperationState::AwaitingApproval);

        auto const step = mintStepFor(prepared, frozen->operation);
        REQUIRE(step.has_value());
        auto host = deliveringHost(prepared);
        CHECK_FALSE(prepared.store.reserveDispatch(
            proposed.operationId,
            step->operation.revision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"authority-1"},
            std::nullopt
        ).has_value());
    }
}
