local plans = require("./plans")
local reduce = require("./state/reducer")
local map = resource.readJson("map")
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
            canonical_opaque_payload = { regions = map.regions },
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
    reduce = reduce,
}
