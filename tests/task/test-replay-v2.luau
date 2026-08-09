--!strict
-- Contract tests for RuntimeState/Surface-stack replay.

local function state(context: string, surfaces: { string }): any
    return { context = context, surfaces = surfaces }
end

local function runtime_model(): any
    local declared = navigation.DeclaredTransition.new({
        id = "camp_to_training_confirm",
        from = state("camp", { "camp" }),
        trigger = { kind = "action", binding = "training_button_binding", action = "click" },
        to = state("camp", { "camp", "training_confirm" }),
        verification = { kind = "resolve", timeout_ms = 2000, attempts = 3 },
    })
    return {
        surfaces = {
            { id = "camp", kind = "scene", contexts = { "camp" } },
            {
                id = "training_confirm",
                kind = "overlay",
                contexts = { "camp" },
                covers = { "camp" },
            },
        },
        transitions = { declared },
    }
end

local function observed(id: string, to: any): any
    return navigation.ObservedTransition.new({
        id = id,
        from = state("camp", { "camp" }),
        trigger = { kind = "action", binding = "training_button_binding", action = "click" },
        to = to,
        frames = { "before", "after" },
    })
end

local function run()
    local model = runtime_model()
    local matched = replay.check(
        model,
        { observed("observed.matched", state("camp", { "camp", "training_confirm" })) }
    )
    assert(matched.accepted)
    assert(matched.matched_count == 1)
    assert(matched.results[1].status == "matched")
    assert(#matched.findings == 0)

    local mismatch = replay.check(
        model,
        { observed("observed.unexpected", state("camp", { "camp" })) }
    )
    assert(not mismatch.accepted)
    assert(mismatch.findings[1].kind == "unexpected_destination")
    assert(mismatch.findings[1].detail == "observed_destination_differs_from_declared_destination")

    local unknown = replay.check(
        model,
        { observed("observed.unknown", { kind = "unknown", reason = "unmatched_visible_content" }) }
    )
    assert(not unknown.accepted)
    assert(unknown.findings[1].kind == "unknown_surface")

    local ambiguous = replay.check(
        model,
        { observed("observed.ambiguous", { kind = "ambiguous", candidates = { "shop", "equip" } }) }
    )
    assert(not ambiguous.accepted)
    assert(ambiguous.findings[1].kind == "ambiguous_surface")

    return true
end

return { run = run }
