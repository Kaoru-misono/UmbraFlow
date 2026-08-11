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

#include <core/error/contracts.hpp>
#include <core/safety/annotations.hpp>

#include <domain/content-hash.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

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

    // This project's derive reads nothing out of the world: it answers `{}` for
    // every envelope, so the honest schema for its observation is the empty
    // object and nothing more. It is a real constraint -- every non-empty
    // document is refused -- and a weak one, and the weakness is the fixture's
    // rather than the schema's.
    inline constexpr auto k_projectObservationSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/observation",
    "title": "umbraflow fixture ProjectObservation",
    "type": "object",
    "additionalProperties": false
})json"};

    // Every tool in this fixture's catalog takes the same one argument, so the
    // catalog names one definition for all of them. A real catalog names one per
    // tool; this one has nothing to tell them apart by.
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
        }
    }
})json"};

    // This fixture's reconcile is the identity, so its request and its verdict
    // are one shape and the disposition is spelled in the request. Nothing in
    // the suite may rely on that; the arcana exemplar maps the two apart.
    //
    // The root asserts nothing: both definitions are reached by name, and a
    // root oneOf over two identical branches would match neither.
    inline constexpr auto k_reconcileSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/reconcile",
    "title": "umbraflow fixture reconcile documents",
    "$defs": {
        "ReconcileRequest": {
            "type": "object",
            "additionalProperties": false,
            "required": ["disposition"],
            "properties": {
                "disposition": {
                    "enum": [
                        "continue",
                        "confirmed",
                        "rejected",
                        "ambiguous",
                        "diverged"
                    ]
                }
            }
        },
        "ReconcileVerdict": {
            "type": "object",
            "additionalProperties": false,
            "required": ["disposition"],
            "properties": {
                "disposition": {
                    "enum": [
                        "continue",
                        "confirmed",
                        "rejected",
                        "ambiguous",
                        "diverged"
                    ]
                }
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

    // The payload of every OP:`ExpectedEffect` this project's plugin proposes.
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

    // One tool of this project's catalog, in the catalog document's own
    // vocabulary. The names are not namespaced, which
    // schema/umbraflow-operator-v1.schema.json requires of a tool_name; see the
    // note in project-fixture.hpp.
    struct ToolSource final
    {
        std::string_view name{};
        std::string_view version{};
        std::string_view mutability{};
        std::string_view surface{};
    };

    inline constexpr auto k_toolSources = std::array{
        ToolSource{"approval-plan", "1", "mutating", "semantic"},
        ToolSource{"command-1", "1", "mutating", "semantic"},
        ToolSource{"command-2", "1", "mutating", "semantic"},
        ToolSource{"different-command", "1", "mutating", "semantic"},
        ToolSource{"mismatched-plan", "1", "mutating", "semantic"},
        ToolSource{"observe-1", "1", "read_only", "semantic"},
        ToolSource{"oversized-plan", "1", "mutating", "semantic"},
        // The machine-surface tool: it names a coordinate, so no online Agent
        // may be handed it. Read-only so that the p03 cases never contend for
        // the mutation-chain slot the p01 cases are about.
        ToolSource{"raw-coordinate-click", "1", "read_only", "privileged"},
        ToolSource{"reordered-effects", "1", "mutating", "semantic"},
        ToolSource{"two-step-plan", "1", "mutating", "semantic"},
    };

    [[nodiscard]]
    inline auto schemaHashHex(std::string_view bytes) -> std::string
    {
        auto const digest = sha256(std::as_bytes(std::span{bytes}));
        UF_CHECK(digest.has_value());
        return digest->hex();
    }

    // The three documents a deployment writes rather than authors: each carries
    // the sha256 of schema bytes, so each is assembled here from the bytes
    // above rather than transcribed.
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
            m_toolCatalog = R"json({"$comment":)json"
                R"json("The Tool Catalog this registration pins. Mutability and )json"
                R"json(ToolSurface are declared here and nowhere else, so a controller )json"
                R"json(is judged against these bytes.","plugin_id":")json"
                + m_pluginId
                + R"json(","schema":"umbraflow-tool-catalog/v1",)json"
                  R"json("tool_precondition_sha256":")json"
                + schemaHashHex(k_toolPreconditionSchema)
                + R"json(","tools":[)json";
            auto first = true;
            for (auto const& tool : k_toolSources)
            {
                if (!first)
                {
                    m_toolCatalog += ',';
                }
                first = false;
                m_toolCatalog += R"json({"argument_schema":"FixtureArguments",)json"
                    R"json("mutability":")json";
                m_toolCatalog += tool.mutability;
                m_toolCatalog += R"json(","name":")json";
                m_toolCatalog += tool.name;
                m_toolCatalog += R"json(","surface":")json";
                m_toolCatalog += tool.surface;
                m_toolCatalog += R"json(","version":")json";
                m_toolCatalog += tool.version;
                m_toolCatalog += R"json("})json";
            }
            m_toolCatalog += "]}";

            m_journalEventManifest = R"json({"$comment":)json"
                R"json("One payload schema per namespaced event type this project )json"
                R"json(can emit, and the sha256 of that schema's exact bytes -- which )json"
                R"json(is the payload_schema_hash the Operator records beside every )json"
                R"json(entry the schema accepted.","payload_schemas":[)json";
            first = true;
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
                R"json("The disposition is read out of the verdict's own member )json"
                R"json(through this mapping. This project's reconcile is the identity, )json"
                R"json(so its verdict words are the framework's; the arcana exemplar's )json"
                R"json(are not.","dispositions":[)json"
                R"json({"disposition":"ambiguous","value":"ambiguous"},)json"
                R"json({"disposition":"confirmed","value":"confirmed"},)json"
                R"json({"disposition":"continue","value":"continue"},)json"
                R"json({"disposition":"diverged","value":"diverged"},)json"
                R"json({"disposition":"rejected","value":"rejected"}],)json"
                R"json("plugin_id":")json"
                + m_pluginId
                + R"json(","reconcile_schema_sha256":")json"
                + schemaHashHex(k_reconcileSchema)
                + R"json(","request_definition":"ReconcileRequest",)json"
                  R"json("schema":"umbraflow-reconcile-manifest/v1",)json"
                  R"json("verdict_definition":"ReconcileVerdict",)json"
                  R"json("verdict_member":"disposition"})json";
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
