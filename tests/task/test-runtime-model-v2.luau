--!strict
-- P1 contract tests.  The host test harness will publish `model` and `project`
-- as framework globals when this script is wired into the task test target.

local function check(condition: boolean, message: string)
    if not condition then error(message, 0) end
end

local function rejects(action: () -> any, message: string)
    local ok = pcall(action)
    check(not ok, "expected rejection: " .. message)
end

local valid = [[
schema_version = 1
base_resolution = [1920, 1080]
base_dpi = [96, 96]

[[context]]
id = "camp"

[[target]]
id = "confirm_button"
kind = "control"
geometry = { kind = "fixed", rect = [100, 200, 180, 64] }
locators = ["confirm.geometry"]
readers = ["confirm.presence"]

[[locator]]
id = "confirm.geometry"
target = "confirm_button"
kind = "geometry"
rect = [100, 200, 180, 64]

[[reader]]
id = "confirm.presence"
target = "confirm_button"
kind = "presence"
confidence_floor = 0.8

[[surface]]
id = "camp_scene"
kind = "scene"
contexts = ["camp"]

[[binding]]
id = "camp.confirm"
surface = "camp_scene"
target = "confirm_button"
placement = { kind = "target_geometry" }
identity = { all = [{ kind = "locator_present", locator = { locator = "confirm.geometry" } }], any = [], none = [] }
readers = ["confirm.presence"]

[[binding.actions]]
id = "click"
kind = "click"
locator = { locator = "confirm.geometry" }
preconditions = []

[[binding.actions]]
id = "drag"
kind = "drag"
locator = { locator = "confirm.geometry" }
destination = { kind = "point", point = [320, 240] }
preconditions = []

[[transition]]
id = "camp.confirm_loop"
from = { context = "camp", surfaces = ["camp_scene"] }
trigger = { kind = "action", binding = "camp.confirm", action = "click" }
to = { context = "camp", surfaces = ["camp_scene"] }
verification = { kind = "resolve", timeout_ms = 1000, attempts = 2 }
]]

local document = project.parse(valid)
local runtime = project.build(nil, document)
check(model.is_runtime_model(runtime), "compiler must return a framework model")
check(runtime.schema_version == 1, "schema version")
check(runtime.asset_handles == nil, "runtime model must not expose asset handles")
local hashed = project.build(nil, document, string.rep("a", 64))
check(model.model_hash(hashed) == string.rep("a", 64), "model hash must be attached to the compiled model")
local state = model.RuntimeState.new({ context = "camp", surfaces = { "camp_scene" } })
check(model.is_runtime_state(state), "runtime state must be minted")
check(runtime.context_by_id.camp ~= nil, "context index")
check(runtime.target_by_id.confirm_button ~= nil, "target index")
check(runtime.locator_by_id["confirm.geometry"] ~= nil, "locator index")
check(runtime.reader_by_id["confirm.presence"] ~= nil, "reader index")
check(runtime.surface_by_id.camp_scene ~= nil, "surface index")
check(runtime.binding_by_id["camp.confirm"] ~= nil, "binding index")
check(runtime.transition_by_id["camp.confirm_loop"] ~= nil, "transition index")
check(#runtime.bindings_by_surface.camp_scene == 1, "surface binding index")
check(#runtime.bindings_by_target.confirm_button == 1, "target binding index")
check(#runtime.surfaces_by_kind.scene == 1, "surface kind index")

local encoded = project.encode(runtime)
local roundTrip = project.build(nil, project.parse(encoded))
check(roundTrip.target_by_id.confirm_button ~= nil, "encoded model must parse")
check(roundTrip.binding_by_id["camp.confirm"] ~= nil, "encoded binding must parse")
check(#roundTrip.binding_by_id["camp.confirm"].actions == 2, "encoded drag action must parse")
check(roundTrip.binding_by_id["camp.confirm"].actions[2].destination.point[1] == 320, "drag point must round-trip")

local nested = string.gsub(valid, 'geometry = { kind = "fixed", rect = %[100, 200, 180, 64%] }\n', "")
nested = string.gsub(nested, 'readers = %["confirm.presence"%]\n\n%[%[locator%]%]', 'readers = ["confirm.presence"]\n\n[target.geometry]\nkind = "fixed"\nrect = [100, 200, 180, 64]\n\n[[locator]]')
local nestedRuntime = project.build(nil, project.parse(nested))
check(nestedRuntime.target_by_id.confirm_button.geometry.kind == "fixed", "nested geometry must parse")

-- Every public object is minted and deeply immutable.
check(model.is_context(runtime.contexts[1]), "context must be minted")
check(model.is_target(runtime.targets[1]), "target must be minted")
check(model.is_locator(runtime.locators[1]), "locator must be minted")
check(model.is_reader(runtime.readers[1]), "reader must be minted")
check(model.is_surface(runtime.surfaces[1]), "surface must be minted")
check(model.is_binding(runtime.bindings[1]), "binding must be minted")
check(model.is_action(runtime.bindings[1].actions[1]), "action must be minted")
check(model.is_transition(runtime.transitions[1]), "transition must be minted")
rejects(function() runtime.contexts[1].id = "changed" end, "frozen context")
rejects(function() runtime.targets[1].locators[1] = "changed" end, "frozen target list")
rejects(function() runtime.bindings[1].identity.all[1].kind = "changed" end, "frozen predicate")

local template = model.Locator.new({
    id = "confirm.template",
    target = "confirm_button",
    kind = "template",
    asset = { path = "templates/confirm.png", sha256 = string.rep("c", 64) },
    threshold = 0.92,
    variants = { "confirm.enabled" },
})
local variant = model.LocatorVariant.new({
    id = "confirm.enabled",
    locator = "confirm.template",
    asset = { path = "templates/confirm-enabled.png", sha256 = string.rep("d", 64) },
})
local blockReader = model.Reader.new({
    id = "confirm.block",
    target = "confirm_button",
    kind = "block",
    confidence_floor = 0.75,
    normalization = "trim",
    join = "newline",
})
local itemsReader = model.Reader.new({
    id = "confirm.items",
    target = "confirm_button",
    kind = "items",
    confidence_floor = 0.75,
    item_kind = "line",
    min_items = 1,
    max_items = 3,
})
check(model.is_locator(template), "template locator must be minted")
check(model.is_locator_variant(variant), "locator variant must be minted")
check(model.is_reader(blockReader), "block reader must be minted")
check(model.is_reader(itemsReader), "items reader must be minted")
check(model.Surface.new({ id = "any_interrupt", kind = "interrupt", contexts = {} }).kind == "interrupt", "interrupt surface")
check(model.Action.new({ id = "press_e", kind = "key", key = "E", preconditions = {} }).kind == "key", "key action")
check(model.Action.new({ id = "scroll_down", kind = "scroll", axis = "vertical", delta = 1, preconditions = {} }).kind == "scroll", "scroll action")
check(model.Action.new({
    id = "drag_point",
    kind = "drag",
    locator = { locator = "confirm.geometry" },
    destination = { kind = "point", point = { 10, 20 } },
    preconditions = {},
}).kind == "drag", "drag action")
check(model.Transition.new({
    id = "timeout_transition",
    from = { context = "camp", surfaces = { "camp_scene" } },
    trigger = { kind = "timeout", milliseconds = 10 },
    to = { context = "camp", surfaces = { "camp_scene" } },
    verification = { kind = "resolve", timeout_ms = 100, attempts = 1 },
}).id == "timeout_transition", "timeout transition")

-- Old runtime vocabulary is not accepted anywhere.
rejects(function() model.Target.new({ name = "old", form = "fixed", capabilities = { "read" } }) end, "old Element fields")
rejects(function() project.parse('schema = "umbraflow-project/l2-v2"') end, "old project schema")
rejects(function() project.parse(valid .. '\n[[element]]\nname = "old"\n') end, "old element section")

-- Cross-object and safety constraints are semantic compiler checks.
rejects(function()
    local broken = project.parse(valid .. [[

[[surface]]
id = "bad_overlay"
kind = "overlay"
contexts = ["camp"]
covers = ["missing"]
]])
    project.build(nil, broken)
end, "unknown surface reference")
rejects(function()
    local noIdentity = string.gsub(valid, "identity = .*", "identity = { all = [], any = [], none = [] }")
    project.build(nil, project.parse(noIdentity))
end, "surface without positive identity")
rejects(function()
    model.LocatorVariant.new({
        id = "bad.variant",
        locator = "confirm.geometry",
        asset = { path = "../escape.png", sha256 = string.rep("a", 64) },
    })
end, "escaping runtime asset path")
rejects(function()
    model.LocatorVariant.new({
        id = "bad.hash",
        locator = "confirm.geometry",
        asset = { path = "templates/ok.png", sha256 = string.rep("A", 64) },
    })
end, "non-lowercase asset hash")
rejects(function()
    model.RuntimeState.new({ context = "camp", surfaces = { "camp_scene", "camp_scene" } })
end, "duplicate runtime state surface")
rejects(function()
    model.Surface.new({ id = "bad.scene", kind = "scene", contexts = { "camp" }, covers = {} })
end, "scene covers")

return 1
