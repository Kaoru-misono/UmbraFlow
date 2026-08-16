#include "project-deployment.hpp"

#include <json/error.hpp>
#include <json/schema.hpp>
#include <json/value.hpp>

#include <schema/framework-schema-catalog.hpp>

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
        // schemas -- schema/umbraflow-operator-v1.schema.json,
        // schema/umbraflow-journal-v1.schema.json and, for StateResolution's
        // readings, schema/umbraflow-runtime-v2.schema.json -- so a document
        // this module accepts is a document those accept.
        //
        // These bytes are compiled in and read no file: a Host that judges a
        // plugin's derive input may not depend on a document a project could
        // swap. The published schema is still the source of the shape, and
        // readings_contract_errors in tests/test-runtime-surface.py derives
        // this restatement from it, so editing either alone is red.
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
            "$comment": "What the trusted Luau resolver serializes: the kind, the ordered surface stack a resolved state carries, the readings that state reports, and the reason the other kinds failed. Absent members are absent rather than null, so only kind is required. diagnostic is carried only when visible content matched nothing, which is the one unknown that a bounded reason cannot describe; the resolver's other unknown branches send reason alone, so requiring it here would refuse them.",
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
                "readings": {
                    "$comment": "Every reading a resolved state reports, ordered by ui_target then reader so one world produces one document. One entry per Reader every reporting Binding named, whatever the outcome, so the length is decided by the model and the resolved stack rather than by what the Host managed to answer. A Binding owns the visual variants that share its placement and reads, so changing which one matches does not change the reporting subject. Multiple present Bindings sharing a UiTarget still report nothing for it because they are distinct placements or roles and therefore remain ambiguous.",
                    "type": "array",
                    "items": {
                        "type": "object",
                        "additionalProperties": false,
                        "required": ["kind", "reader", "ui_target"],
                        "properties": {
                            "kind": {"enum": ["read", "absent", "unknown"]},
                            "lines": {
                                "type": "array",
                                "minItems": 1,
                                "items": {
                                    "type": "object",
                                    "additionalProperties": false,
                                    "required": ["rect", "text"],
                                    "properties": {
                                        "rect": {
                                            "type": "array",
                                            "prefixItems": [
                                                {"type": "integer", "minimum": 0},
                                                {"type": "integer", "minimum": 0},
                                                {"type": "integer", "minimum": 1},
                                                {"type": "integer", "minimum": 1}
                                            ],
                                            "items": false,
                                            "minItems": 4,
                                            "maxItems": 4
                                        },
                                        "text": {"type": "string"}
                                    }
                                }
                            },
                            "reader": {"$ref": "#/$defs/Identifier"},
                            "reason": {
                                "enum": [
                                    "not_measured",
                                    "budget_exhausted",
                                    "low_confidence",
                                    "ocr_unreadable",
                                    "locator_failed",
                                    "stale_cycle",
                                    "host_unavailable",
                                    "internal_error"
                                ]
                            },
                            "ui_target": {"$ref": "#/$defs/Identifier"}
                        },
                        "allOf": [
                            {
                                "if": {
                                    "properties": {"kind": {"const": "read"}},
                                    "required": ["kind"]
                                },
                                "then": {"required": ["lines"]},
                                "else": {"not": {"required": ["lines"]}}
                            },
                            {
                                "if": {
                                    "properties": {"kind": {"const": "unknown"}},
                                    "required": ["kind"]
                                },
                                "then": {"required": ["reason"]},
                                "else": {"not": {"required": ["reason"]}}
                            }
                        ]
                    }
                },
                "reason": {"type": "string", "minLength": 1},
                "diagnostic": {"type": "string", "minLength": 1}
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
            "items": {"$ref": "#/$defs/JournalEvent"}
        },
        "prior_project_state": {
            "$comment": "null for initial reduction. journal_events contains the declared baseline or is empty when the project declares none.",
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
        "effects": {"type": "array", "items": {"$ref": "#/$defs/EffectEnvelope"}},
        "tool_name": {
            "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
        },
        "tool_version": {
            "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
        },
        "workflow_limits": {"$ref": "#/$defs/WorkflowLimits"}
    },
    "$defs": {
        "EffectEnvelope": {
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
    "required": [
        "effect_payload_sha256s",
        "plugin_id",
        "schema",
        "tool_precondition_sha256",
        "tools"
    ],
    "properties": {
        "$comment": {"type": "string"},
        "effect_payload_sha256s": {
            "$comment": "The sha256 of each OP:EffectEnvelope payload schema this deployment supplies, and the only path by which those bytes reach tool_catalog_hash and so project_registration_hash. The array may be empty: a project that proposes no effect has nothing to pin.",
            "type": "array",
            "items": {
                "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Hash"
            },
            "uniqueItems": true
        },
        "plugin_id": {
            "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
        },
        "schema": {"const": "umbraflow-tool-catalog/v1"},
        "tool_precondition_sha256": {
            "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Hash"
        },
        "tools": {
            "$comment": "One complete ToolDescriptor per tool. Every bound a plan is judged against is declared here and nowhere else, so a widened bound moves tool_catalog_hash and therefore project_registration_hash.",
            "type": "array",
            "minItems": 1,
            "items": {
                "type": "object",
                "additionalProperties": false,
                "required": [
                    "argument_schema",
                    "effect_bounds",
                    "idempotency",
                    "mutability",
                    "name",
                    "required_capabilities",
                    "surface",
                    "timeout_policy",
                    "ui_action_bounds",
                    "version",
                    "workflow_limits"
                ],
                "properties": {
                    "argument_schema": {
                        "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
                    },
                    "effect_bounds": {
                        "$comment": "The complete set of OP:EffectEnvelope this tool may propose. An empty set is a tool that may propose none, which is the honest declaration for a read_only tool.",
                        "type": "array",
                        "uniqueItems": true,
                        "items": {
                            "type": "object",
                            "additionalProperties": false,
                            "required": [
                                "maximum_risk",
                                "namespaced_type",
                                "payload_schema_hash",
                                "scope_kind"
                            ],
                            "properties": {
                                "maximum_risk": {
                                    "enum": [
                                        "read_only",
                                        "low",
                                        "medium",
                                        "high",
                                        "critical"
                                    ]
                                },
                                "namespaced_type": {
                                    "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/NamespacedIdentifier"
                                },
                                "payload_schema_hash": {
                                    "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Hash"
                                },
                                "scope_kind": {
                                    "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
                                }
                            }
                        }
                    },
                    "idempotency": {
                        "enum": [
                            "read_safe",
                            "delivery_safe",
                            "keyed_external",
                            "non_idempotent"
                        ]
                    },
                    "mutability": {"enum": ["read_only", "mutating"]},
                    "name": {
                        "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
                    },
                    "required_capabilities": {
                        "type": "array",
                        "uniqueItems": true,
                        "items": {
                            "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
                        }
                    },
                    "surface": {"enum": ["semantic", "privileged"]},
                    "timeout_policy": {
                        "type": "object",
                        "additionalProperties": false,
                        "required": ["maximum_elapsed_ms", "on_timeout"],
                        "properties": {
                            "maximum_elapsed_ms": {"type": "integer", "minimum": 1},
                            "on_timeout": {
                                "enum": ["reobserve", "reconcile", "stop"]
                            }
                        }
                    },
                    "ui_action_bounds": {
                        "$comment": "The complete set of OP:EffectivePlan allowed_ui_actions entries this tool may propose.",
                        "type": "array",
                        "uniqueItems": true,
                        "items": {
                            "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
                        }
                    },
                    "version": {
                        "$ref": "https://umbraflow.dev/schema/operator/common#/$defs/Identifier"
                    },
                    "workflow_limits": {
                        "$comment": "This tool's own ceiling. There is no second one compiled into the Operator: a limit stated per tool and another stated in C++ would be two authorities over one number.",
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
                            "maximum_dispatches": {
                                "type": "integer",
                                "minimum": 1,
                                "maximum": 4294967295
                            },
                            "maximum_elapsed_ms": {
                                "type": "integer",
                                "minimum": 1,
                                "maximum": 9007199254740991
                            },
                            "maximum_observations": {
                                "type": "integer",
                                "minimum": 1,
                                "maximum": 4294967295
                            },
                            "maximum_steps": {
                                "type": "integer",
                                "minimum": 1,
                                "maximum": 4294967295
                            },
                            "maximum_waits": {
                                "type": "integer",
                                "minimum": 0,
                                "maximum": 4294967295
                            }
                        }
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

        [[nodiscard]]
        auto sharedSchemaDocuments() -> std::vector<json::Schema::Document>
        {
            auto documents = std::vector<json::Schema::Document>{};
            auto const catalog = framework_schema::frameworkSchemaCatalog();
            documents.reserve(catalog.size());
            for (auto const& published : catalog)
            {
                documents.emplace_back(json::Schema::Document{
                    .label      = published.relativePath,
                    .exactBytes = published.exactBytes,
                });
            }
            return documents;
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

        // The four project schemas declare a fixed identity, but an observed
        // identity schema declares its own: the compiled authority answers by
        // whatever absolute $id the document carries, which is why the
        // registration restates no ID at all. compile() below enforces that
        // the value is an absolute URI; presence is this check's.
        [[nodiscard]]
        auto identityIdOf(std::string_view label, std::string_view exactBytes)
            -> Result<std::string>
        {
            UF_TRY_VALUE(document, parseDocument(exactBytes));
            auto const* const p_id = document.find("$id");
            if (p_id == nullptr || p_id->kind() != json::ValueKind::String
                || p_id->string().empty())
            {
                return refuse(std::format(
                    "{} must declare its own absolute \"$id\"",
                    label
                ));
            }
            return std::string{p_id->string()};
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

        constexpr auto k_mutabilities = std::array{
            operator_runtime::ToolMutability::ReadOnly,
            operator_runtime::ToolMutability::Mutating,
        };

        constexpr auto k_surfaces = std::array{
            operator_runtime::ToolSurface::Semantic,
            operator_runtime::ToolSurface::Privileged,
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
            std::string                      argumentDefinition{};
            operator_runtime::ToolDescriptor descriptor{};
        };

        constexpr auto k_idempotencies = std::array{
            operator_runtime::ToolIdempotency::ReadSafe,
            operator_runtime::ToolIdempotency::DeliverySafe,
            operator_runtime::ToolIdempotency::KeyedExternal,
            operator_runtime::ToolIdempotency::NonIdempotent,
        };

        struct DeliveryClassName final
        {
            std::string_view                wire{};
            operator_runtime::DeliveryClass deliveryClass{};
        };

        constexpr auto k_deliveryClasses = std::array{
            DeliveryClassName{
                "delivery_safe",
                operator_runtime::DeliveryClass::DeliverySafe,
            },
            DeliveryClassName{
                "keyed_external",
                operator_runtime::DeliveryClass::KeyedExternal,
            },
            DeliveryClassName{
                "non_idempotent",
                operator_runtime::DeliveryClass::NonIdempotent,
            },
        };

        constexpr auto k_timeoutActions = std::array{
            operator_runtime::TimeoutAction::Reobserve,
            operator_runtime::TimeoutAction::Reconcile,
            operator_runtime::TimeoutAction::Stop,
        };

        [[nodiscard]]
        auto names(json::Value const& array) -> std::vector<std::string>
        {
            auto values = std::vector<std::string>{};
            for (auto const& item : array.items())
            {
                values.emplace_back(item.string());
            }
            return values;
        }

        // OP:`TimeoutPolicy`, which both step intents and every tool descriptor
        // carry. The schema has already bounded both members, so this reads
        // rather than judges.
        [[nodiscard]]
        auto readTimeoutPolicy(
            json::Value const& policy
        ) -> operator_runtime::TimeoutPolicy
        {
            auto const action = std::ranges::find(
                k_timeoutActions,
                member(policy, "on_timeout").string(),
                operator_runtime::timeoutActionWireName
            );
            UF_CHECK(action != k_timeoutActions.end());
            return operator_runtime::TimeoutPolicy{
                .maximumElapsedMillis = static_cast<uint64>(
                    member(policy, "maximum_elapsed_ms").number()
                ),
                .onTimeout = *action,
            };
        }

        // One payload schema and the identity it answers under: the sha256 of
        // its own exact bytes, which is what a journal manifest entry and an
        // OP:`EffectEnvelope` each name.
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
            if (schemas.empty())
            {
                return "nothing";
            }
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

        // One observed identity schema this deployment compiled, in the order
        // the deployment block named the documents. The registration pins the
        // same bytes by sha256, and ObservedInstanceIdentitySchemas::create
        // refuses any validator set that is not exactly that pinned set.
        struct IdentitySchema final
        {
            std::string  schemaId{};
            ContentHash  schemaHash;
            json::Schema schema;
        };
        std::vector<IdentitySchema> identitySchemas{};

        std::string requestDefinition{};
        std::string verdictDefinition{};
        std::string verdictMember{};

        [[nodiscard]] auto findTool(std::string_view name) const -> ToolEntry const*;

        [[nodiscard]]
        auto validateIdentityBasis(
            std::size_t index,
            json::Value const& basis
        ) const -> Status;

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

    auto ProjectDeployment::State::validateIdentityBasis(
        std::size_t index,
        json::Value const& basis
    ) const -> Status
    {
        auto const& schema = identitySchemas[index].schema;
        return adopt(
            schema.validate(basis),
            std::format(
                "observed identity basis under {}",
                identitySchemas[index].schemaId
            )
        );
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
                    "an EffectEnvelope names payload schema {}, which this "
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

    auto canonicalJsonValidator() -> operator_runtime::CanonicalJsonValidator
    {
        return [](std::string_view exactJcs) -> Result<json::Value>
        {
            // json::requireExactCanonical stays the only statement of RFC 8785
            // exactness in the tree, and the value is then read out of the same
            // bytes it accepted. Reading it back is one parse this module would
            // rather not spend; removing it needs a json::parseExactCanonical,
            // which belongs to modules/json rather than here, and a second
            // exactness rule spelled locally would agree with that one by test
            // instead of by construction.
            UF_TRY(adopt(json::requireExactCanonical(exactJcs), "canonical bytes"));
            return parseDocument(exactJcs);
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
        auto const timeout = readTimeoutPolicy(member(document, "timeout_policy"));
        auto const* const p_action = document.find("action");
        if (p_action == nullptr)
        {
            return operator_runtime::StepIntentClaims{
                .stepKey = std::string{member(document, "step_key").string()},
                .timeout = timeout,
                .kind    = operator_runtime::StepKind::Wait,
            };
        }
        auto const deliveryClass = std::ranges::find(
            k_deliveryClasses,
            member(document, "delivery_class").string(),
            &DeliveryClassName::wire
        );
        UF_CHECK(deliveryClass != k_deliveryClasses.end());
        // canonical_parameters is re-serialized for the reason canonical_args
        // is in readPlanProposal: the document is its own RFC 8785 form, so a
        // member's canonical bytes are the bytes it occupied -- which is what
        // lets mintStep compare it member by member against the frozen plan's
        // arguments without either side being re-canonicalized on the way.
        return operator_runtime::StepIntentClaims{
            .stepKey    = std::string{member(document, "step_key").string()},
            .surfaceId  = std::string{member(*p_action, "surface_id").string()},
            .uiTargetId = std::string{member(*p_action, "ui_target_id").string()},
            .actionId   = std::string{member(*p_action, "action_id").string()},
            .canonicalParameters = json::canonicalBytes(
                member(*p_action, "canonical_parameters")
            ),
            .timeout       = timeout,
            .deliveryClass = deliveryClass->deliveryClass,
            .kind          = operator_runtime::StepKind::UiAction,
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

        auto const commonOnly      = std::array{common};
        auto const sharedDocuments = sharedSchemaDocuments();

        // A set that embeds a project document also has to resolve what that
        // document references, so the published fragments belong in every set
        // below and not only in the project's own compilations.
        auto const withShared =
            [&sharedDocuments](std::vector<json::Schema::Document> local)
        {
            local.insert(
                local.end(),
                sharedDocuments.begin(),
                sharedDocuments.end()
            );
            return local;
        };
        auto const withState = withShared({common, projectStateDocument});
        auto const withWorld = withShared({
            common,
            projectStateDocument,
            projectObservationDocument,
        });

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
            compile("project/state", sources.projectState, sharedDocuments)
        );
        UF_TRY_VALUE(
            projectObservation,
            compile(
                "project/observation",
                sources.projectObservation,
                sharedDocuments
            )
        );
        UF_TRY_VALUE(
            toolPrecondition,
            compile(
                "project/tool-precondition",
                sources.toolPrecondition,
                sharedDocuments
            )
        );
        UF_TRY_VALUE(
            reconcile,
            compile("project/reconcile", sources.reconcile, sharedDocuments)
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
            .identitySchemas       = {},
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
                compile(
                    std::format("journal payload {}", hash.hex()),
                    bytes,
                    sharedDocuments
                )
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
                compile(
                    std::format("effect payload {}", hash.hex()),
                    bytes,
                    sharedDocuments
                )
            );
            state->effectPayloadSchemas.emplace_back(PayloadSchema{
                .hash   = hash,
                .schema = std::move(schema),
            });
        }

        // The observed identity schemas, compiled under the same closed
        // keyword set as every other schema this deployment applies. Each is
        // compiled with the whole identity set around it and with nothing
        // else: one identity document may reference another by its $id, two
        // documents declaring the same $id are refused here rather than
        // surfacing as an authority with one key answered twice, and a $ref
        // that reaches past the registered set cannot compile. The framework
        // catalog is outside the resolution domain for exactly this reason:
        // a catalog schema's bytes are not pinned by the registration, so
        // editing one must not be able to change what the identity set
        // answers while every identity byte and project_registration_hash
        // stay unchanged.
        // The labels are owned strings the documents' views name: a view into
        // a formatted temporary would dangle before the compiles below read
        // it, and the label is what a refusal prints for the document.
        auto identityLabels = std::vector<std::string>{};
        identityLabels.reserve(sources.observedInstanceIdentitySchemas.size());
        for (auto const bytes : sources.observedInstanceIdentitySchemas)
        {
            UF_TRY_VALUE(hash, hashOf(bytes));
            identityLabels.emplace_back(
                std::format("observed identity {}", hash.hex())
            );
        }

        auto identityDocuments = std::vector<json::Schema::Document>{};
        identityDocuments.reserve(identityLabels.size());
        for (auto index = std::size_t{0}; index < identityLabels.size(); ++index)
        {
            identityDocuments.emplace_back(json::Schema::Document{
                .label      = identityLabels[index],
                .exactBytes = sources.observedInstanceIdentitySchemas[index],
            });
        }
        for (auto index = std::size_t{0}; index < identityDocuments.size(); ++index)
        {
            UF_TRY_VALUE(
                schemaId,
                identityIdOf(
                    identityDocuments[index].label,
                    identityDocuments[index].exactBytes
                )
            );
            auto around = std::vector<json::Schema::Document>{};
            around.reserve(identityDocuments.size() - 1U);
            for (auto other = std::size_t{0}; other < identityDocuments.size(); ++other)
            {
                if (other != index)
                {
                    around.emplace_back(identityDocuments[other]);
                }
            }
            UF_TRY_VALUE(
                schema,
                compile(
                    identityDocuments[index].label,
                    identityDocuments[index].exactBytes,
                    around
                )
            );
            UF_TRY_VALUE(hash, hashOf(identityDocuments[index].exactBytes));
            state->identitySchemas.emplace_back(State::IdentitySchema{
                .schemaId   = std::move(schemaId),
                .schemaHash = hash,
                .schema     = std::move(schema),
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

        // The effect payload schemas' only route into any digest. Nothing else
        // in a project names them: no member of ProjectRegistrationClaims pins
        // one and no manifest lists one, so without this member editing a byte
        // of a pinned effect payload schema would move no hash anywhere and the
        // first consequence would be a Plan refused much later
        // (project-as-data.md 2.2, 7.0 Q3). Naming them here puts their bytes
        // inside tool_catalog_hash and so inside project_registration_hash.
        //
        // Both directions, for the reason the journal manifest's pair states: a
        // digest naming bytes nobody supplied is a link that is only a
        // convention, and a schema no digest names is bytes inside no hash.
        for (auto const& declared : member(catalog, "effect_payload_sha256s").items())
        {
            auto const named = declared.string();
            auto const found = std::ranges::find_if(
                state->effectPayloadSchemas,
                [named](PayloadSchema const& candidate)
                {
                    return candidate.hash.hex() == named;
                }
            );
            if (found == state->effectPayloadSchemas.end())
            {
                return refuse(std::format(
                    "the Tool Catalog names effect payload schema {}, which "
                    "this deployment does not carry: its effect_payload_schemas "
                    "hash to {}",
                    named,
                    carriedDigests(state->effectPayloadSchemas)
                ));
            }
        }
        for (auto const& supplied : state->effectPayloadSchemas)
        {
            auto const named = std::ranges::any_of(
                member(catalog, "effect_payload_sha256s").items(),
                [&supplied](json::Value const& declared)
                {
                    return declared.string() == supplied.hash.hex();
                }
            );
            if (!named)
            {
                return refuse(std::format(
                    "this deployment supplies an effect payload schema hashing "
                    "to {}, which the Tool Catalog's effect_payload_sha256s "
                    "does not name",
                    supplied.hash.hex()
                ));
            }
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
                operator_runtime::toolMutabilityWireName
            );
            auto const surface = std::ranges::find(
                k_surfaces,
                member(tool, "surface").string(),
                operator_runtime::toolSurfaceWireName
            );
            auto const idempotency = std::ranges::find(
                k_idempotencies,
                member(tool, "idempotency").string(),
                operator_runtime::toolIdempotencyWireName
            );
            UF_CHECK(mutability != k_mutabilities.end());
            UF_CHECK(surface != k_surfaces.end());
            UF_CHECK(idempotency != k_idempotencies.end());

            auto bounds = std::vector<operator_runtime::EffectBound>{};
            for (auto const& bound : member(tool, "effect_bounds").items())
            {
                auto const named = member(bound, "payload_schema_hash").string();
                auto const carried = std::ranges::find_if(
                    state->effectPayloadSchemas,
                    [named](PayloadSchema const& candidate)
                    {
                        return candidate.hash.hex() == named;
                    }
                );
                if (carried == state->effectPayloadSchemas.end())
                {
                    // A bound naming bytes nobody supplied would admit an
                    // effect whose payload nothing could judge, so the join is
                    // made where the bound is read rather than where the
                    // effect arrives.
                    return refuse(std::format(
                        "the Tool Catalog bounds an effect to payload schema "
                        "{}, which this deployment does not carry: its "
                        "effect_payload_schemas hash to {}",
                        named,
                        carriedDigests(state->effectPayloadSchemas)
                    ));
                }
                auto const risk = std::ranges::find(
                    k_risks,
                    member(bound, "maximum_risk").string(),
                    operator_runtime::riskWireName
                );
                UF_CHECK(risk != k_risks.end());
                bounds.emplace_back(operator_runtime::EffectBound{
                    .namespacedType = std::string{
                        member(bound, "namespaced_type").string()
                    },
                    .scopeKind = std::string{member(bound, "scope_kind").string()},
                    .payloadSchemaHash = carried->hash,
                    .maximumRisk       = *risk,
                });
            }

            auto const& declaredLimits = member(tool, "workflow_limits");
            state->tools.emplace_back(ToolEntry{
                .name               = std::string{member(tool, "name").string()},
                .argumentDefinition = std::string{definition},
                .descriptor         = operator_runtime::ToolDescriptor{
                    .toolVersion = std::string{member(tool, "version").string()},
                    .requiredCapabilities = names(
                        member(tool, "required_capabilities")
                    ),
                    .effectBounds   = std::move(bounds),
                    .uiActionBounds = names(member(tool, "ui_action_bounds")),
                    .limits         = operator_runtime::WorkflowLimits{
                        .maximumSteps = static_cast<uint32>(
                            member(declaredLimits, "maximum_steps").number()
                        ),
                        .maximumDispatches = static_cast<uint32>(
                            member(declaredLimits, "maximum_dispatches").number()
                        ),
                        .maximumObservations = static_cast<uint32>(
                            member(declaredLimits, "maximum_observations").number()
                        ),
                        .maximumWaits = static_cast<uint32>(
                            member(declaredLimits, "maximum_waits").number()
                        ),
                        .maximumElapsedMillis = static_cast<uint64>(
                            member(declaredLimits, "maximum_elapsed_ms").number()
                        ),
                    },
                    .timeout     = readTimeoutPolicy(member(tool, "timeout_policy")),
                    .mutability  = *mutability,
                    .surface     = *surface,
                    .idempotency = *idempotency,
                },
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
        return p_tool->descriptor;
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

    auto ProjectDeployment::toolCatalogReader() const
        -> operator_runtime::ToolCatalogReader
    {
        return [p_state = m_state]()
                   -> Result<std::vector<operator_runtime::ToolCatalogEntry>>
        {
            auto entries = std::vector<operator_runtime::ToolCatalogEntry>{};
            entries.reserve(p_state->tools.size());
            for (auto const& tool : p_state->tools)
            {
                entries.emplace_back(operator_runtime::ToolCatalogEntry{
                    .name       = tool.name,
                    .descriptor = tool.descriptor,
                });
            }
            return entries;
        };
    }

    auto ProjectDeployment::toolArgumentValidator() const
        -> operator_runtime::ToolArgumentValidator
    {
        return [p_state = m_state](
                   std::string_view toolName,
                   std::string_view exactArgsJcs
               ) -> Status
        {
            UF_TRY_VALUE(arguments, parseDocument(exactArgsJcs));
            return p_state->validateToolArguments(toolName, arguments);
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

    auto ProjectDeployment::observedIdentitySchemas() const
        -> std::vector<operator_runtime::ObservedInstanceIdentitySchema>
    {
        auto bindings = std::vector<operator_runtime::ObservedInstanceIdentitySchema>{};
        bindings.reserve(m_state->identitySchemas.size());
        for (auto index = std::size_t{0}; index < m_state->identitySchemas.size(); ++index)
        {
            bindings.emplace_back(operator_runtime::ObservedInstanceIdentitySchema{
                .schemaId   = m_state->identitySchemas[index].schemaId,
                .schemaHash = m_state->identitySchemas[index].schemaHash,
                .validate   = [p_state = m_state, index](json::Value const& basis) -> Status
                {
                    return p_state->validateIdentityBasis(index, basis);
                },
            });
        }
        return bindings;
    }
}
