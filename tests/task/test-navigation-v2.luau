--!strict
-- Contract tests for P4 Navigation. The host test loader provides the trusted
-- framework exports as globals; this file intentionally uses no legacy model
-- names and no screenshot source.

local function state(context: string, surfaces: { string }): any
    return { context = context, surfaces = surfaces }
end

local function model(): any
    local declared = navigation.DeclaredTransition.new({
        id = "camp_to_training_confirm",
        from = state("camp", { "camp" }),
        trigger = { kind = "action", binding = "training_button_binding", action = "click" },
        to = state("camp", { "camp", "training_confirm" }),
        verification = { kind = "resolve", timeout_ms = 2000, attempts = 3 },
    })
    return {
        contexts = { { id = "camp" } },
        surfaces = {
            { id = "camp", kind = "scene", contexts = { "camp" } },
            {
                id = "training_confirm",
                kind = "overlay",
                contexts = { "camp" },
                covers = { "camp" },
            },
        },
        bindings = {
            {
                id = "training_button_binding",
                target = "training_button",
                actions = { { id = "click", kind = "click" } },
            },
        },
        transitions = { declared },
    }
end

local function run()
    local runtime_model = model()
    local declared = runtime_model.transitions[1]
    local observed = navigation.ObservedTransition.new({
        id = "observation-001",
        from = state("camp", { "camp" }),
        trigger = { kind = "action", binding = "training_button_binding", action = "click" },
        to = state("camp", { "camp", "training_confirm" }),
        frames = { "frame.transition.before", "frame.transition.after" },
    })

    assert(navigation.DeclaredTransition.is(declared))
    assert(navigation.ObservedTransition.is(observed))
    assert(not navigation.DeclaredTransition.is(observed))
    assert(not navigation.ObservedTransition.is(declared))
    assert(navigation.state_equal(declared.from, observed.from))
    assert(navigation.state_equal(declared.to, observed.to))
    assert(navigation.validate_surface_stack(runtime_model, observed.from))
    assert(navigation.validate_surface_stack(runtime_model, observed.to))
    assert(navigation.transition_trigger_matches(runtime_model, declared, observed.trigger))

    return true
end

return { run = run }
