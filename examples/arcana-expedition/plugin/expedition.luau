local schema = "0acb39b8ba6f60a152caf4890116f300b5cd39a954de1fc433c0116d999c215a"
local function effect(risk, camp)
    return '{"namespaced_type":"expedition.march","opaque_project_payload":{"turn":0}'
        .. ',"payload_schema_hash":"' .. schema .. '","risk":"' .. risk
        .. '","scope_key":"' .. camp .. '","scope_kind":"camp"}'
end
local ordinary = '[' .. effect("low", "north") .. ',' .. effect("medium", "south") .. ']'
local risky = '[' .. effect("high", "north") .. ']'
local function proposal(tool, effects, steps, dispatches)
    return '{"allowed_ui_actions":["expedition.step"],"canonical_args":{"steps":2}'
        .. ',"effects":' .. effects
        .. ',"tool_name":"' .. tool .. '","tool_version":"3"'
        .. ',"workflow_limits":{"maximum_dispatches":' .. dispatches
        .. ',"maximum_elapsed_ms":60000,"maximum_observations":16,"maximum_steps":'
        .. steps .. ',"maximum_waits":4}}'
end
local plans = {
    ["expedition.move"] = proposal("expedition.move", ordinary, "8", "8"),
    ["expedition.trade"] = proposal("expedition.trade", ordinary, "8", "8"),
    ["expedition.approval"] = proposal("expedition.approval", risky, "8", "8"),
}
local step_intent = '{"action":{"action_id":"expedition.press"'
    .. ',"canonical_parameters":{"steps":2},"surface_id":"expedition.surface"'
    .. ',"ui_target_id":"expedition.target"},"binding_variant_constraints":[]'
    .. ',"delivery_class":"delivery_safe","expected_ui_postconditions":[]'
    .. ',"required_ui_preconditions":[],"step_key":"expedition.step"'
    .. ',"timeout_policy":{"maximum_elapsed_ms":5000,"on_timeout":"reobserve"}}'
return {
    plugin_id = "arcana.expedition",
    derive = function(_input) return '{"visible":true}' end,
    plan = function(input)
        local tool = string.match(input, '"tool_name":"([^"]*)"')
        local answer = plans[tool]
        if answer == nil then
            error("expedition has no plan for " .. tostring(tool))
        end
        return answer
    end,
    next_step = function(_input) return step_intent end,
    reconcile = function(input)
        if input == '{"observed":"advanced"}' then return '{"verdict":"underway"}' end
        if input == '{"observed":"arrived"}' then return '{"verdict":"settled"}' end
        if input == '{"observed":"blocked"}' then return '{"verdict":"refused"}' end
        return '{"verdict":"unclear"}'
    end,
    reduce = function(_input) return '{"turn":0}' end,
}
