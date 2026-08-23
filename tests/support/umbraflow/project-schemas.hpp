#pragma once

// Everything this repository's own exemplar deployment holds for one
// ProjectRegistration: seven JSON Schema documents and the three manifests that
// name them by sha256.
//
// They are here rather than inside project-fixture.hpp because a deployment's
// schemas are the deployment's, not the fixture's: tests/deployment compiles
// them through modules/deployment without opening a store, and the fixture
// builds its five authorities from the same bytes.
//
// This project is a fixture and its documents say so. Its events are markers
// rather than data, and its observation is empty, so several schemas below can
// only pin the marker their event carries. Where a payload's accepted and
// refused values are fixed outside this directory, the schema says which file
// fixes them.

#include <deployment/project-deployment.hpp>

#include <project/tool-catalog.hpp>

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
    // The reducer counts confirmed reconciliations. Nothing else is in this
    // project's state, so a reduce output carrying anything else -- the
    // `{"value":99}` tests/operator/test-ledger.cpp:53 rewrites the plugin to
    // return -- fails `required` and `additionalProperties` together.
    inline constexpr auto k_projectStateSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/state",
    "title": "umbraflow fixture ProjectState",
    "type": "object",
    "additionalProperties": false,
    "required": ["revision"],
    "properties": {
        "revision": {"type": "integer", "minimum": 0}
    }
})json"};

    inline constexpr auto k_toolPreconditionSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/tool-precondition",
    "title": "umbraflow fixture tool arguments",
    "$defs": {
        "FixtureArguments": {
            "type": "object",
            "additionalProperties": false,
            "required": ["value"],
            "properties": {
                "value": {"type": "integer", "minimum": 1, "maximum": 8}
            }
        },
        "FixtureResult": {
            "type": "object",
            "additionalProperties": false,
            "required": ["outcome"],
            "properties": {
                "outcome": {"type": "string"}
            }
        }
    }
})json"};

    // The same argument shape, opened for the one member the observed-instance
    // gate resolves: a canonical argument spelling an observed_instance_id
    // minted elsewhere. The gate's whole subject is such arguments, and the
    // strict fixture schema refuses them, so a case that feeds the gate
    // admits the id explicitly and keeps everything else as strict as before.
    inline constexpr auto k_toolPreconditionSchemaWithInstanceIds =
        std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/tool-precondition-instance-ids",
    "title": "umbraflow fixture tool arguments",
    "$defs": {
        "FixtureArguments": {
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
        },
        "FixtureResult": {
            "type": "object",
            "additionalProperties": false,
            "required": ["outcome"],
            "properties": {
                "outcome": {"type": "string"}
            }
        }
    }
})json"};

    inline constexpr auto k_baselinePayloadSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/journal/fixture.baseline",
    "title": "umbraflow fixture.baseline payload",
    "type": "object",
    "additionalProperties": false,
    "required": ["kind"],
    "properties": {
        "kind": {"const": "baseline"}
    }
})json"};

    // A marker with one step. `{"value":2}` and `{"value":99}` must be refused
    // here: tests/operator/test-ledger.cpp:616-623 and
    // tests/operator/test-agent-audit-contract.cpp:1105-1110 assert that a
    // payload this event's own schema does not accept cannot be minted, and
    // they spell those two documents.
    inline constexpr auto k_progressPayloadSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/journal/fixture.progress",
    "title": "umbraflow fixture.progress payload",
    "type": "object",
    "additionalProperties": false,
    "required": ["value"],
    "properties": {
        "value": {"const": 1}
    }
})json"};

    // The two confirmations this fixture reaches: the one a first
    // reconciliation commits and the one a second commits over it.
    inline constexpr auto k_confirmedPayloadSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/journal/fixture.confirmed",
    "title": "umbraflow fixture.confirmed payload",
    "type": "object",
    "additionalProperties": false,
    "required": ["value"],
    "properties": {
        "value": {"enum": [1, 2]}
    }
})json"};

    inline constexpr auto k_duplicatePayloadSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/journal/fixture.duplicate",
    "title": "umbraflow fixture.duplicate payload",
    "type": "object",
    "additionalProperties": false,
    "required": ["value"],
    "properties": {
        "value": {"const": 3}
    }
})json"};

    // The payload of every OP:`EffectEnvelope` this project's plugin proposes.
    // Its sha256 is the payload_schema_hash those effects carry, so the plugin
    // source is built with that hex rather than with a made-up one.
    inline constexpr auto k_effectPayloadSchema = std::string_view{R"json({
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
    "$id": "https://fixture.example/identity/overlay/v1",
    "title": "umbraflow fixture observed instance identity basis",
    "type": "object",
    "additionalProperties": false,
    "required": ["native_id", "surface_epoch"],
    "properties": {
        "native_id": {"type": "string", "minLength": 1},
        "surface_epoch": {"type": "integer", "minimum": 0}
    }
})json"};

    struct JournalPayloadSource final
    {
        std::string_view eventType{};
        std::string_view schema{};
    };

    inline constexpr auto k_journalPayloadSources = std::array{
        JournalPayloadSource{"fixture.baseline", k_baselinePayloadSchema},
        JournalPayloadSource{"fixture.confirmed", k_confirmedPayloadSchema},
        JournalPayloadSource{"fixture.duplicate", k_duplicatePayloadSchema},
        JournalPayloadSource{"fixture.progress", k_progressPayloadSchema},
    };

    inline constexpr auto k_journalPayloadSchemas = std::array{
        k_baselinePayloadSchema,
        k_confirmedPayloadSchema,
        k_duplicatePayloadSchema,
        k_progressPayloadSchema,
    };

    inline constexpr auto k_effectPayloadSchemas = std::array{k_effectPayloadSchema};

    inline constexpr auto k_observedIdentitySchemas = std::array{k_observedIdentitySchema};

    // One tool of this project's catalog, in the catalog document's own
    // vocabulary. `name` is the LOCAL half of the Tool name: the catalog's own
    // name is that local half under the namespace the deployment registers, so
    // the same source list yields `fixture.alpha.command-1` for one plugin id
    // and `fixture.control.command-1` for another. It is composed rather than
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
        uint32           workflowSteps{};
        uint32           workflowDispatches{};
        uint64           timeoutMillis{};
        bool             usedByExample{};
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
            .workflowSteps      = 8,
            .workflowDispatches = 8,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
            .usedByExample      = true,
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
            .workflowSteps      = 8,
            .workflowDispatches = 8,
            .timeoutMillis      = 1'000,
            .usedByExample      = false,
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
            .workflowSteps      = 1,
            .workflowDispatches = 1,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
            .usedByExample      = false,
        },
        ToolSource{
            .localName          = "command-1",
            .mutability         = ToolMutability::Mutating,
            .surface            = ToolSurface::Semantic,
            .idempotency        = k_ordinaryIdempotency,
            .requiredCapability = "",
            .uiActionBound      = k_uiActionBound,
            .workflowSteps      = 8,
            .workflowDispatches = 8,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
            .usedByExample      = true,
        },
        ToolSource{
            .localName          = "command-2",
            .mutability         = ToolMutability::Mutating,
            .surface            = ToolSurface::Semantic,
            .idempotency        = k_ordinaryIdempotency,
            .requiredCapability = "",
            .uiActionBound      = k_uiActionBound,
            .workflowSteps      = 8,
            .workflowDispatches = 8,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
            .usedByExample      = true,
        },
        ToolSource{
            .localName          = "different-command",
            .mutability         = ToolMutability::Mutating,
            .surface            = ToolSurface::Semantic,
            .idempotency        = k_ordinaryIdempotency,
            .requiredCapability = "",
            .uiActionBound      = k_uiActionBound,
            .workflowSteps      = 8,
            .workflowDispatches = 8,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
            .usedByExample      = true,
        },
        ToolSource{
            .localName          = "mismatched-plan",
            .mutability         = ToolMutability::Mutating,
            .surface            = ToolSurface::Semantic,
            .idempotency        = k_ordinaryIdempotency,
            .requiredCapability = "",
            .uiActionBound      = k_uiActionBound,
            .workflowSteps      = 8,
            .workflowDispatches = 8,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
            .usedByExample      = true,
        },
        ToolSource{
            .localName          = "observe-1",
            .mutability         = ToolMutability::ReadOnly,
            .surface            = ToolSurface::Semantic,
            .idempotency        = ToolIdempotency::ReadSafe,
            .requiredCapability = "",
            .uiActionBound      = k_uiActionBound,
            .workflowSteps      = 8,
            .workflowDispatches = 8,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
            .usedByExample      = true,
        },
        // Its own descriptor is the clamp the oversized proposal meets.
        ToolSource{
            .localName          = "oversized-plan",
            .mutability         = ToolMutability::Mutating,
            .surface            = ToolSurface::Semantic,
            .idempotency        = k_ordinaryIdempotency,
            .requiredCapability = "",
            .uiActionBound      = k_uiActionBound,
            .workflowSteps      = 8,
            .workflowDispatches = 8,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
            .usedByExample      = true,
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
            .workflowSteps      = 8,
            .workflowDispatches = 8,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
            .usedByExample      = true,
        },
        ToolSource{
            .localName          = "reordered-effects",
            .mutability         = ToolMutability::Mutating,
            .surface            = ToolSurface::Semantic,
            .idempotency        = k_ordinaryIdempotency,
            .requiredCapability = "",
            .uiActionBound      = k_uiActionBound,
            .workflowSteps      = 8,
            .workflowDispatches = 8,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
            .usedByExample      = true,
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
            .workflowSteps      = 8,
            .workflowDispatches = 8,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
            .usedByExample      = false,
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
            .workflowSteps      = 8,
            .workflowDispatches = 8,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
            .usedByExample      = false,
        },
        ToolSource{
            .localName          = "two-step-plan",
            .mutability         = ToolMutability::Mutating,
            .surface            = ToolSurface::Semantic,
            .idempotency        = k_ordinaryIdempotency,
            .requiredCapability = "",
            .uiActionBound      = k_uiActionBound,
            .workflowSteps      = 2,
            .workflowDispatches = 2,
            .timeoutMillis      = k_ordinaryTimeoutMillis,
            .usedByExample      = true,
        },
    };

    // Every tool of this catalog carries the same version, because nothing in
    // this project is about a version and a per-tool one would be a number each
    // case had to remember.
    inline constexpr auto k_toolVersion = std::string_view{"1"};

    // The one effect type and scope kind this project's plugin proposes, and the
    // highest risk its descriptors admit. `critical` is deliberately outside the
    // bound so that a case can reach the policy's default deny without
    // rewriting the catalog.
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

    // One catalog name of this fixture, spelled the only way a Tool name is
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
    inline auto makeToolCatalogDeclaration(
        std::string_view pluginId,
        bool exampleOnly
    ) -> project::ToolCatalogDeclaration
    {
        auto const payloadHash = schemaHash(k_effectPayloadSchema);
        auto tools             = std::vector<project::DeclaredTool>{};
        tools.reserve(k_toolSources.size());
        for (auto const& tool : k_toolSources)
        {
            if (exampleOnly && !tool.usedByExample)
            {
                continue;
            }
            auto requiredCapabilities = std::vector<std::string>{};
            if (!tool.requiredCapability.empty())
            {
                requiredCapabilities.emplace_back(tool.requiredCapability);
            }
            tools.emplace_back(project::DeclaredTool{
                .name           = fixtureToolName(pluginId, tool.localName),
                .argumentSchema = "FixtureArguments",
                .resultSchema   = "FixtureResult",
                .descriptor     = ToolDescriptor{
                    .toolVersion          = std::string{k_toolVersion},
                    .requiredCapabilities = std::move(requiredCapabilities),
                    .effectBounds = {
                        EffectBound{
                            .namespacedType    = std::string{k_effectType},
                            .scopeKind         = std::string{k_effectScopeKind},
                            .payloadSchemaHash = payloadHash,
                            .maximumRisk       = Risk::High,
                        },
                    },
                    .uiActionBounds = {std::string{tool.uiActionBound}},
                    .limits         = WorkflowLimits{
                        .maximumSteps        = tool.workflowSteps,
                        .maximumDispatches   = tool.workflowDispatches,
                        .maximumObservations = 256,
                        .maximumWaits        = 64,
                        .maximumElapsedMillis = 600'000,
                    },
                    .timeout = TimeoutPolicy{
                        .maximumElapsedMillis = tool.timeoutMillis,
                        .onTimeout            = TimeoutAction::Reobserve,
                    },
                    .mutability  = tool.mutability,
                    .surface     = tool.surface,
                    .idempotency = tool.idempotency,
                },
            });
        }
        return project::ToolCatalogDeclaration{
            .comment = "The Tool Catalog this registration pins. Mutability and "
                "ToolSurface are declared here and nowhere else, so a controller "
                "is judged against these bytes.",
            .pluginId                   = std::string{pluginId},
            .toolPreconditionSchemaHash = schemaHash(k_toolPreconditionSchema),
            .effectPayloadSchemaHashes  = {payloadHash},
            .tools                      = std::move(tools),
        };
    }

    [[nodiscard]]
    inline auto toolCatalogDeclaration(
        std::string_view pluginId
    ) -> project::ToolCatalogDeclaration
    {
        return makeToolCatalogDeclaration(pluginId, false);
    }

    [[nodiscard]]
    inline auto exampleToolCatalogDeclaration(
        std::string_view pluginId,
        ContentHash toolPreconditionSchemaHash,
        ContentHash effectPayloadSchemaHash
    ) -> project::ToolCatalogDeclaration
    {
        auto declaration                       = makeToolCatalogDeclaration(pluginId, true);
        declaration.toolPreconditionSchemaHash = toolPreconditionSchemaHash;
        declaration.effectPayloadSchemaHashes  = {effectPayloadSchemaHash};
        for (auto& tool : declaration.tools)
        {
            for (auto& bound : tool.descriptor.effectBounds)
            {
                bound.payloadSchemaHash = effectPayloadSchemaHash;
            }
        }
        return declaration;
    }

    // The two documents a deployment writes rather than authors: each carries
    // the sha256 of schema bytes, so each is assembled here from the bytes
    // above rather than transcribed.
    class DeploymentBundle final
    {
        std::string m_pluginId{};
        std::string m_toolCatalog{};
        std::string m_journalEventManifest{};

    public:
        explicit DeploymentBundle(std::string_view pluginId)
            : m_pluginId{pluginId}
        {
            auto catalog = project::generateToolCatalog(
                toolCatalogDeclaration(pluginId)
            );
            UF_CHECK(catalog.has_value());
            m_toolCatalog = *std::move(catalog);

            m_journalEventManifest = R"json({"$comment":)json"
                R"json("One payload schema per namespaced event type this project )json"
                R"json(can emit, and the sha256 of that schema's exact bytes -- which )json"
                R"json(is the payload_schema_hash the Operator records beside every )json"
                R"json(entry the schema accepted.","payload_schemas":[)json";
            auto first = true;
            for (auto const& payload : k_journalPayloadSources)
            {
                if (!first)
                {
                    m_journalEventManifest += ',';
                }
                first = false;
                m_journalEventManifest += R"json({"namespaced_event_type":")json";
                m_journalEventManifest += payload.eventType;
                m_journalEventManifest += R"json(","sha256":")json";
                m_journalEventManifest += schemaHashHex(payload.schema);
                m_journalEventManifest += R"json("})json";
            }
            m_journalEventManifest += R"json(],"plugin_id":")json"
                + m_pluginId
                + R"json(","schema":"umbraflow-journal-event-schema-manifest/v1"})json";
        }

        [[nodiscard]]
        auto toolCatalog() const UF_LIFETIME_BOUND -> std::string const&
        {
            return m_toolCatalog;
        }

        [[nodiscard]]
        auto journalEventManifest() const UF_LIFETIME_BOUND -> std::string const&
        {
            return m_journalEventManifest;
        }

        // Views into this bundle and into the static schema storage above, so
        // the result must not outlive the bundle it came from.
        [[nodiscard]]
        auto sources() const UF_LIFETIME_BOUND
            -> deployment::ProjectDeploymentSources
        {
            return deployment::ProjectDeploymentSources{
                .pluginId                        = m_pluginId,
                .projectState                    = k_projectStateSchema,
                .toolPrecondition                = k_toolPreconditionSchema,
                .toolCatalog                     = m_toolCatalog,
                .journalEventManifest            = m_journalEventManifest,
                .journalPayloadSchemas           = k_journalPayloadSchemas,
                .effectPayloadSchemas            = k_effectPayloadSchemas,
                .observedInstanceIdentitySchemas = k_observedIdentitySchemas,
            };
        }
    };
}
