--!strict
-- Surface resolution and same-cycle Binding resolution are separate.

local function detector(locator: string): any
    return {
        all = { { kind = "locator_present", locator = locator } },
        any = {},
        none = {},
    }
end

local function placement(x: number, y: number, actionable: boolean): any
    return {
        kind = "fixed",
        rect = { x, y, 120, 50 },
        action_point = if actionable then { x + 60, y + 25 } else nil,
    }
end

local runtime = model.compile({
    schema_version = 2,
    base_resolution = { 1280, 720 },
    base_dpi = { 96, 96 },
    ui_targets = {
        { id = "scene_anchor", kind = "region" },
        { id = "other_anchor", kind = "region" },
        { id = "dialog_anchor", kind = "region" },
        { id = "save", kind = "control" },
    },
    locators = {
        { id = "scene.locator", kind = "template", asset_path = "assets/scene.png", threshold = 0.9 },
        { id = "other.locator", kind = "template", asset_path = "assets/other.png", threshold = 0.9 },
        { id = "dialog.locator", kind = "template", asset_path = "assets/dialog.png", threshold = 0.9 },
        { id = "save.primary", kind = "template", asset_path = "assets/save-primary.png", threshold = 0.9 },
        { id = "save.alternate", kind = "template", asset_path = "assets/save-alternate.png", threshold = 0.9 },
    },
    readers = {},
    bindings = {
        { id = "scene.identity", surface = "scene", ui_target = "scene_anchor", variant = "default", placement = placement(10, 10, false), detector = detector("scene.locator"), actions = {} },
        { id = "other.identity", surface = "other", ui_target = "other_anchor", variant = "default", placement = placement(10, 10, false), detector = detector("other.locator"), actions = {} },
        { id = "dialog.identity", surface = "dialog", ui_target = "dialog_anchor", variant = "default", placement = placement(400, 100, false), detector = detector("dialog.locator"), actions = {} },
        { id = "save.primary", surface = "dialog", ui_target = "save", variant = "primary", placement = placement(800, 600, true), detector = detector("save.primary"), actions = { { id = "activate", kind = "click", proof_locator = "save.primary" } } },
        { id = "save.alternate", surface = "dialog", ui_target = "save", variant = "alternate", placement = placement(950, 600, true), detector = detector("save.alternate"), actions = { { id = "activate", kind = "click", proof_locator = "save.alternate" } } },
    },
    surfaces = {
        { id = "scene", kind = "scene", covers = {}, identity = { all = { "scene.identity" }, any = {}, none = {} } },
        { id = "other", kind = "scene", covers = {}, identity = { all = { "other.identity" }, any = {}, none = {} } },
        { id = "dialog", kind = "overlay", covers = { "scene" }, identity = { all = { "dialog.identity" }, any = {}, none = {} } },
    },
    transitions = {},
})

local function record(cycle: any, binding: string, locator: string, kind: string): any
    local source = { kind = "template", binding = binding, locator = locator }
    if kind == "present" then return evidence.present(cycle, source, { locator = locator }, 0.99) end
    if kind == "absent" then return evidence.absent(cycle, source) end
    return evidence.unknown(cycle, "locator_failed", source)
end

local function put(result: any, cycle: any, binding: string, locator: string, kind: string)
    local predicate = { kind = "locator_present", locator = locator }
    result[model.measurement_key(binding, predicate)] = record(cycle, binding, locator, kind)
end

local function stateRecords(cycle: any, scene: string, other: string, dialog: string): any
    local result = {}
    put(result, cycle, "scene.identity", "scene.locator", scene)
    put(result, cycle, "other.identity", "other.locator", other)
    put(result, cycle, "dialog.identity", "dialog.locator", dialog)
    return result
end

do
    local cycle = evidence.begin_cycle()
    local records = stateRecords(cycle, "present", "absent", "present")
    put(records, cycle, "save.primary", "save.primary", "present")
    put(records, cycle, "save.alternate", "save.alternate", "absent")
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

-- A known Binding cannot outrank an Unknown same-target variant.
do
    local cycle = evidence.begin_cycle()
    local records = stateRecords(cycle, "present", "absent", "present")
    put(records, cycle, "save.primary", "save.primary", "present")
    put(records, cycle, "save.alternate", "save.alternate", "unknown")
    local state = resolution.resolve_state(runtime, cycle, records)
    local binding = resolution.resolve_binding(runtime, cycle, state, "save", "save.primary", records)
    assert(binding.kind == "unknown_binding")
    assert(binding.reason == "unknown_competitor")
    evidence.end_cycle(cycle)
end

return 1
