#pragma once

// Everything a second game's deployment declares for one ProjectRegistration.
//
// Nothing here is shared with this repository's own exemplar: a bounded numeric
// tool argument of its own, its own effect type, and its own identity schema.
//
// It is written the way a consumer writes one: the whole declaration is inline
// in umbraflow-project.json, and this header is what renders the same bytes for
// a test that builds the deployment without opening a project directory.

#include <deployment/project-deployment.hpp>

#include <json/value.hpp>

#include <core/error/contracts.hpp>
#include <core/safety/annotations.hpp>

#include <domain/content-hash.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime::conformance::expedition
{
    // Every expedition tool is stated in the expedition's own vocabulary -- a
    // number of leagues to march, and nothing that describes the screen. Eight
    // is the longest march a single command may order.
    inline constexpr auto k_toolArgumentSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "title": "arcana expedition tool arguments",
    "type": "object",
    "additionalProperties": false,
    "required": ["steps"],
    "properties": {
        "steps": {"type": "integer", "minimum": 1, "maximum": 8}
    }
})json"};

    // The preimage whose sha256 this project's effect bounds pin, and which the
    // plugin writes into each effect it proposes. The framework compares the
    // digest and never reads what the payload means.
    inline constexpr auto k_effectPayloadPreimage = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/effect/expedition.march",
    "title": "arcana expedition.march effect payload",
    "type": "object",
    "additionalProperties": false,
    "required": ["turn"],
    "properties": {
        "turn": {"type": "integer", "minimum": 0}
    }
})json"};

    // The one observed-instance identity basis this project's plugins publish.
    inline constexpr auto k_observedIdentitySchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "title": "arcana expedition observed instance identity basis",
    "type": "object",
    "additionalProperties": false,
    "required": ["native_id", "surface_epoch"],
    "properties": {
        "native_id": {"type": "string", "minLength": 1},
        "surface_epoch": {"type": "integer", "minimum": 0}
    }
})json"};

    inline constexpr auto k_observedIdentitySchemaName = std::string_view{
        "https://arcana.example/identity/expedition/v1"
    };

    inline constexpr auto k_observedIdentitySchemas = std::array{
        deployment::ProjectIdentitySchemaSource{
            .name   = k_observedIdentitySchemaName,
            .schema = k_observedIdentitySchema,
        },
    };

    // `localName` is the LOCAL half of a Tool name. The declared name is that
    // half under the namespace the deployment registers, so the expedition and
    // the rival declare four Tools each and share none of the eight names --
    // which is what a Tool belonging to its registration means.
    struct ToolSource final
    {
        std::string_view localName{};
        std::string_view version{};
        ToolMutability   mutability{ToolMutability::Mutating};
    };

    inline constexpr auto k_toolSources = std::array{
        ToolSource{"approval", "3", ToolMutability::Mutating},
        ToolSource{"move", "3", ToolMutability::Mutating},
        ToolSource{"survey", "2", ToolMutability::ReadOnly},
        ToolSource{"trade", "3", ToolMutability::Mutating},
    };

    // One declared name of this exemplar, spelled the only way a Tool name is
    // spelled: the local half under the namespace its registrant owns.
    [[nodiscard]]
    inline auto exampleToolName(
        std::string_view pluginId,
        std::string_view localName
    ) -> std::string
    {
        return std::string{pluginId} + "." + std::string{localName};
    }

    [[nodiscard]]
    inline auto schemaHash(std::string_view bytes) -> ContentHash
    {
        auto const digest = sha256(std::as_bytes(std::span{bytes}));
        UF_CHECK(digest.has_value());
        return *digest;
    }

    [[nodiscard]]
    inline auto schemaHashHex(std::string_view bytes) -> std::string
    {
        return schemaHash(bytes).hex();
    }

    [[nodiscard]]
    inline auto parsedJson(std::string_view bytes) -> json::Value
    {
        auto parsed = json::parse(bytes);
        UF_CHECK(parsed.has_value());
        return *std::move(parsed);
    }

    // This deployment's `tools` array, as the exact canonical bytes the loader
    // renders from the same document.
    [[nodiscard]]
    inline auto toolDeclarations(
        std::string_view pluginId,
        ContentHash const& effectPayloadHash
    ) -> std::string
    {
        auto tools = std::vector<json::Value>{};
        tools.reserve(k_toolSources.size());
        for (auto const& tool : k_toolSources)
        {
            auto const idempotency = tool.mutability == ToolMutability::ReadOnly
                ? ToolIdempotency::ReadSafe
                : ToolIdempotency::DeliverySafe;
            tools.emplace_back(json::Value::ofObject({
                {"argument_schema", parsedJson(k_toolArgumentSchema)},
                {"body", json::Value::ofBoolean(false)},
                {"child_effects", json::Value::ofObject({
                     {"child_tool_names", json::Value::ofArray({})},
                     {"maximum_child_calls", json::Value::ofNumber(0)},
                     {"maximum_child_mutability", json::Value::ofString("read_only")},
                     {"maximum_child_risk", json::Value::ofString("read_only")},
                     {"maximum_child_surface", json::Value::ofString("semantic")},
                 })},
                {"effect_bounds", json::Value::ofArray({json::Value::ofObject({
                     {"maximum_risk", json::Value::ofString("high")},
                     {"namespaced_type", json::Value::ofString("expedition.march")},
                     {"payload_schema_hash", json::Value::ofString(effectPayloadHash.hex())},
                     {"scope_kind", json::Value::ofString("camp")},
                 })})},
                {"idempotency", json::Value::ofString(std::string{
                     toolIdempotencyWireName(idempotency)
                 })},
                {"mutability", json::Value::ofString(std::string{
                     toolMutabilityWireName(tool.mutability)
                 })},
                {"name", json::Value::ofString(
                     exampleToolName(pluginId, tool.localName)
                 )},
                {"required_capabilities", json::Value::ofArray({})},
                {"surface", json::Value::ofString("semantic")},
                {"timeout_policy", json::Value::ofObject({
                     {"maximum_elapsed_ms", json::Value::ofNumber(60'000)},
                     {"on_timeout", json::Value::ofString("reobserve")},
                 })},
                {"ui_action_bounds", json::Value::ofArray({
                     json::Value::ofString("expedition.step"),
                 })},
                {"version", json::Value::ofString(std::string{tool.version})},
                {"workflow_limits", json::Value::ofObject({
                     {"maximum_dispatches", json::Value::ofNumber(8)},
                     {"maximum_elapsed_ms", json::Value::ofNumber(600'000)},
                     {"maximum_observations", json::Value::ofNumber(256)},
                     {"maximum_steps", json::Value::ofNumber(8)},
                     {"maximum_waits", json::Value::ofNumber(64)},
                 })},
            }));
        }
        return json::canonicalBytes(json::Value::ofArray(std::move(tools)));
    }

    // The declaration one deployment is built from, as bytes this bundle owns:
    // ProjectDeploymentSources takes views, so the rendered array has to
    // outlive the create call.
    class DeploymentBundle final
    {
        std::string m_pluginId{};
        std::string m_tools{};

    public:
        explicit DeploymentBundle(std::string_view pluginId)
            : m_pluginId{pluginId}
            , m_tools{
                  toolDeclarations(pluginId, schemaHash(k_effectPayloadPreimage))
              }
        {
        }

        [[nodiscard]]
        auto tools() const UF_LIFETIME_BOUND -> std::string const&
        {
            return m_tools;
        }

        // Views into this bundle and into the static schema storage above, so
        // the result must not outlive the bundle it came from.
        [[nodiscard]]
        auto sources() const UF_LIFETIME_BOUND
            -> deployment::ProjectDeploymentSources
        {
            return deployment::ProjectDeploymentSources{
                .pluginId                        = m_pluginId,
                .tools                           = m_tools,
                .observedInstanceIdentitySchemas = k_observedIdentitySchemas,
            };
        }
    };
}
