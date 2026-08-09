#include <operator/manifest.hpp>

#include "project-fixture.hpp"

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
        auto sessionSpec() -> SessionManifestSpec
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

    TEST_CASE("contract-product-p01")
    {
        auto const schema     = readSchema("umbraflow-operator-v1.schema.json");
        auto const invocation = definition(schema, "ToolInvocation");
        auto const command    = definition(schema, "CommandRecord");
        auto const operation  = definition(schema, "Operation");
        checkStrictObject(invocation);
        checkStrictObject(command);
        checkStrictObject(operation);
        CHECK(invocation.find("\"snapshot_token\"") != std::string::npos);
        CHECK(invocation.find("authenticated_controller_id") == std::string::npos);
        CHECK(invocation.find("receipt_ref") == std::string::npos);
        CHECK(command.find("\"authenticated_controller_id\"") != std::string::npos);
        CHECK(command.find("\"command_fingerprint\"") != std::string::npos);
        CHECK(operation.find("\"plan_versions\"") != std::string::npos);
        CHECK(operation.find("\"dispatches\"") != std::string::npos);
    }

    TEST_CASE("contract-product-p02")
    {
        auto const schema        = readSchema("umbraflow-operator-v1.schema.json");
        auto const transition    = definition(schema, "ControlTransition");
        auto const externalInput = definition(schema, "ExternalInputFinding");
        checkStrictObject(transition);
        checkStrictObject(externalInput);
        CHECK(transition.find("\"takeover\"") != std::string::npos);
        CHECK(transition.find("\"fencing_token\"") != std::string::npos);
        CHECK(externalInput.find("\"freeze_and_reobserve\"") != std::string::npos);
        CHECK(externalInput.find("\"freeze_and_reconcile\"") != std::string::npos);
    }

    TEST_CASE("contract-product-p03")
    {
        auto const schema     = readSchema("umbraflow-operator-v1.schema.json");
        auto const capability = definition(schema, "ControllerCapability");
        checkStrictObject(capability);
        CHECK(capability.find("\"allowed_tools\"") != std::string::npos);
        CHECK(capability.find("\"allowed_effect_types\"") != std::string::npos);
        CHECK(capability.find("\"takeover\"") != std::string::npos);
        CHECK(capability.find("receipt") == std::string::npos);
        CHECK(capability.find("coordinate") == std::string::npos);
        CHECK(capability.find("native_input") == std::string::npos);
    }

    TEST_CASE("contract-product-p04")
    {
        auto const first = test_support::makeProject(
            "fixture.alpha",
            "plugin-alpha"
        );
        CHECK(
            first.registration.canonicalJcs().find(
                "\"plugin_id\":\"fixture.alpha\""
            ) != std::string::npos
        );

        auto const changedCode = test_support::makeProject(
            "fixture.alpha",
            "plugin-beta"
        );
        CHECK(first.registration.hash() != changedCode.registration.hash());
    }

    TEST_CASE("contract-product-p06")
    {
        auto const schema  = readSchema("umbraflow-operator-v1.schema.json");
        auto const session = definition(schema, "OperatorSession");
        checkStrictObject(session);
        CHECK(session.find("\"project_instance_key\"") != std::string::npos);
        CHECK(session.find("\"project_registration_hash\"") != std::string::npos);
        CHECK(session.find("\"session_epoch\"") != std::string::npos);

        auto firstSpec   = sessionSpec();
        auto const first = SessionManifest::create(firstSpec);
        REQUIRE(first.has_value());
        firstSpec.policyArtifactHash = hashOf("other-policy");
        auto const changedPolicy = SessionManifest::create(firstSpec);
        REQUIRE(changedPolicy.has_value());
        CHECK(first->hash() != changedPolicy->hash());
    }
}
