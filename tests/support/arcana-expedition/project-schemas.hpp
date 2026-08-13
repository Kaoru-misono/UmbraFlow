#pragma once

// Everything a second game's deployment holds for one ProjectRegistration.
//
// Nothing here is shared with this repository's own exemplar: a different state
// shape, a non-empty observation, a bounded numeric tool argument, four journal
// payloads of three different shapes, and a reconcile vocabulary in which the
// request and the verdict say different things -- so the disposition the
// authority reads is nowhere in the document that produced it.
//
// It is written the way a consumer writes one: the schemas are files a project
// authors, and the three manifests are what a deployment assembles from them,
// because each carries the sha256 of bytes only the deployment can hash.

#include <deployment/project-deployment.hpp>

#include <project/tool-catalog.hpp>

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
    // How far the expedition has travelled, counted in turns. The reducer
    // rebuilds it from the journal prefix and nothing else is in it.
    inline constexpr auto k_projectStateSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/state",
    "title": "arcana expedition ProjectState",
    "type": "object",
    "additionalProperties": false,
    "required": ["turn"],
    "properties": {
        "turn": {"type": "integer", "minimum": 0}
    }
})json"};

    // What derive reads off one observation: whether the camp is on screen.
    inline constexpr auto k_projectObservationSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/observation",
    "title": "arcana expedition ProjectObservation",
    "type": "object",
    "additionalProperties": false,
    "required": ["visible"],
    "properties": {
        "visible": {"type": "boolean"}
    }
})json"};

    // Every expedition tool is stated in the expedition's own vocabulary -- a
    // number of leagues to march, and nothing that describes the screen. Eight
    // is the longest march a single command may order.
    inline constexpr auto k_toolPreconditionSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/tool-precondition",
    "title": "arcana expedition tool arguments",
    "$defs": {
        "MarchArguments": {
            "type": "object",
            "additionalProperties": false,
            "required": ["steps"],
            "properties": {
                "steps": {"type": "integer", "minimum": 1, "maximum": 8}
            }
        }
    }
})json"};

    // The request carries what was observed; the verdict carries what the
    // expedition concluded. They are deliberately different vocabularies, and
    // neither word is one of the framework's five dispositions: the mapping
    // lives in the reconcile manifest, so only this project decides which
    // observation becomes which conclusion.
    inline constexpr auto k_reconcileSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/reconcile",
    "title": "arcana expedition reconcile documents",
    "$defs": {
        "ReconcileRequest": {
            "type": "object",
            "additionalProperties": false,
            "required": ["observed"],
            "properties": {
                "observed": {
                    "enum": ["advanced", "arrived", "blocked", "nothing"]
                }
            }
        },
        "ReconcileVerdict": {
            "type": "object",
            "additionalProperties": false,
            "required": ["verdict"],
            "properties": {
                "verdict": {
                    "enum": ["underway", "settled", "refused", "unclear"]
                }
            }
        }
    }
})json"};

    inline constexpr auto k_foundedPayloadSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/journal/expedition.founded",
    "title": "arcana expedition.founded payload",
    "type": "object",
    "additionalProperties": false,
    "required": ["camp"],
    "properties": {
        "camp": {"enum": ["north", "south"]}
    }
})json"};

    // The three march events carry the same measurement and differ only in what
    // they say happened, so one payload shape serves all three. Their schemas
    // are separate documents because the Operator records one payload schema
    // hash per event type.
    inline constexpr auto k_advancedPayloadSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/journal/expedition.advanced",
    "title": "arcana expedition.advanced payload",
    "type": "object",
    "additionalProperties": false,
    "required": ["leagues"],
    "properties": {
        "leagues": {"type": "integer", "minimum": 0, "maximum": 64}
    }
})json"};

    inline constexpr auto k_arrivedPayloadSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/journal/expedition.arrived",
    "title": "arcana expedition.arrived payload",
    "type": "object",
    "additionalProperties": false,
    "required": ["leagues"],
    "properties": {
        "leagues": {"type": "integer", "minimum": 0, "maximum": 64}
    }
})json"};

    inline constexpr auto k_blockedPayloadSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/journal/expedition.blocked",
    "title": "arcana expedition.blocked payload",
    "type": "object",
    "additionalProperties": false,
    "required": ["leagues"],
    "properties": {
        "leagues": {"type": "integer", "minimum": 0, "maximum": 64}
    }
})json"};

    // The payload of every OP:`ExpectedEffect` this project proposes: the turn
    // the march would be taken on. Its sha256 is the payload_schema_hash the
    // plugin writes into each effect.
    inline constexpr auto k_effectPayloadSchema = std::string_view{R"json({
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

    struct JournalPayloadSource final
    {
        std::string_view eventType{};
        std::string_view schema{};
    };

    inline constexpr auto k_journalPayloadSources = std::array{
        JournalPayloadSource{"expedition.advanced", k_advancedPayloadSchema},
        JournalPayloadSource{"expedition.arrived", k_arrivedPayloadSchema},
        JournalPayloadSource{"expedition.blocked", k_blockedPayloadSchema},
        JournalPayloadSource{"expedition.founded", k_foundedPayloadSchema},
    };

    inline constexpr auto k_journalPayloadSchemas = std::array{
        k_advancedPayloadSchema,
        k_arrivedPayloadSchema,
        k_blockedPayloadSchema,
        k_foundedPayloadSchema,
    };

    inline constexpr auto k_effectPayloadSchemas = std::array{k_effectPayloadSchema};

    struct ToolSource final
    {
        std::string_view name{};
        std::string_view version{};
        ToolMutability   mutability{ToolMutability::Mutating};
    };

    inline constexpr auto k_toolSources = std::array{
        ToolSource{"expedition.approval", "3", ToolMutability::Mutating},
        ToolSource{"expedition.move", "3", ToolMutability::Mutating},
        ToolSource{"expedition.survey", "2", ToolMutability::ReadOnly},
        ToolSource{"expedition.trade", "3", ToolMutability::Mutating},
    };

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
    inline auto toolCatalogDeclaration(
        std::string_view pluginId
    ) -> project::ToolCatalogDeclaration
    {
        auto const payloadHash = schemaHash(k_effectPayloadSchema);
        auto tools             = std::vector<project::DeclaredTool>{};
        tools.reserve(k_toolSources.size());
        for (auto const& tool : k_toolSources)
        {
            tools.emplace_back(project::DeclaredTool{
                .name           = std::string{tool.name},
                .argumentSchema = "MarchArguments",
                .descriptor     = ToolDescriptor{
                    .toolVersion          = std::string{tool.version},
                    .requiredCapabilities = {},
                    .effectBounds = {
                        EffectBound{
                            .namespacedType    = "expedition.march",
                            .scopeKind         = "camp",
                            .payloadSchemaHash = payloadHash,
                            .maximumRisk       = Risk::High,
                        },
                    },
                    .uiActionBounds = {"expedition.step"},
                    .limits         = WorkflowLimits{
                        .maximumSteps        = 8,
                        .maximumDispatches   = 8,
                        .maximumObservations = 256,
                        .maximumWaits        = 64,
                        .maximumElapsedMillis = 600'000,
                    },
                    .timeout = TimeoutPolicy{
                        .maximumElapsedMillis = 60'000,
                        .onTimeout            = TimeoutAction::Reobserve,
                    },
                    .mutability = tool.mutability,
                    .surface    = ToolSurface::Semantic,
                    .idempotency = tool.mutability == ToolMutability::ReadOnly
                        ? ToolIdempotency::ReadSafe
                        : ToolIdempotency::DeliverySafe,
                },
            });
        }
        return project::ToolCatalogDeclaration{
            .comment = "The expedition's Tool Catalog. Every tool is stated in "
                "the expedition's own vocabulary, so the catalog declares them "
                "semantic; leaving that to the default would declare the opposite "
                "by omission.",
            .pluginId                   = std::string{pluginId},
            .toolPreconditionSchemaHash = schemaHash(k_toolPreconditionSchema),
            .effectPayloadSchemaHashes  = {payloadHash},
            .tools                      = std::move(tools),
        };
    }

    [[nodiscard]]
    inline auto exampleToolCatalogDeclaration(
        std::string_view pluginId,
        ContentHash toolPreconditionSchemaHash,
        ContentHash effectPayloadSchemaHash
    ) -> project::ToolCatalogDeclaration
    {
        auto declaration                       = toolCatalogDeclaration(pluginId);
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

    // The three documents this deployment assembles rather than authors. Each
    // names the schema bytes it governs by sha256 and the registration it
    // belongs to by plugin id, so neither link is a convention.
    class DeploymentBundle final
    {
        std::string m_pluginId{};
        std::string m_toolCatalog{};
        std::string m_journalEventManifest{};
        std::string m_reconcileManifest{};

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
                R"json("One payload schema per namespaced event type the expedition )json"
                R"json(can emit, and the sha256 of that schema's exact bytes.",)json"
                R"json("payload_schemas":[)json";
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

            m_reconcileManifest = R"json({"$comment":)json"
                R"json("The expedition's four verdict words and the framework )json"
                R"json(disposition each one means. No word here appears in a )json"
                R"json(ReconcileRequest, so the disposition cannot be read off the )json"
                R"json(document that produced the verdict.","dispositions":[)json"
                R"json({"disposition":"continue","value":"underway"},)json"
                R"json({"disposition":"confirmed","value":"settled"},)json"
                R"json({"disposition":"rejected","value":"refused"},)json"
                R"json({"disposition":"ambiguous","value":"unclear"}],)json"
                R"json("plugin_id":")json"
                + m_pluginId
                + R"json(","reconcile_schema_sha256":")json"
                + schemaHashHex(k_reconcileSchema)
                + R"json(","request_definition":"ReconcileRequest",)json"
                  R"json("schema":"umbraflow-reconcile-manifest/v1",)json"
                  R"json("verdict_definition":"ReconcileVerdict",)json"
                  R"json("verdict_member":"verdict"})json";
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

        [[nodiscard]]
        auto reconcileManifest() const UF_LIFETIME_BOUND -> std::string const&
        {
            return m_reconcileManifest;
        }

        // Views into this bundle and into the static schema storage above, so
        // the result must not outlive the bundle it came from.
        [[nodiscard]]
        auto sources() const UF_LIFETIME_BOUND
            -> deployment::ProjectDeploymentSources
        {
            return deployment::ProjectDeploymentSources{
                .pluginId              = m_pluginId,
                .projectState          = k_projectStateSchema,
                .projectObservation    = k_projectObservationSchema,
                .toolPrecondition      = k_toolPreconditionSchema,
                .reconcile             = k_reconcileSchema,
                .toolCatalog           = m_toolCatalog,
                .journalEventManifest  = m_journalEventManifest,
                .reconcileManifest     = m_reconcileManifest,
                .journalPayloadSchemas = k_journalPayloadSchemas,
                .effectPayloadSchemas  = k_effectPayloadSchemas,
            };
        }
    };
}
