local schema = "0acb39b8ba6f60a152caf4890116f300b5cd39a954de1fc433c0116d999c215a"
local function effect(risk, camp)
    return {
        namespaced_type = "expedition.march",
        opaque_project_payload = { turn = 0 },
        payload_schema_hash = schema,
        risk = risk,
        scope_key = camp,
        scope_kind = "camp",
    }
end
local ordinary = { effect("low", "north"), effect("medium", "south") }
local risky = { effect("high", "north") }
local function proposal(tool, effects, steps, dispatches)
    return {
        allowed_ui_actions = { "expedition.step" },
        canonical_args = { steps = 2 },
        effects = effects,
        tool_name = tool,
        tool_version = "3",
        workflow_limits = {
            maximum_dispatches = dispatches,
            maximum_elapsed_ms = 60000,
            maximum_observations = 16,
            maximum_steps = steps,
            maximum_waits = 4,
        },
    }
end
local plans = {
    ["expedition.move"] = proposal("expedition.move", ordinary, 8, 8),
    ["expedition.trade"] = proposal("expedition.trade", ordinary, 8, 8),
    ["expedition.approval"] = proposal("expedition.approval", risky, 8, 8),
}
local verdicts = {
    advanced = "underway",
    arrived = "settled",
    blocked = "refused",
}
return {
    plugin_id = "arcana.rival",
    -- The same observed instance its deployment's identity schema admits, so
    -- the rival role can only ever mint a step against an instance of its own
    -- registration: the U2c gate resolves ui_target_id through the session's
    -- binding, which no other registration minted.
    derive = function(_input)
        return {
            schema = "umbraflow-project-observation-proposal/v1",
            canonical_opaque_payload = {},
            project_tool_preconditions = {},
            observed_instance_proposals = {
                {
                    local_ref = "expedition.target",
                    kind = "expedition.control",
                    identity_schema_id = "https://arcana.example/identity/expedition/v1",
                    semantic_identity_basis = {
                        native_id = "expedition.target",
                        surface_epoch = 1,
                    },
                    opaque_project_payload = {},
                },
            },
        }
    end,
    plan = function(input)
        local answer = plans[input.tool_name]
        if answer == nil then
            error("expedition has no plan for " .. tostring(input.tool_name))
        end
        return answer
    end,
    next_step = function(input)
        return {
            action = {
                action_id = "expedition.press",
                canonical_parameters = { steps = 2 },
                surface_id = "expedition.surface",
                ui_target_id = input.project_observation.observed_instances[1].observed_instance_id,
            },
            binding_variant_constraints = {},
            delivery_class = "delivery_safe",
            expected_ui_postconditions = {},
            required_ui_preconditions = {},
            step_key = "expedition.step",
            timeout_policy = { maximum_elapsed_ms = 5000, on_timeout = "reobserve" },
        }
    end,
    reconcile = function(input)
        local verdict = verdicts[input.observed]
        if verdict == nil then
            return { verdict = "unclear" }
        end
        return { verdict = verdict }
    end,
    reduce = function(_input) return { turn = 0 } end,
}
