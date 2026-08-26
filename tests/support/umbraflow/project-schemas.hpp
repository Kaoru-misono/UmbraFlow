#pragma once

// Everything this repository's own exemplar deployment declares for one
// ProjectRegistration: the Tools it carries, the argument schema each of them
// states inline, and the one observed-instance identity schema it supplies.
//
// They are here rather than inside project-fixture.hpp because a deployment's
// declaration is the deployment's, not the fixture's: tests/deployment compiles
// it through modules/deployment without opening a store, and the fixture builds
// its authorities from the same bytes.
//
// This project is a fixture and its documents say so. Where a payload's
// accepted and refused values are fixed outside this directory, the schema says
// which file fixes them.

#include <deployment/project-deployment.hpp>

#include <json/value.hpp>

#include <core/error/contracts.hpp>
#include <core/safety/annotations.hpp>

#include <domain/content-hash.hpp>

#include <core/types/integer.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime::test_support
{
    // The argument shape every Tool of this fixture states inline. It is one
    // constant rather than one per Tool because nothing here is about a Tool
    // having its own arguments, and two Tools repeating a definition is the
    // accepted price of there being no cross-Tool indirection.
    inline constexpr auto k_toolArgumentSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "title": "umbraflow fixture tool arguments",
    "type": "object",
    "additionalProperties": false,
    "required": ["value"],
    "properties": {
        "value": {"type": "integer", "minimum": 1, "maximum": 8}
    }
})json"};

    // The same argument shape, opened for the one member the observed-instance
    // gate resolves: a canonical argument spelling an observed_instance_id
    // minted elsewhere. The gate's whole subject is such arguments, and the
    // strict fixture schema refuses them, so a case that feeds the gate admits
    // the id explicitly and keeps everything else as strict as before.
    inline constexpr auto k_toolArgumentSchemaWithInstanceIds =
        std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "title": "umbraflow fixture tool arguments",
    "type": "object",
    "additionalProperties": false,
    "required": ["value"],
    "properties": {
        "value": {"type": "integer", "minimum": 1, "maximum": 8},
        "observed_instance_id": {
            "type": "string",
            "pattern": "^oi1_[0-9a-f]{64}$"
        }
    }
})json"};

    // The preimage whose sha256 this fixture's effect bounds pin.
    //
    // The framework does not read an effect payload's meaning: what it enforces
    // is that a proposed OP:`EffectEnvelope` carries exactly the digest its
    // Tool declared, which is what stops a plan widening a Tool's blast radius
    // without moving tool_catalog_hash. These bytes are therefore a stable
    // preimage for that tag rather than a schema anything applies, and the
    // plugin source is built with this hex rather than with a made-up one.
    inline constexpr auto k_effectPayloadPreimage = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/effect/fixture.write",
    "title": "umbraflow fixture.write effect payload",
    "type": "object",
    "additionalProperties": false,
    "required": ["value"],
    "properties": {
        "value": {"type": "integer", "minimum": 0}
    }
})json"};

    // The one observed-instance identity schema this fixture deployment
    // supplies. The ledger's hand-written basis validators demand exactly the
    // two members below and refuse anything else, so this document states the
    // same constraint as a schema and the authority compiled from it accepts
    // exactly the bases those callbacks accepted.
    inline constexpr auto k_observedIdentitySchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "title": "umbraflow fixture observed instance identity basis",
    "type": "object",
    "additionalProperties": false,
    "required": ["native_id", "surface_epoch"],
    "properties": {
        "native_id": {"type": "string", "minLength": 1},
        "surface_epoch": {"type": "integer", "minimum": 0}
    }
})json"};

    // The name this deployment declares that schema under, and therefore the
    // identity_schema_id every observation proposal in these tests carries.
    inline constexpr auto k_observedIdentitySchemaName = std::string_view{
        "https://fixture.example/identity/overlay/v1"
    };

    inline constexpr auto k_observedIdentitySchemas = std::array{
        deployment::ProjectIdentitySchemaSource{
            .name   = k_observedIdentitySchemaName,
            .schema = k_observedIdentitySchema,
        },
    };

    // One tool of this project's declaration. `name` is the LOCAL half of the
    // Tool name: the declared name is that local half under the namespace the
    // deployment registers, so the same source list yields
    // `fixture.alpha.command-1` for one plugin id and
    // `fixture.control.command-1` for another. It is composed rather than
    // written out because a Tool's namespace is its owner's, and a list of
    // fully spelled names could only belong to one owner.
    //
    // Every member varies for one case's sake, so each is supplied explicitly
    // below rather than defaulted: the descriptor is the only bound a plan is
    // judged against, and a tool that differed from its neighbours by accident
    // would make some case pass for a reason nobody chose.
    struct ToolSource final
    {
        std::string_view localName{};
        ToolMutability   mutability{ToolMutability::Mutating};
        ToolSurface      surface{ToolSurface::Privileged};
        ToolIdempotency  idempotency{ToolIdempotency::NonIdempotent};
        std::string_view requiredCapability{};
        std::string_view uiActionBound{};
        uint64           timeoutMillis{};
    };

    // The plugin's one next_step names delivery_safe and a 5000 ms timeout, so
    // a tool declaring delivery_safe and 60000 ms admits it, and the two tools
    // that declare less are the only ones whose steps their descriptors refuse.
    inline constexpr auto k_ordinaryIdempotency = ToolIdempotency::DeliverySafe;
    inline constexpr auto k_ordinaryTimeoutMillis = uint64{60'000};

    // The one entry of most tools' ui_action_bounds, which is the step key the
    // plugin's proposals allow and its next_step names, and one no proposal
    // ever names.
    inline constexpr auto k_uiActionBound = std::string_view{"fixture.step"};
    inline constexpr auto k_unboundUiAction = std::string_view{"fixture.elsewhere"};

    inline constexpr auto k_toolSources = std::array{
        ToolSource{
            .localName          = "approval-plan",
            .mutability         = ToolMutability::Mutating,
            .surface            = ToolSurface::Semantic,
            .idempotency        = k_ordinaryIdempotency,
            .requiredCapability = "",
            .uiActionBound      = k_uiActionBound,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
        },
        // Shorter than the 5000 ms the plugin's one step intent names, so
        // the per-tool timeout_policy is what refuses that step and nothing
        // else about the tool differs.
        ToolSource{
            .localName          = "brief-timeout",
            .mutability         = ToolMutability::Mutating,
            .surface            = ToolSurface::Semantic,
            .idempotency        = k_ordinaryIdempotency,
            .requiredCapability = "",
            .uiActionBound      = k_uiActionBound,
            .timeoutMillis      = 1'000,
        },
        // The tool no session in this project holds a capability for. It is
        // Semantic and read-only, so the only thing that can keep it out of
        // an offered set is required_capabilities.
        ToolSource{
            .localName          = "capability-gated",
            .mutability         = ToolMutability::ReadOnly,
            .surface            = ToolSurface::Semantic,
            .idempotency        = ToolIdempotency::ReadSafe,
            .requiredCapability = "authoring",
            .uiActionBound      = k_uiActionBound,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
        },
        ToolSource{
            .localName          = "command-1",
            .mutability         = ToolMutability::Mutating,
            .surface            = ToolSurface::Semantic,
            .idempotency        = k_ordinaryIdempotency,
            .requiredCapability = "",
            .uiActionBound      = k_uiActionBound,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
        },
        ToolSource{
            .localName          = "command-2",
            .mutability         = ToolMutability::Mutating,
            .surface            = ToolSurface::Semantic,
            .idempotency        = k_ordinaryIdempotency,
            .requiredCapability = "",
            .uiActionBound      = k_uiActionBound,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
        },
        ToolSource{
            .localName          = "different-command",
            .mutability         = ToolMutability::Mutating,
            .surface            = ToolSurface::Semantic,
            .idempotency        = k_ordinaryIdempotency,
            .requiredCapability = "",
            .uiActionBound      = k_uiActionBound,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
        },
        ToolSource{
            .localName          = "mismatched-plan",
            .mutability         = ToolMutability::Mutating,
            .surface            = ToolSurface::Semantic,
            .idempotency        = k_ordinaryIdempotency,
            .requiredCapability = "",
            .uiActionBound      = k_uiActionBound,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
        },
        ToolSource{
            .localName          = "observe-1",
            .mutability         = ToolMutability::ReadOnly,
            .surface            = ToolSurface::Semantic,
            .idempotency        = ToolIdempotency::ReadSafe,
            .requiredCapability = "",
            .uiActionBound      = k_uiActionBound,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
        },
        // Its own descriptor is the clamp the oversized proposal meets.
        ToolSource{
            .localName          = "oversized-plan",
            .mutability         = ToolMutability::Mutating,
            .surface            = ToolSurface::Semantic,
            .idempotency        = k_ordinaryIdempotency,
            .requiredCapability = "",
            .uiActionBound      = k_uiActionBound,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
        },
        // The machine-surface tool: it names a coordinate, so no online Agent
        // may be handed it. Read-only so that the p03 cases never contend
        // for the mutation-chain slot the p01 cases are about.
        ToolSource{
            .localName          = "raw-coordinate-click",
            .mutability         = ToolMutability::ReadOnly,
            .surface            = ToolSurface::Privileged,
            .idempotency        = ToolIdempotency::ReadSafe,
            .requiredCapability = "",
            .uiActionBound      = k_uiActionBound,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
        },
        ToolSource{
            .localName          = "reordered-effects",
            .mutability         = ToolMutability::Mutating,
            .surface            = ToolSurface::Semantic,
            .idempotency        = k_ordinaryIdempotency,
            .requiredCapability = "",
            .uiActionBound      = k_uiActionBound,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
        },
        // Admits no step claiming it is safe to redeliver, which is the one
        // claim the plugin's step intent makes.
        ToolSource{
            .localName          = "strict-delivery",
            .mutability         = ToolMutability::Mutating,
            .surface            = ToolSurface::Semantic,
            .idempotency        = ToolIdempotency::NonIdempotent,
            .requiredCapability = "",
            .uiActionBound      = k_uiActionBound,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
        },
        // Bounds a UI action the plugin never proposes, so the allowed set of
        // its otherwise ordinary plan is what its ui_action_bounds refuses.
        ToolSource{
            .localName          = "stray-action",
            .mutability         = ToolMutability::Mutating,
            .surface            = ToolSurface::Semantic,
            .idempotency        = k_ordinaryIdempotency,
            .requiredCapability = "",
            .uiActionBound      = k_unboundUiAction,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
        },
        ToolSource{
            .localName          = "two-step-plan",
            .mutability         = ToolMutability::Mutating,
            .surface            = ToolSurface::Semantic,
            .idempotency        = k_ordinaryIdempotency,
            .requiredCapability = "",
            .uiActionBound      = k_uiActionBound,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
        },
    };

    // Every tool of this declaration carries the same version, because nothing
    // in this project is about a version and a per-tool one would be a number
    // each case had to remember.
    inline constexpr auto k_toolVersion = std::string_view{"1"};

    // The one effect type and scope kind this project's plugin proposes, and the
    // highest risk its descriptors admit. `critical` is deliberately outside the
    // bound so that a case can reach the policy's default deny without
    // rewriting the declaration.
    inline constexpr auto k_effectType = std::string_view{"fixture.write"};
    inline constexpr auto k_effectScopeKind = std::string_view{"instance"};

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

    // One declared name of this fixture, spelled the only way a Tool name is
    // spelled: the local half under the namespace the registrant owns. Every
    // case that names a fixture tool names it this way, because the same local
    // half belongs to a different Tool under every plugin id.
    [[nodiscard]]
    inline auto fixtureToolName(
        std::string_view pluginId,
        std::string_view localName
    ) -> std::string
    {
        return std::string{pluginId} + "." + std::string{localName};
    }

    [[nodiscard]]
    inline auto parsedJson(std::string_view bytes) -> json::Value
    {
        auto parsed = json::parse(bytes);
        UF_CHECK(parsed.has_value());
        return *std::move(parsed);
    }

    [[nodiscard]]
    inline auto namedStrings(std::span<std::string const> values) -> json::Value
    {
        auto items = std::vector<json::Value>{};
        items.reserve(values.size());
        for (auto const& value : values)
        {
            items.emplace_back(json::Value::ofString(value));
        }
        return json::Value::ofArray(std::move(items));
    }

    // The deployment's `tools` array, rendered as the exact canonical bytes the
    // loader would render from the same document. There is no catalog document
    // and no second serialization: this is the declaration, and its sha256 is
    // the registration's tool_catalog_hash.
    [[nodiscard]]
    inline auto toolDeclarations(
        std::string_view pluginId,
        std::string_view argumentSchema = k_toolArgumentSchema
    ) -> std::string
    {
        auto const effectPayloadHash = schemaHash(k_effectPayloadPreimage);
        auto tools                   = std::vector<json::Value>{};
        tools.reserve(k_toolSources.size());
        for (auto const& tool : k_toolSources)
        {
            auto requiredCapabilities = std::vector<std::string>{};
            if (!tool.requiredCapability.empty())
            {
                requiredCapabilities.emplace_back(tool.requiredCapability);
            }
            tools.emplace_back(json::Value::ofObject({
                {"argument_schema", parsedJson(argumentSchema)},
                {"description", json::Value::ofString("Run this Project Tool leaf handler.")},
                {"effect_bounds", json::Value::ofArray({json::Value::ofObject({
                     {"maximum_risk", json::Value::ofString("high")},
                     {"namespaced_type", json::Value::ofString(std::string{k_effectType})},
                     {"payload_schema_hash", json::Value::ofString(effectPayloadHash.hex())},
                     {"scope_kind", json::Value::ofString(std::string{k_effectScopeKind})},
                 })})},
                {"idempotency", json::Value::ofString(std::string{
                     toolIdempotencyWireName(tool.idempotency)
                 })},
                {"mutability", json::Value::ofString(std::string{
                     toolMutabilityWireName(tool.mutability)
                 })},
                {"name", json::Value::ofString(
                     fixtureToolName(pluginId, tool.localName)
                 )},
                {"required_capabilities", namedStrings(requiredCapabilities)},
                {"surface", json::Value::ofString(std::string{
                     toolSurfaceWireName(tool.surface)
                 })},
                {"timeout_policy", json::Value::ofObject({
                     {"maximum_elapsed_ms", json::Value::ofNumber(
                          static_cast<double>(tool.timeoutMillis)
                      )},
                     {"on_timeout", json::Value::ofString("reobserve")},
                 })},
                {"ui_action_bounds", json::Value::ofArray({
                     json::Value::ofString(std::string{tool.uiActionBound}),
                 })},
                {"version", json::Value::ofString(std::string{k_toolVersion})},
            }));
        }
        return json::canonicalBytes(json::Value::ofArray(std::move(tools)));
    }

    // The declaration one deployment is built from, as bytes this bundle owns.
    // It exists because ProjectDeploymentSources takes views: the rendered
    // `tools` array has to outlive the create call, and the bundle is what owns
    // it.
    class DeploymentBundle final
    {
        std::string m_pluginId{};
        std::string m_tools{};

    public:
        explicit DeploymentBundle(
            std::string_view pluginId,
            std::string_view argumentSchema = k_toolArgumentSchema
        )
            : m_pluginId{pluginId}
            , m_tools{toolDeclarations(pluginId, argumentSchema)}
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
