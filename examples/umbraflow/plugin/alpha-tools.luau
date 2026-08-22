-- The `alpha` deployment's tool closure: one entry per Tool its Tool Catalog
-- declares, joined to that Tool by the deployment's `tool_bindings`. The entry
-- name is the Tool's local name with `-` written `_`, because a closure entry
-- is a Luau field name and a Tool name is not.
--
-- Every Tool of this catalog declares `FixtureResult`, so every entry answers
-- one. The outcome names the Tool that ran, which is the only way a dispatch
-- is observable in a project whose whole state is one counter.
local plugin_id = "fixture.alpha"

local function answering(outcome)
    return function(_arguments)
        return { outcome = outcome }
    end
end

return {
    plugin_id = plugin_id,
    approval_plan = answering("approval-plan"),
    command_1 = answering("command-1"),
    command_2 = answering("command-2"),
    different_command = answering("different-command"),
    mismatched_plan = answering("mismatched-plan"),
    observe_1 = answering("observe-1"),
    oversized_plan = answering("oversized-plan"),
    raw_coordinate_click = answering("raw-coordinate-click"),
    reordered_effects = answering("reordered-effects"),
    two_step_plan = answering("two-step-plan"),
}
