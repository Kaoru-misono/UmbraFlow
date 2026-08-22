local schema = "ca425bcb531fb9c9631ddbc15bb2cba2ef7133c91efd85dd112bc968e5976953"
local function effect(scope_key, risk)
    return {
        namespaced_type = "fixture.write",
        opaque_project_payload = { value = 1 },
        payload_schema_hash = schema,
        risk = risk,
        scope_key = scope_key,
        scope_kind = "instance",
    }
end
local ordinary = { effect("alpha", "low"), effect("beta", "medium") }
local reordered = { effect("beta", "medium"), effect("alpha", "low") }
local risky = { effect("alpha", "high") }
local function proposal(tool_name, effects, steps, dispatches)
    return {
        allowed_ui_actions = { "fixture.step" },
        canonical_args = { value = 1 },
        effects = effects,
        tool_name = tool_name,
        tool_version = "1",
        workflow_limits = {
            maximum_dispatches = dispatches,
            maximum_elapsed_ms = 60000,
            maximum_observations = 16,
            maximum_steps = steps,
            maximum_waits = 4,
        },
    }
end
-- Every Tool this deployment declares is named inside the namespace its
-- plugin_id registers, so the plans are keyed by the same full names the Tool
-- Catalog carries rather than by a bare local half no catalog could declare.
local ns = "fixture.foreign"
local proposals = {
    [ns .. ".command-1"] = proposal(ns .. ".command-1", ordinary, 8, 8),
    [ns .. ".command-2"] = proposal(ns .. ".command-2", ordinary, 8, 8),
    [ns .. ".different-command"] = proposal(ns .. ".different-command", ordinary, 8, 8),
    -- The proposal names the tool the Operation was NOT created for.
    [ns .. ".mismatched-plan"] = proposal(ns .. ".command-1", ordinary, 8, 8),
    [ns .. ".oversized-plan"] = proposal(ns .. ".oversized-plan", ordinary, 4096, 4096),
    [ns .. ".two-step-plan"] = proposal(ns .. ".two-step-plan", ordinary, 2, 2),
    [ns .. ".approval-plan"] = proposal(ns .. ".approval-plan", risky, 8, 8),
    [ns .. ".reordered-effects"] = proposal(ns .. ".reordered-effects", reordered, 8, 8),
    [ns .. ".observe-1"] = proposal(ns .. ".observe-1", ordinary, 8, 8),
    [ns .. ".raw-coordinate-click"] = proposal(ns .. ".raw-coordinate-click", ordinary, 8, 8),
}
return {
    plugin_id = "fixture.foreign",
    -- The same observed instance its deployment's identity schema admits, so
    -- the foreign role can only ever mint a step against an instance of its
    -- own registration: the U2c gate resolves ui_target_id through the
    -- session's binding, which no other registration minted.
    derive = function(_input)
        return {
            schema = "umbraflow-project-observation-proposal/v1",
            canonical_opaque_payload = {},
            project_tool_preconditions = {},
            observed_instance_proposals = {
                {
                    local_ref = "fixture.target",
                    kind = "fixture.control",
                    identity_schema_id = "https://fixture.example/identity/overlay/v1",
                    semantic_identity_basis = {
                        native_id = "fixture.target",
                        surface_epoch = 1,
                    },
                    opaque_project_payload = {},
                },
            },
        }
    end,
    plan = function(input)
        local proposal = proposals[input.tool_name]
        if proposal == nil then
            error("fixture plugin has no plan for " .. tostring(input.tool_name))
        end
        return proposal
    end,
    next_step = function(input)
        return {
            action = {
                action_id = "fixture.press",
                canonical_parameters = { value = 1 },
                surface_id = "fixture.surface",
                ui_target_id = input.project_observation.observed_instances[1].observed_instance_id,
            },
            binding_variant_constraints = {},
            delivery_class = "delivery_safe",
            expected_ui_postconditions = {},
            required_ui_preconditions = {},
            step_key = "fixture.step",
            timeout_policy = { maximum_elapsed_ms = 5000, on_timeout = "reobserve" },
        }
    end,
    reconcile = function(input) return input end,
    reduce = function(input)
        for _, event in ipairs(input.journal_events) do
            if event.namespaced_event_type == "fixture.confirmed" then
                return { revision = 1 }
            end
        end
        return { revision = 0 }
    end,
}
