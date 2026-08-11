#include "project-deployment.hpp"

#include <json/error.hpp>
#include <json/schema.hpp>
#include <json/value.hpp>

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::deployment
{
    namespace
    {
        // Definitions the Operator owns. A project restates none of them: a
        // provenance, a hash or a state resolution is the framework's shape
        // wherever it appears, and the envelope schemas below reference this
        // document rather than repeating it once per function.
        //
        // Every definition here is copied from the repository's own published
        // schemas -- schema/umbraflow-operator-v1.schema.json and
        // schema/umbraflow-journal-v1.schema.json -- so a document this module
        // accepts is a document those accept.
        constexpr auto k_commonSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/operator/common",
    "$defs": {
        "Hash": {
            "type": "string",
            "pattern": "^[0-9a-f]{64}$"
        },
        "Identifier": {
            "type": "string",
            "pattern": "^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$"
        },
        "NamespacedIdentifier": {
            "type": "string",
            "pattern": "^[A-Za-z][A-Za-z0-9_-]*(?:\\.[A-Za-z0-9][A-Za-z0-9_-]*)+$"
        },
        "JournalProvenance": {
            "type": "object",
            "additionalProperties": false,
            "required": ["kind", "observation_ids", "principal_id", "source_hashes"],
            "properties": {
                "kind": {
                    "enum": [
                        "client_db",
                        "observation",
                        "inference",
                        "policy",
                        "human_correction"
                    ]
                },
                "observation_ids": {
                    "type": "array",
                    "items": {"$ref": "#/$defs/Identifier"},
                    "uniqueItems": true
                },
                "principal_id": {
                    "oneOf": [{"type": "null"}, {"$ref": "#/$defs/Identifier"}]
                },
                "source_hashes": {
                    "type": "array",
                    "items": {"$ref": "#/$defs/Hash"},
                    "uniqueItems": true
                }
            }
        },
        "PendingOperationTransition": {
            "$comment": "The one non-terminal Operation an instance may have. The state enumerators are exactly the seven the Snapshot Coordinator's own query selects.",
            "type": "object",
            "additionalProperties": false,
            "required": ["operation_id", "revision", "state"],
            "properties": {
                "operation_id": {"$ref": "#/$defs/Identifier"},
                "revision": {"type": "integer", "minimum": 0},
                "state": {
                    "enum": [
                        "proposed",
                        "awaiting_approval",
                        "ready",
                        "needs_revalidation",
                        "running",
                        "reconciling",
                        "ambiguous"
                    ]
                }
            }
        },
        "StateResolution": {
            "$comment": "What the trusted Luau resolver serializes: the kind, the ordered surface stack a resolved state carries, and the reason the other kinds failed. Absent members are absent rather than null, so only kind is required.",
            "type": "object",
            "additionalProperties": false,
            "required": ["kind"],
            "properties": {
                "kind": {
                    "enum": ["resolved_state", "ambiguous_state", "unknown_state"]
                },
                "ordered_surface_stack": {
                    "type": "array",
                    "items": {"$ref": "#/$defs/Identifier"}
                },
                "reason": {"type": "string", "minLength": 1}
            }
        }
    }
})json"};

        // The exact bytes ProjectPlugin.derive is called with. The Operator
        // assembles this envelope from what the world currently holds, so its
        // shape is the Operator's; the two members that are the project's are
        // referenced rather than described.
        constexpr auto k_deriveInputSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/operator/derive-input",
    "title": "ProjectPlugin.derive input",
    "type": "object",
    "additionalProperties": false,
    "required": [
        "pending_operation_transition",
        "pinned_project_artifact_identities",
        "prior_project_observation",
        "project_state",
        "ui_snapshot"
    ],
    "properties": {
        "pending_operation_transition": {
            "oneOf": [
                {"type": "null"},
                {"$ref": "https://umbraflow.dev/schema/operator/common#/$defs/PendingOperationTransition"}
            ]
        },
        "pinned_project_artifact_identities": {
            "type": "array",
            "items": {"$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Hash"}
        },
        "prior_project_observation": {
            "$comment": "null before this instance has ever been derived.",
            "oneOf": [
                {"type": "null"},
                {"$ref": "https://umbraflow.dev/schema/project/observation"}
            ]
        },
        "project_state": {"$ref": "https://umbraflow.dev/schema/project/state"},
        "ui_snapshot": {
            "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/StateResolution"
        }
    }
})json"};

        constexpr auto k_planInputSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/operator/plan-input",
    "title": "ProjectPlugin.plan input",
    "type": "object",
    "additionalProperties": false,
    "required": [
        "canonical_args",
        "project_observation",
        "project_state",
        "tool_name",
        "tool_version"
    ],
    "properties": {
        "canonical_args": {
            "$comment": "Judged against the argument definition this project's Tool Catalog names for tool_name, which no single subschema can select."
        },
        "project_observation": {
            "$ref": "https://umbraflow.dev/schema/project/observation"
        },
        "project_state": {"$ref": "https://umbraflow.dev/schema/project/state"},
        "tool_name": {
            "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
        },
        "tool_version": {
            "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
        }
    }
})json"};

        constexpr auto k_stepInputSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/operator/step-input",
    "title": "ProjectPlugin.next_step input",
    "type": "object",
    "additionalProperties": false,
    "required": [
        "frozen_plan_hash",
        "project_observation",
        "project_state",
        "step_index"
    ],
    "properties": {
        "frozen_plan_hash": {
            "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Hash"
        },
        "project_observation": {
            "$ref": "https://umbraflow.dev/schema/project/observation"
        },
        "project_state": {"$ref": "https://umbraflow.dev/schema/project/state"},
        "step_index": {
            "$comment": "Dense and monotone from one: the Operator mints at MAX(step_index) + 1.",
            "type": "integer",
            "minimum": 1
        }
    }
})json"};

        constexpr auto k_reduceInputSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/operator/reduce-input",
    "title": "ProjectPlugin.reduce input",
    "type": "object",
    "additionalProperties": false,
    "required": ["journal_events", "prior_project_state"],
    "properties": {
        "journal_events": {
            "type": "array",
            "minItems": 1,
            "items": {"$ref": "#/$defs/JournalEvent"}
        },
        "prior_project_state": {
            "$comment": "null for a baseline, which is a value here rather than an absent member.",
            "oneOf": [
                {"type": "null"},
                {"$ref": "https://umbraflow.dev/schema/project/state"}
            ]
        }
    },
    "$defs": {
        "JournalEvent": {
            "type": "object",
            "additionalProperties": false,
            "required": [
                "namespaced_event_type",
                "opaque_project_payload",
                "provenance"
            ],
            "properties": {
                "namespaced_event_type": {
                    "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/NamespacedIdentifier"
                },
                "opaque_project_payload": {
                    "$comment": "Judged against the payload schema this project's journal event schema manifest names for namespaced_event_type."
                },
                "provenance": {
                    "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/JournalProvenance"
                }
            }
        }
    }
})json"};

        // OP:`PlanProposal`, from schema/umbraflow-operator-v1.schema.json with
        // one deliberate difference: tool_name is Identifier rather than
        // NamespacedIdentifier, and is required instead to name a tool this
        // project's own Tool Catalog declares -- which is the stronger of the
        // two statements. See the note in ProjectDeployment::create.
        constexpr auto k_planProposalSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/operator/plan-proposal",
    "title": "OP:PlanProposal",
    "type": "object",
    "additionalProperties": false,
    "required": [
        "allowed_ui_actions",
        "canonical_args",
        "effects",
        "tool_name",
        "tool_version",
        "workflow_limits"
    ],
    "properties": {
        "allowed_ui_actions": {
            "type": "array",
            "items": {
                "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/NamespacedIdentifier"
            },
            "uniqueItems": true
        },
        "canonical_args": {
            "$comment": "Judged against the argument definition this project's Tool Catalog names for tool_name."
        },
        "effects": {"type": "array", "items": {"$ref": "#/$defs/ExpectedEffect"}},
        "tool_name": {
            "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
        },
        "tool_version": {
            "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
        },
        "workflow_limits": {"$ref": "#/$defs/WorkflowLimits"}
    },
    "$defs": {
        "ExpectedEffect": {
            "type": "object",
            "additionalProperties": false,
            "required": [
                "namespaced_type",
                "opaque_project_payload",
                "payload_schema_hash",
                "risk",
                "scope_key",
                "scope_kind"
            ],
            "properties": {
                "namespaced_type": {
                    "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/NamespacedIdentifier"
                },
                "opaque_project_payload": {
                    "$comment": "Judged against the effect payload schema whose sha256 is payload_schema_hash."
                },
                "payload_schema_hash": {
                    "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Hash"
                },
                "risk": {
                    "enum": ["read_only", "low", "medium", "high", "critical"]
                },
                "scope_key": {
                    "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
                },
                "scope_kind": {
                    "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
                }
            }
        },
        "WorkflowLimits": {
            "type": "object",
            "additionalProperties": false,
            "required": [
                "maximum_dispatches",
                "maximum_elapsed_ms",
                "maximum_observations",
                "maximum_steps",
                "maximum_waits"
            ],
            "properties": {
                "maximum_dispatches": {"type": "integer", "minimum": 0},
                "maximum_elapsed_ms": {"type": "integer", "minimum": 1},
                "maximum_observations": {"type": "integer", "minimum": 1},
                "maximum_steps": {"type": "integer", "minimum": 0},
                "maximum_waits": {"type": "integer", "minimum": 0}
            }
        }
    }
})json"};

        // OP:`UIActionIntent` and OP:`WaitIntent`, told apart by their complete
        // member sets because the schema gives them no discriminator. oneOf
        // rather than anyOf for that reason: a document satisfying both would
        // be a step of two kinds.
        constexpr auto k_stepIntentSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/operator/step-intent",
    "title": "OP:UIActionIntent or OP:WaitIntent",
    "oneOf": [{"$ref": "#/$defs/UiActionIntent"}, {"$ref": "#/$defs/WaitIntent"}],
    "$defs": {
        "UiActionIntent": {
            "type": "object",
            "additionalProperties": false,
            "required": [
                "action",
                "binding_variant_constraints",
                "delivery_class",
                "expected_ui_postconditions",
                "required_ui_preconditions",
                "step_key",
                "timeout_policy"
            ],
            "properties": {
                "action": {
                    "type": "object",
                    "additionalProperties": false,
                    "required": [
                        "action_id",
                        "canonical_parameters",
                        "surface_id",
                        "ui_target_id"
                    ],
                    "properties": {
                        "action_id": {
                            "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
                        },
                        "canonical_parameters": {
                            "$comment": "Project-owned and judged by nothing here: no member of ProjectRegistrationClaims pins a schema for a UI action's parameters, so this module has no authority to invent one."
                        },
                        "surface_id": {
                            "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
                        },
                        "ui_target_id": {
                            "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
                        }
                    }
                },
                "binding_variant_constraints": {"type": "array"},
                "delivery_class": {
                    "enum": ["delivery_safe", "keyed_external", "non_idempotent"]
                },
                "expected_ui_postconditions": {"type": "array"},
                "required_ui_preconditions": {"type": "array"},
                "step_key": {
                    "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
                },
                "timeout_policy": {"$ref": "#/$defs/TimeoutPolicy"}
            }
        },
        "WaitIntent": {
            "type": "object",
            "additionalProperties": false,
            "required": [
                "condition",
                "observation_budget",
                "step_key",
                "timeout_policy"
            ],
            "properties": {
                "condition": {"$comment": "Project-owned, as canonical_parameters is."},
                "observation_budget": {"type": "integer", "minimum": 1},
                "step_key": {
                    "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
                },
                "timeout_policy": {"$ref": "#/$defs/TimeoutPolicy"}
            }
        },
        "TimeoutPolicy": {
            "type": "object",
            "additionalProperties": false,
            "required": ["maximum_elapsed_ms", "on_timeout"],
            "properties": {
                "maximum_elapsed_ms": {"type": "integer", "minimum": 1},
                "on_timeout": {"enum": ["reobserve", "reconcile", "stop"]}
            }
        }
    }
})json"};

        constexpr auto k_toolCatalogSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/operator/tool-catalog",
    "title": "Tool Catalog document",
    "type": "object",
    "additionalProperties": false,
    "required": ["plugin_id", "schema", "tool_precondition_sha256", "tools"],
    "properties": {
        "$comment": {"type": "string"},
        "plugin_id": {
            "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
        },
        "schema": {"const": "umbraflow-tool-catalog/v1"},
        "tool_precondition_sha256": {
            "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Hash"
        },
        "tools": {
            "type": "array",
            "minItems": 1,
            "items": {
                "type": "object",
                "additionalProperties": false,
                "required": [
                    "argument_schema",
                    "mutability",
                    "name",
                    "surface",
                    "version"
                ],
                "properties": {
                    "argument_schema": {
                        "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
                    },
                    "mutability": {"enum": ["read_only", "mutating"]},
                    "name": {
                        "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
                    },
                    "surface": {"enum": ["semantic", "privileged"]},
                    "version": {
                        "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
                    }
                }
            }
        }
    }
})json"};

        constexpr auto k_journalManifestSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/operator/journal-event-schema-manifest",
    "title": "Journal event schema manifest",
    "type": "object",
    "additionalProperties": false,
    "required": ["payload_schemas", "plugin_id", "schema"],
    "properties": {
        "$comment": {"type": "string"},
        "plugin_id": {
            "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
        },
        "schema": {"const": "umbraflow-journal-event-schema-manifest/v1"},
        "payload_schemas": {
            "type": "array",
            "minItems": 1,
            "items": {
                "type": "object",
                "additionalProperties": false,
                "required": ["namespaced_event_type", "sha256"],
                "properties": {
                    "namespaced_event_type": {
                        "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/NamespacedIdentifier"
                    },
                    "sha256": {
                        "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Hash"
                    }
                }
            }
        }
    }
})json"};

        constexpr auto k_reconcileManifestSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/operator/reconcile-manifest",
    "title": "Reconcile payload schema manifest",
    "$comment": "The disposition is read out of one named member of a verdict this project's own schema accepted, through the mapping declared here. It is never read out of the request that produced the verdict.",
    "type": "object",
    "additionalProperties": false,
    "required": [
        "dispositions",
        "plugin_id",
        "reconcile_schema_sha256",
        "request_definition",
        "schema",
        "verdict_definition",
        "verdict_member"
    ],
    "properties": {
        "$comment": {"type": "string"},
        "plugin_id": {
            "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
        },
        "schema": {"const": "umbraflow-reconcile-manifest/v1"},
        "reconcile_schema_sha256": {
            "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Hash"
        },
        "request_definition": {
            "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
        },
        "verdict_definition": {
            "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
        },
        "verdict_member": {
            "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
        },
        "dispositions": {
            "type": "array",
            "minItems": 1,
            "items": {
                "type": "object",
                "additionalProperties": false,
                "required": ["disposition", "value"],
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
                    "value": {"type": "string", "minLength": 1}
                }
            }
        }
    }
})json"};

        // One document a deployment authors whose format is the framework's:
        // the value its `schema` member must carry, the label its refusals are
        // named by, and the exact bytes that judge it. The three are spelled
        // once here because two readers need them -- create() below, and
        // validateFrameworkFormat, which is what holds the specification's
        // worked examples to these bytes.
        struct FrameworkDocument final
        {
            std::string_view schemaName{};
            std::string_view label{};
            std::string_view exactBytes{};
        };

        constexpr auto k_toolCatalogDocument = FrameworkDocument{
            .schemaName = "umbraflow-tool-catalog/v1",
            .label      = "operator/tool-catalog",
            .exactBytes = k_toolCatalogSchema,
        };
        constexpr auto k_journalManifestDocument = FrameworkDocument{
            .schemaName = "umbraflow-journal-event-schema-manifest/v1",
            .label      = "operator/journal-event-schema-manifest",
            .exactBytes = k_journalManifestSchema,
        };
        constexpr auto k_reconcileManifestDocument = FrameworkDocument{
            .schemaName = "umbraflow-reconcile-manifest/v1",
            .label      = "operator/reconcile-manifest",
            .exactBytes = k_reconcileManifestSchema,
        };

        constexpr auto k_frameworkDocuments = std::array{
            k_journalManifestDocument,
            k_reconcileManifestDocument,
            k_toolCatalogDocument,
        };

        [[nodiscard]]
        auto refuse(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        // A json failure, restated in the Operator's vocabulary. The message is
        // carried whole because it names the document and the clause, which is
        // the entire diagnosis a red suite has.
        [[nodiscard]]
        auto adopt(Status outcome, std::string_view what) -> Status
        {
            if (outcome.has_value())
            {
                return ok();
            }
            return refuse(
                std::format("{}: {}", what, outcome.error().message())
            );
        }

        [[nodiscard]]
        auto compile(
            std::string_view label,
            std::string_view exactBytes,
            std::span<json::Schema::Document const> referenced
        ) -> Result<json::Schema>
        {
            auto compiled = json::Schema::compile(
                json::Schema::Document{.label = label, .exactBytes = exactBytes},
                referenced
            );
            if (!compiled.has_value())
            {
                return refuse(std::format(
                    "{} is not a schema this deployment can apply: {}",
                    label,
                    compiled.error().message()
                ));
            }
            return *std::move(compiled);
        }

        // The two operator protocol schemas that reference no project document.
        // Every other schema this module compiles is a project's or names one,
        // so it belongs to a deployment; these two are the Operator's own and
        // one compilation answers for every deployment.
        struct EnvelopeSchemas final
        {
            json::Schema planProposal;
            json::Schema stepIntent;
        };

        [[nodiscard]]
        auto envelopeSchemas() -> Result<EnvelopeSchemas>
        {
            static auto const s_compiled = []() -> Result<EnvelopeSchemas>
            {
                auto const commonOnly = std::array{json::Schema::Document{
                    .label      = "operator/common",
                    .exactBytes = k_commonSchema,
                }};
                UF_TRY_VALUE(
                    planProposal,
                    compile(
                        "operator/plan-proposal",
                        k_planProposalSchema,
                        commonOnly
                    )
                );
                UF_TRY_VALUE(
                    stepIntent,
                    compile("operator/step-intent", k_stepIntentSchema, commonOnly)
                );
                return EnvelopeSchemas{
                    .planProposal = std::move(planProposal),
                    .stepIntent   = std::move(stepIntent),
                };
            }();
            if (!s_compiled.has_value())
            {
                return std::unexpected{s_compiled.error().clone()};
            }
            return *s_compiled;
        }

        // The one thing a ValidatedDocument does not state about itself. Only a
        // ProjectSchemaOwner can mint one, so holding it proves the bytes are
        // exact RFC 8785 that owner's schema accepted -- but a Reduce output is
        // the same type, and reading one as a PlanProposal would reach a member
        // the operator protocol never put in it, where `member` below aborts on
        // a contract rather than refusing.
        //
        // The direction half cannot be turned red today: ProjectSchemaOwner's
        // validateOutput is the sole mint and stamps Output every time, so no
        // ValidatedDocument carries Input. It is named here rather than
        // dropped, so that a green mutation campaign is not read as coverage of
        // it, and because it is what would notice a second mint.
        [[nodiscard]]
        auto requireOutputOf(
            operator_runtime::ValidatedDocument const& document,
            operator_runtime::ProjectPluginFunction function,
            std::string_view what
        ) -> Status
        {
            using operator_runtime::ProjectDocumentDirection;

            if (
                document.function() != function
                || document.direction() != ProjectDocumentDirection::Output
            )
            {
                return refuse(std::format(
                    "{} is not what this ValidatedDocument was stamped as",
                    what
                ));
            }
            return ok();
        }

        [[nodiscard]]
        auto parseDocument(std::string_view exactJcs) -> Result<json::Value>
        {
            auto parsed = json::parse(exactJcs);
            if (!parsed.has_value())
            {
                return refuse(std::format(
                    "document is not JSON: {}",
                    parsed.error().message()
                ));
            }
            return *std::move(parsed);
        }

        [[nodiscard]]
        auto requireIdentity(
            std::string_view label,
            std::string_view exactBytes,
            std::string_view expected
        ) -> Status
        {
            UF_TRY_VALUE(document, parseDocument(exactBytes));
            auto const* const p_id = document.find("$id");
            if (p_id == nullptr || p_id->kind() != json::ValueKind::String
                || p_id->string() != expected)
            {
                return refuse(std::format(
                    "{} must declare \"$id\": \"{}\"",
                    label,
                    expected
                ));
            }
            return ok();
        }

        [[nodiscard]]
        auto hashOf(std::string_view bytes) -> Result<ContentHash>
        {
            return sha256(std::as_bytes(std::span{bytes}));
        }

        // Every observer below reads a document json::Schema has already
        // accepted, so the member is present and of the stated kind. The
        // contract check is what makes that assumption falsifiable rather than
        // undefined if a schema is ever loosened.
        [[nodiscard]]
        auto member(json::Value const& object UF_LIFETIME_BOUND, std::string_view name)
            -> json::Value const&
        {
            auto const* const p_member = object.find(name);
            UF_CHECK(p_member != nullptr);
            return *p_member;
        }

        [[nodiscard]]
        auto requirePluginId(
            json::Value const& manifest,
            std::string_view expected,
            std::string_view what
        ) -> Status
        {
            auto const* const p_declared = manifest.find("plugin_id");
            UF_CHECK(p_declared != nullptr);
            if (p_declared->string() != expected)
            {
                return refuse(std::format(
                    "{} belongs to plugin {}, not to {}",
                    what,
                    p_declared->string(),
                    expected
                ));
            }
            return ok();
        }

        struct MutabilityName final
        {
            std::string_view                 wire{};
            operator_runtime::ToolMutability mutability{};
        };

        constexpr auto k_mutabilities = std::array{
            MutabilityName{"mutating", operator_runtime::ToolMutability::Mutating},
            MutabilityName{"read_only", operator_runtime::ToolMutability::ReadOnly},
        };

        struct SurfaceName final
        {
            std::string_view              wire{};
            operator_runtime::ToolSurface surface{};
        };

        constexpr auto k_surfaces = std::array{
            SurfaceName{"privileged", operator_runtime::ToolSurface::Privileged},
            SurfaceName{"semantic", operator_runtime::ToolSurface::Semantic},
        };

        struct DispositionName final
        {
            std::string_view                       wire{};
            operator_runtime::ReconcileDisposition disposition{};
        };

        constexpr auto k_dispositions = std::array{
            DispositionName{
                "ambiguous",
                operator_runtime::ReconcileDisposition::Ambiguous,
            },
            DispositionName{
                "confirmed",
                operator_runtime::ReconcileDisposition::Confirmed,
            },
            DispositionName{
                "continue",
                operator_runtime::ReconcileDisposition::Continue,
            },
            DispositionName{
                "diverged",
                operator_runtime::ReconcileDisposition::Diverged,
            },
            DispositionName{
                "rejected",
                operator_runtime::ReconcileDisposition::Rejected,
            },
        };

        // OP:`Risk`, spelled once. The wire names are riskWireName's and are
        // not restated here: the enumerators are the domain and the projection
        // is the mapping, so a name that drifted would drift in one place.
        constexpr auto k_risks = std::array{
            operator_runtime::Risk::ReadOnly,
            operator_runtime::Risk::Low,
            operator_runtime::Risk::Medium,
            operator_runtime::Risk::High,
            operator_runtime::Risk::Critical,
        };

        // The largest value each OP:`WorkflowLimits` member holds. 2^53 for the
        // millisecond bound rather than uint64's maximum: past it a double no
        // longer represents consecutive integers, so a larger ceiling would
        // admit a value the document did not spell.
        constexpr auto k_workflowCountBound  = uint64{0xFFFF'FFFF};
        constexpr auto k_workflowMillisBound = uint64{1} << 53U;

        // One OP:`WorkflowLimits` member, narrowed. The schema bounds each of
        // the five from below and none of them from above, and converting a
        // double outside the destination's range is undefined rather than
        // merely large -- so the upper bound is stated where the narrowing
        // happens and nowhere else.
        [[nodiscard]]
        auto workflowBound(
            json::Value const& limits,
            std::string_view name,
            uint64 ceiling
        ) -> Result<uint64>
        {
            auto const declared = member(limits, name).number();
            if (declared < 0.0 || declared > static_cast<double>(ceiling))
            {
                return refuse(std::format(
                    "a PlanProposal's {} is outside the range that workflow "
                    "bound holds",
                    name
                ));
            }
            return static_cast<uint64>(declared);
        }

        struct ToolEntry final
        {
            std::string                      name{};
            std::string                      version{};
            std::string                      argumentDefinition{};
            operator_runtime::ToolMutability mutability{};
            operator_runtime::ToolSurface    surface{};
        };

        // One payload schema and the identity it answers under: the sha256 of
        // its own exact bytes, which is what a journal manifest entry and an
        // OP:`ExpectedEffect` each name.
        struct PayloadSchema final
        {
            ContentHash  hash;
            json::Schema schema;
        };

        // The digests of the bytes this deployment holds, for a refusal that
        // has to say what the document could have named. R5
        // (project-as-data.md 2.7) requires a digest disagreement to print the
        // stated digest and what the deployment carries, and for a set of
        // schemas named only by digest that set is the whole of the other side.
        [[nodiscard]]
        auto carriedDigests(std::span<PayloadSchema const> schemas) -> std::string
        {
            auto listed = std::string{};
            for (auto const& schema : schemas)
            {
                if (!listed.empty())
                {
                    listed += ", ";
                }
                listed += schema.hash.hex();
            }
            return listed;
        }

        struct JournalPayload final
        {
            std::string eventType{};
            std::size_t schemaIndex{};
        };

        struct DispositionEntry final
        {
            std::string                            value{};
            operator_runtime::ReconcileDisposition disposition{};
        };
    }

    // Everything create() compiled and read, and the whole of what judging a
    // document consults. It carries the operations rather than leaving them as
    // free functions, because six of them would otherwise take it as a first
    // parameter.
    class ProjectDeployment::State final
    {
    public:
        json::Schema deriveInput;
        json::Schema planInput;
        json::Schema stepInput;
        json::Schema reduceInput;
        json::Schema planProposal;
        json::Schema stepIntent;
        json::Schema projectState;
        json::Schema projectObservation;
        json::Schema toolPrecondition;
        json::Schema reconcile;

        std::vector<ToolEntry>        tools{};
        std::vector<PayloadSchema>    journalPayloadSchemas{};
        std::vector<JournalPayload>   journalPayloads{};
        std::vector<PayloadSchema>    effectPayloadSchemas{};
        std::vector<DispositionEntry> dispositions{};

        std::string requestDefinition{};
        std::string verdictDefinition{};
        std::string verdictMember{};

        [[nodiscard]] auto findTool(std::string_view name) const -> ToolEntry const*;

        [[nodiscard]]
        auto validateToolArguments(
            std::string_view toolName,
            json::Value const& arguments
        ) const -> Status;

        [[nodiscard]]
        auto validateJournalPayload(
            std::string_view eventType,
            json::Value const& payload
        ) const -> Status;

        [[nodiscard]]
        auto validateEffectPayloads(json::Value const& proposal) const -> Status;

        [[nodiscard]]
        auto validateInput(
            operator_runtime::ProjectPluginFunction function,
            json::Value const& document
        ) const -> Status;

        [[nodiscard]]
        auto validateOutput(
            operator_runtime::ProjectPluginFunction function,
            json::Value const& document
        ) const -> Status;
    };

    auto ProjectDeployment::State::findTool(std::string_view name) const
        -> ToolEntry const*
    {
        auto const found = std::ranges::find(tools, name, &ToolEntry::name);
        return found == tools.end() ? nullptr : &*found;
    }

    // The arguments of one call, against the definition this project's Tool
    // Catalog names for that tool. A tool the catalog does not declare is a
    // refusal here, which is what makes tool_name inside an OP:`PlanProposal` a
    // stronger statement than the operator protocol's own NamespacedIdentifier.
    auto ProjectDeployment::State::validateToolArguments(
        std::string_view toolName,
        json::Value const& arguments
    ) const -> Status
    {
        auto const* const p_tool = findTool(toolName);
        if (p_tool == nullptr)
        {
            return refuse(std::format(
                "this project's Tool Catalog declares no tool named {}",
                toolName
            ));
        }
        return adopt(
            toolPrecondition.validateDefinition(p_tool->argumentDefinition, arguments),
            std::format("arguments of {}", toolName)
        );
    }

    auto ProjectDeployment::State::validateJournalPayload(
        std::string_view eventType,
        json::Value const& payload
    ) const -> Status
    {
        auto const found = std::ranges::find(
            journalPayloads,
            eventType,
            &JournalPayload::eventType
        );
        if (found == journalPayloads.end())
        {
            return refuse(std::format(
                "this project's journal event schema manifest names no payload "
                "schema for {}",
                eventType
            ));
        }
        return adopt(
            journalPayloadSchemas[found->schemaIndex].schema.validate(payload),
            std::format("payload of {}", eventType)
        );
    }

    auto ProjectDeployment::State::validateEffectPayloads(
        json::Value const& proposal
    ) const -> Status
    {
        for (auto const& effect : member(proposal, "effects").items())
        {
            auto const declared = member(effect, "payload_schema_hash").string();
            UF_TRY_VALUE(
                hash,
                ContentHash::parse("sha256:" + std::string{declared})
            );
            auto const found = std::ranges::find(
                effectPayloadSchemas,
                hash,
                &PayloadSchema::hash
            );
            if (found == effectPayloadSchemas.end())
            {
                return refuse(std::format(
                    "an ExpectedEffect names payload schema {}, which this "
                    "deployment does not carry",
                    declared
                ));
            }
            UF_TRY(adopt(
                found->schema.validate(member(effect, "opaque_project_payload")),
                std::format(
                    "payload of effect {}",
                    member(effect, "namespaced_type").string()
                )
            ));
        }
        return ok();
    }

    auto ProjectDeployment::State::validateInput(
        operator_runtime::ProjectPluginFunction function,
        json::Value const& document
    ) const -> Status
    {
        switch (function)
        {
        case operator_runtime::ProjectPluginFunction::Derive:
        {
            return adopt(deriveInput.validate(document), "derive input");
        }
        case operator_runtime::ProjectPluginFunction::Plan:
        {
            UF_TRY(adopt(planInput.validate(document), "plan input"));
            return validateToolArguments(
                member(document, "tool_name").string(),
                member(document, "canonical_args")
            );
        }
        case operator_runtime::ProjectPluginFunction::NextStep:
        {
            return adopt(stepInput.validate(document), "next_step input");
        }
        case operator_runtime::ProjectPluginFunction::Reconcile:
        {
            return adopt(
                reconcile.validateDefinition(requestDefinition, document),
                "reconcile input"
            );
        }
        case operator_runtime::ProjectPluginFunction::Reduce:
        {
            UF_TRY(adopt(reduceInput.validate(document), "reduce input"));
            for (auto const& event : member(document, "journal_events").items())
            {
                UF_TRY(validateJournalPayload(
                    member(event, "namespaced_event_type").string(),
                    member(event, "opaque_project_payload")
                ));
            }
            return ok();
        }
        }

        UF_UNREACHABLE_MSG("unknown ProjectPluginFunction");
    }

    auto ProjectDeployment::State::validateOutput(
        operator_runtime::ProjectPluginFunction function,
        json::Value const& document
    ) const -> Status
    {
        switch (function)
        {
        case operator_runtime::ProjectPluginFunction::Derive:
        {
            return adopt(
                projectObservation.validate(document),
                "derived ProjectObservation"
            );
        }
        case operator_runtime::ProjectPluginFunction::Plan:
        {
            UF_TRY(adopt(planProposal.validate(document), "PlanProposal"));
            UF_TRY(validateToolArguments(
                member(document, "tool_name").string(),
                member(document, "canonical_args")
            ));
            return validateEffectPayloads(document);
        }
        case operator_runtime::ProjectPluginFunction::NextStep:
        {
            return adopt(stepIntent.validate(document), "step intent");
        }
        case operator_runtime::ProjectPluginFunction::Reconcile:
        {
            return adopt(
                reconcile.validateDefinition(verdictDefinition, document),
                "reconcile output"
            );
        }
        case operator_runtime::ProjectPluginFunction::Reduce:
        {
            return adopt(projectState.validate(document), "reduced ProjectState");
        }
        }

        UF_UNREACHABLE_MSG("unknown ProjectPluginFunction");
    }

    auto validateFrameworkFormat(std::string_view exactBytes) -> Status
    {
        UF_TRY_VALUE(document, parseDocument(exactBytes));
        auto const* const p_schema = document.find("schema");
        if (p_schema == nullptr || p_schema->kind() != json::ValueKind::String)
        {
            return refuse(
                "a framework-format document names its own format in a schema "
                "member, and this one carries no such member"
            );
        }
        auto const named = std::ranges::find(
            k_frameworkDocuments,
            p_schema->string(),
            &FrameworkDocument::schemaName
        );
        if (named == k_frameworkDocuments.end())
        {
            return refuse(std::format(
                "no framework document format is named {}",
                p_schema->string()
            ));
        }

        auto const commonOnly = std::array{json::Schema::Document{
            .label      = "operator/common",
            .exactBytes = k_commonSchema,
        }};
        UF_TRY_VALUE(
            schema,
            compile(named->label, named->exactBytes, commonOnly)
        );
        return adopt(schema.validate(document), named->schemaName);
    }

    auto toolMutabilityWireName(operator_runtime::ToolMutability mutability) noexcept
        -> std::string_view
    {
        auto const found = std::ranges::find(
            k_mutabilities,
            mutability,
            &MutabilityName::mutability
        );
        UF_CHECK(found != k_mutabilities.end());
        return found->wire;
    }

    auto canonicalJsonValidator() -> operator_runtime::CanonicalJsonValidator
    {
        return [](std::string_view exactJcs) -> Status
        {
            return adopt(json::requireExactCanonical(exactJcs), "canonical bytes");
        };
    }

    auto readPlanProposal(operator_runtime::ValidatedDocument const& proposal)
        -> Result<operator_runtime::PlanProposalClaims>
    {
        UF_TRY(requireOutputOf(
            proposal,
            operator_runtime::ProjectPluginFunction::Plan,
            "a PlanProposal"
        ));
        UF_TRY_VALUE(document, parseDocument(proposal.bytes()));

        // canonical_args and every opaque_project_payload below are
        // re-serialized rather than sliced out of the input. The document is
        // its own RFC 8785 form, so a member's canonical bytes are the bytes it
        // occupied -- which is the property the plan hash rests on.
        auto claims = operator_runtime::PlanProposalClaims{
            .toolName         = std::string{member(document, "tool_name").string()},
            .toolVersion      = std::string{member(document, "tool_version").string()},
            .canonicalArgs    = json::canonicalBytes(member(document, "canonical_args")),
            .effects          = {},
            .allowedUiActions = {},
            .limits           = {},
        };
        for (auto const& action : member(document, "allowed_ui_actions").items())
        {
            claims.allowedUiActions.emplace_back(action.string());
        }
        for (auto const& effect : member(document, "effects").items())
        {
            auto const risk = std::ranges::find(
                k_risks,
                member(effect, "risk").string(),
                operator_runtime::riskWireName
            );
            UF_CHECK(risk != k_risks.end());
            // OP:`Hash` is bare lowercase hex; ContentHash spells its own
            // canonical form with the algorithm in front.
            UF_TRY_VALUE(
                payloadSchemaHash,
                ContentHash::parse(
                    "sha256:"
                    + std::string{member(effect, "payload_schema_hash").string()}
                )
            );
            claims.effects.emplace_back(operator_runtime::ProposedEffect{
                .namespacedType    = std::string{member(effect, "namespaced_type").string()},
                .risk              = *risk,
                .scopeKind         = std::string{member(effect, "scope_kind").string()},
                .scopeKey          = std::string{member(effect, "scope_key").string()},
                .payloadSchemaHash = payloadSchemaHash,
                .opaqueProjectPayload = json::canonicalBytes(
                    member(effect, "opaque_project_payload")
                ),
            });
        }

        auto const& limits = member(document, "workflow_limits");
        UF_TRY_VALUE(
            steps,
            workflowBound(limits, "maximum_steps", k_workflowCountBound)
        );
        UF_TRY_VALUE(
            dispatches,
            workflowBound(limits, "maximum_dispatches", k_workflowCountBound)
        );
        UF_TRY_VALUE(
            observations,
            workflowBound(limits, "maximum_observations", k_workflowCountBound)
        );
        UF_TRY_VALUE(
            waits,
            workflowBound(limits, "maximum_waits", k_workflowCountBound)
        );
        UF_TRY_VALUE(
            elapsed,
            workflowBound(limits, "maximum_elapsed_ms", k_workflowMillisBound)
        );
        claims.limits = operator_runtime::WorkflowLimits{
            .maximumSteps         = static_cast<uint32>(steps),
            .maximumDispatches    = static_cast<uint32>(dispatches),
            .maximumObservations  = static_cast<uint32>(observations),
            .maximumWaits         = static_cast<uint32>(waits),
            .maximumElapsedMillis = elapsed,
        };
        return claims;
    }

    auto readStepIntent(operator_runtime::ValidatedDocument const& intent)
        -> Result<operator_runtime::StepIntentClaims>
    {
        UF_TRY(requireOutputOf(
            intent,
            operator_runtime::ProjectPluginFunction::NextStep,
            "a step intent"
        ));
        UF_TRY_VALUE(document, parseDocument(intent.bytes()));

        // Which of the two the oneOf matched, read back off the document:
        // `action` is required by OP:`UIActionIntent` and forbidden by
        // OP:`WaitIntent`, so its presence is the answer the schema already
        // reached. A wait names no UI and leaves the three identifiers empty,
        // which is what mintStep refuses a UI-action step for.
        auto const* const p_action = document.find("action");
        if (p_action == nullptr)
        {
            return operator_runtime::StepIntentClaims{
                .stepKey = std::string{member(document, "step_key").string()},
                .kind    = operator_runtime::StepKind::Wait,
            };
        }
        return operator_runtime::StepIntentClaims{
            .stepKey    = std::string{member(document, "step_key").string()},
            .surfaceId  = std::string{member(*p_action, "surface_id").string()},
            .uiTargetId = std::string{member(*p_action, "ui_target_id").string()},
            .actionId   = std::string{member(*p_action, "action_id").string()},
            .kind       = operator_runtime::StepKind::UiAction,
        };
    }

    ProjectDeployment::ProjectDeployment(std::shared_ptr<State const> p_state) noexcept
        : m_state{std::move(p_state)}
    {
    }

    auto ProjectDeployment::create(ProjectDeploymentSources const& sources)
        -> Result<ProjectDeployment>
    {
        UF_TRY(requireIdentity(
            "the project state schema",
            sources.projectState,
            k_projectStateSchemaId
        ));
        UF_TRY(requireIdentity(
            "the project observation schema",
            sources.projectObservation,
            k_projectObservationSchemaId
        ));
        UF_TRY(requireIdentity(
            "the tool precondition schema",
            sources.toolPrecondition,
            k_toolPreconditionSchemaId
        ));
        UF_TRY(requireIdentity(
            "the reconcile schema",
            sources.reconcile,
            k_reconcileSchemaId
        ));

        auto const common = json::Schema::Document{
            .label      = "operator/common",
            .exactBytes = k_commonSchema,
        };
        auto const projectStateDocument = json::Schema::Document{
            .label      = "project/state",
            .exactBytes = sources.projectState,
        };
        auto const projectObservationDocument = json::Schema::Document{
            .label      = "project/observation",
            .exactBytes = sources.projectObservation,
        };

        auto const commonOnly = std::array{common};
        auto const withState  = std::array{common, projectStateDocument};
        auto const withWorld  = std::array{
            common,
            projectStateDocument,
            projectObservationDocument,
        };

        UF_TRY_VALUE(
            deriveInput,
            compile("operator/derive-input", k_deriveInputSchema, withWorld)
        );
        UF_TRY_VALUE(
            planInput,
            compile("operator/plan-input", k_planInputSchema, withWorld)
        );
        UF_TRY_VALUE(
            stepInput,
            compile("operator/step-input", k_stepInputSchema, withWorld)
        );
        UF_TRY_VALUE(
            reduceInput,
            compile("operator/reduce-input", k_reduceInputSchema, withState)
        );
        UF_TRY_VALUE(envelopes, envelopeSchemas());
        UF_TRY_VALUE(
            projectState,
            compile("project/state", sources.projectState, {})
        );
        UF_TRY_VALUE(
            projectObservation,
            compile("project/observation", sources.projectObservation, {})
        );
        UF_TRY_VALUE(
            toolPrecondition,
            compile("project/tool-precondition", sources.toolPrecondition, {})
        );
        UF_TRY_VALUE(
            reconcile,
            compile("project/reconcile", sources.reconcile, {})
        );

        auto state = std::make_shared<State>(State{
            .deriveInput           = std::move(deriveInput),
            .planInput             = std::move(planInput),
            .stepInput             = std::move(stepInput),
            .reduceInput           = std::move(reduceInput),
            .planProposal          = std::move(envelopes.planProposal),
            .stepIntent            = std::move(envelopes.stepIntent),
            .projectState          = std::move(projectState),
            .projectObservation    = std::move(projectObservation),
            .toolPrecondition      = std::move(toolPrecondition),
            .reconcile             = std::move(reconcile),
            .tools                 = {},
            .journalPayloadSchemas = {},
            .journalPayloads       = {},
            .effectPayloadSchemas  = {},
            .dispositions          = {},
            .requestDefinition     = {},
            .verdictDefinition     = {},
            .verdictMember         = {},
        });

        // The payload schema sets first, because every manifest below names one
        // of them by sha256 and a manifest naming bytes nobody supplied is the
        // link that would otherwise be a convention.
        for (auto const bytes : sources.journalPayloadSchemas)
        {
            UF_TRY_VALUE(hash, hashOf(bytes));
            UF_TRY_VALUE(
                schema,
                compile(std::format("journal payload {}", hash.hex()), bytes, {})
            );
            state->journalPayloadSchemas.emplace_back(PayloadSchema{
                .hash   = hash,
                .schema = std::move(schema),
            });
        }
        for (auto const bytes : sources.effectPayloadSchemas)
        {
            UF_TRY_VALUE(hash, hashOf(bytes));
            UF_TRY_VALUE(
                schema,
                compile(std::format("effect payload {}", hash.hex()), bytes, {})
            );
            state->effectPayloadSchemas.emplace_back(PayloadSchema{
                .hash   = hash,
                .schema = std::move(schema),
            });
        }

        UF_TRY_VALUE(
            toolCatalogSchema,
            compile(
                k_toolCatalogDocument.label,
                k_toolCatalogDocument.exactBytes,
                commonOnly
            )
        );
        UF_TRY_VALUE(catalog, parseDocument(sources.toolCatalog));
        UF_TRY(adopt(toolCatalogSchema.validate(catalog), "the Tool Catalog"));
        UF_TRY(requirePluginId(catalog, sources.pluginId, "the Tool Catalog"));

        UF_TRY_VALUE(toolPreconditionHash, hashOf(sources.toolPrecondition));
        if (member(catalog, "tool_precondition_sha256").string()
            != toolPreconditionHash.hex())
        {
            return refuse(std::format(
                "the Tool Catalog names tool precondition schema {}, and the "
                "schema this deployment carries hashes to {}",
                member(catalog, "tool_precondition_sha256").string(),
                toolPreconditionHash.hex()
            ));
        }
        for (auto const& tool : member(catalog, "tools").items())
        {
            auto const definition = member(tool, "argument_schema").string();
            if (!state->toolPrecondition.hasDefinition(definition))
            {
                return refuse(std::format(
                    "the Tool Catalog names argument schema {}, which the tool "
                    "precondition schema does not declare",
                    definition
                ));
            }
            auto const mutability = std::ranges::find(
                k_mutabilities,
                member(tool, "mutability").string(),
                &MutabilityName::wire
            );
            auto const surface = std::ranges::find(
                k_surfaces,
                member(tool, "surface").string(),
                &SurfaceName::wire
            );
            UF_CHECK(mutability != k_mutabilities.end());
            UF_CHECK(surface != k_surfaces.end());
            state->tools.emplace_back(ToolEntry{
                .name               = std::string{member(tool, "name").string()},
                .version            = std::string{member(tool, "version").string()},
                .argumentDefinition = std::string{definition},
                .mutability         = mutability->mutability,
                .surface            = surface->surface,
            });
        }

        UF_TRY_VALUE(
            journalManifestSchema,
            compile(
                k_journalManifestDocument.label,
                k_journalManifestDocument.exactBytes,
                commonOnly
            )
        );
        UF_TRY_VALUE(journalManifest, parseDocument(sources.journalEventManifest));
        UF_TRY(adopt(
            journalManifestSchema.validate(journalManifest),
            "the journal event schema manifest"
        ));
        UF_TRY(requirePluginId(
            journalManifest,
            sources.pluginId,
            "the journal event schema manifest"
        ));
        for (auto const& entry : member(journalManifest, "payload_schemas").items())
        {
            auto const declared = member(entry, "sha256").string();
            auto const found    = std::ranges::find_if(
                state->journalPayloadSchemas,
                [declared](PayloadSchema const& candidate)
                {
                    return candidate.hash.hex() == declared;
                }
            );
            if (found == state->journalPayloadSchemas.end())
            {
                return refuse(std::format(
                    "the journal event schema manifest names payload schema {} "
                    "for {}, which this deployment does not carry: its "
                    "journal_payload_schemas hash to {}",
                    declared,
                    member(entry, "namespaced_event_type").string(),
                    carriedDigests(state->journalPayloadSchemas)
                ));
            }
            state->journalPayloads.emplace_back(JournalPayload{
                .eventType = std::string{
                    member(entry, "namespaced_event_type").string(),
                },
                .schemaIndex = static_cast<std::size_t>(
                    found - state->journalPayloadSchemas.begin()
                ),
            });
        }

        // The other direction of the same agreement, and it is the direction
        // that decides whether the bytes are inside any digest at all. A
        // payload schema no entry names reaches
        // journal_event_schema_manifest_hash through nothing, so it would be
        // compiled, held by this deployment, and consulted by no document ever
        // -- the shape project-as-data.md 7.0 rules out for pinned schemas.
        // Both halves are authored in one directory, which is R8's criterion.
        for (auto index = std::size_t{0};
             index < state->journalPayloadSchemas.size();
             ++index)
        {
            auto const named = std::ranges::find(
                state->journalPayloads,
                index,
                &JournalPayload::schemaIndex
            );
            if (named == state->journalPayloads.end())
            {
                return refuse(std::format(
                    "this deployment supplies a journal payload schema hashing "
                    "to {}, which the journal event schema manifest names under "
                    "no event type",
                    state->journalPayloadSchemas[index].hash.hex()
                ));
            }
        }

        UF_TRY_VALUE(
            reconcileManifestSchema,
            compile(
                k_reconcileManifestDocument.label,
                k_reconcileManifestDocument.exactBytes,
                commonOnly
            )
        );
        UF_TRY_VALUE(reconcileManifest, parseDocument(sources.reconcileManifest));
        UF_TRY(adopt(
            reconcileManifestSchema.validate(reconcileManifest),
            "the reconcile payload schema manifest"
        ));
        UF_TRY(requirePluginId(
            reconcileManifest,
            sources.pluginId,
            "the reconcile payload schema manifest"
        ));
        UF_TRY_VALUE(reconcileHash, hashOf(sources.reconcile));
        if (member(reconcileManifest, "reconcile_schema_sha256").string()
            != reconcileHash.hex())
        {
            return refuse(std::format(
                "the reconcile manifest names reconcile schema {}, and the "
                "schema this deployment carries hashes to {}",
                member(reconcileManifest, "reconcile_schema_sha256").string(),
                reconcileHash.hex()
            ));
        }
        state->requestDefinition = std::string{
            member(reconcileManifest, "request_definition").string(),
        };
        state->verdictDefinition = std::string{
            member(reconcileManifest, "verdict_definition").string(),
        };
        state->verdictMember = std::string{
            member(reconcileManifest, "verdict_member").string(),
        };
        for (auto const& name : std::array{
                 state->requestDefinition,
                 state->verdictDefinition,
             })
        {
            if (!state->reconcile.hasDefinition(name))
            {
                return refuse(std::format(
                    "the reconcile manifest names definition {}, which the "
                    "reconcile schema does not declare",
                    name
                ));
            }
        }
        for (auto const& entry : member(reconcileManifest, "dispositions").items())
        {
            auto const named = std::ranges::find(
                k_dispositions,
                member(entry, "disposition").string(),
                &DispositionName::wire
            );
            UF_CHECK(named != k_dispositions.end());
            state->dispositions.emplace_back(DispositionEntry{
                .value       = std::string{member(entry, "value").string()},
                .disposition = named->disposition,
            });
        }

        return ProjectDeployment{std::shared_ptr<State const>{std::move(state)}};
    }

    auto ProjectDeployment::carriedTool(std::string_view name) const
        -> std::optional<operator_runtime::ToolDescriptor>
    {
        auto const* const p_tool = m_state->findTool(name);
        if (p_tool == nullptr)
        {
            return std::nullopt;
        }
        return operator_runtime::ToolDescriptor{
            .toolVersion = p_tool->version,
            .mutability  = p_tool->mutability,
            .surface     = p_tool->surface,
        };
    }

    auto ProjectDeployment::documentValidator() const
        -> operator_runtime::ProjectDocumentValidator
    {
        return [p_state = m_state](
                   operator_runtime::ProjectPluginFunction function,
                   operator_runtime::ProjectDocumentDirection direction,
                   std::string_view exactJcs
               ) -> Status
        {
            UF_TRY_VALUE(document, parseDocument(exactJcs));
            switch (direction)
            {
            case operator_runtime::ProjectDocumentDirection::Input:
                return p_state->validateInput(function, document);
            case operator_runtime::ProjectDocumentDirection::Output:
                return p_state->validateOutput(function, document);
            }

            UF_UNREACHABLE_MSG("unknown ProjectDocumentDirection");
        };
    }

    auto ProjectDeployment::journalPayloadValidator() const
        -> operator_runtime::JournalPayloadSchemaValidator
    {
        return [p_state = m_state](
                   std::string_view namespacedEventType,
                   std::string_view exactPayloadJcs
               ) -> Result<ContentHash>
        {
            auto const found = std::ranges::find(
                p_state->journalPayloads,
                namespacedEventType,
                &JournalPayload::eventType
            );
            if (found == p_state->journalPayloads.end())
            {
                return refuse(std::format(
                    "this project's journal event schema manifest names no payload "
                    "schema for {}",
                    namespacedEventType
                ));
            }
            auto const& pinned = p_state->journalPayloadSchemas[found->schemaIndex];
            UF_TRY_VALUE(payload, parseDocument(exactPayloadJcs));
            UF_TRY(adopt(
                pinned.schema.validate(payload),
                std::format("payload of {}", namespacedEventType)
            ));
            return pinned.hash;
        };
    }

    auto ProjectDeployment::toolCatalogValidator() const
        -> operator_runtime::ToolCatalogValidator
    {
        return [p_state = m_state](
                   std::string_view toolName,
                   std::string_view exactArgsJcs
               ) -> Result<operator_runtime::ToolDescriptor>
        {
            auto const* const p_tool = p_state->findTool(toolName);
            if (p_tool == nullptr)
            {
                return refuse(std::format(
                    "this project's Tool Catalog declares no tool named {}",
                    toolName
                ));
            }
            UF_TRY_VALUE(arguments, parseDocument(exactArgsJcs));
            UF_TRY(p_state->validateToolArguments(toolName, arguments));
            return operator_runtime::ToolDescriptor{
                .toolVersion = p_tool->version,
                .mutability  = p_tool->mutability,
                .surface     = p_tool->surface,
            };
        };
    }

    auto ProjectDeployment::reconcileDispositionReader() const
        -> operator_runtime::ReconcileDispositionReader
    {
        return [p_state = m_state](std::string_view exactJcs)
                   -> Result<operator_runtime::ReconcileDisposition>
        {
            UF_TRY_VALUE(verdict, parseDocument(exactJcs));
            UF_TRY(adopt(
                p_state->reconcile.validateDefinition(
                    p_state->verdictDefinition,
                    verdict
                ),
                "reconcile output"
            ));
            auto const* const p_member = verdict.find(p_state->verdictMember);
            if (p_member == nullptr || p_member->kind() != json::ValueKind::String)
            {
                return refuse(std::format(
                    "the reconcile output carries no {} member to read a "
                    "disposition from",
                    p_state->verdictMember
                ));
            }
            auto const found = std::ranges::find(
                p_state->dispositions,
                p_member->string(),
                &DispositionEntry::value
            );
            if (found == p_state->dispositions.end())
            {
                return refuse(std::format(
                    "the reconcile manifest maps no disposition to {}",
                    p_member->string()
                ));
            }
            return found->disposition;
        };
    }
}
