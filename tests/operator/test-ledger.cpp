// What the Operator's own ledger owns: the production database, RuntimeArtifact
// installation and reclamation, and the exact schema identity every stored
// generation of it is admitted under. The properties a project's registration
// decides -- catalog mutability, schema-owner binding, who owns a disposition
// -- are the exported conformance suite's, because a consuming repository
// proves them against its own project; see conformance/source/. No property is
// asserted in both places.

#include <operator/ledger.hpp>
#include <operator/manifest.hpp>
#include <operator/tool-admission-request.hpp>

#include "project-fixture.hpp"
#include "tool-call-fixture.hpp"
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
        using test_support::toolCallAt;

        // The Operator schema identity a fresh database is created with, and
        // therefore the identity every registered migration below must land
        // on. It is one constant rather than a copy per fixture because it
        // moves whenever the stored DDL does, and sixteen hand-copies were
        // sixteen places to forget.
        constexpr auto k_targetSchemaIdentity = std::string_view{
            "sha256:181f202e9d7516dff603a006dfabf4fef372a0413710e2bb23ad9cb75dc1bc12"
        };

        // The identity the generation immediately before genesis_transitions
        // created, and the source of its pair. A root written by that
        // generation is what every Operator root on disk today is, and it also
        // pins the genesis RuntimeArtifact of the format era that generation
        // belonged to.
        constexpr auto k_genesisTransitionsSourceIdentity = std::string_view{
            "sha256:f6a8064ca9b4d6fb0cfdce68e3d99f8e3cfd9e507183366f0d313458146afe77"
        };

        // The identity the generation immediately before the genesis
        // generation created, and the source of its pair. A root written by
        // that generation is what every Operator root on disk today is.
        constexpr auto k_genesisGenerationSourceIdentity = std::string_view{
            "sha256:5ed5e558e04f24347a97a0de03550eaa02f6e48ddb64c29bf09e05c50e55d194"
        };

        // The identity the generation immediately before the state cut
        // created, and the source of that cut's own pair.
        constexpr auto k_projectStateInterpretationIdentity = std::string_view{
            "sha256:9cd2477518ee53c63c0412fe95202cde66011d3226f3243b8266119f7dbc4d76"
        };

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

        // Winds a fresh database back past the generation that gave a run its
        // own durable state and retired the `rejected` Tool call state. Both
        // tables are rebuilt into their exact prior DDL TEXT, pasted rather
        // than derived for the reason restoreOperationDispatchSchema states:
        // the historical identity IS that text, so one changed byte reproduces
        // a schema that generation never had.
        //
        // Rows are carried through an untyped copy rather than dropped: every
        // migration fixture below reads something back across its upgrade, and
        // a wind-back that emptied these tables would prove the migration
        // preserved nothing.
        auto restorePriorToolRunSchema(
            test_support::OperatorDatabaseProbe& database
        ) -> void
        {
            database.execute(
                // Both tables are foreign-key targets, so they are rebuilt
                // rather than altered and the enforcement is lifted for the
                // rebuild exactly as the migration itself defers it.
                "PRAGMA foreign_keys=OFF;"
                "CREATE TABLE carried_tool_root_requests("
                "root_identity TEXT PRIMARY KEY,caller_namespace TEXT NOT NULL,"
                "request_key TEXT NOT NULL,request_preimage TEXT NOT NULL,"
                "request_preimage_hash TEXT NOT NULL) STRICT;"
                "INSERT INTO carried_tool_root_requests SELECT root_identity,"
                "caller_namespace,request_key,request_preimage,"
                "request_preimage_hash FROM tool_root_requests;"
                "DROP TABLE tool_root_requests;"
                "CREATE TABLE tool_root_requests(root_identity TEXT PRIMARY KEY "
                "CHECK(length(root_identity)=64 AND root_identity NOT GLOB "
                "'*[^0-9a-f]*'),"
                "caller_namespace TEXT NOT NULL CHECK(length(CAST(caller_namespace "
                "AS BLOB)) BETWEEN 1 AND 256),"
                "request_key TEXT NOT NULL CHECK(length(CAST(request_key AS BLOB)) "
                "BETWEEN 1 AND 256),"
                "request_preimage TEXT NOT NULL CHECK(length(CAST(request_preimage "
                "AS BLOB)) > 0),"
                "request_preimage_hash TEXT NOT NULL "
                "CHECK(length(request_preimage_hash)=64 AND request_preimage_hash "
                "NOT GLOB '*[^0-9a-f]*'),"
                "UNIQUE(caller_namespace, request_key)) STRICT;"
                "INSERT INTO tool_root_requests SELECT * FROM "
                "carried_tool_root_requests;"
                "DROP TABLE carried_tool_root_requests;"
                "CREATE TABLE carried_tool_call_history("
                "call_identity TEXT PRIMARY KEY,mutating INTEGER NOT NULL,"
                "state TEXT NOT NULL,revision INTEGER NOT NULL,"
                "active_admission_attempt INTEGER NOT NULL,outcome_payload TEXT,"
                "outcome_payload_hash TEXT,evidence TEXT,evidence_hash TEXT"
                ") STRICT;"
                "INSERT INTO carried_tool_call_history SELECT call_identity,"
                "mutating,state,revision,active_admission_attempt,"
                "outcome_payload,outcome_payload_hash,evidence,evidence_hash "
                "FROM tool_call_history;"
                "DROP TABLE tool_call_history;"
                "CREATE TABLE tool_call_history(call_identity TEXT PRIMARY KEY "
                "REFERENCES tool_call_positions(call_identity),"
                "mutating INTEGER NOT NULL CHECK(mutating IN (0,1)),"
                "state TEXT NOT NULL CHECK(state IN ('proposed','admitted',"
                "'dispatching','confirmed','proven_absent','possible','rejected',"
                "'terminal_failure','terminally_unresolved')),"
                "revision INTEGER NOT NULL CHECK(revision > 0),"
                "active_admission_attempt INTEGER NOT NULL "
                "CHECK(active_admission_attempt >= 0),outcome_payload TEXT,"
                "outcome_payload_hash TEXT,evidence TEXT,evidence_hash TEXT,"
                "CHECK((evidence IS NULL AND evidence_hash IS NULL) OR (evidence IS "
                "NOT NULL AND evidence_hash IS NOT NULL AND "
                "length(evidence_hash)=64 AND evidence_hash NOT GLOB "
                "'*[^0-9a-f]*')),"
                "CHECK((state='proposed' AND active_admission_attempt=0 AND "
                "outcome_payload IS NULL AND outcome_payload_hash IS NULL AND "
                "evidence IS NULL AND evidence_hash IS NULL) OR "
                "(state IN ('admitted','dispatching') AND "
                "active_admission_attempt>0 AND outcome_payload "
                "IS NULL AND outcome_payload_hash IS NULL AND evidence IS NULL AND "
                "evidence_hash IS NULL) OR (state IN ('confirmed','proven_absent',"
                "'possible','rejected','terminal_failure',"
                "'terminally_unresolved') AND active_admission_attempt>0 AND "
                "outcome_payload IS NOT NULL AND outcome_payload_hash IS NOT NULL "
                "AND length(outcome_payload_hash)=64 AND outcome_payload_hash NOT "
                "GLOB '*[^0-9a-f]*'))) STRICT;"
                "INSERT INTO tool_call_history SELECT * FROM "
                "carried_tool_call_history;"
                "DROP TABLE carried_tool_call_history;"
                "PRAGMA foreign_keys=ON;"
            );
        }

        // Every migration fixture below constructs the exact schema of the
        // generation its pair migrates FROM, and all of those generations
        // predate the call-bound Journal proposal tables. Dropping them is part
        // of winding a fresh database back, exactly as restoring a dropped
        // table is: the identity is taken over the stored DDL text, so a table
        // the wind-back left in place fails the pinned source hash even though
        // no row is ever touched.
        auto dropJournalProposalTables(
            test_support::OperatorDatabaseProbe& database
        ) -> void
        {
            database.execute(
                "DROP TABLE journal_proposal_effects;"
                "DROP TABLE journal_proposal_events;"
                "DROP TABLE journal_batch_proposals;"
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

        // The five tables the Operation dispatch spine owned, restored with the
        // exact stored CREATE text the generation before this one wrote. It is
        // pasted rather than derived because that generation no longer exists in
        // the tree to derive it from, and the historical identity IS the DDL
        // text: one changed byte of indentation or of the comment inside
        // `approvals` reproduces a schema that generation never had.
        auto restoreOperationDispatchSchema(
            test_support::OperatorDatabaseProbe& database
        ) -> void
        {
            database.execute(R"sql(
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

            )sql");

            // ledger_events carried a delivery_outcome_recorded arm in its
            // CHECK for as long as a dispatch could record one.
            database.execute(R"sql(
                PRAGMA writable_schema=ON;
                UPDATE sqlite_schema SET sql=replace(
                    sql,
                    '''diverged'')))',
                    '''diverged'')) OR (kind=''delivery_outcome_recorded'''
                    || ' AND detail IN (''not_delivered'', ''delivered'', '
                    || '''transport_unknown'')))'
                ) WHERE type='table' AND name='ledger_events';
                PRAGMA writable_schema=OFF;
            )sql");
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

        // The exact stored CREATE of one table. The historical identity is the
        // DDL text, so a restore that rebuilt a table from a hand-copied
        // statement would drift from the one this generation actually writes;
        // taking the current text and undoing what this generation changed
        // cannot.
        [[nodiscard]]
        auto storedCreate(
            test_support::OperatorDatabaseProbe& database,
            std::string_view table
        ) -> std::string
        {
            auto const rows = database.readRows(
                "SELECT sql FROM sqlite_schema WHERE type='table' AND name='"
                + std::string{table} + "'"
            );
            REQUIRE(rows.size() == 1U);
            REQUIRE(rows.front().size() == 1U);
            return rows.front().front();
        }

        // The stored CREATE INDEX text for one index, read back the same way.
        [[nodiscard]]
        auto storedCreateIndex(
            test_support::OperatorDatabaseProbe& database,
            std::string_view index
        ) -> std::string
        {
            auto const rows = database.readRows(
                "SELECT sql FROM sqlite_schema WHERE type='index' AND name='"
                + std::string{index} + "'"
            );
            REQUIRE(rows.size() == 1U);
            REQUIRE(rows.front().size() == 1U);
            return rows.front().front();
        }

        // The stored CREATE with the installed-generation CHECK wound back to
        // the positive form the generation before the genesis generation
        // stored. Exactly one occurrence per table, and the REQUIRE is what
        // says so.
        [[nodiscard]]
        auto positiveGenerationCheck(std::string prior) -> std::string
        {
            constexpr auto relaxed = std::string_view{
                "CHECK(installed_generation >= 0)"
            };
            constexpr auto positive = std::string_view{
                "CHECK(installed_generation > 0)"
            };
            auto const at = prior.find(relaxed);
            REQUIRE(at != std::string::npos);
            REQUIRE(prior.find(relaxed, at + relaxed.size()) == std::string::npos);
            prior.replace(at, relaxed.size(), positive);
            return prior;
        }

        // The same, with one declared column removed.
        [[nodiscard]]
        auto storedCreateWithout(
            test_support::OperatorDatabaseProbe& database,
            std::string_view table,
            std::string_view addedColumn
        ) -> std::string
        {
            auto prior    = storedCreate(database, table);
            auto const at = prior.find(addedColumn);
            REQUIRE(at != std::string::npos);
            prior.erase(at, addedColumn.size());
            return prior;
        }

        // The stored CREATE with its trailing whitespace removed. SQLite keeps
        // every byte from the table name to the statement's terminator, so a
        // table this generation creates from a named constant whose raw string
        // ends in a newline and an indent stores those bytes too. The
        // generation being reproduced created the same table inside one block
        // terminated immediately after STRICT, so the restore has to end there.
        [[nodiscard]]
        auto withoutTrailingSpace(std::string prior) -> std::string
        {
            auto const end = prior.find_last_not_of(" \t\r\n");
            REQUIRE(end != std::string::npos);
            prior.erase(end + 1U);
            return prior;
        }

        // The same, with one removed column put back ahead of the declaration
        // that took its place. Insertion position is part of the DDL text and
        // therefore part of the historical identity, so the anchor names the
        // column the restored one used to sit above rather than an offset.
        [[nodiscard]]
        auto storedCreateWith(
            test_support::OperatorDatabaseProbe& database,
            std::string_view table,
            std::string_view removedColumn,
            std::string_view above
        ) -> std::string
        {
            auto prior    = storedCreate(database, table);
            auto const at = prior.find(above);
            REQUIRE(at != std::string::npos);
            prior.insert(at, removedColumn);
            return prior;
        }

        // The generation before generation 0 named the genesis RuntimeArtifact:
        // both installed_generation CHECKs demanded a positive value, and no
        // root held a generation-0 row.
        //
        // Both tables are derived from their own surviving text rather than
        // pasted, for the reason storedCreate states -- undoing the one byte
        // this generation changed cannot drift, and a hand-copy can. The
        // runtime_installations text is byte-identical either way because the
        // constant it now comes from reproduces the block the prior generation
        // created it inside, closing brace included.
        //
        // The rows go with the CHECK. A wind-back that left the generation-0
        // row behind would be reproducing a database that generation could
        // never have written, and the restricted CHECK would refuse to carry
        // the row across anyway.
        //
        // It must run FIRST in every wind-back: admitTheGenesisGeneration is
        // the last step of every registered migration, so undoing it is the
        // first thing a fixture that reproduces the source generation does.
        auto restoreLegacyNestedToolCallSchema(
            test_support::OperatorDatabaseProbe& database
        ) -> void;

        // The newest generation's wind-back: the audit table a genesis
        // materialisation that moved is recorded in. Every wind-back chain in
        // this file begins at restoreLegacyNestedToolCallSchema, and that is
        // where this runs, because a chain winds back newest-first.
        auto removeGenesisTransitions(
            test_support::OperatorDatabaseProbe& database
        ) -> void
        {
            database.execute("DROP TABLE genesis_transitions");
        }

        auto restoreGenesisGenerationSchema(
            test_support::OperatorDatabaseProbe& database
        ) -> void
        {
            restoreLegacyNestedToolCallSchema(database);
            auto const priorInstallations = positiveGenerationCheck(
                storedCreate(database, "runtime_installations")
            );
            auto const priorSessions = positiveGenerationCheck(
                storedCreate(database, "sessions")
            );
            auto const priorIndex = storedCreateIndex(
                database,
                "one_active_write_session_per_instance"
            );

            database.execute("PRAGMA foreign_keys=OFF");
            database.execute(
                "DELETE FROM runtime_installations WHERE installed_generation=0"
            );
            database.execute(
                "UPDATE runtime_state SET active_runtime_artifact_root_hash=NULL "
                "WHERE singleton=1 AND installed_generation=0"
            );
            database.execute(
                "DELETE FROM runtime_artifacts WHERE artifact_root_hash NOT IN "
                "(SELECT artifact_root_hash FROM runtime_installations) AND "
                "artifact_root_hash NOT IN (SELECT active_runtime_artifact_root_hash "
                "FROM runtime_state WHERE active_runtime_artifact_root_hash IS NOT NULL)"
            );

            database.execute(
                "CREATE TABLE carried_runtime_installations AS "
                "SELECT * FROM runtime_installations"
            );
            database.execute("DROP TABLE runtime_installations");
            database.execute(priorInstallations);
            database.execute(
                "INSERT INTO runtime_installations SELECT * FROM "
                "carried_runtime_installations"
            );
            database.execute("DROP TABLE carried_runtime_installations");

            database.execute("CREATE TABLE carried_sessions AS SELECT * FROM sessions");
            database.execute("DROP TABLE sessions");
            database.execute(priorSessions);
            database.execute("INSERT INTO sessions SELECT * FROM carried_sessions");
            database.execute("DROP TABLE carried_sessions");
            database.execute(priorIndex);
            database.execute("PRAGMA foreign_keys=ON");
        }

        // The one nullable reference into `operations` that journal_events and
        // external_input_findings each carried, and the declaration each of
        // them sat directly above.
        constexpr auto k_operationReferenceColumn = std::string_view{
            "                        operation_id TEXT REFERENCES operations(operation_id),\n"
        };

        constexpr auto k_journalEventTypeColumn = std::string_view{
            "                        namespaced_event_type TEXT NOT NULL,"
        };

        constexpr auto k_findingRequiredActionColumn = std::string_view{
            "                        required_action TEXT NOT NULL"
        };

        // ledger_events as it stood while an Operation could still be its
        // subject: a detail column, and a CHECK naming the two Operation kinds
        // and the state vocabulary one of them wrote there.
        constexpr auto k_priorLedgerEventsDdl = std::string_view{
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
            "'invalid', 'denied', 'cancelled', 'expired', 'diverged')))"
            ") STRICT"
        };

        // The whole Operation surface as the generation before this one stored
        // it: the two tables it owned, the nullable reference into `operations`
        // that journal_events and external_input_findings each carried, and the
        // ledger_events shape an Operation could be the subject of.
        //
        // Every registered migration below now ends in the step that drops all
        // of that, so every migration fixture's wind-back needs this, and it
        // must run FIRST in each of them: restoreOperationDispatchSchema
        // rewrites the ledger_events CHECK this helper puts back, and its five
        // tables reference operations(operation_id).
        //
        // operations and reconciliations are pasted because that generation no
        // longer exists in the tree to derive them from -- the historical
        // identity IS the stored text, so one changed byte of indentation
        // reproduces a schema that generation never had. The other three are
        // derived from their own surviving text for the reason storedCreate
        // states: undoing what this generation changed cannot drift, and a
        // hand-copy can.
        //
        // Rows are carried through rather than dropped, exactly as
        // restorePriorToolRunSchema carries its own: the fixtures read the
        // ledger back across their upgrade, and a wind-back that emptied these
        // would prove the migration preserved nothing.
        //
        // journal_events is one of the tables it rebuilds, and the state cut
        // deleted it, so restoreProjectStateInterpretationSchema must have put
        // it back before this runs.
        auto restoreOperationSurfaceSchema(
            test_support::OperatorDatabaseProbe& database
        ) -> void
        {
            auto const priorJournalEvents = storedCreateWith(
                database,
                "journal_events",
                k_operationReferenceColumn,
                k_journalEventTypeColumn
            );
            auto const priorExternalInputFindings = storedCreateWith(
                database,
                "external_input_findings",
                k_operationReferenceColumn,
                k_findingRequiredActionColumn
            );
            database.execute(R"sql(
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

                    CREATE TABLE IF NOT EXISTS reconciliations(
                        sequence INTEGER PRIMARY KEY AUTOINCREMENT,
                        operation_id TEXT NOT NULL REFERENCES operations(operation_id),
                        disposition TEXT NOT NULL,
                        canonical_proposal TEXT NOT NULL
                    ) STRICT;
            )sql");

            // The three rebuilt tables are foreign-key participants, so they
            // are carried and re-created rather than altered, and enforcement
            // is lifted for the rebuild exactly as the migration itself defers
            // it. Every restored operation_id lands NULL, which is the only
            // value any writer of that generation ever bound to it.
            database.execute(
                "PRAGMA foreign_keys=OFF;"
                "CREATE TABLE carried_journal_events AS SELECT * FROM journal_events;"
                "DROP TABLE journal_events;"
                + priorJournalEvents
                + ";INSERT INTO journal_events(event_id, plugin_id, "
                  "project_instance_key, sequence, prior_project_state_revision, "
                  "session_manifest_hash, namespaced_event_type, "
                  "payload_schema_hash, opaque_project_payload, provenance) "
                  "SELECT event_id, plugin_id, project_instance_key, sequence, "
                  "prior_project_state_revision, session_manifest_hash, "
                  "namespaced_event_type, payload_schema_hash, "
                  "opaque_project_payload, provenance FROM carried_journal_events;"
                  "DROP TABLE carried_journal_events;"
                  "CREATE TABLE carried_external_input_findings AS SELECT * "
                  "FROM external_input_findings;"
                  "DROP TABLE external_input_findings;"
                + priorExternalInputFindings
                + ";INSERT INTO external_input_findings(finding_id, "
                  "controlled_target_id, session_epoch, reporter_session_id, "
                  "detected_after_cursor, invalidated_snapshot_revision, "
                  "required_action, reason) SELECT finding_id, "
                  "controlled_target_id, session_epoch, reporter_session_id, "
                  "detected_after_cursor, invalidated_snapshot_revision, "
                  "required_action, reason FROM carried_external_input_findings;"
                  "DROP TABLE carried_external_input_findings;"
                  "CREATE TABLE carried_ledger_events AS SELECT * FROM ledger_events;"
                  "DROP TABLE ledger_events;"
                + std::string{k_priorLedgerEventsDdl}
                + ";INSERT INTO ledger_events(sequence, session_epoch, "
                  "controlled_target_id, kind, subject_id) SELECT sequence, "
                  "session_epoch, controlled_target_id, kind, subject_id "
                  "FROM carried_ledger_events;"
                  "DROP TABLE carried_ledger_events;"
                  "PRAGMA foreign_keys=ON;"
            );
        }


        // journal_events and project_state as the generation before the state
        // cut stored them, pasted because that generation no longer exists in
        // the tree to derive them from -- the historical identity IS the
        // stored text, so one changed byte of indentation reproduces a schema
        // that generation never had.
        constexpr auto k_priorJournalEventsDdl = std::string_view{
            R"sql(CREATE TABLE IF NOT EXISTS journal_events(
                        event_id TEXT PRIMARY KEY,
                        plugin_id TEXT NOT NULL,
                        project_instance_key TEXT NOT NULL,
                        sequence INTEGER NOT NULL CHECK(sequence >= 0),
                        prior_project_state_revision INTEGER,
                        session_manifest_hash TEXT NOT NULL,
                        namespaced_event_type TEXT NOT NULL,
                        payload_schema_hash TEXT NOT NULL,
                        opaque_project_payload TEXT NOT NULL,
                        provenance TEXT NOT NULL,
                        FOREIGN KEY(plugin_id, project_instance_key)
                            REFERENCES project_instances(plugin_id, project_instance_key),
                        UNIQUE(plugin_id, project_instance_key, sequence)
                    ) STRICT)sql"
        };

        constexpr auto k_priorProjectStateDdl = std::string_view{
            R"sql(CREATE TABLE IF NOT EXISTS project_state(
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
                    ) STRICT)sql"
        };

        // The three call-bound Journal proposal tables, in the exact one-line
        // spelling the generation that added them created them with.
        constexpr auto k_priorJournalBatchProposalsDdl = std::string_view{
            "CREATE TABLE journal_batch_proposals("
            "proposal_identity TEXT PRIMARY KEY CHECK(length(proposal_identity)=64 "
            "AND proposal_identity NOT GLOB '*[^0-9a-f]*'),"
            "root_identity TEXT NOT NULL REFERENCES tool_runs(root_identity),"
            "call_identity TEXT NOT NULL REFERENCES tool_call_history(call_identity),"
            "call_outcome_revision INTEGER NOT NULL "
            "CHECK(call_outcome_revision > 0),"
            "plugin_id TEXT NOT NULL,"
            "project_instance_key TEXT NOT NULL,"
            "prior_project_state_revision INTEGER NOT NULL "
            "CHECK(prior_project_state_revision >= 0),"
            "published_revision INTEGER CHECK(published_revision IS NULL OR "
            "published_revision > 0),"
            "FOREIGN KEY(plugin_id, project_instance_key) "
            "REFERENCES project_instances(plugin_id, project_instance_key)"
            ") STRICT"
        };

        constexpr auto k_priorJournalProposalEventsDdl = std::string_view{
            "CREATE TABLE journal_proposal_events("
            "proposal_identity TEXT NOT NULL REFERENCES "
            "journal_batch_proposals(proposal_identity),"
            "batch_index INTEGER NOT NULL CHECK(batch_index >= 0),"
            "event_id TEXT NOT NULL,"
            "namespaced_event_type TEXT NOT NULL,"
            "opaque_project_payload TEXT NOT NULL,"
            "provenance TEXT NOT NULL,"
            "PRIMARY KEY(proposal_identity, batch_index)"
            ") STRICT"
        };

        constexpr auto k_priorJournalProposalEffectsDdl = std::string_view{
            "CREATE TABLE journal_proposal_effects("
            "proposal_identity TEXT NOT NULL REFERENCES "
            "journal_batch_proposals(proposal_identity),"
            "call_identity TEXT NOT NULL REFERENCES tool_call_history(call_identity),"
            "outcome_revision INTEGER NOT NULL CHECK(outcome_revision > 0),"
            "PRIMARY KEY(proposal_identity, call_identity)"
            ") STRICT"
        };

        // The ProjectState columns the state cut dropped, each with the
        // declaration it used to sit above. Insertion position is part of the
        // DDL text and therefore part of the historical identity.
        constexpr auto k_observationProjectStateColumns = std::string_view{
            "                        project_state_revision INTEGER NOT NULL\n"
            "                            CHECK(project_state_revision >= 0),\n"
            "                        project_state_hash TEXT NOT NULL,\n"
        };

        constexpr auto k_observationCanonicalColumn = std::string_view{
            "                        canonical_observation TEXT NOT NULL,"
        };

        constexpr auto k_snapshotProjectStateColumn = std::string_view{
            "                        project_state_revision INTEGER NOT NULL\n"
            "                            CHECK(project_state_revision >= 0),\n"
        };

        constexpr auto k_snapshotAvailabilityColumn = std::string_view{
            "                        availability_revision INTEGER NOT NULL"
        };

        constexpr auto k_instanceBaselineEventColumn = std::string_view{
            "baseline_event_id TEXT UNIQUE,"
        };

        constexpr auto k_instancePrimaryKey = std::string_view{
            "PRIMARY KEY(plugin_id, project_instance_key),"
        };

        // Winds a fresh database back past the generation in which the
        // framework stopped interpreting a Project's state: the Journal, the
        // materialized ProjectState, the call-bound proposals, the baseline
        // event a ProjectInstance named and the ProjectState revision and
        // digest the observation and snapshot rows carried.
        //
        // Every registered migration below now ends in that step, so every
        // migration fixture needs this and it must run FIRST in each of them:
        // restoreOperationSurfaceSchema rebuilds journal_events, which only
        // exists again once this has put it back.
        //
        // The two deleted tables are pasted for the reason
        // restoreOperationSurfaceSchema pastes its own; the three surviving
        // ones are derived from their own surviving text, because undoing what
        // this generation changed cannot drift and a hand-copy can.
        //
        // The restored ProjectState columns take revision 0 and an empty
        // digest. Nothing durable holds those values any more -- the framework
        // stores no reading of a Project's state to carry back -- and the
        // schema identity is the DDL text alone, so what the rows say cannot
        // change which generation this reproduces.
        auto restoreProjectStateInterpretationSchema(
            test_support::OperatorDatabaseProbe& database
        ) -> void
        {
            auto const priorProjectInstances = storedCreateWith(
                database,
                "project_instances",
                k_instanceBaselineEventColumn,
                k_instancePrimaryKey
            );
            auto const priorProjectObservations = withoutTrailingSpace(
                storedCreateWith(
                    database,
                    "project_observations",
                    k_observationProjectStateColumns,
                    k_observationCanonicalColumn
                )
            );
            auto const priorSnapshots = withoutTrailingSpace(storedCreateWith(
                database,
                "snapshots",
                k_snapshotProjectStateColumn,
                k_snapshotAvailabilityColumn
            ));

            // All three are foreign-key participants, so they are carried and
            // re-created rather than altered, and enforcement is lifted for
            // the rebuild exactly as the migration itself defers it.
            database.execute(
                "PRAGMA foreign_keys=OFF;"
                "CREATE TABLE carried_project_instances AS SELECT * FROM "
                "project_instances;"
                "DROP TABLE project_instances;"
                + priorProjectInstances
                + ";INSERT INTO project_instances(plugin_id, "
                  "project_instance_key, project_registration_hash) SELECT "
                  "plugin_id, project_instance_key, project_registration_hash "
                  "FROM carried_project_instances;"
                  "DROP TABLE carried_project_instances;"
                  "CREATE TABLE carried_project_observations AS SELECT * FROM "
                  "project_observations;"
                  "DROP TABLE project_observations;"
                + priorProjectObservations
                + ";INSERT INTO project_observations(plugin_id, "
                  "project_instance_key, revision, project_registration_hash, "
                  "state_resolution_hash, project_state_revision, "
                  "project_state_hash, canonical_observation, observation_hash) "
                  "SELECT plugin_id, project_instance_key, revision, "
                  "project_registration_hash, state_resolution_hash, 0, '', "
                  "canonical_observation, observation_hash FROM "
                  "carried_project_observations;"
                  "DROP TABLE carried_project_observations;"
                  "CREATE TABLE carried_snapshots AS SELECT * FROM snapshots;"
                  "DROP TABLE snapshots;"
                + priorSnapshots
                + ";INSERT INTO snapshots(token, session_id, snapshot_revision, "
                  "session_epoch, identity_hash, decision_basis_hash, "
                  "canonical_parts, lease_revision, plugin_id, "
                  "project_instance_key, observation_id, target_generation, "
                  "state_resolution_hash, project_observation_revision, "
                  "project_state_revision, availability_revision) SELECT token, "
                  "session_id, snapshot_revision, session_epoch, identity_hash, "
                  "decision_basis_hash, canonical_parts, lease_revision, "
                  "plugin_id, project_instance_key, observation_id, "
                  "target_generation, state_resolution_hash, "
                  "project_observation_revision, 0, availability_revision "
                  "FROM carried_snapshots;"
                  "DROP TABLE carried_snapshots;"
                + std::string{k_priorJournalEventsDdl} + ";"
                + std::string{k_priorProjectStateDdl} + ";"
                + std::string{k_priorJournalBatchProposalsDdl} + ";"
                + std::string{k_priorJournalProposalEventsDdl} + ";"
                + std::string{k_priorJournalProposalEffectsDdl} + ";"
                  "PRAGMA foreign_keys=ON;"
            );
        }

        constexpr auto k_observationReferenceColumn = std::string_view{
            "observation_reference_hash TEXT CHECK(observation_reference_hash IS NULL "
            "OR (length(observation_reference_hash)=64 AND "
            "observation_reference_hash NOT GLOB '*[^0-9a-f]*')),"
        };

        constexpr auto k_delegationGrantColumn = std::string_view{
            "delegation_grant_id TEXT REFERENCES tool_delegation_grants(grant_id),"
        };

        constexpr auto k_legacyToolDelegationGrantsDdl = std::string_view{
            "CREATE TABLE tool_delegation_grants("
            "grant_id TEXT PRIMARY KEY CHECK(length(grant_id)=64 AND "
            "grant_id NOT GLOB '*[^0-9a-f]*'),"
            "root_identity TEXT NOT NULL REFERENCES tool_runs(root_identity),"
            "parent_call_identity TEXT NOT NULL REFERENCES "
            "tool_call_history(call_identity),"
            "parent_attempt_number INTEGER NOT NULL "
            "CHECK(parent_attempt_number > 0),"
            "parent_tool_name TEXT NOT NULL CHECK("
            "length(CAST(parent_tool_name AS BLOB)) BETWEEN 1 AND 256),"
            "execution_principal_id TEXT NOT NULL,"
            "execution_principal_kind TEXT NOT NULL CHECK(execution_principal_kind "
            "IN ('script','agent','human')),"
            "child_tool_names TEXT NOT NULL,"
            "maximum_child_surface TEXT NOT NULL CHECK(maximum_child_surface IN "
            "('semantic','privileged')),"
            "maximum_child_mutability TEXT NOT NULL CHECK(maximum_child_mutability "
            "IN ('read_only','mutating')),"
            "maximum_child_risk TEXT NOT NULL CHECK(maximum_child_risk IN "
            "('read_only','low','medium','high','critical')),"
            "maximum_child_calls INTEGER NOT NULL CHECK(maximum_child_calls > 0),"
            "UNIQUE(parent_call_identity, parent_attempt_number)"
            ") STRICT"
        };

        // Every registered source generation after the nesting cut carried
        // this column and table. A migration fixture starts from today's leaf
        // schema, so it restores those exact stored DDL bytes before winding
        // back later changes. The one pre-nesting fixture removes them again.
        auto restoreLegacyNestedToolCallSchema(
            test_support::OperatorDatabaseProbe& database
        ) -> void
        {
            removeGenesisTransitions(database);
            auto attempts = storedCreate(database, "tool_admission_attempts");
            if (attempts.find("delegation_grant_id") != std::string::npos)
            {
                return;
            }
            auto const at = attempts.find("CHECK((effect_envelope");
            REQUIRE(at != std::string::npos);
            attempts.insert(at, k_delegationGrantColumn);
            constexpr auto columns = std::string_view{
                "call_identity, attempt_number, root_identity, "
                "origin_principal_id, origin_principal_kind, "
                "execution_principal_id, execution_principal_kind, session_id, "
                "session_epoch, controlled_target_id, project_registration_hash, "
                "policy_hash, capability_profile_hash, lease_id, lease_revision, "
                "fencing_token, budget_snapshot, budget_snapshot_hash, "
                "effect_envelope, effect_envelope_hash, required_approvals, "
                "approval_tokens, approval_expires_at_unix_millis"
            };
            database.execute(
                "PRAGMA foreign_keys=OFF;"
                + std::string{k_legacyToolDelegationGrantsDdl} + ";"
                  "ALTER TABLE tool_admission_attempts RENAME TO "
                  "leaf_tool_admission_attempts;"
                + attempts + ";INSERT INTO tool_admission_attempts("
                + std::string{columns} + ", delegation_grant_id) SELECT "
                + std::string{columns} + ", NULL FROM leaf_tool_admission_attempts;"
                  "DROP TABLE leaf_tool_admission_attempts;"
                  "PRAGMA foreign_keys=ON;"
            );
        }

        constexpr auto k_priorToolCallPositionColumns = std::string_view{
            "call_identity, root_identity, parent_call_identity, call_sequence, "
            "run_identity, framework_release_identity, "
            "tool_runtime_protocol_identity, environment_identity, provider_kind, "
            "project_registration_hash, tool_catalog_hash, tool_name, tool_version, "
            "canonical_args, canonical_args_hash"
        };

        constexpr auto k_rootPositionedParentColumn = std::string_view{
            "parent_call_identity TEXT NOT NULL CHECK("
            "length(parent_call_identity)=64 AND "
            "parent_call_identity NOT GLOB '*[^0-9a-f]*'),"
        };

        constexpr auto k_nullableParentColumn = std::string_view{
            "parent_call_identity TEXT,"
        };

        constexpr auto k_rootPositionedParentUnique = std::string_view{
            "UNIQUE(root_identity, parent_call_identity, call_sequence)"
        };

        constexpr auto k_nullableParentUnique = std::string_view{
            "UNIQUE(root_identity, parent_call_identity, call_sequence),"
            "FOREIGN KEY(root_identity, parent_call_identity) REFERENCES "
            "tool_call_positions(root_identity, call_identity)"
        };

        // The stored CREATE with the root-positioned parent column put back to
        // the nullable column, the composite parent foreign key it allowed,
        // and nothing else changed. Nullability is part of the DDL text, so
        // every generation before the root run became a real positioned call
        // reproduces only with this substitution in place.
        [[nodiscard]]
        auto withNullableToolCallParent(std::string prior) -> std::string
        {
            auto const parentAt = prior.find(k_rootPositionedParentColumn);
            REQUIRE(parentAt != std::string::npos);
            prior.replace(
                parentAt,
                k_rootPositionedParentColumn.size(),
                k_nullableParentColumn
            );
            auto const uniqueAt = prior.find(k_rootPositionedParentUnique);
            REQUIRE(uniqueAt != std::string::npos);
            prior.replace(
                uniqueAt,
                k_rootPositionedParentUnique.size(),
                k_nullableParentUnique
            );
            return prior;
        }

        // Rebuilds tool_call_positions from one historical CREATE statement,
        // carrying `columns` back and putting every row a run's own context
        // issued back to the null parent that generation wrote. It is rebuilt
        // rather than renamed because tool_call_history references it, and a
        // rename would rewrite that reference into the historical identity
        // this restore exists to reproduce.
        auto restoreNullRootedToolCallPositions(
            test_support::OperatorDatabaseProbe& database,
            std::string const& prior,
            std::string const& columns
        ) -> void
        {
            database.execute(
                "PRAGMA foreign_keys=OFF;"
                "CREATE TABLE prior_tool_call_positions AS SELECT " + columns
                + " FROM tool_call_positions;"
                  "DROP INDEX IF EXISTS one_top_level_tool_call_position;"
                  "DROP TABLE tool_call_positions;"
                + prior
                + ";CREATE UNIQUE INDEX one_top_level_tool_call_position ON "
                  "tool_call_positions(root_identity, call_sequence) "
                  "WHERE parent_call_identity IS NULL;"
                  "INSERT INTO tool_call_positions("
                + columns + ") SELECT " + columns
                + " FROM prior_tool_call_positions;"
                  "UPDATE tool_call_positions SET parent_call_identity=NULL "
                  "WHERE parent_call_identity=root_identity;"
                  "DROP TABLE prior_tool_call_positions;"
                  "PRAGMA foreign_keys=ON;"
            );
        }

        // tool_call_positions as it stood before it carried an observation
        // reference, which is also before a run's own calls were positioned
        // under their root request.
        auto restorePriorToolCallPositions(
            test_support::OperatorDatabaseProbe& database
        ) -> void
        {
            restoreNullRootedToolCallPositions(
                database,
                withNullableToolCallParent(
                    storedCreateWithout(
                        database,
                        "tool_call_positions",
                        k_observationReferenceColumn
                    )
                ),
                std::string{k_priorToolCallPositionColumns}
            );
        }

        // The generation immediately before the root run became a real
        // positioned call: the observation reference is already there, and a
        // run's own calls are still stored with no parent at all.
        auto restorePriorNullRootedToolCallPositions(
            test_support::OperatorDatabaseProbe& database
        ) -> void
        {
            restoreNullRootedToolCallPositions(
                database,
                withNullableToolCallParent(
                    storedCreate(database, "tool_call_positions")
                ),
                std::string{k_priorToolCallPositionColumns}
                    + ", observation_reference_hash"
            );
        }

        // The generation immediately before nested calls: no delegation grants,
        // no delegated admission column, and no observation reference at a call
        // coordinate.
        auto restorePriorNestedToolCallSchema(
            test_support::OperatorDatabaseProbe& database
        ) -> void
        {
            auto const prior = storedCreateWithout(
                database,
                "tool_admission_attempts",
                k_delegationGrantColumn
            );
            database.execute(
                "PRAGMA foreign_keys=OFF;"
                "ALTER TABLE tool_admission_attempts RENAME TO "
                "new_tool_admission_attempts;"
                + prior
                + ";INSERT INTO tool_admission_attempts SELECT call_identity, "
                  "attempt_number, root_identity, origin_principal_id, "
                  "origin_principal_kind, execution_principal_id, "
                  "execution_principal_kind, session_id, session_epoch, "
                  "controlled_target_id, project_registration_hash, policy_hash, "
                  "capability_profile_hash, lease_id, lease_revision, "
                  "fencing_token, budget_snapshot, budget_snapshot_hash, "
                  "effect_envelope, effect_envelope_hash, required_approvals, "
                  "approval_tokens, approval_expires_at_unix_millis "
                  "FROM new_tool_admission_attempts;"
                  "DROP TABLE new_tool_admission_attempts;"
                  "DROP TABLE IF EXISTS tool_delegation_grants;"
                  "PRAGMA foreign_keys=ON;"
            );
            restorePriorToolCallPositions(database);
        }

        auto removeToolRuntimePersistence(
            test_support::OperatorDatabaseProbe& database
        ) -> void
        {
            database.execute("DROP TABLE IF EXISTS tool_approvals");
            database.execute("DROP TABLE IF EXISTS tool_admission_attempts");
            database.execute("DROP TABLE IF EXISTS tool_delegation_grants");
            database.execute("DROP TABLE IF EXISTS tool_runs");
            database.execute("DROP TABLE IF EXISTS tool_call_history");
            restorePriorToolCallPositions(database);
        }

        auto restorePriorToolAdmissionAuthority(
            test_support::OperatorDatabaseProbe& database
        ) -> void
        {
            restorePriorToolCallPositions(database);
            database.execute("DROP TABLE IF EXISTS tool_delegation_grants");
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
            restorePriorToolCallPositions(database);
            database.execute("DROP TABLE IF EXISTS tool_delegation_grants");
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
            database.execute("DROP TABLE IF EXISTS tool_approvals");
            database.execute("DROP TABLE IF EXISTS tool_admission_attempts");
            database.execute("DROP TABLE IF EXISTS tool_delegation_grants");
            database.execute("DROP TABLE IF EXISTS tool_runs");
            database.execute("DROP TABLE IF EXISTS tool_call_history");
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

        using test_support::confirmToolCall;
        using test_support::hashOf;
        using test_support::loadGeneration;
        using test_support::makeProject;
        using test_support::sessionManifest;
        using test_support::startToolCall;
        using test_support::toolInvocation;

        // Re-adding any of these members would reopen the P0 hole a
        // request-owned tool or mutability is: it would make the mutation
        // chain opt-out. ToolAdmissionRequest is where it would land -- it is
        // the one caller-supplied admission value, and it names its Tool and
        // mutability only through the coordinate the catalog minted. The
        // checks go through concepts because a member lookup on a concrete
        // type is an error rather than a substitution failure.
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

        static_assert(!NamesMutability<ToolAdmissionRequest>);
        static_assert(!NamesTool<ToolAdmissionRequest>);
        static_assert(!NamesCanonicalArgs<ToolAdmissionRequest>);
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
        static_assert(!std::is_aggregate_v<ValidatedToolInvocation>);
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
        // The shared fixture's prepared store, not a second one shaped like it.
        // The Tool-call fixture's startToolCall takes exactly this type, and a
        // local twin would have made every case here choose between the two.
        // prepareStore below still builds its own, because the cases in this
        // file register `fixture.alpha` and vary the argument schema a
        // registration pins, which the shared builder does not take.
        using test_support::PreparedStore;

        [[nodiscard]]
        auto prepareStore(
            std::filesystem::path const& path,
            std::string_view argumentSchema = test_support::k_toolArgumentSchema
        ) -> PreparedStore
        {
            auto const release = test_support::runtimeRelease(path / "session-source");
            auto storeResult = OperatorCoordinator::open(path / "production");
            REQUIRE_MESSAGE(
                storeResult.has_value(),
                "the fixture Operator must open: ",
                storeResult.error().message()
            );
            auto store = *std::move(storeResult);
            auto installed = store.installRuntimeArtifact(
                RuntimeArtifactInstallRequest{
                    .artifactDirectory           = release.artifactDirectory,
                    .artifactRootHash            = release.artifactRootHash,
                    .expectedInstalledGeneration = 0U,
                }
            );
            auto const installedMessage = installed.has_value()
                                              ? std::string{}
                                              : std::string{installed.error().message()};
            REQUIRE_MESSAGE(installed.has_value(), installedMessage);
            auto const artifactRootHash    = installed->rootHash();
            auto const installedGeneration = installed->installedGeneration();
            auto const project = makeProject("fixture.alpha", argumentSchema);
            auto const manifest = sessionManifest(
                project.registration,
                installed->rootHash(),
                hashOf(test_support::unconstrainedAgentProfileBytes()),
                test_support::policyArtifactBytes()
            );
            auto const projectGeneration = loadGeneration(project);
            REQUIRE(store.registerProject(project.registration).has_value());
            REQUIRE(store.provisionProjectInstance(
                project.registration,
                "instance-1"
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
                test_support::unconstrainedAgentProfile(manifest)
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
                project.registration,
                project.toolCatalogSchemaOwner,
                project.observedInstanceIdentitySchemas,
                conformance::observeOnce(observation)
            );
            REQUIRE(snapshot.has_value());
            auto runtimeModel = observation.host->runtimeModelBinding(
                observation.generation
            );
            REQUIRE(runtimeModel.has_value());
            auto policyAuthority = OperatorPolicyAuthority::create(
                project.registration,
                manifest,
                *runtimeModel,
                "operator",
                test_support::policyArtifactBytes()
            );
            REQUIRE(policyAuthority.has_value());
            return PreparedStore{
                .store                   = std::move(store),
                .generation              = projectGeneration,
                .project                 = project,
                .manifest                = manifest,
                .policyAuthority         = *std::move(policyAuthority),
                .policyArtifact          = test_support::policyArtifactBytes(),
                .controller              = *controller,
                .lease                   = *lease,
                .snapshot                = *std::move(snapshot),
                .observation             = std::move(observation),
                .runtimeArtifactRootHash = artifactRootHash,
                .installedGeneration     = installedGeneration,
            };
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

        // A plan authority over a manifest naming the artifact root given,
        // which is what a production deployment builds.
        [[nodiscard]]
        auto deploymentAuthority(
            PreparedStore& prepared,
            ContentHash const& runtimeArtifactRootHash
        ) -> Result<OperatorPolicyAuthority>
        {
            auto runtimeModel = prepared.observation.host->runtimeModelBinding(
                prepared.observation.generation
            );
            REQUIRE(runtimeModel.has_value());
            return OperatorPolicyAuthority::create(
                prepared.project.registration,
                sessionManifest(
                    prepared.project.registration,
                    runtimeArtifactRootHash,
                    hashOf(test_support::unconstrainedAgentProfileBytes()),
                    test_support::policyArtifactBytes()
                ),
                *runtimeModel,
                "operator",
                test_support::policyArtifactBytes()
            );
        }

        // test_support::runtimeRelease always writes the same page model, so
        // every release it builds has the same content hash and shares one
        // production directory. Reclamation needs two that do not.
        [[nodiscard]]
        auto releaseWithModel(
            std::filesystem::path const& root,
            std::string_view model
        ) -> conformance::ObservationRelease
        {
            auto const artifact = root / "artifact";
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
            return conformance::ObservationRelease{
                .artifactDirectory = artifact,
                .artifactRootHash  = hashOf(manifest),
            };
        }

        [[nodiscard]]
        auto installRequest(
            conformance::ObservationRelease const& release,
            uint64 expectedInstalledGeneration
        ) -> RuntimeArtifactInstallRequest
        {
            return RuntimeArtifactInstallRequest{
                .artifactDirectory           = release.artifactDirectory,
                .artifactRootHash            = release.artifactRootHash,
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
                test_support::runtimeRelease(path / "session-source");
            auto storeResult = OperatorCoordinator::open(path / "production");
            REQUIRE(storeResult.has_value());
            auto store = *std::move(storeResult);
            auto installed = store.installRuntimeArtifact(
                RuntimeArtifactInstallRequest{
                    .artifactDirectory           = release.artifactDirectory,
                    .artifactRootHash            = release.artifactRootHash,
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
                    .agentProfileHash             = hashOf(test_support::unconstrainedAgentProfileBytes()),
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
            prepared.project.registration,
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
            prepared.project.registration,
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
            prepared.project.registration,
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
            prepared.project.registration,
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
            prepared.project.registration,
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
            prepared.project.registration,
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
                prepared.project.registration,
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
                prepared.project.registration,
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
                prepared.project.registration,
                scope,
                schemas,
                duplicatePrecondition
            ),
            ProjectObservationErrorCode::DuplicatePreconditionName
        );

        expectProjectObservationError(
            prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.project.registration,
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
                prepared.project.registration,
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
                prepared.project.registration,
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
                prepared.project.registration,
                scope,
                schemas,
                unregisteredBeforeBasis
            ),
            ProjectObservationErrorCode::ObservedInstanceIdentitySchemaNotRegistered
        );

        expectProjectObservationError(
            prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.project.registration,
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
                prepared.project.registration,
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
                prepared.project.registration,
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
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        // The same plugin id under a different argument schema, which is a
        // different tool_catalog_hash and therefore a different exact
        // registration.
        auto const foreign = makeProject(
            "fixture.alpha",
            test_support::k_toolArgumentSchemaWithInstanceIds
        );
        auto foreignSchemas = foreign.observedInstanceIdentitySchemas;
        REQUIRE(
            foreign.registration.hash()
            != prepared.project.registration.hash()
        );

        expectProjectObservationError(
            prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.project.registration,
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
                prepared.project.registration,
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
            prepared.project.registration,
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
            prepared.project.registration,
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

    TEST_CASE("scope mismatch precedes stale observed instance membership")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto schemas   = prepared.project.observedInstanceIdentitySchemas;
        auto const scope = runScope();
        auto first = prepared.store.publishProjectObservation(
            prepared.lease,
            prepared.project.registration,
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
            prepared.project.registration,
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
                prepared.project.registration,
                runScope(),
                schemas,
                observationProposal({
                    observedInstanceProposal("first", "overlay.stable"),
                })
            );
            REQUIRE(first.has_value());
            return std::tuple{
                prepared.project,
                prepared.generation,
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
            test_support::unconstrainedAgentProfile(manifest)
        ).has_value());
        auto controller = reopened.bindController("session-after-reopen");
        REQUIRE(controller.has_value());
        auto lease = reopened.acquireLease(*controller);
        REQUIRE(lease.has_value());
        auto schemas = project.observedInstanceIdentitySchemas;
        auto afterReopen = reopened.publishProjectObservation(
            *lease,
            project.registration,
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
        auto temporary = TemporaryDirectory{};
        auto const observeRegistration = [](
            std::filesystem::path const& path,
            std::string_view argumentSchema
        )
        {
            auto prepared = prepareStore(path, argumentSchema);
            auto schemas  = prepared.project.observedInstanceIdentitySchemas;
            auto observation = prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.project.registration,
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
            test_support::k_toolArgumentSchema
        );
        auto const [
            secondPluginId,
            secondRegistrationHash,
            secondObservedInstanceId,
            secondDatabasePath
        ] = observeRegistration(
            temporary.path() / "registration-b",
            test_support::k_toolArgumentSchemaWithInstanceIds
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
                        test_support::k_toolArgumentSchema
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
                test_support::schemaHashHex(test_support::k_toolArgumentSchema)
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
            prepared.project.registration,
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
            "instance-2"
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
            test_support::unconstrainedAgentProfile(prepared.manifest)
        ).has_value());
        auto projectController = prepared.store.bindController("session-project-2");
        REQUIRE(projectController.has_value());
        auto projectLease = prepared.store.acquireLease(*projectController);
        REQUIRE(projectLease.has_value());
        auto otherProject = prepared.store.publishProjectObservation(
            *projectLease,
            prepared.project.registration,
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

        auto foreignProject  = makeProject("fixture.foreign");
        auto foreignManifest = test_support::sessionManifest(
            foreignProject.registration,
            prepared.runtimeArtifactRootHash,
            hashOf(test_support::unconstrainedAgentProfileBytes()),
            test_support::policyArtifactBytes()
        );
        REQUIRE(prepared.store.registerProject(
            foreignProject.registration
        ).has_value());
        REQUIRE(prepared.store.provisionProjectInstance(
            foreignProject.registration,
            "instance-1"
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
            test_support::unconstrainedAgentProfile(foreignManifest)
        ).has_value());
        auto foreignController = prepared.store.bindController("session-foreign");
        REQUIRE(foreignController.has_value());
        auto foreignLease = prepared.store.acquireLease(*foreignController);
        REQUIRE(foreignLease.has_value());
        auto foreignSchemas = foreignProject.observedInstanceIdentitySchemas;
        auto foreignObservation = prepared.store.publishProjectObservation(
            *foreignLease,
            foreignProject.registration,
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

    TEST_CASE("the pinned world scope is immutable and survives a restart")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

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
            test_support::unconstrainedAgentProfile(prepared.manifest)
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(
            refused.error().message().contains(
                "already names a different immutable session tuple"
            )
        );

        // Across a restart the stored columns still name the same scope: a
        // session pinned on the same target with the same generation is
        // admitted and composes, because the pin reads the scope back out of
        // the stored tuple and not out of the caller. The first coordinator
        // holds the runtime directory exclusively, so it is released before
        // the reopened door can take it.
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
            test_support::unconstrainedAgentProfile(prepared.manifest)
        ).has_value());
        auto afterRestartController = restarted->bindController("session-after-restart");
        REQUIRE(afterRestartController.has_value());
        auto afterRestartLease = restarted->acquireLease(*afterRestartController);
        REQUIRE(afterRestartLease.has_value());
        auto afterRestartSnapshot = restarted->createSnapshot(
            *afterRestartLease,
            prepared.project.registration,
            prepared.project.toolCatalogSchemaOwner,
            prepared.project.observedInstanceIdentitySchemas,
            conformance::observeOnce(prepared.observation)
        );
        CHECK(afterRestartSnapshot.has_value());
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

    TEST_CASE("Tool root and root-positioned call identities rejoin across restart")
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
        REQUIRE(observeArguments.has_value());
        auto observe = frameworkCatalog->validate(
            "framework.screen.capture",
            std::move(*observeArguments)
        );
        REQUIRE(observe.has_value());

        auto const execution = ToolExecutionIdentity{
            .runIdentity                 = hashOf("run-1"),
            .frameworkReleaseIdentity    = hashOf("framework-release-1"),
            .toolRuntimeProtocolIdentity = hashOf("tool-runtime-protocol-1"),
            .environmentIdentity         = hashOf("environment-1"),
        };
        auto first = toolCallAt(
            *root,
            nullptr,
            1U,
            execution,
            *observe
        );
        auto second = toolCallAt(
            *root,
            nullptr,
            2U,
            execution,
            *observe
        );
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());

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

            REQUIRE(store->persistToolCallPosition(*root, *second).has_value());

            auto foreignPreimage = CanonicalJson::parseExact("{}");
            REQUIRE(foreignPreimage.has_value());
            auto foreignRoot = ToolRootRequestIdentity::create(
                "script:fixture",
                "request-2",
                std::move(*foreignPreimage)
            );
            REQUIRE(foreignRoot.has_value());
            auto foreignCall = toolCallAt(
                *foreignRoot,
                nullptr,
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
                    "WHERE call_identity='" + second->identity().hex() + "'"
                )
                == std::vector<std::vector<std::string>>{
                    {
                        root->identity().hex(),
                        root->identity().hex(),
                        "2",
                        second->canonicalArgs(),
                    },
                }
            );
            CHECK(
                database.readRows("SELECT count(*) FROM tool_call_positions")
                == std::vector<std::vector<std::string>>{{"2"}}
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

    // Section 5.3 item 3: a deterministic-replay divergence terminates the RUN,
    // not only the call that noticed it. This case is the whole definition of
    // what terminated means, in both directions: the root takes no new work,
    // and everything already terminal stays readable and rejoinable. A frame
    // still dispatching when the divergence lands cannot publish an outcome.
    // It also carries both arms of the coordinate-miss read, which
    // is what tells a changed parent from an ordinal past the frontier.
    TEST_CASE("a replay divergence stops the run and closes it to new work")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto preimage  = CanonicalJson::parseExact(R"({"objective":"diverge"})");
        REQUIRE(preimage.has_value());
        auto root = ToolRootRequestIdentity::create(
            "controller-1",
            "divergence-request",
            std::move(*preimage)
        );
        REQUIRE(root.has_value());
        auto catalog = FrameworkToolCatalogOwner::create();
        REQUIRE(catalog.has_value());
        auto observeArguments = CanonicalJson::parseExact("{}");
        auto waitArguments    = CanonicalJson::parseExact(
            R"({"duration_ms":250})"
        );
        REQUIRE(observeArguments.has_value());
        REQUIRE(waitArguments.has_value());
        auto observe = catalog->validate(
            "framework.screen.capture",
            std::move(*observeArguments)
        );
        auto wait = catalog->validate(
            "framework.workflow.wait",
            std::move(*waitArguments)
        );
        REQUIRE(observe.has_value());
        REQUIRE(wait.has_value());
        auto const execution = ToolExecutionIdentity{
            .runIdentity                 = hashOf("divergence-run"),
            .frameworkReleaseIdentity    = hashOf("divergence-framework"),
            .toolRuntimeProtocolIdentity = hashOf("divergence-protocol"),
            .environmentIdentity         = hashOf("divergence-environment"),
        };
        auto recorded = toolCallAt(*root, nullptr, 1U, execution, *observe);
        auto inFlight = toolCallAt(*root, nullptr, 2U, execution, *observe);
        auto beyond   = toolCallAt(*root, nullptr, 3U, execution, *observe);
        auto changed  = toolCallAt(*root, nullptr, 1U, execution, *wait);
        REQUIRE(recorded.has_value());
        REQUIRE(inFlight.has_value());
        REQUIRE(beyond.has_value());
        REQUIRE(changed.has_value());

        REQUIRE(prepared.store.persistToolRootRequest(*root).has_value());
        REQUIRE(
            prepared.store.persistToolCallPosition(*root, *recorded).has_value()
        );
        auto const admitted = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = *root,
                .call            = *recorded,
                .policyAuthority = prepared.policyAuthority,
            }
        );
        REQUIRE_MESSAGE(admitted.has_value(), admitted.error().message());
        auto const dispatch = prepared.store.beginToolCallDispatch(*admitted);
        REQUIRE_MESSAGE(dispatch.has_value(), dispatch.error().message());
        auto result = CanonicalJson::parseExact(R"({"snapshot_ref":"one"})");
        REQUIRE(result.has_value());
        REQUIRE(prepared.store.completeToolCallDispatch(
            *dispatch,
            ToolCallCompletion::confirmed(*result)
        ).has_value());

        // A second call is left mid-dispatch, so the run stops around a
        // boundary a provider has already crossed.
        REQUIRE(
            prepared.store.persistToolCallPosition(*root, *inFlight).has_value()
        );
        auto const inFlightAdmitted = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = *root,
                .call            = *inFlight,
                .policyAuthority = prepared.policyAuthority,
            }
        );
        REQUIRE_MESSAGE(
            inFlightAdmitted.has_value(),
            inFlightAdmitted.error().message()
        );
        auto const inFlightDispatch =
            prepared.store.beginToolCallDispatch(*inFlightAdmitted);
        REQUIRE_MESSAGE(
            inFlightDispatch.has_value(),
            inFlightDispatch.error().message()
        );

        auto const diverged =
            prepared.store.persistToolCallPosition(*root, *changed);
        REQUIRE_FALSE(diverged.has_value());
        CHECK(diverged.error().message().contains(
            "diverged from durable history: tool_name changed"
        ));
        CHECK(diverged.error().message().contains(
            "ordinal 1 under parent coordinate " + root->identity().hex()
        ));

        auto const divergenceReason =
            std::string{diverged.error().message()};

        // What terminated does NOT touch. Rule 1 stands: a call matching a
        // terminal durable row still receives its recorded result, and the
        // coordinate it sits at still rejoins rather than being refused as new.
        auto const replayed = prepared.store.replayToolCall(*root, *recorded);
        REQUIRE_MESSAGE(replayed.has_value(), replayed.error().message());
        CHECK(replayed->state == ToolCallState::Confirmed);
        REQUIRE(replayed->payload.has_value());
        CHECK(replayed->payload->bytes() == result->bytes());
        auto const rejoined =
            prepared.store.persistToolCallPosition(*root, *recorded);
        REQUIRE_MESSAGE(rejoined.has_value(), rejoined.error().message());
        CHECK(rejoined->lookup == ToolIdentityLookup::Existing);

        // A dispatch still in flight cannot publish an outcome after the run's
        // hard refusal. Otherwise an enclosing handler could consume that
        // outcome as an ordinary failure and claim success for the root.
        auto const late = CanonicalJson::parseExact(
            R"({"snapshot_ref":"mid-flight"})"
        );
        REQUIRE(late.has_value());
        auto const completedLate = prepared.store.completeToolCallDispatch(
            *inFlightDispatch,
            ToolCallCompletion::confirmed(*late)
        );
        REQUIRE_FALSE(completedLate.has_value());
        CHECK(completedLate.error().message().contains(
            "was stopped by deterministic-replay divergence"
        ));

        // What terminated does close: every door that starts new work.
        auto const refusedPosition =
            prepared.store.persistToolCallPosition(*root, *beyond);
        REQUIRE_FALSE(refusedPosition.has_value());
        CHECK(refusedPosition.error().message().contains(
            "was stopped by deterministic-replay divergence"
        ));
        auto const refusedAdmission = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = *root,
                .call            = *recorded,
                .policyAuthority = prepared.policyAuthority,
            }
        );
        REQUIRE_FALSE(refusedAdmission.has_value());
        CHECK(refusedAdmission.error().message().contains(
            "was stopped by deterministic-replay divergence"
        ));
        auto const refusedReentry = prepared.store.reenterToolCallDispatch(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = *root,
                .call            = *recorded,
                .policyAuthority = prepared.policyAuthority,
            }
        );
        REQUIRE_FALSE(refusedReentry.has_value());
        CHECK(refusedReentry.error().message().contains(
            "was stopped by deterministic-replay divergence"
        ));

        // The coordinate-miss read, both arms, on a run of its own because the
        // second arm stops it.
        auto otherPreimage = CanonicalJson::parseExact(
            R"({"objective":"coordinate-miss"})"
        );
        REQUIRE(otherPreimage.has_value());
        auto other = ToolRootRequestIdentity::create(
            "controller-1",
            "coordinate-miss-request",
            std::move(*otherPreimage)
        );
        REQUIRE(other.has_value());
        auto firstAtOther = toolCallAt(*other, nullptr, 1U, execution, *observe);
        REQUIRE(firstAtOther.has_value());
        auto secondAtOther =
            toolCallAt(*other, nullptr, 2U, execution, *observe);
        REQUIRE(secondAtOther.has_value());
        REQUIRE(prepared.store.persistToolRootRequest(*other).has_value());
        REQUIRE(
            prepared.store.persistToolCallPosition(*other, *firstAtOther)
                .has_value()
        );

        // A miss whose parent the record DOES have is an ordinal past that
        // context's frontier. It is not a divergence and leaves the run live.
        auto const absentOrdinal =
            prepared.store.replayToolCall(*other, *secondAtOther);
        REQUIRE_FALSE(absentOrdinal.has_value());
        CHECK(absentOrdinal.error().message().contains(
            "Tool replay has no recorded call at ordinal 2 under parent "
            "coordinate " + other->identity().hex()
        ));
        REQUIRE(
            prepared.store.persistToolCallPosition(*other, *secondAtOther)
                .has_value()
        );

        // The divergent run says so durably while an ordinal beyond a root
        // context's frontier leaves the other run running.
        auto const databasePath = prepared.store.databasePath();
        {
            auto released = std::move(prepared.store);
        }
        auto database = test_support::OperatorDatabaseProbe{databasePath};
        CHECK(
            database.readRows(
                "SELECT state, termination_reason FROM tool_root_requests "
                "WHERE root_identity='" + root->identity().hex() + "'"
            )
            == std::vector<std::vector<std::string>>{
                {"terminated", divergenceReason},
            }
        );
        auto const otherState = database.readRows(
            "SELECT state FROM tool_root_requests WHERE root_identity='"
            + other->identity().hex() + "'"
        );
        REQUIRE(otherState.size() == 1U);
        REQUIRE(otherState.front().size() == 1U);
        CHECK(otherState.front().front() == "running");
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
            "framework.screen.capture",
            std::move(*arguments)
        );
        REQUIRE(invocation.has_value());
        auto call = toolCallAt(
            *root,
            nullptr,
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
                "diverged from durable history: canonical_args changed"
            ));

            // The same field-by-field comparison on the READ path, which a
            // reconciliation reaches without persisting first. It is the same
            // diagnosis rather than an absent-row one, and it stays available
            // after the run is stopped because reading a durable outcome is not
            // new work.
            auto replayRefused = reopened->replayToolCall(*root, *call);
            REQUIRE_FALSE(replayRefused.has_value());
            CHECK(replayRefused.error().message().contains(
                "diverged from durable history: canonical_args changed"
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
            "framework.screen.capture",
            std::move(*arguments)
        );
        REQUIRE(invocation.has_value());
        auto call = toolCallAt(
            *root,
            nullptr,
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
        {
            auto prepared = prepareStore(temporary.path());
            static_cast<void>(prepared);
        }

        auto sourceIdentity = std::string{};
        {
            auto prior = test_support::OperatorDatabaseProbe{databasePath};
            restoreGenesisGenerationSchema(prior);
            restoreProjectStateInterpretationSchema(prior);
            restoreOperationSurfaceSchema(prior);
            restorePriorToolRunSchema(prior);
            restoreOperationDispatchSchema(prior);

            // The tool identity tables this pair adds have no predecessor at
            // this source, so nothing durable can be seeded in them here and
            // the empty counts below are what proves they arrive created
            // rather than populated.
            removeToolIdentityPersistence(prior);
            dropJournalProposalTables(prior);
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
            == k_targetSchemaIdentity
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
            "framework.screen.capture",
            std::move(*arguments)
        );
        REQUIRE(invocation.has_value());
        auto call = toolCallAt(
            *root,
            nullptr,
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
        auto nextCall = toolCallAt(
            *root,
            nullptr,
            2U,
            call->executionIdentity(),
            *invocation
        );
        REQUIRE(nextCall.has_value());
        REQUIRE(prepared.store.persistToolRootRequest(*root).has_value());
        REQUIRE(prepared.store.persistToolCallPosition(*root, *call).has_value());
        auto proposed = prepared.store.replayToolCall(*root, *call);
        REQUIRE(proposed.has_value());
        CHECK(proposed->state == ToolCallState::Proposed);
        CHECK(proposed->revision == 1U);

        auto admission = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = *root,
                .call            = *call,
                .policyAuthority = prepared.policyAuthority,
            }
        );
        REQUIRE(admission.has_value());
        CHECK(admission->attemptNumber() == 1U);
        CHECK(admission->historyRevision() == 2U);
        auto repeatedAdmission = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = *root,
                .call            = *call,
                .policyAuthority = prepared.policyAuthority,
            }
        );
        REQUIRE(repeatedAdmission.has_value());
        CHECK(repeatedAdmission->attemptNumber() == 1U);
        CHECK(repeatedAdmission->historyRevision() == 2U);
        auto refusedNext = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = *root,
                .call            = *nextCall,
                .policyAuthority = prepared.policyAuthority,
            }
        );
        REQUIRE_FALSE(refusedNext.has_value());
        CHECK(refusedNext.error().message().contains(
            "no deterministic terminal outcome"
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
        auto admittedNext = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = *root,
                .call            = *nextCall,
                .policyAuthority = prepared.policyAuthority,
            }
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

    // A read-only Framework call is a LEAF -- provider code answers it -- but
    // it declares no effect, so an interrupted dispatch of one delivered
    // nothing and there is nothing for a restart to be uncertain about. It
    // survives as dispatching and is re-entered. What the restart does take
    // away is the capability: the history revision moves, so the token the dead
    // incarnation still holds answers nothing.
    TEST_CASE("restart supersedes an unanswered read-only dispatch without declaring it uncertain")
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
            "framework.screen.capture",
            std::move(*arguments)
        );
        REQUIRE(invocation.has_value());
        auto call = toolCallAt(
            *root,
            nullptr,
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
        auto admission = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = *root,
                .call            = *call,
                .policyAuthority = prepared.policyAuthority,
            }
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
        auto continuationCall = toolCallAt(
            *continuationRoot,
            nullptr,
            1U,
            call->executionIdentity(),
            *invocation
        );
        REQUIRE(continuationCall.has_value());
        auto priorAdmission = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = *continuationRoot,
                .call            = *continuationCall,
                .policyAuthority = prepared.policyAuthority,
            }
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
        CHECK(replay->state == ToolCallState::Dispatching);
        CHECK_FALSE(replay->payload.has_value());
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
        auto continuedAdmission = restarted->admitToolCall(
            ToolAdmissionRequest{
                .controller      = *resumed,
                .lease           = *lease,
                .root            = *continuationRoot,
                .call            = *continuationCall,
                .policyAuthority = prepared.policyAuthority,
            }
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
        auto refusedReadmission = restarted->admitToolCall(
            ToolAdmissionRequest{
                .controller      = *resumed,
                .lease           = *lease,
                .root            = *root,
                .call            = *call,
                .policyAuthority = prepared.policyAuthority,
            }
        );
        REQUIRE_FALSE(refusedReadmission.has_value());
        CHECK(refusedReadmission.error().message().contains(
            "cannot enter admission"
        ));

        // Re-admission is refused because re-entry is the door: the new
        // incarnation continues the dispatch the dead one began, and its answer
        // is the call's first and only recorded outcome.
        auto reentered = restarted->reenterToolCallDispatch(
            ToolAdmissionRequest{
                .controller      = *resumed,
                .lease           = *lease,
                .root            = *root,
                .call            = *call,
                .policyAuthority = prepared.policyAuthority,
            }
        );
        REQUIRE_MESSAGE(reentered.has_value(), reentered.error().message());
        auto observedFrame = CanonicalJson::parseExact(
            R"({"frame_id":"frame-after-restart"})"
        );
        REQUIRE(observedFrame.has_value());
        auto answered = restarted->completeToolCallDispatch(
            *reentered,
            ToolCallCompletion::confirmed(*observedFrame)
        );
        REQUIRE_MESSAGE(answered.has_value(), answered.error().message());
        CHECK(answered->state == ToolCallState::Confirmed);
    }

    // A Project handler's effects are recorded children, so its interrupted
    // dispatch remains replayable even when the descriptor is mutating.
    TEST_CASE(
        "restart keeps an unanswered mutating handler dispatching and barring its target"
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
        auto invocation = toolInvocation(
            prepared.project,
            prepared.project.toolName("command-1")
        );
        auto effects = std::vector{
            test_support::routineToolEffect(prepared.project),
        };
        auto const execution = ToolExecutionIdentity{
            .runIdentity                 = hashOf("mutating-crash-run"),
            .frameworkReleaseIdentity    = hashOf("mutating-crash-framework"),
            .toolRuntimeProtocolIdentity = hashOf("mutating-crash-protocol"),
            .environmentIdentity         = hashOf("mutating-crash-environment"),
        };
        auto call = toolCallAt(
            *root,
            nullptr,
            1U,
            execution,
            invocation
        );
        REQUIRE(call.has_value());
        auto admission = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = *root,
                .call            = *call,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = effects,
                },
            }
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
        CHECK(replay->state == ToolCallState::Dispatching);
        CHECK_FALSE(replay->payload.has_value());

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
        auto secondCall = toolCallAt(
            *secondRoot,
            nullptr,
            1U,
            execution,
            invocation
        );
        REQUIRE(secondCall.has_value());
        auto blocked = restarted->admitToolCall(
            ToolAdmissionRequest{
                .controller      = *resumed,
                .lease           = *lease,
                .root            = *secondRoot,
                .call            = *secondCall,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = effects,
                },
            }
        );
        REQUIRE_FALSE(blocked.has_value());
        CHECK(blocked.error().message().contains("state dispatching"));

        // The handler frame is replayed; any uncertain delivery belongs to a
        // recorded Framework child and must be reconciled at that child.
        auto explanation = CanonicalJson::parseExact(
            R"({"reason":"a query has no business here"})"
        );
        auto evidence = CanonicalJson::parseExact(
            R"({"snapshot_ref":"post-restart-evidence"})"
        );
        REQUIRE(explanation.has_value());
        REQUIRE(evidence.has_value());
        auto queried  = uint32{0};
        auto refused = restarted->reconcileMutatingToolCall(
            *resumed,
            *lease,
            *root,
            *call,
            [&queried, &explanation, &evidence](ToolCallPositionIdentity const&)
            {
                ++queried;
                return ToolCallReconciliation::confirmed(*explanation, *evidence);
            }
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains(
            "Only a possible mutating Tool call may be reconciled"
        ));
        CHECK(queried == 0U);

        // Re-entry is the door, and the terminal row it writes releases the
        // barrier.
        auto reentered = restarted->reenterToolCallDispatch(
            ToolAdmissionRequest{
                .controller      = *resumed,
                .lease           = *lease,
                .root            = *root,
                .call            = *call,
                .policyAuthority = prepared.policyAuthority,
            }
        );
        REQUIRE_MESSAGE(reentered.has_value(), reentered.error().message());
        auto result = CanonicalJson::parseExact(R"({"delivered":true})");
        REQUIRE(result.has_value());
        auto answered = restarted->completeToolCallDispatch(
            *reentered,
            ToolCallCompletion::confirmed(*result)
        );
        REQUIRE_MESSAGE(answered.has_value(), answered.error().message());
        CHECK(answered->state == ToolCallState::Confirmed);

        auto unblocked = restarted->admitToolCall(
            ToolAdmissionRequest{
                .controller      = *resumed,
                .lease           = *lease,
                .root            = *secondRoot,
                .call            = *secondCall,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = effects,
                },
            }
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
            "framework.screen.capture",
            std::move(*arguments)
        );
        REQUIRE(invocation.has_value());
        auto const execution = ToolExecutionIdentity{
            .runIdentity                 = hashOf("agent-tool-run"),
            .frameworkReleaseIdentity    = hashOf("agent-tool-framework"),
            .toolRuntimeProtocolIdentity = hashOf("agent-tool-protocol"),
            .environmentIdentity         = hashOf("agent-tool-environment"),
        };
        auto first = toolCallAt(
            *root,
            nullptr,
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
        auto second = toolCallAt(
            *secondRoot,
            nullptr,
            1U,
            execution,
            *invocation
        );
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());

        auto before = prepared.store.remainingBudget(agent);
        REQUIRE(before.has_value());
        CHECK(before->toolCalls == 1U);
        auto admission = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = agent,
                .lease           = *lease,
                .root            = *root,
                .call            = *first,
                .policyAuthority = prepared.policyAuthority,
            }
        );
        REQUIRE(admission.has_value());
        auto after = prepared.store.remainingBudget(agent);
        REQUIRE(after.has_value());
        CHECK(after->toolCalls == 0U);
        CHECK(after->observations == 0U);
        CHECK(after->mutations == 1U);

        auto repeated = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = agent,
                .lease           = *lease,
                .root            = *root,
                .call            = *first,
                .policyAuthority = prepared.policyAuthority,
            }
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
        auto exhausted = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = agent,
                .lease           = *replacementLease,
                .root            = *secondRoot,
                .call            = *second,
                .policyAuthority = prepared.policyAuthority,
            }
        );
        REQUIRE_FALSE(exhausted.has_value());
        CHECK(exhausted.error().message().contains(
            "tool-call budget is exhausted"
        ));
        auto secondReplay = prepared.store.replayToolCall(*secondRoot, *second);
        REQUIRE(secondReplay.has_value());
        CHECK(secondReplay->state == ToolCallState::Proposed);
    }

    // The descriptor inside the coordinate is the only statement of whether a
    // Tool mutates, so a request cannot disagree with it -- it can only fail to
    // carry what that answer requires. Both directions are refused: admitting a
    // mutating Tool with no mutation proposal would evaluate no policy at all,
    // and a read-only Tool carrying one would evaluate policy over effects no
    // catalog entry declares.
    TEST_CASE("Tool admission requires the mutation the descriptor implies")
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
        auto invocation = toolInvocation(
            prepared.project,
            prepared.project.toolName("command-1")
        );
        REQUIRE(invocation.descriptor().mutability == ToolMutability::Mutating);
        auto call = toolCallAt(
            *root,
            nullptr,
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
        auto refusedBare = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = *root,
                .call            = *call,
                .policyAuthority = prepared.policyAuthority,
            }
        );
        REQUIRE_FALSE(refusedBare.has_value());
        CHECK(refusedBare.error().message().contains(
            "requires a mutation proposal"
        ));

        auto catalog   = FrameworkToolCatalogOwner::create();
        auto arguments = CanonicalJson::parseExact("{}");
        REQUIRE(catalog.has_value());
        REQUIRE(arguments.has_value());
        auto observe = catalog->validate(
            "framework.screen.capture",
            std::move(*arguments)
        );
        REQUIRE(observe.has_value());
        REQUIRE(observe->descriptor().mutability == ToolMutability::ReadOnly);
        auto readOnlyCall = toolCallAt(
            *root,
            nullptr,
            2U,
            call->executionIdentity(),
            *observe
        );
        REQUIRE(readOnlyCall.has_value());
        auto const effects = std::vector{
            test_support::routineToolEffect(prepared.project),
        };
        auto refusedProposal = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = *root,
                .call            = *readOnlyCall,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = effects,
                },
            }
        );
        REQUIRE_FALSE(refusedProposal.has_value());
        CHECK(refusedProposal.error().message().contains(
            "cannot carry a mutation proposal"
        ));
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
        auto invocation = toolInvocation(
            prepared.project,
            prepared.project.toolName("command-1")
        );
        auto effects = std::vector{
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
        auto firstCall = toolCallAt(
            *firstRoot,
            nullptr,
            1U,
            execution,
            invocation
        );
        REQUIRE(firstCall.has_value());
        auto admitted = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = agent,
                .lease           = *lease,
                .root            = *firstRoot,
                .call            = *firstCall,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = effects,
                },
            }
        );
        REQUIRE(admitted.has_value());
        auto afterFirst = prepared.store.remainingBudget(agent);
        REQUIRE(afterFirst.has_value());
        CHECK(afterFirst->toolCalls == 1U);
        CHECK(afterFirst->mutations == 0U);
        CHECK(afterFirst->observations == 2U);

        auto repeated = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = agent,
                .lease           = *lease,
                .root            = *firstRoot,
                .call            = *firstCall,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = effects,
                },
            }
        );
        REQUIRE(repeated.has_value());
        CHECK(repeated->attemptNumber() == admitted->attemptNumber());
        auto afterRepeat = prepared.store.remainingBudget(agent);
        REQUIRE(afterRepeat.has_value());
        CHECK(afterRepeat->toolCalls == 1U);
        CHECK(afterRepeat->mutations == 0U);

        auto dispatch = prepared.store.beginToolCallDispatch(*admitted);
        REQUIRE(dispatch.has_value());
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
        auto secondCall = toolCallAt(
            *secondRoot,
            nullptr,
            1U,
            execution,
            invocation
        );
        REQUIRE(secondCall.has_value());
        auto exhausted = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = agent,
                .lease           = *lease,
                .root            = *secondRoot,
                .call            = *secondCall,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = effects,
                },
            }
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
        auto invocation = toolInvocation(
            prepared.project,
            prepared.project.toolName("command-1")
        );
        auto call = toolCallAt(
            *root,
            nullptr,
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
        auto effects = std::vector{
            test_support::routineToolEffect(prepared.project),
        };
        auto expected = deriveEffectiveEffectEnvelope(
            std::vector<ProposedEffect>{effects.begin(), effects.end()}
        );
        REQUIRE(expected.has_value());
        auto admitted = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = *root,
                .call            = *call,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = effects,
                },
            }
        );
        REQUIRE(admitted.has_value());

        auto changedEffects = effects;
        changedEffects.front().scopeKey = "another-fixture-instance";
        auto changed = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = *root,
                .call            = *call,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = changedEffects,
                },
            }
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
        auto invocation = toolInvocation(
            prepared.project,
            prepared.project.toolName("command-1")
        );
        auto call = toolCallAt(
            *root,
            nullptr,
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
        auto effects = std::vector{effect};
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
        auto refused = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = *root,
                .call            = *call,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = effects,
                },
            }
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
            prepared.policyAuthority,
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
            prepared.policyAuthority,
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
            prepared.policyAuthority,
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
            prepared.policyAuthority,
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
        auto forged = std::vector{
            ToolApprovalGrant{
                .token = approval->token,
                .authorityDecisionId = AuthorityDecisionId{"another-decision"},
            },
        };
        auto mismatched = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = *root,
                .call            = *call,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects   = effects,
                    .approvals = forged,
                },
            }
        );
        REQUIRE_FALSE(mismatched.has_value());
        CHECK(mismatched.error().message().contains(
            "stale, expired, mismatched, or already consumed"
        ));

        auto approvals = std::vector{*approval};
        auto admitted = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = *root,
                .call            = *call,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects   = effects,
                    .approvals = approvals,
                },
            }
        );
        REQUIRE(admitted.has_value());
        auto repeated = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = *root,
                .call            = *call,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects   = effects,
                    .approvals = approvals,
                },
            }
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
        auto secondCall = toolCallAt(
            *secondRoot,
            nullptr,
            1U,
            call->executionIdentity(),
            invocation
        );
        REQUIRE(secondCall.has_value());
        auto reused = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = *secondRoot,
                .call            = *secondCall,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects   = effects,
                    .approvals = approvals,
                },
            }
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
            prepared.policyAuthority,
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
        auto leaseBoundApprovals = std::vector{*leaseBoundApproval};
        auto staleLeaseApproval = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = *replacementLease,
                .root            = *secondRoot,
                .call            = *secondCall,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects   = effects,
                    .approvals = leaseBoundApprovals,
                },
            }
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
                "framework.screen.capture",
                std::move(*arguments)
            );
            REQUIRE(invocation.has_value());
            auto call = toolCallAt(
                *root,
                nullptr,
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
            auto admitted = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller      = prepared.controller,
                    .lease           = prepared.lease,
                    .root            = *root,
                    .call            = *call,
                    .policyAuthority = prepared.policyAuthority,
                }
            );
            REQUIRE(admitted.has_value());
        }

        auto oldRows        = std::vector<std::vector<std::string>>{};
        auto sourceIdentity = std::string{};
        {
            auto prior = test_support::OperatorDatabaseProbe{databasePath};
            restoreGenesisGenerationSchema(prior);
            restoreProjectStateInterpretationSchema(prior);
            restoreOperationSurfaceSchema(prior);
            restorePriorToolRunSchema(prior);
            restoreOperationDispatchSchema(prior);
            oldRows = prior.readRows(
                "SELECT call_identity, attempt_number, root_identity, "
                "policy_hash, budget_snapshot_hash FROM tool_admission_attempts"
            );
            restorePriorToolAdmissionAuthority(prior);
            dropJournalProposalTables(prior);
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
            == k_targetSchemaIdentity
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
                    std::string{k_targetSchemaIdentity},
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
        auto invocation = toolInvocation(
            prepared.project,
            prepared.project.toolName("command-1")
        );
        auto call = toolCallAt(
            *root,
            nullptr,
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
        auto effects = std::vector{effect};
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
            prepared.policyAuthority,
            effects,
            ToolApprovalRequest{
                .approverCapability  = "approve",
                .expiresAtUnixMillis = expiresAt,
            },
            AuthorityDecisionId{"expiry-decision"}
        );
        REQUIRE(approval.has_value());
        auto approvals = std::vector{*approval};
        auto admitted = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = *root,
                .call            = *call,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects   = effects,
                    .approvals = approvals,
                },
            }
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

    TEST_CASE("the immediate-prior nested-call schema migrates call rows exactly")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        {
            auto prepared = prepareStore(temporary.path());
            auto preimage = CanonicalJson::parseExact(
                R"({"objective":"nested-schema-migration"})"
            );
            REQUIRE(preimage.has_value());
            auto root = ToolRootRequestIdentity::create(
                "controller-1",
                "nested-schema-migration",
                std::move(*preimage)
            );
            REQUIRE(root.has_value());
            auto invocation = toolInvocation(
                prepared.project,
                prepared.project.toolName("command-1")
            );
            auto call = toolCallAt(
                *root,
                nullptr,
                1U,
                ToolExecutionIdentity{
                    .runIdentity = hashOf("nested-migration-run"),
                    .frameworkReleaseIdentity =
                        hashOf("nested-migration-framework"),
                    .toolRuntimeProtocolIdentity =
                        hashOf("nested-migration-protocol"),
                    .environmentIdentity =
                        hashOf("nested-migration-environment"),
                },
                invocation
            );
            REQUIRE(call.has_value());
            auto effects = std::vector{
                test_support::routineToolEffect(prepared.project),
            };
            auto admitted = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller      = prepared.controller,
                    .lease           = prepared.lease,
                    .root            = *root,
                    .call            = *call,
                    .policyAuthority = prepared.policyAuthority,
                    .mutation   = ToolAdmissionRequest::Mutation{
                        .effects         = effects,
                    },
                }
            );
            REQUIRE(admitted.has_value());
        }

        auto positionRows   = std::vector<std::vector<std::string>>{};
        auto attemptRows    = std::vector<std::vector<std::string>>{};
        auto sourceIdentity = std::string{};
        {
            auto prior   = test_support::OperatorDatabaseProbe{databasePath};
            restoreGenesisGenerationSchema(prior);
            restoreProjectStateInterpretationSchema(prior);
            restoreOperationSurfaceSchema(prior);
            restorePriorToolRunSchema(prior);
            restoreOperationDispatchSchema(prior);
            positionRows = prior.readRows(
                "SELECT call_identity, root_identity, call_sequence, "
                "canonical_args FROM tool_call_positions"
            );
            attemptRows = prior.readRows(
                "SELECT call_identity, attempt_number, origin_principal_id, "
                "execution_principal_id FROM tool_admission_attempts"
            );
            restorePriorNestedToolCallSchema(prior);
            dropJournalProposalTables(prior);
            sourceIdentity = exactSchemaIdentity(prior);
        }
        REQUIRE_FALSE(positionRows.empty());
        REQUIRE_FALSE(attemptRows.empty());
        CHECK(
            sourceIdentity
            == "sha256:53c56cce2064c47a07bd29529320aac7e7f8f4e8c01a74dc54da936159dd44f8"
        );

        {
            auto migrated = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(migrated.has_value(), migrated.error().message());
        }
        auto target = test_support::OperatorDatabaseProbe{databasePath};
        CHECK(
            target.readRows(
                "SELECT call_identity, root_identity, call_sequence, "
                "canonical_args FROM tool_call_positions"
            ) == positionRows
        );
        CHECK(
            target.readRows(
                "SELECT call_identity, attempt_number, origin_principal_id, "
                "execution_principal_id FROM tool_admission_attempts"
            ) == attemptRows
        );

        // The observation reference is backfilled absent. Nested-call storage
        // does not survive the leaf migration at all.
        CHECK(
            target.readRows(
                "SELECT count(*) FROM tool_call_positions "
                "WHERE observation_reference_hash IS NOT NULL"
            ) == std::vector<std::vector<std::string>>{{"0"}}
        );
        CHECK(
            target.readRows(
                "SELECT count(*) FROM pragma_table_info('tool_admission_attempts') "
                "WHERE name='delegation_grant_id'"
            ) == std::vector<std::vector<std::string>>{{"0"}}
        );
        CHECK(
            target.readRows(
                "SELECT count(*) FROM sqlite_schema WHERE type='table' "
                "AND name='tool_delegation_grants'"
            )
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

    TEST_CASE("the nested-call schema migrates to Project Tool leaf storage exactly")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        {
            auto prepared = prepareStore(temporary.path());
            auto preimage = CanonicalJson::parseExact(
                R"({"objective":"leaf-schema-migration"})"
            );
            REQUIRE(preimage.has_value());
            auto root = ToolRootRequestIdentity::create(
                "controller-1",
                "leaf-schema-migration",
                std::move(*preimage)
            );
            REQUIRE(root.has_value());
            auto invocation = toolInvocation(
                prepared.project,
                prepared.project.toolName("command-1")
            );
            auto call = toolCallAt(
                *root,
                nullptr,
                1U,
                ToolExecutionIdentity{
                    .runIdentity = hashOf("leaf-migration-run"),
                    .frameworkReleaseIdentity =
                        hashOf("leaf-migration-framework"),
                    .toolRuntimeProtocolIdentity =
                        hashOf("leaf-migration-protocol"),
                    .environmentIdentity =
                        hashOf("leaf-migration-environment"),
                },
                invocation
            );
            REQUIRE(call.has_value());
            auto effects = std::vector{
                test_support::routineToolEffect(prepared.project),
            };
            auto admitted = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller      = prepared.controller,
                    .lease           = prepared.lease,
                    .root            = *root,
                    .call            = *call,
                    .policyAuthority = prepared.policyAuthority,
                    .mutation = ToolAdmissionRequest::Mutation{
                        .effects = effects,
                    },
                }
            );
            REQUIRE(admitted.has_value());
        }

        auto attemptRows = std::vector<std::vector<std::string>>{};
        {
            auto nested = test_support::OperatorDatabaseProbe{databasePath};
            restoreLegacyNestedToolCallSchema(nested);
            attemptRows = nested.readRows(
                "SELECT call_identity, attempt_number, origin_principal_id, "
                "execution_principal_id FROM tool_admission_attempts"
            );
            CHECK(
                exactSchemaIdentity(nested)
                == "sha256:045925eefabef97b964f6a21db0da81cdc6a2c293c21e7f495011fe3d1b9277f"
            );
        }
        REQUIRE_FALSE(attemptRows.empty());

        {
            auto migrated = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(migrated.has_value(), migrated.error().message());
        }
        auto target = test_support::OperatorDatabaseProbe{databasePath};
        CHECK(
            target.readRows(
                "SELECT call_identity, attempt_number, origin_principal_id, "
                "execution_principal_id FROM tool_admission_attempts"
            ) == attemptRows
        );
        CHECK(
            target.readRows(
                "SELECT count(*) FROM pragma_table_info('tool_admission_attempts') "
                "WHERE name='delegation_grant_id'"
            ) == std::vector<std::vector<std::string>>{{"0"}}
        );
        CHECK(
            target.readRows(
                "SELECT count(*) FROM sqlite_schema WHERE type='table' "
                "AND name='tool_delegation_grants'"
            )
            == std::vector<std::vector<std::string>>{{"0"}}
        );
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
            auto invocation = toolInvocation(
                prepared.project,
                prepared.project.toolName("command-1")
            );
            auto call = toolCallAt(
                *root,
                nullptr,
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
            auto effects = std::vector{
                test_support::routineToolEffect(prepared.project),
            };
            auto admitted = prepared.store.admitToolCall(
                ToolAdmissionRequest{
                    .controller      = prepared.controller,
                    .lease           = prepared.lease,
                    .root            = *root,
                    .call            = *call,
                    .policyAuthority = prepared.policyAuthority,
                    .mutation   = ToolAdmissionRequest::Mutation{
                        .effects         = effects,
                    },
                }
            );
            REQUIRE(admitted.has_value());
        }

        auto authorityRows  = std::vector<std::vector<std::string>>{};
        auto sourceIdentity = std::string{};
        {
            auto prior = test_support::OperatorDatabaseProbe{databasePath};
            restoreGenesisGenerationSchema(prior);
            restoreProjectStateInterpretationSchema(prior);
            restoreOperationSurfaceSchema(prior);
            restorePriorToolRunSchema(prior);
            restoreOperationDispatchSchema(prior);
            authorityRows = prior.readRows(
                "SELECT call_identity, attempt_number, effect_envelope, "
                "effect_envelope_hash, required_approvals FROM "
                "tool_admission_attempts"
            );
            restorePriorToolApprovalSchema(prior);
            dropJournalProposalTables(prior);
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

    TEST_CASE("null-rooted Tool call positions migrate onto their root request")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        auto preimage           = CanonicalJson::parseExact("{}");
        REQUIRE(preimage.has_value());
        auto root = ToolRootRequestIdentity::create(
            "root-position-principal",
            "root-position-request",
            std::move(*preimage)
        );
        REQUIRE(root.has_value());
        auto catalog   = FrameworkToolCatalogOwner::create();
        auto arguments = CanonicalJson::parseExact("{}");
        REQUIRE(catalog.has_value());
        REQUIRE(arguments.has_value());
        auto invocation = catalog->validate(
            "framework.screen.capture",
            std::move(*arguments)
        );
        REQUIRE(invocation.has_value());
        auto call = toolCallAt(
            *root,
            nullptr,
            1U,
            ToolExecutionIdentity{
                .runIdentity                 = hashOf("root-position-run"),
                .frameworkReleaseIdentity    = hashOf("root-position-framework"),
                .toolRuntimeProtocolIdentity = hashOf("root-position-protocol"),
                .environmentIdentity         = hashOf("root-position-environment"),
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
            restoreGenesisGenerationSchema(prior);
            restoreProjectStateInterpretationSchema(prior);
            restoreOperationSurfaceSchema(prior);
            restorePriorToolRunSchema(prior);
            restoreOperationDispatchSchema(prior);
            identityRows = prior.readRows(
                "SELECT call_identity, root_identity, call_sequence, "
                "canonical_args FROM tool_call_positions"
            );
            restorePriorNullRootedToolCallPositions(prior);
            dropJournalProposalTables(prior);
            sourceIdentity = exactSchemaIdentity(prior);

            // The generation this reproduces really did store the run's own
            // call with no parent, which is the reading that goes away.
            CHECK(
                prior.readRows(
                    "SELECT count(*) FROM tool_call_positions "
                    "WHERE parent_call_identity IS NULL"
                ) == std::vector<std::vector<std::string>>{{"1"}}
            );
        }
        CHECK(
            sourceIdentity
            == "sha256:6caa3b9a5f712571a59242bb9a7c34277e6f9e7624fcf1102f74846f46f7631c"
        );

        {
            auto migrated = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(migrated.has_value(), migrated.error().message());
        }
        {
            auto target = test_support::OperatorDatabaseProbe{databasePath};
            CHECK(
                exactSchemaIdentity(target)
                == k_targetSchemaIdentity
            );
            CHECK(
                target.readRows(
                    "SELECT call_identity, root_identity, call_sequence, "
                    "canonical_args FROM tool_call_positions"
                ) == identityRows
            );
            CHECK(
                target.readRows(
                    "SELECT count(*) FROM tool_call_positions position "
                    "JOIN tool_root_requests request "
                    "ON request.root_identity=position.parent_call_identity"
                ) == std::vector<std::vector<std::string>>{{"1"}}
            );
            CHECK(
                target.readRows(
                    "SELECT count(*) FROM sqlite_schema WHERE type='index' "
                    "AND name='one_top_level_tool_call_position'"
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
                        std::string{k_targetSchemaIdentity},
                    },
                }
            );
        }

        // The migrated row is the row this generation would have written, so
        // the same position rejoins it and replays rather than diverging.
        auto migrated = OperatorCoordinator::open(production);
        REQUIRE(migrated.has_value());
        auto restored = migrated->persistToolCallPosition(*root, *call);
        REQUIRE_MESSAGE(restored.has_value(), restored.error().message());
        CHECK(restored->lookup == ToolIdentityLookup::Existing);
        auto replay = migrated->replayToolCall(*root, *call);
        REQUIRE(replay.has_value());
        CHECK(replay->state == ToolCallState::Proposed);
    }

    // The pair that gave a run its own durable state and retired the `rejected`
    // Tool call state. It is the newest generation, so its source identity is
    // the schema the immediately prior generation created, and the fixture
    // winds a fresh database back by rebuilding exactly the two tables that
    // moved. Both halves are proved: the wind-back must reproduce that
    // identity, and the recorded run must read back across the upgrade with
    // `running` written onto it.
    TEST_CASE("a run's state and the retired rejected state migrate under their exact pair")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        auto rootIdentity = std::string{};
        auto callIdentity = std::string{};
        {
            auto prepared = prepareStore(temporary.path());
            auto preimage = CanonicalJson::parseExact(
                R"({"objective":"run-state-migration"})"
            );
            REQUIRE(preimage.has_value());
            auto root = ToolRootRequestIdentity::create(
                "controller-1",
                "run-state-migration-request",
                std::move(*preimage)
            );
            REQUIRE(root.has_value());
            auto catalog   = FrameworkToolCatalogOwner::create();
            auto arguments = CanonicalJson::parseExact("{}");
            REQUIRE(catalog.has_value());
            REQUIRE(arguments.has_value());
            auto invocation = catalog->validate(
                "framework.screen.capture",
                std::move(*arguments)
            );
            REQUIRE(invocation.has_value());
            auto call = toolCallAt(
                *root,
                nullptr,
                1U,
                ToolExecutionIdentity{
                    .runIdentity              = hashOf("run-state-run"),
                    .frameworkReleaseIdentity = hashOf("run-state-framework"),
                    .toolRuntimeProtocolIdentity =
                        hashOf("run-state-protocol"),
                    .environmentIdentity = hashOf("run-state-environment"),
                },
                *invocation
            );
            REQUIRE(call.has_value());
            rootIdentity = root->identity().hex();
            callIdentity = call->identity().hex();
            REQUIRE(prepared.store.persistToolRootRequest(*root).has_value());
            REQUIRE(
                prepared.store.persistToolCallPosition(*root, *call).has_value()
            );
        }

        auto sourceIdentity = std::string{};
        auto rootRows       = std::vector<std::vector<std::string>>{};
        auto historyRows    = std::vector<std::vector<std::string>>{};
        {
            auto prior = test_support::OperatorDatabaseProbe{databasePath};
            restoreGenesisGenerationSchema(prior);
            restoreProjectStateInterpretationSchema(prior);
            restoreOperationSurfaceSchema(prior);
            restorePriorToolRunSchema(prior);
            rootRows = prior.readRows(
                "SELECT root_identity, caller_namespace, request_key, "
                "request_preimage_hash FROM tool_root_requests"
            );
            historyRows = prior.readRows(
                "SELECT call_identity, mutating, state, revision "
                "FROM tool_call_history"
            );
            sourceIdentity = exactSchemaIdentity(prior);
        }
        REQUIRE(rootRows.size() == 1U);
        REQUIRE(historyRows.size() == 1U);
        CHECK_MESSAGE(
            sourceIdentity
                == "sha256:f34626677a80bbf2436bd7bb476385e5f5d142c8b07882e0481946f73f41d2df",
            "the fixture must reproduce the exact identity this pair migrates from"
        );

        {
            auto migrated = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(migrated.has_value(), migrated.error().message());
        }
        {
            auto target = test_support::OperatorDatabaseProbe{databasePath};
            CHECK(
                exactSchemaIdentity(target)
                == k_targetSchemaIdentity
            );

            // The run came across, and it came across running: nothing in a
            // database written before the column existed could have terminated
            // a run, because nothing could write a termination.
            CHECK(
                target.readRows(
                    "SELECT root_identity, caller_namespace, request_key, "
                    "request_preimage_hash FROM tool_root_requests"
                ) == rootRows
            );
            CHECK(
                target.readRows(
                    "SELECT state, coalesce(termination_reason, '') "
                    "FROM tool_root_requests WHERE root_identity='"
                    + rootIdentity + "'"
                )
                == std::vector<std::vector<std::string>>{{"running", ""}}
            );
            CHECK(
                target.readRows(
                    "SELECT call_identity, mutating, state, revision "
                    "FROM tool_call_history"
                ) == historyRows
            );

            // `rejected` is gone from the vocabulary the row may carry, so the
            // rebuilt CHECK is what refuses it rather than a comment.
            CHECK(
                target.readRows(
                    "SELECT count(*) FROM sqlite_schema WHERE type='table' "
                    "AND name='tool_call_history' AND sql LIKE '%rejected%'"
                )
                == std::vector<std::vector<std::string>>{{"0"}}
            );
            CHECK(
                target.readRows(
                    "SELECT source_identity, target_identity FROM "
                    "schema_identity_transitions WHERE source_identity='"
                    + sourceIdentity + "'"
                )
                == std::vector<std::vector<std::string>>{
                    {
                        sourceIdentity,
                        std::string{k_targetSchemaIdentity},
                    },
                }
            );
            CHECK(callIdentity == historyRows.front().front());
        }
    }

    // The pair that deleted the Operation dispatch spine. Its five tables had
    // no writer left once step minting was gone, so the migration drops them
    // and rebuilds ledger_events without the delivery_outcome_recorded arm the
    // dropped writers were the only producers of. Every other row survives.
    TEST_CASE("the Operation dispatch tables are dropped under their exact pair")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        auto rootIdentity = std::string{};
        auto callIdentity = std::string{};
        {
            auto prepared      = prepareStore(temporary.path());
            auto const started = startToolCall(
                prepared,
                "dispatch-removal-request",
                prepared.project.toolName("command-1")
            );

            // Settled before the store is dropped. A dispatching mutating call
            // is exactly what the restart sweep exists to reclassify, and its
            // revision is the sweep's to move -- comparing that number across
            // the reopen would be reading what the restart owns and calling it
            // migration damage.
            confirmToolCall(prepared, started);
            rootIdentity = started.root.identity().hex();
            callIdentity = started.call.identity().hex();
        }

        auto sourceIdentity = std::string{};
        auto rootRows       = std::vector<std::vector<std::string>>{};
        auto historyRows    = std::vector<std::vector<std::string>>{};
        auto eventRows      = std::vector<std::vector<std::string>>{};
        {
            auto prior = test_support::OperatorDatabaseProbe{databasePath};
            restoreGenesisGenerationSchema(prior);
            restoreProjectStateInterpretationSchema(prior);
            restoreOperationSurfaceSchema(prior);
            restorePriorToolRunSchema(prior);
            restoreOperationDispatchSchema(prior);
            rootRows = prior.readRows(
                "SELECT root_identity, caller_namespace, request_key, "
                "request_preimage_hash FROM tool_root_requests"
            );
            historyRows = prior.readRows(
                "SELECT call_identity, mutating, state, revision "
                "FROM tool_call_history"
            );
            eventRows = prior.readRows(
                "SELECT kind, subject_id FROM ledger_events ORDER BY sequence"
            );
            dropJournalProposalTables(prior);
            sourceIdentity = exactSchemaIdentity(prior);
        }
        REQUIRE(rootRows.size() == 1U);
        REQUIRE(historyRows.size() == 1U);
        REQUIRE_FALSE(eventRows.empty());
        CHECK(rootIdentity == rootRows.front().front());
        CHECK(callIdentity == historyRows.front().front());
        CHECK_MESSAGE(
            sourceIdentity
                == "sha256:5c0e9a22691b36600cf861157a8b08385e4ec546a86ad0c29849adc5584014ee",
            "the fixture must reproduce the exact identity this pair migrates from"
        );

        {
            auto migrated = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(migrated.has_value(), migrated.error().message());
        }
        {
            auto target = test_support::OperatorDatabaseProbe{databasePath};
            CHECK(
                exactSchemaIdentity(target)
                == k_targetSchemaIdentity
            );

            // The five tables are gone rather than emptied.
            CHECK(
                target.readRows(
                    "SELECT name FROM sqlite_schema WHERE type='table' AND "
                    "name IN ('operation_plans', 'authority_decisions', "
                    "'dispatches', 'operation_steps', 'approvals')"
                )
                    .empty()
            );

            // Everything the dropped tables did not own survives.
            CHECK(
                target.readRows(
                    "SELECT root_identity, caller_namespace, request_key, "
                    "request_preimage_hash FROM tool_root_requests"
                ) == rootRows
            );
            CHECK(
                target.readRows(
                    "SELECT call_identity, mutating, state, revision "
                    "FROM tool_call_history"
                ) == historyRows
            );
            CHECK(
                target.readRows(
                    "SELECT kind, subject_id FROM ledger_events ORDER BY sequence"
                ) == eventRows
            );
            CHECK(
                target.readRows(
                    "SELECT source_identity, target_identity FROM "
                    "schema_identity_transitions WHERE source_identity='"
                    + sourceIdentity + "'"
                )
                == std::vector<std::vector<std::string>>{
                    {
                        sourceIdentity,
                        std::string{k_targetSchemaIdentity},
                    },
                }
            );
        }
    }

    // The pair in which the framework stopped interpreting a Project's state.
    // The Journal, the materialized ProjectState, the call-bound proposals,
    // the baseline event a ProjectInstance named and the ProjectState revision
    // and digest the observation and snapshot rows carried are all deleted,
    // because each of them was the framework's own reading of what a Project's
    // world holds.
    //
    // Its source is the immediately prior generation, so the fixture winds a
    // fresh database back with restoreProjectStateInterpretationSchema alone:
    // nothing else moved in this pair.
    TEST_CASE("the ProjectState interpretation is dropped under its exact pair")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        auto instanceRows    = std::vector<std::vector<std::string>>{};
        auto observationRows = std::vector<std::vector<std::string>>{};
        auto snapshotRows    = std::vector<std::vector<std::string>>{};
        {
            auto prepared = prepareStore(temporary.path());
            static_cast<void>(prepared);
        }

        auto sourceIdentity = std::string{};
        {
            auto prior = test_support::OperatorDatabaseProbe{databasePath};
            restoreGenesisGenerationSchema(prior);
            restoreProjectStateInterpretationSchema(prior);
            instanceRows = prior.readRows(
                "SELECT plugin_id, project_instance_key, "
                "project_registration_hash FROM project_instances"
            );
            observationRows = prior.readRows(
                "SELECT plugin_id, project_instance_key, revision, "
                "observation_hash FROM project_observations ORDER BY revision"
            );
            snapshotRows = prior.readRows(
                "SELECT token, identity_hash, decision_basis_hash FROM snapshots "
                "ORDER BY snapshot_revision"
            );
            sourceIdentity = exactSchemaIdentity(prior);
        }
        REQUIRE(instanceRows.size() == 1U);
        REQUIRE_FALSE(observationRows.empty());
        REQUIRE_FALSE(snapshotRows.empty());
        CHECK_MESSAGE(
            sourceIdentity
                == k_projectStateInterpretationIdentity,
            "the fixture must reproduce the exact identity this pair migrates from"
        );

        {
            auto migrated = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(migrated.has_value(), migrated.error().message());
        }
        {
            auto target = test_support::OperatorDatabaseProbe{databasePath};
            auto const targetIdentity = exactSchemaIdentity(target);
            CHECK(targetIdentity == k_targetSchemaIdentity);

            // The five tables are gone rather than emptied, and the three
            // columns that carried the framework's reading are gone from the
            // tables that survive.
            CHECK(
                target.readRows(
                    "SELECT count(*) FROM sqlite_schema WHERE type='table' AND "
                    "name IN ('journal_events', 'project_state', "
                    "'journal_batch_proposals', 'journal_proposal_events', "
                    "'journal_proposal_effects')"
                ) == std::vector<std::vector<std::string>>{{"0"}}
            );
            CHECK(
                target.readRows(
                    "SELECT count(*) FROM pragma_table_info('project_instances') "
                    "WHERE name='baseline_event_id'"
                ) == std::vector<std::vector<std::string>>{{"0"}}
            );
            CHECK(
                target.readRows(
                    "SELECT count(*) FROM "
                    "pragma_table_info('project_observations') WHERE name IN "
                    "('project_state_revision', 'project_state_hash')"
                ) == std::vector<std::vector<std::string>>{{"0"}}
            );
            CHECK(
                target.readRows(
                    "SELECT count(*) FROM pragma_table_info('snapshots') "
                    "WHERE name='project_state_revision'"
                ) == std::vector<std::vector<std::string>>{{"0"}}
            );

            // Everything the deleted reading did not own comes across row for
            // row.
            CHECK(
                target.readRows(
                    "SELECT plugin_id, project_instance_key, "
                    "project_registration_hash FROM project_instances"
                ) == instanceRows
            );
            CHECK(
                target.readRows(
                    "SELECT plugin_id, project_instance_key, revision, "
                    "observation_hash FROM project_observations ORDER BY revision"
                ) == observationRows
            );
            CHECK(
                target.readRows(
                    "SELECT token, identity_hash, decision_basis_hash "
                    "FROM snapshots ORDER BY snapshot_revision"
                ) == snapshotRows
            );
            CHECK(
                target.readRows(
                    "SELECT source_identity, target_identity FROM "
                    "schema_identity_transitions WHERE source_identity='"
                    + sourceIdentity + "'"
                )
                == std::vector<std::vector<std::string>>{
                    {sourceIdentity, targetIdentity},
                }
            );
        }
    }

    // The pair that took the Operation surface out of the ledger entirely.
    // operations had one writer and four readers, all deleted together;
    // reconciliations went with it because its only non-audit column was a NOT
    // NULL reference into it; and the two nullable references into it that
    // journal_events and external_input_findings carried were columns no value
    // could ever occupy. ledger_events loses the detail column and the two
    // kinds that named an Operation as their subject.
    //
    // Its source is the generation immediately before that removal, so the
    // fixture winds a fresh database back with the state-cut restore -- which
    // every pair now ends in -- and restoreOperationSurfaceSchema, and nothing
    // else moved in this pair.
    TEST_CASE("the Operation surface is dropped under its exact pair")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        auto rootIdentity = std::string{};
        auto callIdentity = std::string{};
        {
            auto prepared      = prepareStore(temporary.path());
            auto const started = startToolCall(
                prepared,
                "operation-surface-removal-request",
                prepared.project.toolName("command-1")
            );
            rootIdentity = started.root.identity().hex();
            callIdentity = started.call.identity().hex();
        }

        auto sourceIdentity = std::string{};
        auto rootRows       = std::vector<std::vector<std::string>>{};
        auto positionRows   = std::vector<std::vector<std::string>>{};
        auto eventRows      = std::vector<std::vector<std::string>>{};
        {
            auto prior = test_support::OperatorDatabaseProbe{databasePath};
            restoreGenesisGenerationSchema(prior);
            restoreProjectStateInterpretationSchema(prior);
            restoreOperationSurfaceSchema(prior);
            rootRows = prior.readRows(
                "SELECT root_identity, caller_namespace, request_key, "
                "request_preimage_hash FROM tool_root_requests"
            );
            positionRows = prior.readRows(
                "SELECT call_identity, root_identity, call_sequence, "
                "canonical_args FROM tool_call_positions"
            );
            eventRows = prior.readRows(
                "SELECT kind, controlled_target_id, subject_id FROM ledger_events "
                "ORDER BY sequence"
            );
            sourceIdentity = exactSchemaIdentity(prior);
        }
        REQUIRE(rootRows.size() == 1U);
        REQUIRE(positionRows.size() == 1U);
        REQUIRE_FALSE(eventRows.empty());
        CHECK(rootIdentity == rootRows.front().front());
        CHECK(callIdentity == positionRows.front().front());
        CHECK_MESSAGE(
            sourceIdentity
                == "sha256:bd087692cab06397a98d74e60c7f8e792e7f8f7195e73960daefa1be60c3c62d",
            "the fixture must reproduce the exact identity this pair migrates from"
        );

        {
            auto migrated = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(migrated.has_value(), migrated.error().message());
        }
        {
            auto target = test_support::OperatorDatabaseProbe{databasePath};
            CHECK(
                exactSchemaIdentity(target)
                == k_targetSchemaIdentity
            );

            // The two tables are gone rather than emptied, and the columns that
            // referenced them are gone from the tables that survive.
            CHECK(
                target.readRows(
                    "SELECT count(*) FROM sqlite_schema WHERE type='table' "
                    "AND name IN ('operations', 'reconciliations')"
                ) == std::vector<std::vector<std::string>>{{"0"}}
            );
            CHECK(
                target.readRows(
                    "SELECT count(*) FROM "
                    "pragma_table_info('external_input_findings') "
                    "WHERE name='operation_id'"
                ) == std::vector<std::vector<std::string>>{{"0"}}
            );
            CHECK(
                target.readRows(
                    "SELECT count(*) FROM pragma_table_info('ledger_events') "
                    "WHERE name='detail'"
                ) == std::vector<std::vector<std::string>>{{"0"}}
            );

            // Everything the dropped surface did not own comes across byte for
            // byte, the ledger events included: no row of this database names
            // an Operation as its subject, so the rebuilt CHECK discards none
            // of them.
            CHECK(
                target.readRows(
                    "SELECT root_identity, caller_namespace, request_key, "
                    "request_preimage_hash FROM tool_root_requests"
                ) == rootRows
            );
            CHECK(
                target.readRows(
                    "SELECT call_identity, root_identity, call_sequence, "
                    "canonical_args FROM tool_call_positions"
                ) == positionRows
            );
            CHECK(
                target.readRows(
                    "SELECT kind, controlled_target_id, subject_id FROM "
                    "ledger_events ORDER BY sequence"
                ) == eventRows
            );
            CHECK(
                target.readRows(
                    "SELECT source_identity, target_identity FROM "
                    "schema_identity_transitions WHERE source_identity='"
                    + sourceIdentity + "'"
                )
                == std::vector<std::vector<std::string>>{
                    {
                        sourceIdentity,
                        std::string{k_targetSchemaIdentity},
                    },
                }
            );
        }
    }

    // The generation that stood immediately before call-bound Journal batch
    // proposals were given a durable home. Its three tables were added by the
    // pair that followed it and are deleted again by the state cut, so a
    // database at this source now arrives at a target that has neither them
    // nor the Journal they proposed into. The fixture winds a fresh schema
    // back past the state cut and then drops those three tables, which is the
    // schema this generation actually had, and proves the pair still lands.
    TEST_CASE("the pair before the Journal proposal tables migrates past them")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        {
            auto prepared = prepareStore(temporary.path());
            CHECK(prepared.store.databasePath() == databasePath);
        }

        auto sourceIdentity = std::string{};
        {
            auto prior  = test_support::OperatorDatabaseProbe{databasePath};
            restoreGenesisGenerationSchema(prior);
            restoreProjectStateInterpretationSchema(prior);
            restoreOperationSurfaceSchema(prior);
            restorePriorToolRunSchema(prior);
            dropJournalProposalTables(prior);
            sourceIdentity = exactSchemaIdentity(prior);
        }
        CHECK_MESSAGE(
            sourceIdentity
                == "sha256:d6b81490eb210f8f271bd72523a4475b1eca878235fa0a077598f8016fb11c02",
            "the fixture must reproduce the exact identity this pair migrates from"
        );

        {
            auto migrated = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(migrated.has_value(), migrated.error().message());
        }
        {
            auto target = test_support::OperatorDatabaseProbe{databasePath};
            CHECK(
                exactSchemaIdentity(target)
                == k_targetSchemaIdentity
            );
            // Neither the proposals nor the Journal they proposed into
            // survives the chain this pair now ends in.
            CHECK(
                target.readRows(
                    "SELECT count(*) FROM sqlite_schema WHERE type='table' AND "
                    "name IN ('journal_batch_proposals', "
                    "'journal_proposal_effects', 'journal_proposal_events', "
                    "'journal_events', 'project_state')"
                ) == std::vector<std::vector<std::string>>{{"0"}}
            );
            CHECK(
                target.readRows(
                    "SELECT source_identity, target_identity FROM "
                    "schema_identity_transitions WHERE source_identity='"
                    + sourceIdentity + "'"
                )
                == std::vector<std::vector<std::string>>{
                    {
                        sourceIdentity,
                        std::string{k_targetSchemaIdentity},
                    },
                }
            );
        }
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
            "framework.screen.capture",
            std::move(*arguments)
        );
        REQUIRE(invocation.has_value());
        auto call = toolCallAt(
            *root,
            nullptr,
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
            restoreGenesisGenerationSchema(prior);
            restoreProjectStateInterpretationSchema(prior);
            restoreOperationSurfaceSchema(prior);
            restorePriorToolRunSchema(prior);
            restoreOperationDispatchSchema(prior);
            identityRows = prior.readRows(
                "SELECT call_identity, root_identity, canonical_args "
                "FROM tool_call_positions"
            );
            removeToolRuntimePersistence(prior);
            dropJournalProposalTables(prior);
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
                == k_targetSchemaIdentity
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
                        std::string{k_targetSchemaIdentity},
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
        { auto releasedStore = std::move(prepared.store); }

        auto sourceIdentity = std::string{};
        auto historicalRows = std::vector<std::vector<std::string>>{};
        {
            auto prior = test_support::OperatorDatabaseProbe{databasePath};
            restoreGenesisGenerationSchema(prior);
            restoreProjectStateInterpretationSchema(prior);
            restoreOperationSurfaceSchema(prior);
            restorePriorToolRunSchema(prior);
            restoreOperationDispatchSchema(prior);
            restoreFormat2RegistrationIdentity(prior);
            dropJournalProposalTables(prior);
            sourceIdentity = exactSchemaIdentity(prior);
            historicalRows = prior.readRows(
                "SELECT registration_hash, plugin_id, plugin_hash, canonical_manifest "
                "FROM project_registrations ORDER BY registration_hash"
            );
        }
        CHECK(sourceIdentity
              == "sha256:b26344e031574f95020ed445e16e9de396f76442d98c5a3b758a91d84660237e");

        {
            auto migrated = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(migrated.has_value(), migrated.error().message());
        }
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
            restoreGenesisGenerationSchema(source);
            restoreProjectStateInterpretationSchema(source);
            restoreOperationSurfaceSchema(source);
            restoreOperationDispatchSchema(source);
            restoreFormat2RegistrationIdentity(source);
            removeReleaseUpgradeEvidenceTables(source);
            restorePriorRegistrationStateSchemaHash(source);
            removeSessionWorldScopeColumns(source);
            removeObservedInstanceBindingLocalRefColumn(source);
            dropJournalProposalTables(source);
            sourceIdentity = exactSchemaIdentity(source);
            replayBefore = source.readRows(
                "SELECT sequence, kind, controlled_target_id, subject_id "
                "FROM ledger_events ORDER BY sequence"
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
            "SELECT sequence, kind, controlled_target_id, subject_id "
            "FROM ledger_events ORDER BY sequence"
        );
        CHECK_MESSAGE(
            replayAfter == replayBefore,
            "replaying the pre-upgrade ledger must produce an empty "
            "domain-history difference"
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
            restoreGenesisGenerationSchema(prior);
            restoreProjectStateInterpretationSchema(prior);
            restoreOperationSurfaceSchema(prior);
            restorePriorToolRunSchema(prior);
            restoreOperationDispatchSchema(prior);
            restoreFormat2RegistrationIdentity(prior);
            removeSessionWorldScopeColumns(prior);
            removeObservedInstanceBindingLocalRefColumn(prior);
            dropJournalProposalTables(prior);
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

    // The pair for the local_ref the binding gained, which resolves an
    // observed instance to the model target it was observed at. Pre-local_ref
    // bindings cannot have their model target reconstructed -- the ruling
    // forbids inferring one -- so the backfill is the empty-string sentinel:
    // the NOT NULL column accepts it, and an instance carrying it is
    // undeliverable rather than delivered under a model target it never had.
    // Every other byte of the row survives.
    TEST_CASE("the binding local_ref column migrates pre-target bindings fail-closed")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        auto prepared = prepareStore(temporary.path());

        // One published observation, so the table this pair rebuilds has a row
        // in it. Publication is the only mint of a persistent binding, so a
        // case about that table has to reach it.
        REQUIRE(prepared.store.publishProjectObservation(
            prepared.lease,
            prepared.project.registration,
            runScope("target-1", 1U),
            prepared.project.observedInstanceIdentitySchemas,
            observationProposal({
                observedInstanceProposal("migrated", "migrated-native"),
            })
        ).has_value());

        // The fixture store holds the runtime directory exclusively, so the
        // reopened door below can only take it after this one is released.
        { auto releasedStore = std::move(prepared.store); }

        auto sourceIdentity = std::string{};
        auto bindingRows    = std::vector<std::vector<std::string>>{};
        {
            auto prior = test_support::OperatorDatabaseProbe{databasePath};
            restoreGenesisGenerationSchema(prior);
            restoreProjectStateInterpretationSchema(prior);
            restoreOperationSurfaceSchema(prior);
            restorePriorToolRunSchema(prior);
            restoreOperationDispatchSchema(prior);
            restoreFormat2RegistrationIdentity(prior);
            removeObservedInstanceBindingLocalRefColumn(prior);
            dropJournalProposalTables(prior);
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
        // The row survives the rebuild and the backfill wrote the empty
        // sentinel, which is the only local_ref a pre-target binding can
        // honestly carry.
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
            restoreGenesisGenerationSchema(prior);
            restoreProjectStateInterpretationSchema(prior);
            restoreOperationSurfaceSchema(prior);
            restorePriorToolRunSchema(prior);
            restoreOperationDispatchSchema(prior);
            restoreFormat2RegistrationIdentity(prior);
            restorePriorRegistrationStateSchemaHash(prior);
            removeSessionWorldScopeColumns(prior);
            removeObservedInstanceBindingLocalRefColumn(prior);
            dropJournalProposalTables(prior);
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
            restoreGenesisGenerationSchema(prior);
            restoreProjectStateInterpretationSchema(prior);
            restoreOperationSurfaceSchema(prior);
            restorePriorToolRunSchema(prior);
            restoreOperationDispatchSchema(prior);
            restoreFormat2RegistrationIdentity(prior);
            removeReleaseUpgradeEvidenceTables(prior);
            restorePriorSnapshotIdentityComment(prior);
            restorePriorRegistrationStateSchemaHash(prior);
            removeSessionWorldScopeColumns(prior);
            removeObservedInstanceBindingLocalRefColumn(prior);
            dropJournalProposalTables(prior);
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
        auto leaseId             = std::string{};
        auto artifactRootHash    = std::optional<ContentHash>{};
        auto installedGeneration = uint64{};
        {
            auto prepared = prepareStore(temporary.path());

            // The lease this store acquired is the subject of the one ledger
            // event provisioning leaves behind, and that event is what the
            // audit-trace read-back below follows across the upgrade. No Tool
            // row can be seeded here: restoreFormat2RegistrationIdentity winds
            // the tool identity tables away entirely.
            leaseId             = prepared.lease.leaseId;
            artifactRootHash    = prepared.runtimeArtifactRootHash;
            installedGeneration = prepared.installedGeneration;
        }

        auto sourceIdentity = std::string{};
        {
            auto priorSchema = test_support::OperatorDatabaseProbe{databasePath};
            restoreGenesisGenerationSchema(priorSchema);
            restoreProjectStateInterpretationSchema(priorSchema);
            restoreOperationSurfaceSchema(priorSchema);
            restoreOperationDispatchSchema(priorSchema);
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
            dropJournalProposalTables(priorSchema);
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

        // The Operation surface is one of the things this chain of pairs now
        // ends by removing, so the two tables it owned are asserted gone rather
        // than read back: a row of either could not have survived the last step
        // of the chain, and a fixture that still looked for one would be
        // asserting the migration failed.
        CHECK_MESSAGE(
            migrated.readRows(
                "SELECT count(*) FROM sqlite_schema WHERE type='table' "
                "AND name IN ('operations', 'reconciliations')"
            ) == std::vector<std::vector<std::string>>{{"0"}},
            "the upgraded chain must leave no Operation table behind"
        );

        auto const expectedAuditTrace = std::vector<std::vector<std::string>>{
            {
                "control_transitioned",
                "target-1",
                leaseId,
            },
        };
        CHECK_MESSAGE(
            migrated.readRows(
                "SELECT kind, controlled_target_id, subject_id FROM ledger_events "
                "WHERE kind='control_transitioned' AND controlled_target_id='target-1'"
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
            test_support::unconstrainedAgentProfile(manifest)
        );
        REQUIRE_FALSE(disagreeing.has_value());
        CHECK(
            disagreeing.error().message().contains("does not bind the selected")
        );
        CHECK(disagreeing.error().message().contains(manifestRegistration.hex()));
        CHECK(disagreeing.error().message().contains(pinRegistration.hex()));
    }

    // A release upgrade replaces the RuntimeArtifact the whole production root
    // runs, so a mutating Tool call still holding the barrier would resume
    // under bytes other than the ones it was admitted against. What the gate
    // reads is the unterminated row itself rather than a target: it is scoped
    // to no target and excludes no chain, because a session pin is inside none.
    TEST_CASE("session pin refuses an unterminated Tool call and names it")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // Admitted holds the barrier too, but only a dispatching call can be
        // settled -- the terminal outcome is recorded against the dispatch
        // token -- so reaching the boundary is what lets the second half of
        // this case terminate the call at all.
        auto const started = startToolCall(
            prepared,
            "upgrade-mutation",
            prepared.project.toolName("command-1")
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
            hashOf(test_support::unconstrainedAgentProfileBytes()),
            test_support::policyArtifactBytes()
        );
        auto const refused = prepared.store.pinSession(
            pin,
            manifest,
            test_support::unconstrainedAgentProfile(manifest)
        );

        REQUIRE_FALSE_MESSAGE(
            refused.has_value(),
            "an unterminated mutating Tool call must refuse a new session pin"
        );
        CHECK_MESSAGE(
            refused.error().message().contains("unterminated mutating Tool call"),
            "the refusal must come from the mutation quiescence guard"
        );
        CHECK_MESSAGE(
            refused.error().message().contains(started.call.identity().hex()),
            "the mutation refusal must name the Tool call blocking the pin"
        );

        confirmToolCall(prepared, started);
        auto const accepted = prepared.store.pinSession(
            pin,
            manifest,
            test_support::unconstrainedAgentProfile(manifest)
        );
        CHECK_MESSAGE(
            accepted.has_value(),
            "a pin must succeed after its mutation terminates: ",
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
                hashOf(test_support::unconstrainedAgentProfileBytes()),
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
                test_support::unconstrainedAgentProfile(manifest)
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
                test_support::unconstrainedAgentProfile(prepared.manifest)
            );
            REQUIRE_FALSE_MESSAGE(
                failed.has_value(),
                "the manifest mismatch must inject a failure after publication and before pin"
            );
            CHECK_MESSAGE(
                failed.error().message().contains(
                    "SessionManifest RuntimeArtifact is not production-installed"
                ),
                "a pin whose manifest names an artifact root that was not "
                "installed must be refused by that name"
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
                hashOf(test_support::unconstrainedAgentProfileBytes()),
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
                test_support::unconstrainedAgentProfile(manifest)
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
                test_support::unconstrainedAgentProfile(manifest)
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
            hashOf(test_support::unconstrainedAgentProfileBytes()),
            test_support::policyArtifactBytes()
        );
        REQUIRE(prepared.store.pinSession(
            samePin,
            stored,
            test_support::unconstrainedAgentProfile(stored)
        ).has_value());

        auto movedResult = SessionManifest::create(
            SessionManifestSpec{
                .runtimeModelArtifactRootHash = prepared.runtimeArtifactRootHash,
                .operatorProtocolSchemaHash   = hashOf("operator"),
                .projectRegistrationHash      = prepared.project.registration.hash(),
                .policyArtifactHash           = hashOf("a policy this session was not pinned to"),
                .agentProfileHash             = hashOf(test_support::unconstrainedAgentProfileBytes()),
            }
        );
        REQUIRE(movedResult.has_value());
        auto const moved = *std::move(movedResult);
        REQUIRE(moved.hash() != stored.hash());
        auto const refused = prepared.store.pinSession(
            samePin,
            moved,
            test_support::unconstrainedAgentProfile(moved)
        );
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
            test_support::unconstrainedAgentProfile(manifest)
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

    // Genesis is part of an Operator root's layout, beside the directories, the
    // staging root and the empty database OperatorCoordinator::open already
    // creates. That is the whole of what makes "init then explore --runtime
    // <root>" reach a first session: a root nobody has upgraded still HAS a
    // RuntimeArtifact to pin, and it grants nothing, because a model that
    // declares nothing can do nothing.
    //
    // Deleting the ensureGenesisGeneration call from open reds this.
    TEST_CASE("a created Operator root holds the genesis RuntimeArtifact")
    {
        auto const temporary   = TemporaryDirectory{};
        auto const production  = temporary.path() / "production";
        auto const genesisHash = task::genesisArtifactRootHash();
        REQUIRE(genesisHash.has_value());

        auto coordinator = OperatorCoordinator::open(production);
        auto const openWhy = coordinator.has_value()
            ? std::string{}
            : std::string{coordinator.error().message()};
        REQUIRE_MESSAGE(coordinator.has_value(), openWhy);

        auto const genesisDirectory =
            production / "runtime-artifacts" / genesisHash->hex();
        CHECK_MESSAGE(
            std::filesystem::is_directory(genesisDirectory),
            "a created Operator root holds the genesis artifact at "
            "<root>/runtime-artifacts/<H_genesis hex>"
        );

        // The bytes, not the name. loadRuntimeArtifact hashes the manifest it
        // reads and compares it against the hash handed in, then checks the
        // directory's file closure and every declared size and digest under it.
        auto const opened = task::loadRuntimeArtifact(
            genesisDirectory,
            *genesisHash
        );
        CHECK_MESSAGE(
            opened.has_value(),
            "the genesis artifact's bytes verify against H_genesis"
        );

        // Pinned, and pinned at generation 0 -- the number a first real
        // installation compares against.
        auto const active = coordinator->activeRuntimeArtifactPin();
        REQUIRE(active.has_value());
        CHECK_MESSAGE(
            active->installedGeneration == 0U,
            "the genesis generation is generation 0"
        );
        CHECK(active->artifactRootHash == *genesisHash);
    }

    // Genesis is layout, not a release, and the number that says so is 0. An
    // upgrade into a root that has never had one still compares against the
    // absence of any release and still lands on 1.
    //
    // Pinning genesis at generation 1 instead reds this.
    TEST_CASE("the first real installation into a genesis root is generation 1")
    {
        auto const temporary   = TemporaryDirectory{};
        auto const release     = test_support::runtimeRelease(temporary.path());
        auto const genesisHash = task::genesisArtifactRootHash();
        REQUIRE(genesisHash.has_value());
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";

        {
            auto coordinator = OperatorCoordinator::open(production);
            REQUIRE(coordinator.has_value());

            auto const baseline = coordinator->activeRuntimeArtifactPin();
            REQUIRE(baseline.has_value());
            REQUIRE(baseline->artifactRootHash == *genesisHash);

            auto const installed = coordinator->installRuntimeArtifact(
                RuntimeArtifactInstallRequest{
                    .artifactDirectory = release.artifactDirectory,
                    .artifactRootHash  = release.artifactRootHash,
                    .expectedInstalledGeneration = baseline->installedGeneration,
                }
            );
            auto const installWhy = installed.has_value()
                ? std::string{}
                : std::string{installed.error().message()};
            REQUIRE_MESSAGE(installed.has_value(), installWhy);
            CHECK_MESSAGE(
                installed->installedGeneration() == 1U,
                "genesis did not consume the bootstrap baseline: the first real "
                "installation is generation 1"
            );
        }

        auto probe = test_support::OperatorDatabaseProbe{databasePath};
        auto const expectedGenerations = std::vector<std::vector<std::string>>{
            {"0", genesisHash->hex()},
            {"1", release.artifactRootHash.hex()},
        };
        CHECK_MESSAGE(
            probe.readRows(
                "SELECT installed_generation, artifact_root_hash FROM "
                "runtime_installations ORDER BY installed_generation"
            ) == expectedGenerations,
            "generation 0 stays genesis and the release takes generation 1"
        );
    }

    // OperatorCoordinator::open runs on every command, so it is what a root
    // written before the genesis generation existed meets first. It gains
    // genesis there -- schema CHECK through the registered migration pair, rows
    // through the same ensure a fresh root runs -- and nothing it already had
    // installed moves.
    //
    // Removing admitTheGenesisGeneration from migrateGenesisGeneration, or the
    // IS NULL guard from the ensure's active-pin claim, reds this.
    TEST_CASE("an Operator root written before the genesis generation gains it")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        auto const genesisHash  = task::genesisArtifactRootHash();
        REQUIRE(genesisHash.has_value());

        auto priorRootHash   = std::string{};
        auto priorGeneration = uint64{};
        {
            auto prepared   = prepareStore(temporary.path());
            priorRootHash   = prepared.runtimeArtifactRootHash.hex();
            priorGeneration = prepared.installedGeneration;
        }

        auto sourceIdentity = std::string{};
        auto priorSessions  = std::vector<std::vector<std::string>>{};
        {
            auto prior = test_support::OperatorDatabaseProbe{databasePath};
            restoreGenesisGenerationSchema(prior);
            sourceIdentity = exactSchemaIdentity(prior);
            priorSessions  = prior.readRows(
                "SELECT session_id, runtime_artifact_root_hash, "
                "installed_generation FROM sessions ORDER BY session_id"
            );
        }
        CHECK_MESSAGE(
            sourceIdentity == k_genesisGenerationSourceIdentity,
            "the fixture must reproduce the exact identity this pair migrates from"
        );

        // A root written before this change has no genesis directory either.
        auto discarded = std::error_code{};
        std::filesystem::remove_all(
            production / "runtime-artifacts" / genesisHash->hex(),
            discarded
        );

        {
            auto migrated = OperatorCoordinator::open(production);
            auto const migratedWhy = migrated.has_value()
                ? std::string{}
                : std::string{migrated.error().message()};
            REQUIRE_MESSAGE(migrated.has_value(), migratedWhy);
            auto const active = migrated->activeRuntimeArtifactPin();
            REQUIRE(active.has_value());
            CHECK_MESSAGE(
                active->installedGeneration == priorGeneration,
                "gaining genesis leaves the installed generation the root "
                "already had active"
            );
            CHECK_MESSAGE(
                active->artifactRootHash.hex() == priorRootHash,
                "gaining genesis leaves the RuntimeArtifact the root already "
                "had active"
            );
        }

        CHECK_MESSAGE(
            std::filesystem::is_directory(
                production / "runtime-artifacts" / genesisHash->hex()
            ),
            "a root written before the genesis generation gains its artifact "
            "when it is next opened"
        );

        auto after = test_support::OperatorDatabaseProbe{databasePath};
        CHECK(exactSchemaIdentity(after) == k_targetSchemaIdentity);
        auto const expectedGenesisRow =
            std::vector<std::vector<std::string>>{{genesisHash->hex()}};
        CHECK_MESSAGE(
            after.readRows(
                "SELECT artifact_root_hash FROM runtime_installations "
                "WHERE installed_generation=0"
            ) == expectedGenesisRow,
            "a root written before the genesis generation gains it at "
            "generation 0"
        );
        CHECK_MESSAGE(
            after.readRows(
                "SELECT session_id, runtime_artifact_root_hash, "
                "installed_generation FROM sessions ORDER BY session_id"
            ) == priorSessions,
            "every session the root already recorded keeps its bytes"
        );
        auto const expectedTransition = std::vector<std::vector<std::string>>{
            {sourceIdentity, std::string{k_targetSchemaIdentity}},
        };
        CHECK(
            after.readRows(
                "SELECT source_identity, target_identity FROM "
                "schema_identity_transitions WHERE source_identity='"
                + sourceIdentity + "'"
            ) == expectedTransition
        );
    }

    // H_genesis is a framework constant, and the framework has moved it: the
    // RuntimeModel format cut changed the three bytes of the empty model, so
    // every root created before the cut pins generation 0 to a digest this
    // binary no longer computes. Generation 0 is layout rather than history, so
    // the root's materialisation of the constant is migrated to follow it --
    // and every row a future pin travels through moves with it, while the pure
    // event logs keep their bytes.
    //
    // The fixture is a whole pre-cut root: the schema that generation stored
    // AND the genesis digest that generation wrote, which is what every
    // Operator root on disk today actually is. It therefore doubles as the
    // reproduction the genesis_transitions schema pair owes.
    //
    // Emptying k_formerGenesisArtifactRootHashes, or dropping any one of the
    // three rewrites in migrateGenesisArtifactRoot, reds this.
    TEST_CASE("an Operator root pinned to a superseded genesis migrates onto the current one")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        auto const genesisHash  = task::genesisArtifactRootHash();
        REQUIRE(genesisHash.has_value());
        REQUIRE_FALSE(task::k_formerGenesisArtifactRootHashes.empty());
        auto const superseded =
            std::string{task::k_formerGenesisArtifactRootHashes.front()};
        REQUIRE(superseded != genesisHash->hex());

        auto releaseRootHash = std::string{};
        {
            auto prepared   = prepareStore(temporary.path());
            releaseRootHash = prepared.runtimeArtifactRootHash.hex();
        }

        // The superseded genesis artifact as a pre-cut root holds it: a
        // directory under the production root named by the digest that root
        // pins. Its BYTES are deliberately not reproduced -- the old
        // RuntimeModel spelling does not live in this repository any more, and
        // the migration rewrites digests without reading one.
        auto const supersededDirectory =
            production / "runtime-artifacts" / superseded;
        REQUIRE(std::filesystem::create_directory(supersededDirectory));
        test_support::writeFile(
            supersededDirectory / "runtime-artifact.manifest.json",
            "the superseded genesis artifact this root was created with"
        );
        auto discarded = std::error_code{};
        std::filesystem::remove_all(
            production / "runtime-artifacts" / genesisHash->hex(),
            discarded
        );

        auto sourceIdentity = std::string{};
        {
            auto prior = test_support::OperatorDatabaseProbe{databasePath};

            // Generation 0 holds the superseded digest, and so does the
            // generation a failed upgrade rolled back onto -- which is the
            // active pin. rollbackRuntimeArtifactUpgrade appends the restored
            // hash as a NEW generation, so genesis is reachable above 0.
            prior.execute(
                "INSERT INTO runtime_artifacts(artifact_root_hash) VALUES('"
                + superseded + "')"
            );
            prior.execute(
                "UPDATE runtime_installations SET artifact_root_hash='"
                + superseded + "' WHERE installed_generation=0"
            );
            prior.execute(
                "INSERT INTO runtime_installations(installed_generation, "
                "artifact_root_hash) VALUES(2, '" + superseded + "')"
            );
            prior.execute(
                "UPDATE runtime_state SET installed_generation=2, "
                "active_runtime_artifact_root_hash='" + superseded
                + "' WHERE singleton=1"
            );
            prior.execute(
                "UPDATE sessions SET installed_generation=2, "
                "runtime_artifact_root_hash='" + superseded
                + "' WHERE session_id='session-1'"
            );

            // The two pure event logs, each naming the superseded digest.
            // Neither carries a foreign key and neither is a pin a future load
            // travels through, so both must come out byte-identical.
            prior.execute(
                "INSERT INTO runtime_upgrade_failures(attempted_generation, "
                "attempted_artifact_root_hash, restored_generation, "
                "restored_artifact_root_hash, reason) VALUES(1, '"
                + releaseRootHash + "', 2, '" + superseded
                + "', 'the fixture upgrade failed')"
            );
            prior.execute(
                "INSERT INTO release_capability_approvals(artifact_root_hash, "
                "capability_profile_hash, controller_capabilities, "
                "evidence_hash, session_epoch) VALUES('" + superseded
                + "', 'profile', '[]', 'evidence', 1)"
            );

            // And the schema that generation stored.
            removeGenesisTransitions(prior);
            sourceIdentity = exactSchemaIdentity(prior);
        }
        CHECK_MESSAGE(
            sourceIdentity == k_genesisTransitionsSourceIdentity,
            "the fixture must reproduce the exact identity this pair migrates from"
        );

        auto const priorFailures = std::vector<std::vector<std::string>>{
            {"1", releaseRootHash, "2", superseded, "the fixture upgrade failed"},
        };
        auto const priorApprovals = std::vector<std::vector<std::string>>{
            {superseded, "profile", "[]", "evidence", "1"},
        };

        {
            auto migrated = OperatorCoordinator::open(production);
            auto const migratedWhy = migrated.has_value()
                ? std::string{}
                : std::string{migrated.error().message()};
            REQUIRE_MESSAGE(migrated.has_value(), migratedWhy);

            auto const active = migrated->activeRuntimeArtifactPin();
            REQUIRE(active.has_value());
            CHECK_MESSAGE(
                active->installedGeneration == 2U,
                "the generation a rollback produced is untouched by the "
                "materialisation moving"
            );
            CHECK_MESSAGE(
                active->artifactRootHash == *genesisHash,
                "the active pin names the RuntimeArtifact this binary computes"
            );

            // Nothing names the superseded digest any more, so the collector
            // that already owns unreferenced artifacts takes it -- there is no
            // hand-deletion inside the migration and none is needed.
            auto const reclaimed = migrated->reclaimUnreferencedRuntimeArtifacts();
            REQUIRE(reclaimed.has_value());
            CHECK_MESSAGE(
                !std::filesystem::exists(supersededDirectory),
                "the superseded genesis artifact becomes reclaimable"
            );
        }

        auto after = test_support::OperatorDatabaseProbe{databasePath};
        CHECK(exactSchemaIdentity(after) == k_targetSchemaIdentity);
        auto const expectedInstallations = std::vector<std::vector<std::string>>{
            {"0", genesisHash->hex()},
            {"1", releaseRootHash},
            {"2", genesisHash->hex()},
        };
        CHECK_MESSAGE(
            after.readRows(
                "SELECT installed_generation, artifact_root_hash FROM "
                "runtime_installations ORDER BY installed_generation"
            ) == expectedInstallations,
            "every generation holding the superseded genesis moves, not only "
            "generation 0"
        );
        auto const expectedSession = std::vector<std::vector<std::string>>{
            {"session-1", "2", genesisHash->hex()},
        };
        CHECK_MESSAGE(
            after.readRows(
                "SELECT session_id, installed_generation, "
                "runtime_artifact_root_hash FROM sessions ORDER BY session_id"
            ) == expectedSession,
            "a session pinned to the superseded genesis follows its parent"
        );
        auto const expectedState =
            std::vector<std::vector<std::string>>{{"2", genesisHash->hex()}};
        CHECK_MESSAGE(
            after.readRows(
                "SELECT installed_generation, active_runtime_artifact_root_hash "
                "FROM runtime_state WHERE singleton=1"
            ) == expectedState,
            "a never-upgraded root would otherwise open its first session "
            "against an artifact the current parser refuses by declared format"
        );
        CHECK_MESSAGE(
            after.readRows(
                "SELECT attempted_generation, attempted_artifact_root_hash, "
                "restored_generation, restored_artifact_root_hash, reason FROM "
                "runtime_upgrade_failures"
            ) == priorFailures,
            "a pure event log records what happened and keeps its bytes"
        );
        CHECK_MESSAGE(
            after.readRows(
                "SELECT artifact_root_hash, capability_profile_hash, "
                "controller_capabilities, evidence_hash, session_epoch FROM "
                "release_capability_approvals"
            ) == priorApprovals,
            "a pure event log records what happened and keeps its bytes"
        );
        auto const expectedGenesisTransition =
            std::vector<std::vector<std::string>>{{superseded, genesisHash->hex()}};
        CHECK(
            after.readRows(
                "SELECT source_artifact_root_hash, target_artifact_root_hash "
                "FROM genesis_transitions"
            ) == expectedGenesisTransition
        );
        auto const expectedSchemaTransition = std::vector<std::vector<std::string>>{
            {sourceIdentity, std::string{k_targetSchemaIdentity}},
        };
        CHECK(
            after.readRows(
                "SELECT source_identity, target_identity FROM "
                "schema_identity_transitions WHERE source_identity='"
                + sourceIdentity + "'"
            ) == expectedSchemaTransition
        );
    }

    // The other side of the same door, and the only thing the refusal still
    // claims: generation 0 names the framework's empty model, and a digest the
    // framework never wrote is refused by name rather than carried. Recognising
    // a superseded genesis is not a licence to accept an unknown one.
    //
    // Adding the unregistered digest to k_formerGenesisArtifactRootHashes, or
    // dropping the refusal branch, reds this.
    TEST_CASE("an Operator root pinning generation 0 to an unregistered digest is refused")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        auto const genesisHash  = task::genesisArtifactRootHash();
        REQUIRE(genesisHash.has_value());
        auto const unregistered =
            hashOf("a genesis this framework never wrote").hex();
        REQUIRE_FALSE(std::ranges::contains(
            task::k_formerGenesisArtifactRootHashes,
            std::string_view{unregistered}
        ));

        {
            auto const created = OperatorCoordinator::open(production);
            REQUIRE(created.has_value());
        }
        {
            auto prior = test_support::OperatorDatabaseProbe{databasePath};
            prior.execute(
                "INSERT INTO runtime_artifacts(artifact_root_hash) VALUES('"
                + unregistered + "')"
            );
            prior.execute(
                "UPDATE runtime_installations SET artifact_root_hash='"
                + unregistered + "' WHERE installed_generation=0"
            );
            prior.execute(
                "UPDATE runtime_state SET active_runtime_artifact_root_hash='"
                + unregistered + "' WHERE singleton=1"
            );
        }

        auto const refused = OperatorCoordinator::open(production);
        REQUIRE_FALSE(refused.has_value());
        auto const why = std::string{refused.error().message()};
        CHECK_MESSAGE(
            why.contains("sha256:" + unregistered),
            "the refusal names the digest the root actually pins: ",
            why
        );
        CHECK_MESSAGE(
            why.contains("sha256:" + genesisHash->hex()),
            "the refusal names the genesis RuntimeArtifact this binary "
            "computes: ",
            why
        );
        CHECK_MESSAGE(
            why.contains("nor a genesis this framework superseded"),
            "the refusal says what it now claims -- not merely that the two "
            "digests differ: ",
            why
        );

        // Refused, and left intact: nothing about the root was rewritten on
        // the way out.
        auto after = test_support::OperatorDatabaseProbe{databasePath};
        auto const expected =
            std::vector<std::vector<std::string>>{{unregistered}};
        CHECK(
            after.readRows(
                "SELECT artifact_root_hash FROM runtime_installations "
                "WHERE installed_generation=0"
            ) == expected
        );
        CHECK(
            after.readRows(
                "SELECT source_artifact_root_hash FROM genesis_transitions"
            ).empty()
        );
    }

    // Reclamation removes an artifact root no installation names and that is
    // not the active pin. Genesis is named by the generation-0 installation, so
    // it is referenced by exactly the mechanism every other kept artifact is
    // referenced by -- there is no exemption for it and none is needed.
    //
    // Deleting the generation-0 row from ensureGenesisGeneration reds this.
    TEST_CASE("reclamation keeps the genesis RuntimeArtifact")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const genesisHash  = task::genesisArtifactRootHash();
        REQUIRE(genesisHash.has_value());
        auto const genesisDirectory =
            production / "runtime-artifacts" / genesisHash->hex();

        // A root running a real release, so genesis is NOT the active pin and
        // its survival is the installation row rather than the pin.
        auto prepared = prepareStore(temporary.path());
        REQUIRE(prepared.runtimeArtifactRootHash != *genesisHash);
        REQUIRE(std::filesystem::is_directory(genesisDirectory));

        auto const reclaimed = prepared.store.reclaimUnreferencedRuntimeArtifacts();
        auto const reclaimWhy = reclaimed.has_value()
            ? std::string{}
            : std::string{reclaimed.error().message()};
        REQUIRE_MESSAGE(reclaimed.has_value(), reclaimWhy);
        CHECK_MESSAGE(
            std::filesystem::is_directory(genesisDirectory),
            "reclamation does not remove the genesis RuntimeArtifact"
        );
        CHECK(
            task::loadRuntimeArtifact(genesisDirectory, *genesisHash).has_value()
        );
    }

    TEST_CASE("production RuntimeArtifact installation owns activation CAS")
    {
        auto temporary = TemporaryDirectory{};
        auto const release = test_support::runtimeRelease(temporary.path());
        auto coordinator = OperatorCoordinator::open(temporary.path() / "production");
        REQUIRE(coordinator.has_value());

        auto installed = coordinator->installRuntimeArtifact(
            RuntimeArtifactInstallRequest{
                .artifactDirectory           = release.artifactDirectory,
                .artifactRootHash            = release.artifactRootHash,
                .expectedInstalledGeneration = 0U,
            }
        );
        REQUIRE(installed.has_value());
        CHECK(installed->installedGeneration() == 1U);
        CHECK(installed->rootHash() == release.artifactRootHash);

        CHECK_FALSE(coordinator->installRuntimeArtifact(
            RuntimeArtifactInstallRequest{
                .artifactDirectory           = release.artifactDirectory,
                .artifactRootHash            = release.artifactRootHash,
                .expectedInstalledGeneration = 0U,
            }
        ).has_value());

        test_support::writeFile(
            release.artifactDirectory / task::k_runtimeModelFileName,
            "the authoring source changed"
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

        CHECK_FALSE(prepared.store.createSnapshot(
            prepared.lease,
            prepared.project.registration,
            prepared.project.toolCatalogSchemaOwner,
            prepared.project.observedInstanceIdentitySchemas,
            conformance::observeOnce(prepared.observation)
        ).has_value());
    }

    // Durable idempotency belongs to the root request. The caller namespace and
    // request key name the row; the preimage bytes decide whether a repeat
    // rejoins that row or is a second intent filed under one key. The relation
    // and the ledger are asserted together deliberately: relationTo answers
    // from the two values alone and persistToolRootRequest answers from what is
    // stored, and the whole point of the rule is that those two agree.
    TEST_CASE("Tool root requests are durable-idempotent and conflict on changed bytes")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto preimage  = CanonicalJson::parseExact(
            R"({"objective":"durable-idempotency"})"
        );
        REQUIRE(preimage.has_value());
        auto const first = ToolRootRequestIdentity::create(
            "controller-1",
            "request-1",
            *preimage
        );
        REQUIRE(first.has_value());
        auto const created = prepared.store.persistToolRootRequest(*first);
        REQUIRE(created.has_value());
        CHECK(created->lookup == ToolIdentityLookup::Created);

        // The same namespace, key and exact preimage bytes rejoin the stored
        // row at the same identity rather than minting a second.
        auto const repeated = ToolRootRequestIdentity::create(
            "controller-1",
            "request-1",
            *preimage
        );
        REQUIRE(repeated.has_value());
        CHECK(first->relationTo(*repeated) == RootRequestRelation::SameRequest);
        auto const existing = prepared.store.persistToolRootRequest(*repeated);
        REQUIRE(existing.has_value());
        CHECK(existing->lookup == ToolIdentityLookup::Existing);
        CHECK(existing->rootIdentity == created->rootIdentity);

        // The same key with different bytes is two intents under one key, so
        // it is a conflict rather than a second row.
        auto conflictingPreimage = CanonicalJson::parseExact(
            R"({"objective":"a second intent under one key"})"
        );
        REQUIRE(conflictingPreimage.has_value());
        auto const conflicting = ToolRootRequestIdentity::create(
            "controller-1",
            "request-1",
            *std::move(conflictingPreimage)
        );
        REQUIRE(conflicting.has_value());
        CHECK(first->relationTo(*conflicting) == RootRequestRelation::Conflict);
        auto const refused = prepared.store.persistToolRootRequest(*conflicting);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains(
            "replayed with different canonical material"
        ));

        // A different key in the same namespace is simply another request, and
        // the same bytes under it are not a replay of anything.
        auto const distinct = ToolRootRequestIdentity::create(
            "controller-1",
            "request-2",
            *std::move(preimage)
        );
        REQUIRE(distinct.has_value());
        CHECK(first->relationTo(*distinct) == RootRequestRelation::Distinct);
        auto const second = prepared.store.persistToolRootRequest(*distinct);
        REQUIRE(second.has_value());
        CHECK(second->lookup == ToolIdentityLookup::Created);
        CHECK(second->rootIdentity != created->rootIdentity);
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
                prepared.generation,
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
                    canonical_observation, observation_hash
                )
                SELECT observation.plugin_id, observation.project_instance_key,
                    revisions.value, observation.project_registration_hash,
                    observation.state_resolution_hash,
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
                project.registration,
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
                hashOf(test_support::unconstrainedAgentProfileBytes()),
                test_support::policyArtifactBytes()
            );
            REQUIRE(prepared.store.provisionProjectInstance(
                prepared.project.registration,
                "instance-ambiguous"
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
                test_support::unconstrainedAgentProfile(manifest)
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

}
