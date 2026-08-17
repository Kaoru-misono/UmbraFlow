#include <project/declarative-workflow-tool.hpp>
#include <project/tool-catalog.hpp>

#include <deployment/project-deployment.hpp>

#include <operator/manifest.hpp>
#include <operator/project-plugin.hpp>
#include <operator/tool-descriptor.hpp>

#include <json/value.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Two authoring paths reach the same five-function SPI: an adapter generated
// from umbraflow-declarative-workflow-tool/v1, and a whole plugin module
// written by hand. Nothing held them to one another, and nothing had ever put
// a generated adapter through ProjectPluginRegistrar::registerPlugin -- every
// generated-tier gate compiles the adapter bare through PureDataProgram, so
// whether a generated adapter satisfies the pinned schemas at the admission
// boundary was unmeasured.
namespace uf::project
{
    namespace
    {
        constexpr auto k_pluginId    = std::string_view{"chaos.project"};
        constexpr auto k_toolName    = std::string_view{"chaos.dismiss_known_overlay"};
        constexpr auto k_uiAction    = std::string_view{"chaos.ui.dismiss_overlay"};
        constexpr auto k_surfaceId   = std::string_view{"chaos.overlay_layer"};
        constexpr auto k_instanceKind = std::string_view{"chaos.overlay"};

        constexpr auto k_targetInstance = std::string_view{
            "oi1_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        };
        constexpr auto k_baselineEventType = std::string_view{
            "chaos.overlay_dismissed"
        };
        constexpr auto k_frozenPlanHash = std::string_view{
            "0000000000000000000000000000000000000000000000000000000000000000"
        };

        // The four project-owned schemas this deployment pins. They are written
        // out rather than borrowed from examples/ because the exemplar's
        // observation schema accepts only the empty object, and every document
        // a dismiss-overlay tool produces or consumes has members.
        constexpr auto k_projectStateSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/state",
    "title": "chaos.project state",
    "type": "object",
    "additionalProperties": false,
    "properties": {
        "dismissed_overlays": {
            "type": "array",
            "items": {"type": "string", "minLength": 1}
        }
    }
})json"};

        // The proposal envelope, which is the derived ProjectObservation the
        // deployment judges. The final envelope never reaches this schema: the
        // Operator mints it, and the plan and step inputs pin it by $ref to the
        // framework schema instead.
        constexpr auto k_projectObservationSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/observation",
    "title": "chaos.project overlay observation",
    "type": "object",
    "additionalProperties": false,
    "required": [
        "schema",
        "canonical_opaque_payload",
        "project_tool_preconditions",
        "observed_instance_proposals"
    ],
    "properties": {
        "schema": {"const": "umbraflow-project-observation-proposal/v1"},
        "canonical_opaque_payload": {
            "type": "object",
            "additionalProperties": false,
            "required": ["surface_observations"],
            "properties": {
                "surface_observations": {
                    "type": "array",
                    "items": {
                        "type": "object",
                        "additionalProperties": false,
                        "required": [
                            "fresh",
                            "resolution",
                            "surface_id",
                            "unambiguous"
                        ],
                        "properties": {
                            "fresh": {"type": "boolean"},
                            "resolution": {"enum": ["resolved", "unresolved"]},
                            "surface_id": {"type": "string", "minLength": 1},
                            "unambiguous": {"type": "boolean"}
                        }
                    }
                }
            }
        },
        "observed_instance_proposals": {
            "type": "array",
            "items": {
                "type": "object",
                "additionalProperties": false,
                "required": [
                    "local_ref",
                    "kind",
                    "identity_schema_id",
                    "semantic_identity_basis",
                    "opaque_project_payload"
                ],
                "properties": {
                    "local_ref": {"type": "string", "minLength": 1},
                    "kind": {"type": "string", "minLength": 1},
                    "identity_schema_id": {"type": "string", "minLength": 1},
                    "semantic_identity_basis": {"type": "object"},
                    "opaque_project_payload": true
                }
            }
        },
        "project_tool_preconditions": {"type": "array"}
    }
})json"};

        constexpr auto k_toolPreconditionSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/tool-precondition",
    "title": "chaos.project tool arguments",
    "$defs": {
        "DismissArguments": {
            "type": "object",
            "additionalProperties": false,
            "required": ["observed_instance_id"],
            "properties": {
                "observed_instance_id": {"type": "string", "minLength": 1}
            }
        }
    }
})json"};

        constexpr auto k_reconcileSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/reconcile",
    "title": "chaos.project reconcile documents",
    "$defs": {
        "SurfaceObservation": {
            "type": "object",
            "additionalProperties": false,
            "required": ["fresh", "resolution", "surface_id", "unambiguous"],
            "properties": {
                "fresh": {"type": "boolean"},
                "resolution": {"enum": ["resolved", "unresolved"]},
                "surface_id": {"type": "string", "minLength": 1},
                "unambiguous": {"type": "boolean"}
            }
        },
        "ReconcileRequest": {
            "type": "object",
            "additionalProperties": false,
            "required": ["canonical_args", "project_observation"],
            "properties": {
                "canonical_args": {
                    "type": "object",
                    "additionalProperties": false,
                    "required": ["observed_instance_id"],
                    "properties": {
                        "observed_instance_id": {
                            "type": "string",
                            "minLength": 1
                        }
                    }
                },
                "project_observation": {
                    "type": "object",
                    "additionalProperties": false,
                    "required": [
                        "schema",
                        "canonical_opaque_payload",
                        "project_tool_preconditions",
                        "observed_instances"
                    ],
                    "properties": {
                        "schema": {
                            "const": "umbraflow-project-observation/v1"
                        },
                        "canonical_opaque_payload": {
                            "type": "object",
                            "additionalProperties": false,
                            "required": ["surface_observations"],
                            "properties": {
                                "surface_observations": {
                                    "type": "array",
                                    "items": {
                                        "$ref": "#/$defs/SurfaceObservation"
                                    }
                                }
                            }
                        },
                        "project_tool_preconditions": {"type": "array"},
                        "observed_instances": {
                            "type": "array",
                            "items": {
                                "type": "object",
                                "additionalProperties": false,
                                "required": [
                                    "kind",
                                    "observed_instance_id",
                                    "opaque_project_payload"
                                ],
                                "properties": {
                                    "kind": {"type": "string", "minLength": 1},
                                    "observed_instance_id": {
                                        "type": "string",
                                        "minLength": 1
                                    },
                                    "opaque_project_payload": true
                                }
                            }
                        }
                    }
                }
            }
        },
        "ReconcileVerdict": {
            "type": "object",
            "additionalProperties": false,
            "required": [
                "disposition",
                "findings",
                "journal_events",
                "observed_outcomes"
            ],
            "properties": {
                "disposition": {
                    "enum": [
                        "continue",
                        "confirmed",
                        "rejected",
                        "ambiguous",
                        "diverged"
                    ]
                },
                "findings": {
                    "type": "array",
                    "items": {
                        "type": "object",
                        "additionalProperties": false,
                        "required": ["kind", "observed_instance_id"],
                        "properties": {
                            "kind": {
                                "enum": [
                                    "observed_instance_absent",
                                    "observed_instance_present"
                                ]
                            },
                            "observed_instance_id": {
                                "type": "string",
                                "minLength": 1
                            }
                        }
                    }
                },
                "journal_events": {"type": "array"},
                "observed_outcomes": {"type": "array"}
            }
        }
    }
})json"};

        constexpr auto k_journalPayloadSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/journal/overlay-dismissed",
    "title": "chaos.overlay_dismissed payload",
    "type": "object",
    "additionalProperties": false,
    "required": ["overlay"],
    "properties": {
        "overlay": {"type": "string", "minLength": 1}
    }
})json"};

        // Two states and two steps, so the generated adapter has to mint both
        // an OP:WaitIntent and an OP:UIActionIntent at the boundary that judges
        // them against one oneOf.
        constexpr auto k_boundedDeclaration = std::string_view{R"json({
  "schema": "umbraflow-declarative-workflow-tool/v1",
  "tool_name": "chaos.dismiss_known_overlay",
  "target_argument": "observed_instance_id",
  "allowed_instance_kinds": ["chaos.overlay"],
  "fresh_observation": {
    "required_surface": "chaos.overlay_layer",
    "require_unambiguous": true
  },
  "ui_finding": {"kind": "observed_instance_absent"},
  "states": [
    {
      "state_key": "await-overlay",
      "kind": "wait",
      "observation_budget": 1,
      "timeout_ms": 1000
    },
    {
      "state_key": "dismiss-overlay",
      "kind": "ui_action",
      "ui_action": "chaos.ui.dismiss_overlay",
      "timeout_ms": 2000
    }
  ],
  "steps": ["await-overlay", "dismiss-overlay"],
  "bounds": {
    "maximum_states": 2,
    "maximum_steps": 2,
    "maximum_dispatches": 1,
    "maximum_observations": 2,
    "maximum_waits": 1,
    "maximum_elapsed_ms": 3000
  }
})json"};

        // The one logical tool the parity cases implement twice.
        constexpr auto k_oneStepDeclaration = std::string_view{R"json({
  "schema": "umbraflow-declarative-workflow-tool/v1",
  "tool_name": "chaos.dismiss_known_overlay",
  "target_argument": "observed_instance_id",
  "allowed_instance_kinds": ["chaos.overlay"],
  "fresh_observation": {
    "required_surface": "chaos.overlay_layer",
    "require_unambiguous": true
  },
  "ui_finding": {"kind": "observed_instance_absent"},
  "states": [
    {
      "state_key": "dismiss-overlay",
      "kind": "ui_action",
      "ui_action": "chaos.ui.dismiss_overlay",
      "timeout_ms": 2000
    }
  ],
  "steps": ["dismiss-overlay"],
  "bounds": {
    "maximum_states": 1,
    "maximum_steps": 1,
    "maximum_dispatches": 1,
    "maximum_observations": 1,
    "maximum_waits": 0,
    "maximum_elapsed_ms": 2000
  }
})json"};

        // The hand-written twin of k_oneStepDeclaration: the same tool, written
        // as a whole five-function module rather than declared. It is written
        // out here rather than derived from the generator's output, because a
        // twin produced by editing generated text would compare the generator
        // with itself.
        //
        // Deployment block justification, as U5c requires of a hand-written
        // plugin: this module exists to be a second, independent implementation
        // of one declared tool, so that the SPI-level observations of the two
        // authoring paths can be compared. umbraflow-declarative-workflow-tool/v1
        // has no member that asks for a second implementation of a tool it
        // already generates, and a declaration that produced this module would
        // be the generated path again under another name.
        constexpr auto k_handWrittenTwin = std::string_view{R"LUAU(
local TOOL_NAME = "chaos.dismiss_known_overlay"
local SURFACE = "chaos.overlay_layer"
local ACTION = "chaos.ui.dismiss_overlay"
local ARGUMENT = "observed_instance_id"
local FINDING_KIND = "observed_instance_absent"
local STEP_KEY = "dismiss-overlay"
local STEP_TIMEOUT_MS = 2000
local ALLOWED_KINDS = { ["chaos.overlay"] = true }
local LIMITS = {
    maximum_dispatches = 1,
    maximum_elapsed_ms = 2000,
    maximum_observations = 1,
    maximum_steps = 1,
    maximum_waits = 0,
}

local function argumentTarget(input)
    local args = input.canonical_args
    if type(args) ~= "table" then
        error("WorkflowCanonicalArgsMissing: canonical_args must be an object")
    end
    local target = args[ARGUMENT]
    if type(target) ~= "string" or target == "" then
        error("WorkflowTargetMissing: canonical args do not name the observed-instance target")
    end
    return target
end

local function freshObservation(input)
    local observed = input.project_observation
    if type(observed) ~= "table" then
        error("MissingStepObservation: every workflow step requires a fresh observation")
    end
    local payload = observed.canonical_opaque_payload
    if type(payload) ~= "table" or type(payload.surface_observations) ~= "table" then
        error("MissingStepObservation: fresh observation carries no Surface evidence")
    end
    for _, surface in ipairs(payload.surface_observations) do
        if surface.surface_id == SURFACE then
            if surface.fresh ~= true then
                error("StaleObservation: required Surface evidence is stale")
            end
            if surface.resolution ~= "resolved" then
                error("FreshSurfaceUnresolved: required Surface is not resolved")
            end
            if surface.unambiguous ~= true then
                error("FreshSurfaceAmbiguous: required Surface is ambiguous")
            end
            return observed
        end
    end
    error("MissingStepObservation: required Surface is absent")
end

local function locate(observed, target)
    if type(observed.observed_instances) ~= "table" then
        error("MissingStepObservation: observed_instances must be an array")
    end
    for _, instance in ipairs(observed.observed_instances) do
        if instance.observed_instance_id == target then
            if ALLOWED_KINDS[instance.kind] ~= true then
                error("WorkflowTargetKindRejected: target kind is outside allowed_instance_kinds")
            end
            return instance
        end
    end
    return nil
end

-- A step envelope carries no canonical_args, so the UI target is the one
-- instance of an allowed kind the fresh observation shows.
local function shownTarget(observed)
    if type(observed.observed_instances) ~= "table" then
        error("MissingStepObservation: observed_instances must be an array")
    end
    local shown = nil
    for _, instance in ipairs(observed.observed_instances) do
        if ALLOWED_KINDS[instance.kind] == true then
            if shown ~= nil then
                error("WorkflowTargetAmbiguous: the fresh observation holds more than one instance of an allowed kind")
            end
            shown = instance
        end
    end
    if shown == nil then
        error("ObservedInstanceStale: no instance of an allowed kind is present in the fresh observation")
    end
    if type(shown.observed_instance_id) ~= "string" or shown.observed_instance_id == "" then
        error("MissingStepObservation: an observed instance carries no identifier")
    end
    return shown.observed_instance_id
end

return {
    plugin_id = "chaos.project",
    derive = function(_input)
        return {
            schema = "umbraflow-project-observation-proposal/v1",
            canonical_opaque_payload = { surface_observations = {} },
            project_tool_preconditions = {},
            observed_instance_proposals = {},
        }
    end,
    plan = function(input)
        if input.tool_name ~= TOOL_NAME then
            error("WorkflowToolMismatch: plan input names another tool")
        end
        local target = argumentTarget(input)
        local observed = freshObservation(input)
        if locate(observed, target) == nil then
            error("ObservedInstanceStale: target is absent from the fresh observation")
        end
        return {
            allowed_ui_actions = { ACTION },
            canonical_args = input.canonical_args,
            effects = {},
            tool_name = TOOL_NAME,
            tool_version = input.tool_version,
            workflow_limits = LIMITS,
        }
    end,
    next_step = function(input)
        if input.step_index ~= 1 then
            error("WorkflowStepBound: step_index exceeds the finite schedule")
        end
        local observed = freshObservation(input)
        local target = shownTarget(observed)
        return {
            action = {
                action_id = ACTION,
                canonical_parameters = { [ARGUMENT] = target },
                surface_id = SURFACE,
                ui_target_id = target,
            },
            binding_variant_constraints = {},
            delivery_class = "delivery_safe",
            expected_ui_postconditions = {},
            required_ui_preconditions = {{
                kind = "fresh_unambiguous_surface",
                required_surface = SURFACE,
            }},
            step_key = STEP_KEY,
            timeout_policy = {
                maximum_elapsed_ms = STEP_TIMEOUT_MS,
                on_timeout = "reconcile",
            },
        }
    end,
    reconcile = function(input)
        local target = argumentTarget(input)
        local observed = freshObservation(input)
        local present = locate(observed, target) ~= nil
        local findings = {}
        if (FINDING_KIND == "observed_instance_present") == present then
            findings = {{ kind = FINDING_KIND, observed_instance_id = target }}
        end
        return {
            disposition = "continue",
            findings = findings,
            journal_events = {},
            observed_outcomes = {},
        }
    end,
    reduce = function(_input)
        return canon.emptyObject
    end,
}
)LUAU"};

        // Every refusal either implementation of this tool can raise, by the
        // code its message opens with. A refusal naming none of them is itself
        // a disagreement: the comparison would otherwise read two unrecognized
        // messages as one shared silence.
        constexpr auto k_refusalCodes = std::array{
            std::string_view{"WorkflowCanonicalArgsMissing"},
            std::string_view{"WorkflowTargetKindRejected"},
            std::string_view{"WorkflowTargetAmbiguous"},
            std::string_view{"MissingStepObservation"},
            std::string_view{"FreshSurfaceUnresolved"},
            std::string_view{"FreshSurfaceAmbiguous"},
            std::string_view{"ObservedInstanceStale"},
            std::string_view{"WorkflowToolMismatch"},
            std::string_view{"WorkflowTargetMissing"},
            std::string_view{"WorkflowStepBound"},
            std::string_view{"StaleObservation"},
        };

        [[nodiscard]]
        auto refuse(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        [[nodiscard]]
        auto hashOf(std::string_view text) -> Result<ContentHash>
        {
            return sha256(std::as_bytes(std::span{text}));
        }

        [[nodiscard]]
        auto canonicalOf(std::string_view text) -> Result<std::string>
        {
            UF_TRY_VALUE(value, json::parse(text));
            return json::canonicalBytes(value);
        }

        [[nodiscard]]
        auto replacedOnce(
            std::string_view source,
            std::string_view before,
            std::string_view after
        ) -> Result<std::string>
        {
            auto const position = source.find(before);
            if (position == std::string_view::npos)
            {
                return refuse(std::format(
                    "a mutation vector must name existing source bytes: \"{}\"",
                    before
                ));
            }
            auto mutated = std::string{source};
            mutated.replace(position, before.size(), after);
            return mutated;
        }

        [[nodiscard]]
        auto toolCatalogBytes(ContentHash toolPreconditionSchemaHash)
            -> Result<std::string>
        {
            return generateToolCatalog(ToolCatalogDeclaration{
                .comment                    = "The one tool both authoring paths implement.",
                .pluginId                   = std::string{k_pluginId},
                .toolPreconditionSchemaHash = toolPreconditionSchemaHash,
                .effectPayloadSchemaHashes  = {},
                .tools = {
                    DeclaredTool{
                        .name           = std::string{k_toolName},
                        .argumentSchema = "DismissArguments",
                        .descriptor     = operator_runtime::ToolDescriptor{
                            .toolVersion          = "1",
                            .requiredCapabilities = {},
                            .effectBounds         = {},
                            .uiActionBounds       = {std::string{k_uiAction}},
                            .limits = operator_runtime::WorkflowLimits{
                                .maximumSteps        = 2,
                                .maximumDispatches   = 1,
                                .maximumObservations = 2,
                                .maximumWaits        = 1,
                                .maximumElapsedMillis = 3'000,
                            },
                            .timeout = operator_runtime::TimeoutPolicy{
                                .maximumElapsedMillis = 3'000,
                                .onTimeout = operator_runtime::TimeoutAction::Reconcile,
                            },
                            .mutability = operator_runtime::ToolMutability::Mutating,
                            .surface    = operator_runtime::ToolSurface::Semantic,
                            .idempotency =
                                operator_runtime::ToolIdempotency::DeliverySafe,
                        },
                    },
                },
            });
        }

        [[nodiscard]]
        auto journalManifestBytes(ContentHash payloadSchemaHash) -> Result<std::string>
        {
            return canonicalOf(std::format(
                R"json({{"payload_schemas":[{{"namespaced_event_type":"{}",)json"
                R"json("sha256":"{}"}}],"plugin_id":"{}",)json"
                R"json("schema":"umbraflow-journal-event-schema-manifest/v1"}})json",
                k_baselineEventType,
                payloadSchemaHash.hex(),
                k_pluginId
            ));
        }

        [[nodiscard]]
        auto reconcileManifestBytes(ContentHash reconcileSchemaHash)
            -> Result<std::string>
        {
            return canonicalOf(std::format(
                R"json({{"dispositions":[)json"
                R"json({{"disposition":"ambiguous","value":"ambiguous"}},)json"
                R"json({{"disposition":"confirmed","value":"confirmed"}},)json"
                R"json({{"disposition":"continue","value":"continue"}},)json"
                R"json({{"disposition":"diverged","value":"diverged"}},)json"
                R"json({{"disposition":"rejected","value":"rejected"}}],)json"
                R"json("plugin_id":"{}","reconcile_schema_sha256":"{}",)json"
                R"json("request_definition":"ReconcileRequest",)json"
                R"json("schema":"umbraflow-reconcile-manifest/v1",)json"
                R"json("verdict_definition":"ReconcileVerdict",)json"
                R"json("verdict_member":"disposition"}})json",
                k_pluginId,
                reconcileSchemaHash.hex()
            ));
        }

        [[nodiscard]]
        auto registrationJcs(
            operator_runtime::ProjectRegistrationClaims const& claims
        ) -> std::string
        {
            auto identityHashes = std::vector<json::Value>{};
            identityHashes.reserve(claims.observedInstanceIdentitySchemaHashes.size());
            for (auto const& hash : claims.observedInstanceIdentitySchemaHashes)
            {
                identityHashes.emplace_back(json::Value::ofString(hash.hex()));
            }
            return json::canonicalBytes(json::Value::ofObject({
                {
                    "baseline_event_type",
                    json::Value::ofString(claims.baselineEventType),
                },
                {
                    "journal_event_schema_manifest_hash",
                    json::Value::ofString(
                        claims.journalEventSchemaManifestHash.hex()
                    ),
                },
                {
                    "observed_instance_identity_schema_hashes",
                    json::Value::ofArray(std::move(identityHashes)),
                },
                {"plugin_hash", json::Value::ofString(claims.pluginHash.hex())},
                {"plugin_id", json::Value::ofString(claims.pluginId)},
                {"project_artifact_roots", json::Value::ofArray({})},
                {
                    "project_observation_schema_hash",
                    json::Value::ofString(claims.projectObservationSchemaHash.hex()),
                },
                {
                    "project_registration_format",
                    json::Value::ofNumber(
                        static_cast<double>(claims.projectRegistrationFormat)
                    ),
                },
                {
                    "project_state_schema_hash",
                    json::Value::ofString(claims.projectStateSchemaHash.hex()),
                },
                {
                    "project_tool_precondition_schema_hash",
                    json::Value::ofString(
                        claims.projectToolPreconditionSchemaHash.hex()
                    ),
                },
                {
                    "reconcile_payload_schema_manifest_hash",
                    json::Value::ofString(
                        claims.reconcilePayloadSchemaManifestHash.hex()
                    ),
                },
                {
                    "tool_catalog_hash",
                    json::Value::ofString(claims.toolCatalogHash.hex()),
                },
            }));
        }

        // Everything registerPlugin is handed apart from the plugin bytes: the
        // verified registration that pins them, and the schema owner bound to
        // that registration and carrying this project's real bidirectional
        // document validation.
        struct AdmissionInputs final
        {
            operator_runtime::VerifiedProjectRegistration registration;
            operator_runtime::ProjectSchemaOwner          schemaOwner;
        };

        [[nodiscard]]
        auto admissionInputsFor(std::string_view pinnedPluginBytes)
            -> Result<AdmissionInputs>
        {
            UF_TRY_VALUE(pluginHash, hashOf(pinnedPluginBytes));
            UF_TRY_VALUE(stateSchemaHash, hashOf(k_projectStateSchema));
            UF_TRY_VALUE(observationSchemaHash, hashOf(k_projectObservationSchema));
            UF_TRY_VALUE(preconditionSchemaHash, hashOf(k_toolPreconditionSchema));
            UF_TRY_VALUE(reconcileSchemaHash, hashOf(k_reconcileSchema));
            UF_TRY_VALUE(journalPayloadHash, hashOf(k_journalPayloadSchema));

            UF_TRY_VALUE(toolCatalog, toolCatalogBytes(preconditionSchemaHash));
            UF_TRY_VALUE(toolCatalogHash, hashOf(toolCatalog));
            UF_TRY_VALUE(journalManifest, journalManifestBytes(journalPayloadHash));
            UF_TRY_VALUE(journalManifestHash, hashOf(journalManifest));
            UF_TRY_VALUE(reconcileManifest, reconcileManifestBytes(reconcileSchemaHash));
            UF_TRY_VALUE(reconcileManifestHash, hashOf(reconcileManifest));

            auto const journalPayloadSchemas = std::array{k_journalPayloadSchema};
            UF_TRY_VALUE(
                projectDeployment,
                deployment::ProjectDeployment::create(
                    deployment::ProjectDeploymentSources{
                        .pluginId                        = k_pluginId,
                        .projectState                    = k_projectStateSchema,
                        .projectObservation              = k_projectObservationSchema,
                        .toolPrecondition                = k_toolPreconditionSchema,
                        .reconcile                       = k_reconcileSchema,
                        .toolCatalog                     = toolCatalog,
                        .journalEventManifest            = journalManifest,
                        .reconcileManifest               = reconcileManifest,
                        .journalPayloadSchemas           = journalPayloadSchemas,
                        .effectPayloadSchemas            = {},
                        .observedInstanceIdentitySchemas = {},
                    }
                )
            );

            auto claims = operator_runtime::ProjectRegistrationClaims{
                .projectRegistrationFormat             =
                    operator_runtime::k_projectRegistrationFormat,
                .pluginId                           = std::string{k_pluginId},
                .pluginHash                         = pluginHash,
                .toolCatalogHash                    = toolCatalogHash,
                .projectStateSchemaHash             = stateSchemaHash,
                .projectObservationSchemaHash       = observationSchemaHash,
                .projectToolPreconditionSchemaHash  = preconditionSchemaHash,
                .reconcilePayloadSchemaManifestHash = reconcileManifestHash,
                .journalEventSchemaManifestHash     = journalManifestHash,
                .baselineEventType                  = std::string{k_baselineEventType},
                .projectArtifactRoots               = {},
            };
            auto exactJcs = registrationJcs(claims);
            UF_TRY_VALUE(rootHash, hashOf(exactJcs));
            UF_TRY_VALUE(
                registrationOwner,
                operator_runtime::ProjectRegistrationSchemaOwner::create(
                    [exactJcs, claims](std::string_view candidate)
                        -> Result<operator_runtime::ProjectRegistrationClaims>
                    {
                        if (candidate != exactJcs)
                        {
                            return refuse(
                                "the registration owner was handed bytes it "
                                "did not mint"
                            );
                        }
                        return claims;
                    }
                )
            );
            UF_TRY_VALUE(
                registration,
                operator_runtime::ProjectRegistration::verifyExact(
                    exactJcs,
                    rootHash,
                    registrationOwner
                )
            );
            UF_TRY_VALUE(
                schemaOwner,
                operator_runtime::ProjectSchemaOwner::create(
                    registration,
                    operator_runtime::ProjectDocumentSchemaBytes{
                        .projectState       = k_projectStateSchema,
                        .projectObservation = k_projectObservationSchema,
                        .toolPrecondition   = k_toolPreconditionSchema,
                    },
                    deployment::canonicalJsonValidator(),
                    projectDeployment.documentValidator()
                )
            );
            return AdmissionInputs{
                .registration = std::move(registration),
                .schemaOwner  = std::move(schemaOwner),
            };
        }

        // Admits one plugin exactly as a hand-written plugin is admitted. The
        // registrar is local because the handle owns its own compiled state;
        // nothing here borrows from the registry.
        [[nodiscard]]
        auto admitPlugin(std::string_view exactPluginBytes)
            -> Result<operator_runtime::ProjectPluginHandle>
        {
            UF_TRY_VALUE(inputs, admissionInputsFor(exactPluginBytes));
            auto registrar = operator_runtime::ProjectPluginRegistrar{};
            return registrar.registerPlugin(
                inputs.registration,
                std::string{exactPluginBytes},
                {},
                inputs.schemaOwner
            );
        }

        // The final envelope the Operator mints, which is what the plan and
        // step inputs pin their project_observation to and what the twins read
        // for their surface evidence and instance locating.
        [[nodiscard]]
        auto observationDocument(
            bool includesTarget,
            bool includesSurface,
            bool surfaceIsFresh
        ) -> std::string
        {
            auto text = std::string{
                R"json({"canonical_opaque_payload":{"surface_observations":[)json"
            };
            if (includesSurface)
            {
                text += R"json({"fresh":)json";
                text += surfaceIsFresh ? "true" : "false";
                text += R"json(,"resolution":"resolved","surface_id":")json";
                text += k_surfaceId;
                text += R"json(","unambiguous":true})json";
            }
            text += R"json(]},"observed_instances":[)json";
            if (includesTarget)
            {
                text += R"json({"kind":")json";
                text += k_instanceKind;
                text += R"json(","observed_instance_id":")json";
                text += k_targetInstance;
                text += R"json(","opaque_project_payload":{}})json";
            }
            text += R"json(],"project_tool_preconditions":[],)json";
            text += R"json("schema":"umbraflow-project-observation/v1"})json";
            return text;
        }

        [[nodiscard]]
        auto canonicalArgs() -> std::string
        {
            return std::string{R"json({"observed_instance_id":")json"}
                + std::string{k_targetInstance}
                + R"json("})json";
        }

        [[nodiscard]]
        auto deriveInput(std::string_view observation) -> Result<std::string>
        {
            return canonicalOf(
                std::string{R"json({"pending_operation_transition":null,)json"}
                + R"json("pinned_project_artifact_identities":[],)json"
                + R"json("prior_project_observation":)json"
                + std::string{observation}
                + R"json(,"project_state":{},)json"
                + R"json("ui_snapshot":{"kind":"resolved_state"}})json"
            );
        }

        [[nodiscard]]
        auto planInput(std::string_view observation) -> Result<std::string>
        {
            return canonicalOf(
                std::string{R"json({"canonical_args":)json"}
                + canonicalArgs()
                + R"json(,"project_observation":)json"
                + std::string{observation}
                + R"json(,"project_state":{},"tool_name":")json"
                + std::string{k_toolName}
                + R"json(","tool_version":"1"})json"
            );
        }

        [[nodiscard]]
        auto stepInput(std::string_view observation, uint32 stepIndex)
            -> Result<std::string>
        {
            return canonicalOf(
                std::string{R"json({"frozen_plan_hash":")json"}
                + std::string{k_frozenPlanHash}
                + R"json(","project_observation":)json"
                + std::string{observation}
                + R"json(,"project_state":{},"step_index":)json"
                + std::to_string(stepIndex)
                + "}"
            );
        }

        [[nodiscard]]
        auto reconcileInput(std::string_view observation) -> Result<std::string>
        {
            return canonicalOf(
                std::string{R"json({"canonical_args":)json"}
                + canonicalArgs()
                + R"json(,"project_observation":)json"
                + std::string{observation}
                + "}"
            );
        }

        [[nodiscard]]
        auto reduceInput() -> Result<std::string>
        {
            return canonicalOf(
                std::string{R"json({"journal_events":[{"namespaced_event_type":")json"}
                + std::string{k_baselineEventType}
                + R"json(","opaque_project_payload":{"overlay":"main"},)json"
                + R"json("provenance":{"kind":"observation","observation_ids":[],)json"
                + R"json("principal_id":null,"source_hashes":[]}}],)json"
                + R"json("prior_project_state":null})json"
            );
        }

        // One of the five functions the handle publishes. A member pointer
        // rather than a name, because the handle has no invoke-by-name and
        // nothing here has to dispatch over one.
        using PluginCall = Result<operator_runtime::ValidatedDocument> (
            operator_runtime::ProjectPluginHandle::*
        )(operator_runtime::CanonicalJson const&) const;

        constexpr auto k_derive    = &operator_runtime::ProjectPluginHandle::derive;
        constexpr auto k_plan      = &operator_runtime::ProjectPluginHandle::plan;
        constexpr auto k_nextStep  = &operator_runtime::ProjectPluginHandle::nextStep;
        constexpr auto k_reconcile = &operator_runtime::ProjectPluginHandle::reconcile;
        constexpr auto k_reduce    = &operator_runtime::ProjectPluginHandle::reduce;

        // One SPI call both twins make: the label a disagreement is reported
        // under, the function to run, and the exact bytes handed to each.
        struct CallVector final
        {
            std::string label{};
            PluginCall  call{k_derive};
            std::string exactJcs{};
        };

        // What one plugin answered for one vector: the canonical output bytes
        // when the boundary accepted the call, or the refusal code the tool
        // raised when it did not.
        struct Answer final
        {
            bool        accepted{false};
            std::string bytes{};
            std::string code{};

            auto operator==(Answer const&) const -> bool = default;
        };

        [[nodiscard]]
        auto refusalCode(std::string_view message) -> std::string
        {
            for (auto const code : k_refusalCodes)
            {
                if (message.find(code) != std::string_view::npos)
                {
                    return std::string{code};
                }
            }
            return std::string{};
        }

        [[nodiscard]]
        auto answerOf(
            operator_runtime::ProjectPluginHandle const& plugin,
            CallVector const& vector
        ) -> Result<Answer>
        {
            UF_TRY_VALUE(input, plugin.canonicalize(vector.exactJcs));
            auto const output = (plugin.*vector.call)(input);
            if (!output.has_value())
            {
                return Answer{
                    .accepted = false,
                    .bytes    = {},
                    .code     = refusalCode(output.error().message()),
                };
            }
            return Answer{
                .accepted = true,
                .bytes    = output->bytes(),
                .code     = {},
            };
        }

        // The observation states both twins are driven over: a fresh reading
        // that holds the target, a fresh reading that does not, a reading whose
        // required Surface is absent, and a reading whose Surface is expired.
        struct ObservationVector final
        {
            std::string label{};
            std::string bytes{};
        };

        [[nodiscard]]
        auto observationVectors() -> std::array<ObservationVector, 4>
        {
            return std::array{
                ObservationVector{
                    .label = "present",
                    .bytes = observationDocument(true, true, true),
                },
                ObservationVector{
                    .label = "target-absent",
                    .bytes = observationDocument(false, true, true),
                },
                ObservationVector{
                    .label = "surface-missing",
                    .bytes = observationDocument(true, false, true),
                },
                ObservationVector{
                    .label = "expired",
                    .bytes = observationDocument(true, true, false),
                },
            };
        }

        // Every call the parity comparison makes: the five SPI functions over
        // every observation state, and next_step over every step index of the
        // one-step schedule up to and including the first one out of bounds.
        [[nodiscard]]
        auto parityCalls() -> Result<std::vector<CallVector>>
        {
            auto calls = std::vector<CallVector>{};
            for (auto const& observation : observationVectors())
            {
                UF_TRY_VALUE(derive, deriveInput(observation.bytes));
                calls.emplace_back(CallVector{
                    .label    = observation.label + "/derive",
                    .call     = k_derive,
                    .exactJcs = std::move(derive),
                });

                UF_TRY_VALUE(plan, planInput(observation.bytes));
                calls.emplace_back(CallVector{
                    .label    = observation.label + "/plan",
                    .call     = k_plan,
                    .exactJcs = std::move(plan),
                });

                for (auto stepIndex = uint32{1}; stepIndex <= 2U; ++stepIndex)
                {
                    UF_TRY_VALUE(step, stepInput(observation.bytes, stepIndex));
                    calls.emplace_back(CallVector{
                        .label = observation.label
                            + "/next_step@"
                            + std::to_string(stepIndex),
                        .call     = k_nextStep,
                        .exactJcs = std::move(step),
                    });
                }

                UF_TRY_VALUE(reconcile, reconcileInput(observation.bytes));
                calls.emplace_back(CallVector{
                    .label    = observation.label + "/reconcile",
                    .call     = k_reconcile,
                    .exactJcs = std::move(reconcile),
                });
            }

            UF_TRY_VALUE(reduce, reduceInput());
            calls.emplace_back(CallVector{
                .label    = "reduce",
                .call     = k_reduce,
                .exactJcs = std::move(reduce),
            });
            return calls;
        }

        // Every way the two implementations failed to answer alike, one line
        // each. It asserts nothing: the caller decides whether an empty result
        // is the expected outcome, which is what lets one comparator serve both
        // the parity case and its positive control.
        //
        // Each vector is run twice against each implementation, in two fresh
        // VMs, so replay determinism is measured here rather than assumed.
        [[nodiscard]]
        auto parityDiff(
            operator_runtime::ProjectPluginHandle const& generated,
            operator_runtime::ProjectPluginHandle const& handWritten,
            std::span<CallVector const> calls
        ) -> Result<std::vector<std::string>>
        {
            auto disagreements = std::vector<std::string>{};
            for (auto const& vector : calls)
            {
                UF_TRY_VALUE(generatedFirst, answerOf(generated, vector));
                UF_TRY_VALUE(generatedSecond, answerOf(generated, vector));
                UF_TRY_VALUE(handWrittenFirst, answerOf(handWritten, vector));
                UF_TRY_VALUE(handWrittenSecond, answerOf(handWritten, vector));

                if (generatedFirst != generatedSecond)
                {
                    disagreements.emplace_back(std::format(
                        "{}: the generated adapter answered two fresh VMs "
                        "differently",
                        vector.label
                    ));
                }
                if (handWrittenFirst != handWrittenSecond)
                {
                    disagreements.emplace_back(std::format(
                        "{}: the hand-written plugin answered two fresh VMs "
                        "differently",
                        vector.label
                    ));
                }

                if (generatedFirst.accepted != handWrittenFirst.accepted)
                {
                    disagreements.emplace_back(std::format(
                        "{}: generated {}, hand-written {}",
                        vector.label,
                        generatedFirst.accepted ? "accepted" : "refused",
                        handWrittenFirst.accepted ? "accepted" : "refused"
                    ));
                    continue;
                }
                if (generatedFirst.accepted)
                {
                    if (generatedFirst.bytes != handWrittenFirst.bytes)
                    {
                        disagreements.emplace_back(std::format(
                            "{}: canonical bytes differ -- generated {}, "
                            "hand-written {}",
                            vector.label,
                            generatedFirst.bytes,
                            handWrittenFirst.bytes
                        ));
                    }
                    continue;
                }
                if (generatedFirst.code != handWrittenFirst.code)
                {
                    disagreements.emplace_back(std::format(
                        "{}: refusal codes differ -- generated \"{}\", "
                        "hand-written \"{}\"",
                        vector.label,
                        generatedFirst.code,
                        handWrittenFirst.code
                    ));
                    continue;
                }
                if (generatedFirst.code.empty())
                {
                    disagreements.emplace_back(std::format(
                        "{}: both refusals named no known code",
                        vector.label
                    ));
                }
            }
            return disagreements;
        }

        [[nodiscard]]
        auto reported(std::span<std::string const> disagreements) -> std::string
        {
            auto text = std::string{};
            for (auto const& line : disagreements)
            {
                if (!text.empty())
                {
                    text += "; ";
                }
                text += line;
            }
            return text;
        }

        template <typename Value>
        [[nodiscard]]
        auto messageOf(Result<Value> const& result) -> std::string
        {
            if (result.has_value())
            {
                return std::string{};
            }
            return std::string{result.error().message()};
        }

        [[nodiscard]]
        auto generatedAdapter(std::string_view declaration) -> std::string
        {
            auto generated = generateDeclarativeWorkflowAdapter(
                k_pluginId,
                declaration
            );
            REQUIRE_MESSAGE(generated.has_value(), messageOf(generated));
            return *std::move(generated);
        }

        [[nodiscard]]
        auto requiredValue(Result<std::string> result) -> std::string
        {
            REQUIRE_MESSAGE(result.has_value(), messageOf(result));
            return *std::move(result);
        }
    } // namespace

    // (a) The generated adapter goes through the admission boundary the
    // hand-written tier is guarded on, with nothing relaxed for it.
    TEST_CASE("a generated adapter is admitted through the hand-written plugin boundary")
    {
        auto const adapter = generatedAdapter(k_boundedDeclaration);

        SUBCASE("the boundary admits it and runs all five functions")
        {
            auto const plugin = admitPlugin(adapter);
            auto const admissionRefusal = std::string{
                "a generated adapter must be admissible through "
                "ProjectPluginRegistrar::registerPlugin: "
            } + messageOf(plugin);
            REQUIRE_MESSAGE(plugin.has_value(), admissionRefusal);
            CHECK(plugin->pluginId() == k_pluginId);

            auto const observation = observationDocument(true, true, true);
            auto const derive      = requiredValue(deriveInput(observation));
            auto const plan        = requiredValue(planInput(observation));
            auto const wait        = requiredValue(stepInput(observation, 1U));
            auto const dispatch    = requiredValue(stepInput(observation, 2U));
            auto const reconcile   = requiredValue(reconcileInput(observation));
            auto const reduce      = requiredValue(reduceInput());

            auto const calls = std::array{
                CallVector{
                    .label    = "derive",
                    .call     = k_derive,
                    .exactJcs = derive,
                },
                CallVector{.label = "plan", .call = k_plan, .exactJcs = plan},
                CallVector{
                    .label    = "next_step@1",
                    .call     = k_nextStep,
                    .exactJcs = wait,
                },
                CallVector{
                    .label    = "next_step@2",
                    .call     = k_nextStep,
                    .exactJcs = dispatch,
                },
                CallVector{
                    .label    = "reconcile",
                    .call     = k_reconcile,
                    .exactJcs = reconcile,
                },
                CallVector{
                    .label    = "reduce",
                    .call     = k_reduce,
                    .exactJcs = reduce,
                },
            };
            for (auto const& vector : calls)
            {
                CAPTURE(vector.label);
                auto const canonical = plugin->canonicalize(vector.exactJcs);
                REQUIRE_MESSAGE(canonical.has_value(), messageOf(canonical));
                auto const output = ((*plugin).*vector.call)(*canonical);
                auto const outputRefusal = std::string{
                    "every generated SPI function must pass the pinned schemas "
                    "at the plugin boundary: "
                } + messageOf(output);
                REQUIRE_MESSAGE(output.has_value(), outputRefusal);
                CHECK(
                    output->direction()
                    == operator_runtime::ProjectDocumentDirection::Output
                );
            }
        }

        SUBCASE("a generated output violating a pinned schema is refused")
        {
            // The observation schema this registration pins closes
            // canonical_opaque_payload. The adapter is otherwise unchanged and
            // is registered against its own bytes, so nothing but the derived
            // document can be what the boundary refuses.
            auto const forged = replacedOnce(
                adapter,
                "canonical_opaque_payload = { surface_observations = {} }",
                "canonical_opaque_payload = { surface_observations = {}, forged = true }"
            );
            REQUIRE_MESSAGE(forged.has_value(), messageOf(forged));

            auto const plugin = admitPlugin(*forged);
            REQUIRE_MESSAGE(plugin.has_value(), messageOf(plugin));

            auto const observation = observationDocument(true, true, true);
            auto const derive      = requiredValue(deriveInput(observation));
            auto const canonical   = plugin->canonicalize(derive);
            REQUIRE_MESSAGE(canonical.has_value(), messageOf(canonical));

            auto const output = plugin->derive(*canonical);
            REQUIRE_FALSE_MESSAGE(
                output.has_value(),
                "a generated adapter whose output violates a pinned schema must "
                "be refused at the boundary, not admitted by its tier"
            );
            CHECK_MESSAGE(
                std::string{output.error().message()}.find("forged")
                    != std::string::npos,
                "the refusal must name the member the pinned schema rejected"
            );
        }

        SUBCASE("the boundary pins the exact generated bytes")
        {
            auto const inputs = admissionInputsFor(adapter);
            REQUIRE_MESSAGE(inputs.has_value(), messageOf(inputs));

            auto registrar = operator_runtime::ProjectPluginRegistrar{};
            auto const substituted = registrar.registerPlugin(
                inputs->registration,
                std::string{k_handWrittenTwin},
                {},
                inputs->schemaOwner
            );
            CHECK_MESSAGE(
                !substituted.has_value(),
                "a registration built for the generated adapter must refuse "
                "other plugin bytes"
            );
        }
    }

    // (b) One logical tool, implemented twice, observed only through the SPI.
    TEST_CASE("the two authoring paths agree function by function over one tool")
    {
        auto const adapter = generatedAdapter(k_oneStepDeclaration);
        auto const generated = admitPlugin(adapter);
        REQUIRE_MESSAGE(generated.has_value(), messageOf(generated));
        auto const handWritten = admitPlugin(k_handWrittenTwin);
        REQUIRE_MESSAGE(handWritten.has_value(), messageOf(handWritten));

        auto const calls = parityCalls();
        REQUIRE_MESSAGE(calls.has_value(), messageOf(calls));

        auto const disagreements = parityDiff(*generated, *handWritten, *calls);
        REQUIRE_MESSAGE(disagreements.has_value(), messageOf(disagreements));
        auto const parityFailure = std::string{
            "the two authoring paths must produce byte-identical canonical "
            "output where they succeed and identical refusal codes where they "
            "refuse: "
        } + reported(*disagreements);
        CHECK_MESSAGE(disagreements->empty(), parityFailure);
    }

    // (c) The positive control for (b). Without it, an empty diff would be
    // equally consistent with a comparator that measures nothing.
    TEST_CASE("a deviating hand-written twin makes the parity comparison red")
    {
        auto const deviating = replacedOnce(
            k_handWrittenTwin,
            R"(local FINDING_KIND = "observed_instance_absent")",
            R"(local FINDING_KIND = "observed_instance_present")"
        );
        REQUIRE_MESSAGE(deviating.has_value(), messageOf(deviating));

        auto const adapter = generatedAdapter(k_oneStepDeclaration);
        auto const generated = admitPlugin(adapter);
        REQUIRE_MESSAGE(generated.has_value(), messageOf(generated));
        auto const handWritten = admitPlugin(*deviating);
        REQUIRE_MESSAGE(handWritten.has_value(), messageOf(handWritten));

        auto const calls = parityCalls();
        REQUIRE_MESSAGE(calls.has_value(), messageOf(calls));

        auto const disagreements = parityDiff(*generated, *handWritten, *calls);
        REQUIRE_MESSAGE(disagreements.has_value(), messageOf(disagreements));
        REQUIRE_MESSAGE(
            !disagreements->empty(),
            "one flipped finding kind must make the parity comparator produce "
            "a non-empty diff"
        );
        auto const namesReconcile = std::ranges::any_of(
            *disagreements,
            [](std::string const& line)
            {
                return line.find("/reconcile") != std::string::npos;
            }
        );
        auto const controlFailure = std::string{
            "the deviation is in the reconcile finding, so the diff must name "
            "reconcile: "
        } + reported(*disagreements);
        CHECK_MESSAGE(namesReconcile, controlFailure);
    }
} // namespace uf::project
