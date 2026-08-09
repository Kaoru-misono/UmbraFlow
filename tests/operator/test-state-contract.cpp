#include <operator/manifest.hpp>

#include <domain/content-hash.hpp>

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
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

        [[nodiscard]]
        auto hashOf(std::string_view value) -> ContentHash
        {
            auto const hash = sha256(std::as_bytes(std::span{value}));
            REQUIRE(hash.has_value());
            return *hash;
        }

        [[nodiscard]]
        auto manifestSpec() -> SessionManifestSpec
        {
            return SessionManifestSpec{
                .hostProtocolSchemaHash        = hashOf("host"),
                .runtimeModelSchemaHash        = hashOf("runtime-schema"),
                .runtimeModelArtifactRootHash  = hashOf("runtime-root"),
                .operatorProtocolSchemaHash    = hashOf("operator"),
                .projectRegistrationHash       = hashOf("registration"),
                .policyArtifactHash            = hashOf("policy"),
                .journalEnvelopeSchemaHash     = hashOf("journal"),
                .agentProfileHash              = hashOf("agent"),
            };
        }
    }

    TEST_CASE("contract-state-s01")
    {
        auto const operatorSchema = readSchema("umbraflow-operator-v1.schema.json");
        auto const journalSchema  = readSchema("umbraflow-journal-v1.schema.json");
        auto const parts          = definition(operatorSchema, "SnapshotParts");
        auto const state          = definition(journalSchema, "ProjectState");
        checkStrictObject(parts);
        checkStrictObject(state);
        CHECK(parts.find("\"observation_id\"") != std::string::npos);
        CHECK(parts.find("\"project_state_revision\"") != std::string::npos);
        CHECK(parts.find("\"lease_id\"") != std::string::npos);
        CHECK(state.find("\"last_journal_sequence\"") != std::string::npos);
        CHECK(state.find("\"canonical_opaque_payload\"") != std::string::npos);
    }

    TEST_CASE("contract-state-s02")
    {
        auto const schema   = readSchema("umbraflow-operator-v1.schema.json");
        auto const snapshot = definition(schema, "ProjectSnapshot");
        checkStrictObject(snapshot);
        CHECK(snapshot.find("\"identity\"") != std::string::npos);
        CHECK(snapshot.find("\"token\"") != std::string::npos);
        CHECK(snapshot.find("\"project_observation\"") != std::string::npos);
        CHECK(snapshot.find("\"project_state\"") != std::string::npos);
        CHECK(snapshot.find("\"event_cursor\"") != std::string::npos);
        CHECK(snapshot.find("frame") == std::string::npos);
    }

    TEST_CASE("contract-state-s03")
    {
        auto const schema = readSchema("umbraflow-operator-v1.schema.json");
        auto const token  = definition(schema, "SnapshotToken");
        CHECK(token.find("\"type\": \"string\"") != std::string::npos);
        CHECK(token.find("{32,128}") != std::string::npos);
        CHECK(token.find("receipt") == std::string::npos);
        CHECK(token.find("fencing") == std::string::npos);
        CHECK(token.find("project_state") == std::string::npos);
    }

    TEST_CASE("contract-state-s04")
    {
        auto const schema = readSchema("umbraflow-operator-v1.schema.json");
        auto const basis  = definition(schema, "DecisionBasis");
        checkStrictObject(basis);
        CHECK(basis.find("\"state_resolution_hash\"") != std::string::npos);
        CHECK(basis.find("\"project_observation_hash\"") != std::string::npos);
        CHECK(basis.find("\"project_state_hash\"") != std::string::npos);
        CHECK(basis.find("\"session_manifest_hash\"") != std::string::npos);
        CHECK(basis.find("\"decision_basis_hash\"") != std::string::npos);
    }

    TEST_CASE("contract-state-s05")
    {
        auto const schema             = readSchema("umbraflow-operator-v1.schema.json");
        auto const manifestDefinition = definition(schema, "SessionManifest");
        checkStrictObject(manifestDefinition);
        CHECK(manifestDefinition.find("\"runtime_model_artifact_root_hash\"") != std::string::npos);
        CHECK(manifestDefinition.find("\"project_registration_hash\"") != std::string::npos);
        CHECK(manifestDefinition.find("\"policy_artifact_hash\"") != std::string::npos);
        CHECK(manifestDefinition.find("\"journal_envelope_schema_hash\"") != std::string::npos);

        auto const first  = SessionManifest::create(manifestSpec());
        auto const second = SessionManifest::create(manifestSpec());
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        CHECK(first->canonicalBytes() == second->canonicalBytes());
        CHECK(first->hash() == second->hash());
    }

    TEST_CASE("contract-state-s06")
    {
        auto const schema   = readSchema("umbraflow-journal-v1.schema.json");
        auto const instance = definition(schema, "ProjectInstance");
        auto const state    = definition(schema, "ProjectState");
        checkStrictObject(instance);
        checkStrictObject(state);
        CHECK(instance.find("\"project_instance_key\"") != std::string::npos);
        CHECK(instance.find("\"creation_event_id\"") != std::string::npos);
        CHECK(instance.find("\"current_project_state_revision\"") != std::string::npos);
        CHECK(instance.find("\"mutation_chain_operation_id\"") != std::string::npos);
        CHECK(state.find("\"project_registration_hash\"") != std::string::npos);
        CHECK(state.find("\"state_hash\"") != std::string::npos);
    }
}
