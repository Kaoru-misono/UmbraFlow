-- The `foreign` deployment's tool closure. It declares the same ten entries as
-- the `alpha` one because both deployments pin catalogs built from the same
-- local names; what makes them different Tools is the plugin id each name sits
-- under, and that is stated once, here and in the registration.
--
-- Every Tool of this catalog declares `FixtureResult`, so every entry answers
-- one, naming the Tool that ran.
local plugin_id = "fixture.foreign"

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
