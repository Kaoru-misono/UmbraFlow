#include <operator/ledger.hpp>
#include <operator/operation.hpp>

#include "project-fixture.hpp"

#include <domain/content-hash.hpp>

#include <doctest/doctest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        [[nodiscard]]
        auto repositoryRoot() -> std::filesystem::path
        {
            auto source = std::filesystem::path{__FILE__};
            if (source.is_relative())
            {
                source = std::filesystem::absolute(source);
            }
            auto candidate = source.parent_path().parent_path().parent_path();
            if (std::filesystem::is_directory(candidate / "schema"))
            {
                return candidate;
            }

            candidate = std::filesystem::current_path();
            while (!candidate.empty())
            {
                if (std::filesystem::is_directory(candidate / "schema"))
                {
                    return candidate;
                }
                auto const parent = candidate.parent_path();
                if (parent == candidate)
                {
                    break;
                }
                candidate = parent;
            }

            FAIL("repository root containing schema/ was not found");
            return {};
        }

        [[nodiscard]]
        auto readSchema(std::string_view filename) -> std::string
        {
            auto stream = std::ifstream{
                repositoryRoot() / "schema" / filename,
                std::ios::binary,
            };
            REQUIRE(stream.good());
            return {
                std::istreambuf_iterator<char>{stream},
                std::istreambuf_iterator<char>{},
            };
        }

        [[nodiscard]]
        auto definition(
            std::string const& schema,
            std::string_view name
        ) -> std::string
        {
            auto const declaration  = std::string{"\""} + std::string{name} + "\"";
            auto const namePosition = schema.find(declaration);
            REQUIRE(namePosition != std::string::npos);
            auto const begin = schema.find('{', namePosition + declaration.size());
            REQUIRE(begin != std::string::npos);

            auto depth    = std::size_t{};
            auto inString = false;
            auto escaped  = false;
            for (auto index = begin; index < schema.size(); ++index)
            {
                auto const character = schema[index];
                if (inString)
                {
                    if (escaped)
                    {
                        escaped = false;
                    }
                    else if (character == '\\')
                    {
                        escaped = true;
                    }
                    else if (character == '"')
                    {
                        inString = false;
                    }
                    continue;
                }
                if (character == '"')
                {
                    inString = true;
                }
                else if (character == '{')
                {
                    ++depth;
                }
                else if (character == '}')
                {
                    REQUIRE(depth > 0);
                    --depth;
                    if (depth == 0)
                    {
                        return schema.substr(begin, index - begin + 1);
                    }
                }
            }

            FAIL("schema definition has no closing object delimiter");
            return {};
        }

        auto checkStrictObject(std::string const& value) -> void
        {
            CHECK(value.find("\"type\": \"object\"") != std::string::npos);
            CHECK(value.find("\"additionalProperties\": false") != std::string::npos);
            CHECK(value.find("\"required\": [") != std::string::npos);
            CHECK(value.find("\"properties\": {") != std::string::npos);
        }

        using test_support::canonical;
        using test_support::hashOf;
        using test_support::journalEntry;
        using test_support::prepareStore;
        using test_support::reconciliationOutcome;
        using test_support::reconcilingOperation;
        using test_support::TemporaryDirectory;
    }

    // A human takeover and a Host delivery share one linearization. The
    // takeover cannot un-click what may already have landed; what it does is
    // close the window in which the ledger could still be told the effect did
    // not happen.
    TEST_CASE("contract-agent-a07")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto host      = test_support::deliveringHost(prepared);

        auto const operation = test_support::createReadyOperation(
            prepared,
            "request-1",
            "command-1"
        );
        auto const reserved = prepared.store.reserveDispatch(
            operation.operationId,
            operation.revision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"authority-1"},
            std::nullopt
        );
        REQUIRE(reserved.has_value());

        // The Host acts. The click has landed and nothing in the ledger says so
        // yet: this is the whole of the race.
        auto const inFlight = host->deliverReport(reserved->authority);
        REQUIRE(inFlight.outcome() == task::DeliveryOutcome::Delivered);
        CHECK(host->clicks() == 1U);

        auto const takeover = prepared.store.takeoverLease(
            "session-1",
            "human takeover while a dispatch was in flight"
        );
        REQUIRE(takeover.has_value());
        CHECK(takeover->lease.fencingToken > prepared.lease.fencingToken);

        // The takeover found the dispatch unanswered and resolved it in the
        // transaction that bumped the fence. The count is the difference
        // between "nothing was in flight" and "one effect may already have
        // landed", and the caller is told which.
        CHECK(takeover->resolvedDispatches == 1U);

        // The displaced controller still holds a real report. It is refused
        // twice over -- the lease it names is no longer the live row, and the
        // dispatch is no longer unanswered.
        CHECK_FALSE(prepared.store.recordDeliveryOutcome(
            prepared.lease,
            reserved->operationRevision,
            inFlight
        ).has_value());

        // What the ledger recorded instead is transport_unknown, which is not
        // proof of absence, so no reconciliation may conclude Rejected for this
        // Operation. Asserted through the one path that reads the column.
        auto const displacedLease = prepared.lease;
        prepared.lease            = takeover->lease;
        auto const resolved       = StoredOperation{
            .operationId = operation.operationId,
            .lookup      = CommandLookup::Existing,
            .state       = OperationState::Reconciling,
            .revision      = reserved->operationRevision + 1U,
            .planFrozen    = true,
            .hasDispatched = true,
        };
        CHECK_FALSE(prepared.store.commitReconciliation(
            prepared.plugin,
            ReconciliationCommit{
                .operationId                  = resolved.operationId,
                .expectedOperationRevision    = resolved.revision,
                .expectedProjectStateRevision = 0U,
                .outcome                      = reconciliationOutcome(
                    prepared,
                    resolved.operationId,
                    "{\"disposition\":\"rejected\"}"
                ),
                .journalEvents                = {},
            }
        ).has_value());

        // The positive control for the revision above: the same Operation, the
        // same revision, a disposition transport_unknown does not forbid.
        REQUIRE(prepared.store.commitReconciliation(
            prepared.plugin,
            ReconciliationCommit{
                .operationId                  = resolved.operationId,
                .expectedOperationRevision    = resolved.revision,
                .expectedProjectStateRevision = 0U,
                .outcome                      = reconciliationOutcome(
                    prepared,
                    resolved.operationId,
                    "{\"disposition\":\"confirmed\"}"
                ),
                .journalEvents                = {
                    JournalAppend{
                        .eventId = "event-1",
                        .entry   = journalEntry(
                            prepared.project,
                            "fixture.confirmed",
                            "{\"value\":1}"
                        ),
                    },
                },
            }
        ).has_value());
        CHECK(displacedLease.fencingToken < prepared.lease.fencingToken);
    }

    // The reverse schedule. A recorded outcome is not re-opened by a takeover
    // that arrives after it, so "resolve what is unanswered" cannot become
    // "overwrite what was answered".
    TEST_CASE("a takeover after the outcome was recorded resolves nothing")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const operation = reconcilingOperation(
            prepared,
            "request-1",
            task::DeliveryOutcome::Delivered
        );

        auto const takeover = prepared.store.takeoverLease(
            "session-1",
            "human takeover after the outcome was recorded"
        );
        REQUIRE(takeover.has_value());
        CHECK(takeover->resolvedDispatches == 0U);

        // The Operation is exactly where the recorded outcome left it: the
        // takeover neither advanced its revision nor moved its state.
        prepared.lease = takeover->lease;
        REQUIRE(prepared.store.commitReconciliation(
            prepared.plugin,
            ReconciliationCommit{
                .operationId                  = operation.operationId,
                .expectedOperationRevision    = operation.revision,
                .expectedProjectStateRevision = 0U,
                .outcome                      = reconciliationOutcome(
                    prepared,
                    operation.operationId,
                    "{\"disposition\":\"confirmed\"}"
                ),
                .journalEvents                = {
                    JournalAppend{
                        .eventId = "event-1",
                        .entry   = journalEntry(
                            prepared.project,
                            "fixture.confirmed",
                            "{\"value\":1}"
                        ),
                    },
                },
            }
        ).has_value());
    }

    TEST_CASE("schema-agent-a01")
    {
        auto const schema = readSchema("umbraflow-operator-v1.schema.json");
        auto const cursor = definition(schema, "SubscriptionCursor");
        auto const resync = definition(schema, "ResyncRequired");
        CHECK(cursor.find("\"type\": \"integer\"") != std::string::npos);
        CHECK(cursor.find("\"minimum\": 0") != std::string::npos);
        checkStrictObject(resync);
        CHECK(resync.find("\"requested_cursor\"") != std::string::npos);
        CHECK(resync.find("\"oldest_available_cursor\"") != std::string::npos);
        CHECK(resync.find("\"current_cursor\"") != std::string::npos);
    }

    TEST_CASE("schema-agent-a02")
    {
        auto const schema   = readSchema("umbraflow-operator-v1.schema.json");
        auto const budget   = definition(schema, "AgentBudget");
        auto const progress = definition(schema, "ProgressMarker");
        checkStrictObject(budget);
        checkStrictObject(progress);
        CHECK(budget.find("\"maximum_tool_calls\"") != std::string::npos);
        CHECK(budget.find("\"maximum_mutations\"") != std::string::npos);
        CHECK(budget.find("\"maximum_observations\"") != std::string::npos);
        CHECK(budget.find("\"maximum_risk_units\"") != std::string::npos);
        CHECK(progress.find("\"same_state_repetitions\"") != std::string::npos);
        CHECK(progress.find("\"elapsed_without_progress_ms\"") != std::string::npos);
    }

    TEST_CASE("schema-agent-a03")
    {
        auto const operatorSchema  = readSchema("umbraflow-operator-v1.schema.json");
        auto const journalSchema   = readSchema("umbraflow-journal-v1.schema.json");
        auto const workspaceSchema = readSchema("umbraflow-annotation-workspace-v2.schema.json");
        auto const traceSchema     = readSchema("umbraflow-trace-v2.schema.json");
        checkStrictObject(definition(operatorSchema, "Operation"));
        checkStrictObject(definition(journalSchema, "JournalEvent"));
        auto const replay = definition(workspaceSchema, "ReplayBundle");
        checkStrictObject(replay);
        CHECK(replay.find("\"baseline_event_id\"") != std::string::npos);
        CHECK(replay.find("\"journal_prefix\"") != std::string::npos);
        CHECK(replay.find("\"operation_rows\"") != std::string::npos);
        CHECK(replay.find("\"session_manifest_hash\"") != std::string::npos);
        CHECK(traceSchema.find("\"additionalProperties\": false") != std::string::npos);

        // The trace forbids screenshot-shaped payloads by refusing the field
        // NAMES, not by listing two banned literals: searching the schema for
        // "screenshot" would be satisfied by a schema that declared such a
        // field, which is the opposite of the requirement.
        auto const fieldName = definition(traceSchema, "safe_field_name");
        CHECK(fieldName.find("\"not\"") != std::string::npos);
        CHECK(fieldName.find("screen[._-]*shot") != std::string::npos);
        CHECK(fieldName.find("frame[._-]*(bytes|data)") != std::string::npos);
    }

    TEST_CASE("contract-agent-a04")
    {
        auto const schema = readSchema("umbraflow-journal-v1.schema.json");
        auto const event  = definition(schema, "JournalEvent");
        checkStrictObject(event);
        CHECK(event.find("\"sequence\"") != std::string::npos);
        CHECK(event.find("\"prior_project_state_revision\"") != std::string::npos);
        CHECK(event.find("\"operation_id\"") != std::string::npos);
        CHECK(event.find("\"session_manifest_hash\"") != std::string::npos);
        CHECK(event.find("\"payload_schema_hash\"") != std::string::npos);
        CHECK(event.find("\"opaque_project_payload\"") != std::string::npos);
        CHECK(event.find("\"const\": 0") != std::string::npos);
        CHECK(event.find("\"type\": \"null\"") != std::string::npos);

        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // Provenance is neither optional nor the caller's to invent: an entry
        // whose provenance is not the pinned JournalProvenance document cannot
        // be minted, so no Journal row can lack one.
        CHECK_FALSE(prepared.project.journalSchemaOwner.validate(
            "fixture.progress",
            canonical(prepared.project.schemaOwner, "{\"value\":1}"),
            canonical(prepared.project.schemaOwner, "{\"kind\":\"forged\"}")
        ).has_value());

        // A payload the event's own schema does not accept cannot be minted
        // either, so a guessed or expected outcome has no spelling as a fact.
        CHECK_FALSE(prepared.project.journalSchemaOwner.validate(
            "fixture.progress",
            canonical(prepared.project.schemaOwner, "{\"value\":99}"),
            canonical(prepared.project.schemaOwner, "{\"kind\":\"fixture\"}")
        ).has_value());

        auto const operation = reconcilingOperation(
            prepared,
            "request-1",
            task::DeliveryOutcome::Delivered
        );
        auto const progressEvent = [&prepared]
        {
            auto events = std::vector<JournalAppend>{};
            events.emplace_back(
                JournalAppend{
                    .eventId = "event-1",
                    .entry = journalEntry(
                        prepared.project,
                        "fixture.progress",
                        "{\"value\":1}"
                    ),
                }
            );
            return events;
        };
        auto attempt = [&prepared, &operation](
            std::string document,
            std::vector<JournalAppend> events
        )
        {
            return prepared.store.commitReconciliation(
                prepared.plugin,
                ReconciliationCommit{
                    .operationId                  = operation.operationId,
                    .expectedOperationRevision    = operation.revision,
                    .expectedProjectStateRevision = 0U,
                    .outcome                      = reconciliationOutcome(
                        prepared,
                        operation.operationId,
                        std::move(document)
                    ),
                    .journalEvents                = std::move(events),
                }
            ).has_value();
        };

        // Rejected asserts the world did not change, and a delivered dispatch
        // is standing proof that it may have.
        CHECK_FALSE(attempt("{\"disposition\":\"rejected\"}", {}));

        // Ambiguous never established an outcome, so it may not write one.
        CHECK_FALSE(attempt("{\"disposition\":\"ambiguous\"}", progressEvent()));

        // Diverged must carry the correction that proves the divergence.
        CHECK_FALSE(attempt("{\"disposition\":\"diverged\"}", {}));

        // Continue is the one that proved something, and it is the one that
        // reaches the Journal.
        CHECK(attempt("{\"disposition\":\"continue\"}", progressEvent()));
    }

    TEST_CASE("schema-agent-a05")
    {
        auto const workspaceSchema    = readSchema("umbraflow-annotation-workspace-v2.schema.json");
        auto const registrationSchema = readSchema("umbraflow-project-registration-v1.schema.json");
        auto const replayGate         = definition(workspaceSchema, "ReplayGate");
        checkStrictObject(replayGate);
        CHECK(replayGate.find("\"ui_model_replay\"") != std::string::npos);
        CHECK(replayGate.find("\"project_operation_replay\"") != std::string::npos);
        CHECK(replayGate.find("\"passed\"") != std::string::npos);
        CHECK(registrationSchema.find("\"plugin_hash\"") != std::string::npos);
        CHECK(registrationSchema.find("\"project_registration_hash\"") == std::string::npos);
    }

    TEST_CASE("contract-agent-a06")
    {
        auto const workspaceSchema = readSchema("umbraflow-annotation-workspace-v2.schema.json");
        auto const artifactSchema  = readSchema("umbraflow-runtime-artifact-v1.schema.json");
        auto const authoringRoot   = definition(workspaceSchema, "AuthoringCapabilityRoot");
        checkStrictObject(authoringRoot);
        CHECK(authoringRoot.find("\"workspace_database\"") != std::string::npos);
        CHECK(authoringRoot.find("\"evidence_blob_root\"") != std::string::npos);
        CHECK(authoringRoot.find("\"replay_bundle_root\"") != std::string::npos);
        CHECK(artifactSchema.find("\"page_model\"") != std::string::npos);
        CHECK(artifactSchema.find("\"assets\"") != std::string::npos);
        CHECK(artifactSchema.find("screenshot") == std::string::npos);
        CHECK(artifactSchema.find("annotation_workspace") == std::string::npos);

        auto temporary = TemporaryDirectory{};
        auto const release = test_support::runtimeRelease(
            temporary.path() / "session-handoff"
        );
        auto store = OperatorCoordinator::open(temporary.path() / "production");
        REQUIRE(store.has_value());
        auto const install = [&release](ContentHash const& expected)
        {
            return RuntimeArtifactInstallRequest{
                .handoffRoot                 = release.handoffRoot,
                .expectedReleaseManifestHash = expected,
                .expectedInstalledGeneration = 0U,
            };
        };

        // The deployment principal re-verifies the release against trusted
        // metadata; it does not take the handoff's word for what it is.
        CHECK_FALSE(
            store->installRuntimeArtifact(install(hashOf("other-release"))).has_value()
        );

        // Three of the four authoring capability roots may never travel with a
        // release, so production has no path to the workspace database, the
        // evidence blobs or the replay bundles. The fourth is the exception the
        // schema pins deliberately: publication copies the committed
        // RuntimeArtifact out of candidate_workspace_root into the handoff file
        // by file, so that root's contents travel as a verified copy while the
        // root itself does not.
        auto const authoringRoots = std::array{
            std::filesystem::path{"workspace.sqlite"},
            std::filesystem::path{"evidence"} / "blob-1.png",
            std::filesystem::path{"replay"} / "bundle-1.jsonl",
        };
        for (auto const& authoringPath : authoringRoots)
        {
            test_support::writeFile(
                release.handoffRoot / authoringPath,
                "authoring bytes"
            );
            CHECK_FALSE(
                store->installRuntimeArtifact(
                    install(release.releaseManifestHash)
                ).has_value()
            );
            auto error = std::error_code{};
            static_cast<void>(std::filesystem::remove_all(
                release.handoffRoot / *authoringPath.begin(),
                error
            ));
            REQUIRE_FALSE(error);
        }

        // With nothing but the manifest-listed runtime files left, the same
        // handoff installs.
        auto const installed = store->installRuntimeArtifact(
            install(release.releaseManifestHash)
        );
        REQUIRE(installed.has_value());
        CHECK(installed->rootHash() == release.artifactRootHash);

        // The authoring side and the production side are also separate stores:
        // a handoff that sits inside the production root is refused rather than
        // read across the boundary.
        auto const nested = test_support::runtimeRelease(
            temporary.path() / "production" / "nested-handoff"
        );
        CHECK_FALSE(store->installRuntimeArtifact(
            RuntimeArtifactInstallRequest{
                .handoffRoot                 = nested.handoffRoot,
                .expectedReleaseManifestHash = nested.releaseManifestHash,
                .expectedInstalledGeneration = 1U,
            }
        ).has_value());
    }

    TEST_CASE("schema-agent-a07")
    {
        auto const schema     = readSchema("umbraflow-operator-v1.schema.json");
        auto const transition = definition(schema, "ControlTransition");
        auto const authority  = definition(schema, "DeliveryAuthority");
        checkStrictObject(transition);
        checkStrictObject(authority);
        CHECK(transition.find("\"takeover\"") != std::string::npos);
        CHECK(transition.find("\"fencing_token\"") != std::string::npos);
        CHECK(authority.find("\"session_epoch\"") != std::string::npos);
        CHECK(authority.find("\"fencing_token\"") != std::string::npos);
        CHECK(authority.find("\"authority_decision_id\"") != std::string::npos);
    }

    TEST_CASE("contract-agent-a08")
    {
        auto machine = OperationMachine{};
        REQUIRE(machine.transition(OperationEvent::ReadyWithoutApproval).has_value());
        REQUIRE(machine.transition(OperationEvent::DispatchStarted).has_value());
        auto const recovery = machine.transition(OperationEvent::PostDispatchAbort);
        REQUIRE(recovery.has_value());
        CHECK(*recovery == OperationState::Reconciling);
        CHECK(machine.mutationLocked());

        auto const schema  = readSchema("umbraflow-operator-v1.schema.json");
        auto const finding = definition(schema, "ExternalInputFinding");
        checkStrictObject(finding);
        CHECK(finding.find("\"freeze_and_reconcile\"") != std::string::npos);
        CHECK(finding.find("\"invalidated_snapshot_revision\"") != std::string::npos);
        CHECK(finding.find("\"operation_id\"") != std::string::npos);
    }
}
