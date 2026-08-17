--!strict
-- Surface resolution and same-cycle Binding resolution are separate.

local function detector(locator: string): any
    return {
        all = { { kind = "locator_present", locator = locator } },
        any = {},
        none = {},
    }
end

local function variants(name: string, value: any): any
    return { { name = name, detector = value } }
end

local function placement(x: number, y: number, actionable: boolean): any
    return {
        kind = "fixed",
        rect = { x, y, 120, 50 },
        action_point = if actionable then { x + 60, y + 25 } else nil,
    }
end

local runtime = model.compile({
    schema_version = 3,
    base_resolution = { 1280, 720 },
    base_dpi = { 96, 96 },
    ui_targets = {
        { id = "scene_anchor", kind = "region" },
        { id = "other_anchor", kind = "region" },
        { id = "dialog_anchor", kind = "region" },
        { id = "save", kind = "control" },
        { id = "retry", kind = "control" },
        { id = "leave", kind = "control" },
    },
    locators = {
        { id = "scene.locator", kind = "template", asset_path = "assets/scene.png", threshold = 0.9 },
        { id = "other.locator", kind = "template", asset_path = "assets/other.png", threshold = 0.9 },
        { id = "dialog.locator", kind = "template", asset_path = "assets/dialog.png", threshold = 0.9 },
        { id = "save.primary", kind = "template", asset_path = "assets/save-primary.png", threshold = 0.9 },
        { id = "save.alternate", kind = "template", asset_path = "assets/save-alternate.png", threshold = 0.9 },
        { id = "retry.locator", kind = "template", asset_path = "assets/retry.png", threshold = 0.9 },
        { id = "leave.scene", kind = "template", asset_path = "assets/leave-scene.png", threshold = 0.9 },
        { id = "leave.other", kind = "template", asset_path = "assets/leave-other.png", threshold = 0.9 },
    },
    readers = {},
    bindings = {
        { id = "scene.identity", surface = "scene", ui_target = "scene_anchor", placement = placement(10, 10, false), variants = variants("default", detector("scene.locator")), actions = {} },
        { id = "other.identity", surface = "other", ui_target = "other_anchor", placement = placement(10, 10, false), variants = variants("default", detector("other.locator")), actions = {} },
        { id = "dialog.identity", surface = "dialog", ui_target = "dialog_anchor", placement = placement(400, 100, false), variants = variants("default", detector("dialog.locator")), actions = {} },
        {
            id = "save.primary", surface = "dialog", ui_target = "save",
            placement = placement(800, 600, true),
            variants = {
                { name = "primary", detector = detector("save.primary") },
                {
                    name = "alternate",
                    detector = {
                        all = {
                            { kind = "locator_present", locator = "save.primary" },
                            { kind = "locator_present", locator = "save.alternate" },
                        },
                        any = {},
                        none = {},
                    },
                },
            },
            actions = { { id = "activate", kind = "click", proof_locator = "save.primary" } },
        },
        { id = "retry.enabled", surface = "network_retry", ui_target = "retry", placement = placement(560, 500, true), variants = variants("enabled", detector("retry.locator")), actions = { { id = "retry", kind = "click", proof_locator = "retry.locator" } } },
        { id = "leave.scene", surface = "scene", ui_target = "leave", placement = placement(100, 600, true), variants = variants("default", detector("leave.scene")), actions = { { id = "leave", kind = "click", proof_locator = "leave.scene" } } },
        { id = "leave.other", surface = "other", ui_target = "leave", placement = placement(900, 600, true), variants = variants("default", detector("leave.other")), actions = { { id = "leave", kind = "click", proof_locator = "leave.other" } } },
    },
    collections = {},
    surfaces = {
        { id = "scene", kind = "scene", covers = {}, identity = { "scene.identity" } },
        { id = "other", kind = "scene", covers = {}, identity = { "other.identity" } },
        { id = "dialog", kind = "overlay", covers = { "scene" }, identity = { "dialog.identity" } },
        { id = "network_retry", kind = "interrupt", covers = { "scene", "other" }, identity = { "retry.enabled" } },
    },
    transitions = {
        {
            id = "dialog.saved",
            from_surfaces = { "scene", "dialog" },
            trigger = { binding = "save.primary", action = "activate" },
            to_surfaces = { "scene" },
        },
    },
})

-- T-003 / T11-equivalent: the offline observation is a real mismatch. Its
-- destination remains in the comparison while the declared policy and its
-- canonical model bytes survive unchanged.
do
    local declaredBytes = model.canonical_bytes(runtime)
    local comparison, reason = resolution.compare_transition(runtime, {
        kind = "observed_transition",
        transition = "dialog.saved",
        observed_to_surfaces = { "other" },
    })
    assert(
        comparison ~= nil and reason == nil,
        "T-003 a valid offline transition observation must produce a comparison"
    )
    local report = comparison :: any
    assert(
        report.matches_policy == false
            and report.declared_to_surfaces[1] == "scene"
            and report.observed_to_surfaces[1] == "other"
            and report.declared_to_surfaces ~= runtime.transition_by_id["dialog.saved"].to_surfaces
            and runtime.transition_by_id["dialog.saved"].to_surfaces[1] == "scene"
            and model.canonical_bytes(runtime) == declaredBytes,
        "T-003 an observed destination differing from policy must report the mismatch without rewriting the model"
    )

    local matching = resolution.compare_transition(runtime, {
        kind = "observed_transition",
        transition = "dialog.saved",
        observed_to_surfaces = { "scene" },
    })
    assert(
        matching ~= nil and matching.matches_policy == true,
        "T-003 an observed destination equal to policy must report a match"
    )
end

local function record(
    cycle: any,
    binding: string,
    locator: string,
    kind: string,
    hostProof: any?
): any
    local source = { kind = "template", binding = binding, locator = locator }
    if kind == "present" then
        return evidence.present(cycle, source, { locator = locator }, 0.99, hostProof)
    end
    if kind == "absent" then return evidence.absent(cycle, source) end
    return evidence.unknown(cycle, "locator_failed", source)
end

local function put(
    result: any,
    cycle: any,
    binding: string,
    locator: string,
    kind: string,
    hostProof: any?
)
    local predicate = { kind = "locator_present", locator = locator }
    result[model.measurement_key(binding, predicate)] = record(
        cycle,
        binding,
        locator,
        kind,
        hostProof
    )
end

local function stateRecords(cycle: any, scene: string, other: string, dialog: string): any
    local result = {}
    put(result, cycle, "scene.identity", "scene.locator", scene)
    put(result, cycle, "other.identity", "other.locator", other)
    put(result, cycle, "dialog.identity", "dialog.locator", dialog)
    put(result, cycle, "retry.enabled", "retry.locator", "absent")
    return result
end

-- T-001 / T03-equivalent: one declared interrupt, one Binding and one action
-- resolve over two different base scenes. The covers list carries compatibility;
-- no second annotation stands in for the same interrupt on the second base.
do
    local interrupt = runtime.surface_by_id.network_retry
    local outcomes = {}
    for _, base in {
        { id = "scene", scene = "present", other = "absent" },
        { id = "other", scene = "absent", other = "present" },
    } do
        local cycle = evidence.begin_cycle()
        local records = stateRecords(cycle, base.scene, base.other, "absent")
        local hostProof = table.freeze({ base = base.id })
        put(records, cycle, "retry.enabled", "retry.locator", "present", hostProof)
        local state = resolution.resolve_state(runtime, cycle, records)
        local resolved = state.kind == "resolved_state"
            and state.ordered_surface_stack[1] == base.id
            and state.ordered_surface_stack[2] == interrupt.id
        local binding = if resolved
            then resolution.resolve_binding(runtime, cycle, state, "retry", nil, records)
            else nil
        local request = nil
        local reason = "binding_not_resolved"
        if binding ~= nil and binding.kind == "resolved_binding" then
            request, reason = resolution.authorize(runtime, cycle, binding, "retry")
        end
        outcomes[base.id] = {
            interrupt = interrupt,
            binding = if binding == nil then nil else binding.binding,
            action = if request == nil then nil else request.action,
            authorized = request ~= nil and reason == nil,
        }
        evidence.end_cycle(cycle)
    end
    assert(
        outcomes.scene.interrupt == outcomes.other.interrupt
            and outcomes.scene.binding == "retry.enabled"
            and outcomes.other.binding == "retry.enabled"
            and outcomes.scene.action == "retry"
            and outcomes.other.action == "retry"
            and outcomes.scene.authorized
            and outcomes.other.authorized,
        "T-001 the same interrupt and action must resolve and authorize over two different base scenes"
    )
end

-- T-005: one API confirms the expected stack and owns escalation. A confirmed
-- expectation ignores an Unknown competitor; the same world without an
-- expectation therefore proves that the fast result was confirmation rather
-- than full resolution. An absent expectation then proves that failure runs
-- full resolution and returns the scene that is actually present.
do
    local confirmationCycle = evidence.begin_cycle()
    local confirmationRecords = stateRecords(
        confirmationCycle,
        "present",
        "unknown",
        "absent"
    )
    local confirmed = resolution.resolve_state(
        runtime,
        confirmationCycle,
        confirmationRecords,
        { "scene" }
    )
    local fullyResolved = resolution.resolve_state(
        runtime,
        confirmationCycle,
        confirmationRecords
    )

    local escalationCycle = evidence.begin_cycle()
    local escalationRecords = stateRecords(
        escalationCycle,
        "present",
        "absent",
        "absent"
    )
    local escalated = resolution.resolve_state(
        runtime,
        escalationCycle,
        escalationRecords,
        { "other" }
    )
    assert(
        confirmed.kind == "resolved_state"
            and confirmed.ordered_surface_stack[1] == "scene"
            and fullyResolved.kind == "unknown_state"
            and escalated.kind == "resolved_state"
            and escalated.ordered_surface_stack[1] == "scene",
        "T-005 resolve_state must confirm only the expected stack and escalate a failed confirmation to full resolution"
    )
    evidence.end_cycle(confirmationCycle)
    evidence.end_cycle(escalationCycle)
end

-- T-004 / T06: one semantic target is bound independently on two different
-- Surfaces. Each world resolves the same target and action through the Binding
-- belonging to its active Surface; no target ownership field participates.
do
    local outcomes = {}
    for _, world in {
        {
            surface = "scene",
            scene = "present",
            other = "absent",
            binding = "leave.scene",
            locator = "leave.scene",
        },
        {
            surface = "other",
            scene = "absent",
            other = "present",
            binding = "leave.other",
            locator = "leave.other",
        },
    } do
        local cycle = evidence.begin_cycle()
        local records = stateRecords(cycle, world.scene, world.other, "absent")
        local hostProof = table.freeze({ surface = world.surface })
        put(records, cycle, world.binding, world.locator, "present", hostProof)
        local state = resolution.resolve_state(runtime, cycle, records)
        local binding = resolution.resolve_binding(runtime, cycle, state, "leave", nil, records)
        local request, reason = resolution.authorize(runtime, cycle, binding, "leave")
        outcomes[world.surface] = {
            binding = binding.binding,
            target = binding.ui_target,
            action = if request == nil then nil else request.action,
            authorized = request ~= nil and reason == nil,
        }
        evidence.end_cycle(cycle)
    end
    assert(
        outcomes.scene.target == "leave"
            and outcomes.other.target == "leave"
            and outcomes.scene.binding == "leave.scene"
            and outcomes.other.binding == "leave.other"
            and outcomes.scene.action == "leave"
            and outcomes.other.action == "leave"
            and outcomes.scene.authorized
            and outcomes.other.authorized,
        "T-004 T06 the same shared target must resolve and authorize through bindings on two different Surfaces"
    )
end

do
    local cycle = evidence.begin_cycle()
    local records = stateRecords(cycle, "present", "absent", "present")
    put(records, cycle, "save.primary", "save.primary", "present")
    put(records, cycle, "save.primary", "save.alternate", "absent")
    local state = resolution.resolve_state(runtime, cycle, records)
    assert(state.kind == "resolved_state")
    assert(state.ordered_surface_stack[1] == "scene")
    assert(state.ordered_surface_stack[2] == "dialog")
    assert(state.binding == nil)
    local binding = resolution.resolve_binding(runtime, cycle, state, "save", nil, records)
    assert(binding.kind == "resolved_binding")
    assert(binding.binding == "save.primary")
    assert(binding.variant == "primary")
    assert(binding.placement == nil)
    evidence.end_cycle(cycle)
end

-- Geometry with no measurement remains Unknown; it never manufactures Present.
do
    local cycle = evidence.begin_cycle()
    local state = resolution.resolve_state(runtime, cycle, {})
    assert(state.kind == "unknown_state")
    evidence.end_cycle(cycle)
end

-- A positive scene cannot outrank an Unknown scene competitor.
do
    local cycle = evidence.begin_cycle()
    local state = resolution.resolve_state(runtime, cycle, stateRecords(cycle, "present", "unknown", "absent"))
    assert(state.kind == "unknown_state")
    assert(state.reason == "unknown_scene_competitor")
    evidence.end_cycle(cycle)
end

-- An applicable Unknown layer blocks a known lower state.
do
    local cycle = evidence.begin_cycle()
    local state = resolution.resolve_state(runtime, cycle, stateRecords(cycle, "present", "absent", "unknown"))
    assert(state.kind == "unknown_state")
    assert(state.reason == "unknown_layer_competitor")
    evidence.end_cycle(cycle)
end

-- Two positive scenes are ambiguous, not declaration-order selection.
do
    local cycle = evidence.begin_cycle()
    local state = resolution.resolve_state(runtime, cycle, stateRecords(cycle, "present", "present", "absent"))
    assert(state.kind == "ambiguous_state")
    assert(#state.candidates == 2)
    evidence.end_cycle(cycle)
end

-- One satisfied variant cannot outrank an Unknown competitor in its Binding.
do
    local cycle = evidence.begin_cycle()
    local records = stateRecords(cycle, "present", "absent", "present")
    put(records, cycle, "save.primary", "save.primary", "present")
    put(records, cycle, "save.primary", "save.alternate", "unknown")
    local state = resolution.resolve_state(runtime, cycle, records)
    local binding = resolution.resolve_binding(runtime, cycle, state, "save", "save.primary", records)
    assert(binding.kind == "unknown_binding")
    assert(binding.reason == "unknown_competitor")
    evidence.end_cycle(cycle)
end

-- Two satisfied variants inside one Binding are reported by the existing
-- ambiguous_binding shape. Declaration order never selects one silently.
do
    local cycle = evidence.begin_cycle()
    local records = stateRecords(cycle, "present", "absent", "present")
    put(records, cycle, "save.primary", "save.primary", "present")
    put(records, cycle, "save.primary", "save.alternate", "present")
    local state = resolution.resolve_state(runtime, cycle, records)
    local binding = resolution.resolve_binding(runtime, cycle, state, "save", nil, records)
    assert(
        binding.kind == "ambiguous_binding"
            and binding.candidates[1] == "save.primary:primary"
            and binding.candidates[2] == "save.primary:alternate",
        "two satisfied variants in one Binding must report ambiguous_binding"
    )
    evidence.end_cycle(cycle)
end

-- Reported readings: what a resolved state says it read, and what it will not.
local reading = model.compile({
    schema_version = 3,
    base_resolution = { 1280, 720 },
    base_dpi = { 96, 96 },
    ui_targets = {
        { id = "scene_anchor", kind = "region" },
        { id = "title", kind = "region" },
        { id = "count", kind = "region" },
        { id = "dialog_anchor", kind = "region" },
        { id = "dialog_title", kind = "region" },
    },
    locators = {
        { id = "scene.locator", kind = "template", asset_path = "assets/scene.png", threshold = 0.9 },
        { id = "title.locator", kind = "template", asset_path = "assets/title.png", threshold = 0.9 },
        { id = "title.alt.locator", kind = "template", asset_path = "assets/title-alt.png", threshold = 0.9 },
        { id = "count.locator", kind = "template", asset_path = "assets/count.png", threshold = 0.9 },
        { id = "dialog.locator", kind = "template", asset_path = "assets/dialog.png", threshold = 0.9 },
        { id = "dialog.title.locator", kind = "template", asset_path = "assets/dialog-title.png", threshold = 0.9 },
    },
    readers = {
        { id = "name.reader", kind = "text", confidence_floor = 0.5, layout = "single_line", normalization = "collapse_whitespace" },
        { id = "flavour.reader", kind = "text", confidence_floor = 0.5, layout = "block", normalization = "trim" },
        { id = "count.reader", kind = "text", confidence_floor = 0.5, layout = "single_line", normalization = "trim" },
    },
    bindings = {
        {
            id = "scene.identity", surface = "scene", ui_target = "scene_anchor",
            placement = placement(10, 10, false),
            variants = variants("default", detector("scene.locator")), actions = {},
        },
        {
            id = "title.primary", surface = "scene", ui_target = "title",
            placement = placement(100, 10, false),
            variants = {
                { name = "primary", detector = detector("title.locator") },
                { name = "alternate", detector = detector("title.alt.locator") },
            },
            actions = {}, reads = { "name.reader", "flavour.reader" },
        },
        {
            id = "count.primary", surface = "scene", ui_target = "count",
            placement = placement(300, 10, false),
            variants = variants("primary", detector("count.locator")), actions = {},
            reads = { "count.reader" },
        },
        -- On a Surface the stack below never contains. Its own detector is
        -- present, so only the stack keeps its reading out of the report.
        {
            id = "dialog.title", surface = "dialog", ui_target = "dialog_title",
            placement = placement(400, 10, false),
            variants = variants("primary", detector("dialog.title.locator")), actions = {},
            reads = { "name.reader" },
        },
        {
            id = "dialog.identity", surface = "dialog", ui_target = "dialog_anchor",
            placement = placement(500, 10, false),
            variants = variants("default", detector("dialog.locator")), actions = {},
        },
    },
    collections = {
        {
            id = "options",
            surface = "scene",
            placement = {
                kind = "detected",
                search_rect = { 100, 80, 1080, 160 },
                reader = "flavour.reader",
                order = "left_to_right",
                slots = { origin = 631, pitch = 316, tolerance = 2 },
            },
            actions = {
                {
                    id = "pick",
                    kind = "click",
                    proof_locator = "title.locator",
                    offset = { 20, 10 },
                },
                {
                    id = "outside",
                    kind = "click",
                    proof_locator = "title.locator",
                    offset = { 200, 10 },
                },
            },
            reads = { { reader = "name.reader", offset = { 0, 50 }, size = { 240, 60 } } },
        },
        {
            id = "map.nodes",
            surface = "scene",
            placement = {
                kind = "detected",
                search_rect = { 100, 80, 1080, 160 },
                reader = "flavour.reader",
                order = "left_to_right",
                slots = { origin = 631, pitch = 316, tolerance = 2 },
            },
            actions = {},
            reads = { { reader = "name.reader", offset = { 0, 50 }, size = { 240, 60 } } },
            predicate = { kind = "text_equals", reader = "name.reader", value = "wanted" },
        },
    },
    stitched_maps = {
        {
            id = "route",
            collection = "map.nodes",
            colour_key = { red = 184, green = 92, blue = 31, tolerance = 12 },
            minimum_ratio = 0.75,
        },
    },
    surfaces = {
        { id = "scene", kind = "scene", covers = {}, identity = { "scene.identity", "count.primary" } },
        {
            id = "dialog", kind = "overlay", covers = { "scene" },
            identity = { "dialog.identity" },
        },
    },
    transitions = {},
})

-- A Reader named in reads must be one the model declares.
assert(not pcall(function()
    return model.compile({
        schema_version = 3,
        base_resolution = { 1280, 720 },
        base_dpi = { 96, 96 },
        ui_targets = { { id = "scene_anchor", kind = "region" } },
        locators = {
            { id = "scene.locator", kind = "template", asset_path = "assets/scene.png", threshold = 0.9 },
        },
        readers = {},
        bindings = {
            {
                id = "scene.identity", surface = "scene", ui_target = "scene_anchor",
                placement = placement(10, 10, false),
                variants = variants("default", detector("scene.locator")), actions = {},
                reads = { "undeclared.reader" },
            },
        },
        surfaces = {
            { id = "scene", kind = "scene", covers = {}, identity = { "scene.identity" } },
        },
        transitions = {},
    })
end))

-- A Binding that declares nothing reports nothing, and is still a valid Binding.
assert(#reading.binding_by_id["scene.identity"].reads == 0)
assert(#reading.binding_by_id["title.primary"].reads == 2)

-- The dialog Surface is never on the stack these build: its identity Binding is
-- absent while the Binding that reads on it is present, which is the only shape
-- that can tell "readings come from the resolved stack" apart from "readings
-- come from every present Binding in the model".
local function readingRecords(cycle: any, title: string, alternate: string, count: string): any
    local result = {}
    put(result, cycle, "scene.identity", "scene.locator", "present")
    put(result, cycle, "title.primary", "title.locator", title)
    put(result, cycle, "title.primary", "title.alt.locator", alternate)
    put(result, cycle, "count.primary", "count.locator", count)
    put(result, cycle, "dialog.identity", "dialog.locator", "absent")
    put(result, cycle, "dialog.title", "dialog.title.locator", "present")
    return result
end

-- Surface identity is one conjunction. Satisfying only the first of two named
-- Bindings cannot resolve the Surface.
do
    local cycle = evidence.begin_cycle()
    local records = readingRecords(cycle, "absent", "absent", "absent")
    local state = resolution.resolve_state(reading, cycle, records)
    assert(
        state.kind == "unknown_state" and state.reason == "no_scene_candidate",
        "a Surface identity naming two Bindings must require both to be present"
    )
    evidence.end_cycle(cycle)
end

local function reads(pairs_: { { any } }): any
    local result = {}
    for _, row in pairs_ do
        result[model.reading_key(row[1], row[2])] = row[3]
    end
    return result
end

local function readValue(text: string): any
    return { kind = "read", lines = { { text = text, rect = { 4, 5, 60, 12 } } }, confidence = 0.91 }
end

-- What a block Reader answers: several lines, each with the rectangle the frame
-- held it in.
local function readBlock(lines: { { any } }): any
    local result = {}
    for index, row in lines do
        result[index] = { text = row[1], rect = row[2] }
    end
    return { kind = "read", lines = result, confidence = 0.91 }
end

-- The reported shape: ui_target, reader, kind and lines, sorted, and nothing else.
do
    local cycle = evidence.begin_cycle()
    local records = readingRecords(cycle, "present", "absent", "present")
    local state = resolution.resolve_state(reading, cycle, records)
    assert(state.kind == "resolved_state")
    assert(#state.ordered_surface_stack == 1 and state.ordered_surface_stack[1] == "scene")
    local reported = resolution.resolve_readings(reading, cycle, state, records, reads({
        { "title.primary", "name.reader", readValue("Wandering Merchant") },
        { "title.primary", "flavour.reader", readValue("A stranger blocks the road.") },
        { "count.primary", "count.reader", readValue("3") },
        -- Present, readable, and on a Surface the stack does not contain.
        { "dialog.title", "name.reader", readValue("Off-stack Dialog") },
    }))
    for _, row in reported do assert(row.ui_target ~= "dialog_title") end
    assert(#reported == 3)
    assert(reported[1].ui_target == "count" and reported[1].reader == "count.reader")
    assert(reported[1].kind == "read" and #reported[1].lines == 1)
    assert(reported[1].lines[1].text == "3")
    assert(reported[2].ui_target == "title" and reported[2].reader == "flavour.reader")
    assert(reported[3].ui_target == "title" and reported[3].reader == "name.reader")
    assert(reported[3].kind == "read" and reported[3].lines[1].text == "Wandering Merchant")

    -- No score, no binding, no variant, and no reason on a reading that
    -- succeeded: four keys and no fifth.
    local keys = 0
    for key in reported[3] do
        assert(key == "ui_target" or key == "reader" or key == "kind" or key == "lines")
        keys += 1
    end
    assert(keys == 4)
    assert(not pcall(function() (reported[3] :: any).lines = "forged" end))
    assert(not pcall(function() (reported :: any)[4] = {} end))
    evidence.end_cycle(cycle)
end

-- A block Reader's reading carries every line it read, in order, each with its
-- own rectangle: the whole point of the layout, and the shape a single-line
-- reading is one element of rather than a second spelling of.
do
    local cycle = evidence.begin_cycle()
    local records = readingRecords(cycle, "present", "absent", "present")
    local state = resolution.resolve_state(reading, cycle, records)
    local reported = resolution.resolve_readings(reading, cycle, state, records, reads({
        { "title.primary", "flavour.reader", readBlock({
            { "The road is blocked.", { 40, 100, 260, 22 } },
            { "A stranger waits.", { 40, 124, 210, 22 } },
        }) },
        { "count.primary", "count.reader", readValue("3") },
    }))
    assert(reported[2].reader == "flavour.reader" and reported[2].kind == "read")
    assert(#reported[2].lines == 2)
    assert(reported[2].lines[1].text == "The road is blocked.")
    assert(reported[2].lines[1].rect[1] == 40 and reported[2].lines[1].rect[2] == 100)
    assert(reported[2].lines[2].text == "A stranger waits.")
    assert(reported[2].lines[2].rect[2] == 124 and reported[2].lines[2].rect[4] == 22)
    evidence.end_cycle(cycle)
end

-- A Binding that is not measured present reads nothing, even with a value cached
-- for it: a Reader reads that Binding's rectangle, so an absent one reads pixels
-- belonging to whatever is there instead.
do
    local cycle = evidence.begin_cycle()
    local records = readingRecords(cycle, "absent", "absent", "present")
    local state = resolution.resolve_state(reading, cycle, records)
    local reported = resolution.resolve_readings(reading, cycle, state, records, reads({
        { "title.primary", "name.reader", readValue("Wandering Merchant") },
        { "count.primary", "count.reader", readValue("3") },
    }))
    assert(#reported == 1)
    assert(reported[1].ui_target == "count")
    evidence.end_cycle(cycle)
end

-- Below the floor, empty, or never taken: reported with the outcome and, for an
-- Unknown, the reason -- and never with the text or the score that decided it.
do
    local cycle = evidence.begin_cycle()
    local records = readingRecords(cycle, "present", "absent", "present")
    local state = resolution.resolve_state(reading, cycle, records)
    local reported = resolution.resolve_readings(reading, cycle, state, records, reads({
        -- Carries the text it rejected and the score that rejected it, so what
        -- keeps both out of the report is the kind and not their absence. The
        -- floor is the trusted Reader's judgement, and a caller handed the
        -- rejected text could re-take it; the reason says only that it failed.
        { "title.primary", "name.reader", {
            kind = "unknown",
            reason = "low_confidence",
            confidence = 0.2,
            lines = { { text = "Wandering Merchant", rect = { 4, 5, 60, 12 } } },
        } },
        { "title.primary", "flavour.reader", { kind = "absent" } },
        { "count.primary", "count.reader", readValue("3") },
    }))
    assert(#reported == 3)
    assert(reported[1].ui_target == "count" and reported[1].kind == "read")
    assert(reported[2].reader == "flavour.reader" and reported[2].kind == "absent")
    assert(reported[2].reason == nil and reported[2].lines == nil)
    assert(reported[3].reader == "name.reader" and reported[3].kind == "unknown")
    assert(reported[3].reason == "low_confidence")
    assert(reported[3].lines == nil)
    assert((reported[3] :: any).confidence == nil)

    -- Three keys for a failed reading, and the reason is one of them.
    local keys = 0
    for key in reported[3] do
        assert(key == "ui_target" or key == "reader" or key == "kind" or key == "reason")
        keys += 1
    end
    assert(keys == 4)
    evidence.end_cycle(cycle)
end

-- A Reader the caller supplied nothing for is `not_measured`, which is what
-- evaluateBinding already does one function up with a predicate nothing
-- measured. The list length is the model's answer, not the Host's.
do
    local cycle = evidence.begin_cycle()
    local records = readingRecords(cycle, "present", "absent", "present")
    local state = resolution.resolve_state(reading, cycle, records)
    local reported = resolution.resolve_readings(reading, cycle, state, records, reads({
        { "count.primary", "count.reader", readValue("3") },
    }))
    assert(#reported == 3)
    assert(reported[2].reader == "flavour.reader" and reported[2].kind == "unknown")
    assert(reported[2].reason == "not_measured")
    assert(reported[3].reader == "name.reader" and reported[3].reason == "not_measured")

    -- A reason outside the one closed vocabulary is not carried through: it
    -- reads as an unknown nothing measured rather than as a new reason.
    local forged = resolution.resolve_readings(reading, cycle, state, records, reads({
        { "count.primary", "count.reader", { kind = "unknown", reason = "made_up" } },
    }))
    assert(forged[1].kind == "unknown" and forged[1].reason == "not_measured")
    evidence.end_cycle(cycle)
end

-- Two present variants of one Binding: that Binding reports nothing because
-- its visual state is ambiguous. The other UiTarget is unaffected, so the
-- refusal is scoped rather than total.
do
    local cycle = evidence.begin_cycle()
    local records = readingRecords(cycle, "present", "present", "present")
    local state = resolution.resolve_state(reading, cycle, records)
    assert(state.kind == "resolved_state")
    local reported = resolution.resolve_readings(reading, cycle, state, records, reads({
        { "title.primary", "name.reader", readValue("Wandering Merchant") },
        { "title.primary", "flavour.reader", readValue("A stranger blocks the road.") },
        { "title.primary", "name.reader", readValue("Wandering Merchant") },
        { "count.primary", "count.reader", readValue("3") },
    }))
    assert(#reported == 1)
    assert(reported[1].ui_target == "count")
    evidence.end_cycle(cycle)
end

-- A state this cycle did not resolve reports nothing, and neither does one whose
-- cycle has ended: a reading is same-cycle evidence on the terms every other
-- resolver result already has.
do
    local cycle = evidence.begin_cycle()
    local records = readingRecords(cycle, "present", "absent", "present")
    local state = resolution.resolve_state(reading, cycle, records)
    local values = reads({ { "count.primary", "count.reader", readValue("3") } })
    assert(#resolution.resolve_readings(reading, cycle, state, records, values) == 3)
    assert(resolution.reading_bindings(reading, cycle, {}) == nil)
    assert(#resolution.resolve_readings(reading, cycle, {}, records, values) == 0)
    assert(#resolution.resolve_readings(runtime, cycle, state, records, values) == 0)
    evidence.end_cycle(cycle)
    assert(#resolution.resolve_readings(reading, cycle, state, records, values) == 0)
end


-- T-002 / T07-equivalent: one declaration resolves three different item
-- counts. Image rectangles determine stable zero-based indices and travel
-- without rewriting.
local function collectionReading(lines: { { any } }): any
    return {
        [model.collection_reading_key("options", "flavour.reader")] = readBlock(lines),
    }
end

local function resolvedCollection(lines: { { any } }): any
    local cycle = evidence.begin_cycle()
    local records = readingRecords(cycle, "present", "absent", "present")
    local state = resolution.resolve_state(reading, cycle, records)
    local result, reason = resolution.resolve_collection(
        reading,
        cycle,
        state,
        "options",
        collectionReading(lines)
    )
    assert(reason == nil, "T-002 collection must resolve from trusted Reader evidence")
    assert(result ~= nil, "T-002 collection must return a resolved value")
    evidence.end_cycle(cycle)
    return result
end

do
    local reversed = resolvedCollection({
        { "Right", { 947, 100, 180, 40 } },
        { "Middle", { 631, 100, 180, 40 } },
        { "Left", { 315, 100, 180, 40 } },
    })
    local forward = resolvedCollection({
        { "Left", { 315, 100, 180, 40 } },
        { "Middle", { 631, 100, 180, 40 } },
        { "Right", { 947, 100, 180, 40 } },
    })
    for position, item in reversed.items do
        local other = forward.items[position]
        assert(
            item.index == other.index
                and item.rect[1] == other.rect[1]
                and item.rect[2] == other.rect[2]
                and item.rect[3] == other.rect[3]
                and item.rect[4] == other.rect[4],
            "T-002 indices must ignore detector enumeration order"
        )
    end

    local cases = {
        {
            lines = { { "Only", { 631, 100, 180, 40 } } },
            expected = { { 631, 100, 180, 40 } },
        },
        {
            lines = {
                { "Left", { 473, 100, 180, 40 } },
                { "Right", { 789, 100, 180, 40 } },
            },
            expected = {
                { 473, 100, 180, 40 },
                { 789, 100, 180, 40 },
            },
        },
        {
            lines = {
                { "Left", { 315, 100, 180, 40 } },
                { "Middle", { 631, 100, 180, 40 } },
                { "Right", { 947, 100, 180, 40 } },
            },
            expected = {
                { 315, 100, 180, 40 },
                { 631, 100, 180, 40 },
                { 947, 100, 180, 40 },
            },
        },
    }
    for expectedCount, case in cases do
        local result = resolvedCollection(case.lines)
        assert(result.count == expectedCount, "T-002 count must follow detected item cardinality")
        assert(result.completeness == "complete", "T-002 fitted item cardinality must be complete")
        assert(#result.items == expectedCount, "T-002 item list must match reported count")
        for position, item in result.items do
            assert(item.index == position - 1, "T-002 item index must be the stable spatial ordinal")
            assert(#item.readings == 1, "each Collection item must report every declared read")
            local expected = case.expected[position]
            assert(
                item.rect[1] == expected[1]
                    and item.rect[2] == expected[2]
                    and item.rect[3] == expected[3]
                    and item.rect[4] == expected[4],
                "T-002 item rectangle must be the Reader's exact detected rectangle"
            )
        end
    end
end

-- The first two slots of a three-item layout cannot be mistaken for a complete
-- two-item layout. All three slots are complete, while geometry fitting no
-- layout reports unknown without inventing an item list.
do
    local partial = resolvedCollection({
        { "Left", { 315, 100, 180, 40 } },
        { "Middle", { 631, 100, 180, 40 } },
    })
    assert(
        partial.completeness == "partial",
        "collection completeness must report partial when a layout slot may be missing"
    )
    assert(partial.count == 2 and #partial.items == 2)

    local complete = resolvedCollection({
        { "Left", { 315, 100, 180, 40 } },
        { "Middle", { 631, 100, 180, 40 } },
        { "Right", { 947, 100, 180, 40 } },
    })
    assert(
        complete.completeness == "complete",
        "collection completeness must report complete when every layout slot is measured"
    )

    local unknown = resolvedCollection({
        { "First", { 400, 100, 180, 40 } },
        { "Second", { 700, 100, 180, 40 } },
    })
    assert(
        unknown.completeness == "unknown" and unknown.items == nil,
        "collection completeness must report unknown without items when no layout fits"
    )
end

-- One Collection declaration aims relative to whichever measured item index is
-- selected. Different item origins must therefore produce different points.
do
    local cycle = evidence.begin_cycle()
    local records = readingRecords(cycle, "present", "absent", "present")
    local state = resolution.resolve_state(reading, cycle, records)
    local supplied = collectionReading({
        { "Left", { 473, 100, 180, 40 } },
        { "Right", { 789, 100, 180, 40 } },
    })
    local left, leftReason = resolution.collection_action_point(
        reading,
        cycle,
        state,
        "options",
        0,
        "pick",
        supplied
    )
    local right, rightReason = resolution.collection_action_point(
        reading,
        cycle,
        state,
        "options",
        1,
        "pick",
        supplied
    )
    assert(leftReason == nil and rightReason == nil and left ~= nil and right ~= nil)
    assert(
        left[1] == 493 and left[2] == 110 and right[1] == 809 and right[2] == 110,
        "a Collection click point must derive from the named item's measured rectangle"
    )
    evidence.end_cycle(cycle)
end

-- Item-relative action points leave the frame by refusal, never by clamping.
do
    local cycle = evidence.begin_cycle()
    local records = readingRecords(cycle, "present", "absent", "present")
    local state = resolution.resolve_state(reading, cycle, records)
    local point, reason = resolution.collection_action_point(
        reading,
        cycle,
        state,
        "options",
        3,
        "outside",
        collectionReading({ { "Outside", { 1105, 100, 17, 40 } } })
    )
    assert(
        point == nil and reason == "action_point_out_of_frame",
        "a Collection action point outside the frame must be refused"
    )
    evidence.end_cycle(cycle)
end

-- A Collection item is a detected line, so its width varies with its text. The
-- read belongs to the containing element and keeps its declared width across
-- items instead of inheriting either line's width.
do
    local cycle = evidence.begin_cycle()
    local records = readingRecords(cycle, "present", "absent", "present")
    local state = resolution.resolve_state(reading, cycle, records)
    local targets, reason = resolution.collection_read_targets(
        reading,
        cycle,
        state,
        "options",
        collectionReading({
            { "Short", { 315, 100, 51, 40 } },
            { "A much longer title", { 631, 100, 160, 40 } },
        })
    )
    assert(reason == nil and targets ~= nil)
    assert(
        targets[1].rect[3] == 240 and targets[2].rect[3] == 240,
        "a Collection read's derived width must not change with the item's width"
    )
    evidence.end_cycle(cycle)
end

-- A declared item read uses the measured item's origin and its own absolute
-- extent, then travels on that item in the same three-outcome shape as a
-- Binding reading.
do
    local cycle = evidence.begin_cycle()
    local records = readingRecords(cycle, "present", "absent", "present")
    local state = resolution.resolve_state(reading, cycle, records)
    local supplied = collectionReading({ { "Only", { 631, 100, 180, 40 } } })
    local targets, targetReason = resolution.collection_read_targets(
        reading,
        cycle,
        state,
        "options",
        supplied
    )
    assert(targetReason == nil and targets ~= nil)
    assert(
        targets[1].rect[1] == 631
            and targets[1].rect[2] == 150
            and targets[1].rect[3] == 240
            and targets[1].rect[4] == 60,
        "a Collection read must combine its origin offset with its absolute size"
    )
    supplied[model.collection_item_reading_key("options", 0, "name.reader")] =
        readValue("Wandering Merchant")
    local result, reason = resolution.resolve_collection(
        reading,
        cycle,
        state,
        "options",
        supplied
    )
    assert(reason == nil and result ~= nil)
    assert(
        result.items[1].readings[1].kind == "read"
            and result.items[1].readings[1].reader == "name.reader"
            and result.items[1].readings[1].lines[1].text == "Wandering Merchant",
        "a Collection item must report the Reader result from its derived rectangle"
    )
    evidence.end_cycle(cycle)
end

-- Applying a Collection read rectangle must remain inside the frame. The resolver
-- refuses the whole Collection instead of clamping the requested rectangle.
do
    local cycle = evidence.begin_cycle()
    local records = readingRecords(cycle, "present", "absent", "present")
    local state = resolution.resolve_state(reading, cycle, records)
    local result, reason = resolution.resolve_collection(
        reading,
        cycle,
        state,
        "options",
        collectionReading({ { "Outside", { 1105, 100, 180, 40 } } })
    )
    assert(
        result == nil and reason == "read_out_of_frame",
        "a Collection read whose derived rectangle leaves the frame must be refused"
    )
    evidence.end_cycle(cycle)
end

-- A bare search rectangle never produces an item. Only an absent result from
-- the trusted Reader can resolve the collection to a known empty value.
do
    local cycle = evidence.begin_cycle()
    local records = readingRecords(cycle, "present", "absent", "present")
    local state = resolution.resolve_state(reading, cycle, records)
    local missing, missingReason = resolution.resolve_collection(
        reading,
        cycle,
        state,
        "options",
        {}
    )
    assert(missing == nil and missingReason == "not_measured", "T-002 geometry must not create Present")
    local empty, emptyReason = resolution.resolve_collection(
        reading,
        cycle,
        state,
        "options",
        {
            [model.collection_reading_key("options", "flavour.reader")] = { kind = "absent" },
        }
    )
    assert(emptyReason == nil and empty.count == 0, "T-002 absent Reader evidence must resolve zero items")
    evidence.end_cycle(cycle)
end

-- Overlapping spans have no stable spatial order. Refusing them is what keeps a
-- detector adapter's enumeration order from becoming item identity by accident.
do
    local cycle = evidence.begin_cycle()
    local records = readingRecords(cycle, "present", "absent", "present")
    local state = resolution.resolve_state(reading, cycle, records)
    local result, reason = resolution.resolve_collection(
        reading,
        cycle,
        state,
        "options",
        collectionReading({
            { "First", { 200, 100, 180, 40 } },
            { "Second", { 300, 100, 180, 40 } },
        })
    )
    assert(result == nil and reason == "ambiguous_order", "T-002 ambiguous spatial order must refuse")
    evidence.end_cycle(cycle)
end

-- T-006/T-007: one Collection predicate controls conditional enumeration. The
-- exact count and spatial order are one runnable falsifier: changing the count,
-- swapping either member, or changing the declared predicate from "wanted"
-- makes the named assertion fail.
do
    local cycle = evidence.begin_cycle()
    local records = readingRecords(cycle, "present", "absent", "present")
    local state = resolution.resolve_state(reading, cycle, records)
    local supplied = {
        [model.collection_reading_key("map.nodes", "flavour.reader")] = readBlock({
            { "First", { 315, 100, 180, 40 } },
            { "Second", { 631, 100, 180, 40 } },
            { "Third", { 947, 100, 180, 40 } },
        }),
        [model.collection_item_reading_key("map.nodes", 0, "name.reader")] =
            readValue("wanted"),
        [model.collection_item_reading_key("map.nodes", 1, "name.reader")] =
            readValue("skip"),
        [model.collection_item_reading_key("map.nodes", 2, "name.reader")] =
            readValue("wanted"),
    }
    local enumerated, reason = resolution.resolve_collection(
        reading,
        cycle,
        state,
        "map.nodes",
        supplied
    )
    assert(reason == nil and enumerated ~= nil)
    assert(
        enumerated.count == 2
            and #enumerated.items == 2
            and enumerated.items[1].index == 0
            and enumerated.items[2].index == 2,
        "T-007 conditional enumeration must preserve predicate, count, and spatial order"
    )

    -- The assembled coordinates deliberately exceed base_resolution. A
    -- one-screen evaluator would reject them or clip them; this one requests
    -- all three unordered pair reads and evaluates the complete topology.
    local assembled = {
        completeness = "complete",
        count = 3,
        items = {
            { index = 0, rect = { 1400, 100, 40, 40 }, readings = {} },
            { index = 2, rect = { 1800, 100, 40, 40 }, readings = {} },
            { index = 4, rect = { 2200, 100, 40, 40 }, readings = {} },
        },
    }
    local targets, targetReason = resolution.stitched_map_connectivity_targets(
        reading,
        "route",
        assembled
    )
    assert(targetReason == nil and targets ~= nil and #targets == 3)
    assert(
        targets[1].start[1] == 1420
            and targets[3].finish[1] == 2220
            and targets[1].colour_key.red == 184,
        "T-006 connectivity reads must use assembled-map coordinates and the project colour key"
    )
    local evaluated, evaluationReason = resolution.evaluate_stitched_map(
        reading,
        "route",
        assembled,
        {
            { from = 0, to = 2, ratio = 0.90 },
            { from = 0, to = 4, ratio = 0.10 },
            { from = 2, to = 4, ratio = 0.80 },
        }
    )
    assert(evaluationReason == nil and evaluated ~= nil)
    assert(
        #evaluated.nodes == 3
            and #evaluated.edges == 2
            and evaluated.edges[1].from == 0
            and evaluated.edges[1].to == 2
            and evaluated.edges[2].from == 2
            and evaluated.edges[2].to == 4,
        "T-006 stitched-map evaluation must preserve the complete assembled topology"
    )
    evidence.end_cycle(cycle)
end

return 1
