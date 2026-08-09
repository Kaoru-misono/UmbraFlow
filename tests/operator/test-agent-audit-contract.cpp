#include <operator/operation.hpp>

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

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
    }

    TEST_CASE("contract-agent-a01")
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

    TEST_CASE("contract-agent-a02")
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

    TEST_CASE("contract-agent-a03")
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
    }

    TEST_CASE("contract-agent-a05")
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
    }

    TEST_CASE("contract-agent-a07")
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
