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

-- A Tool's namespace is its registration's, so the same three plans belong to
-- two disjoint sets of names: the expedition answers for `arcana.expedition.*`
-- and the rival for `arcana.rival.*`. The plans are composed per namespace
-- rather than written out twice, and each deployment's entry module picks the
-- set its own plugin_id owns.
local function plans_for(namespace)
    return {
        [namespace .. ".move"] = proposal(namespace .. ".move", ordinary, 8, 8),
        [namespace .. ".trade"] = proposal(namespace .. ".trade", ordinary, 8, 8),
        [namespace .. ".approval"] = proposal(namespace .. ".approval", risky, 8, 8),
    }
end

return {
    ["arcana.expedition"] = plans_for("arcana.expedition"),
    ["arcana.rival"] = plans_for("arcana.rival"),
}
