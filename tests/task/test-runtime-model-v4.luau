--!strict
-- Unique TOML parser and immutable RuntimeModel v4 field contract.

local function rejects(action: () -> any, message: string?)
    assert(not pcall(action), message)
end

local valid = [==[
schema_version = 4
base_resolution = [1920, 1080]
base_dpi = [96, 96]

[[ui_target]]
id = "header"
kind = "region"

[[ui_target]]
id = "save_button"
kind = "control"

[[locator]]
id = "header.template"
kind = "template"
asset_path = "assets/templates/header.png"
threshold = 0.90

[[locator]]
id = "save.template"
kind = "template"
asset_path = "assets/templates/save.png"
threshold = 0.92

[[binding]]
id = "settings.header"
surface = "settings"
ui_target = "header"
placement = {
  kind = "fixed",
  rect = [40, 30, 600, 80],
}
variants = [{
  name = "default",
  detector = {
    all = [{ kind = "locator_present", locator = "header.template" }],
    any = [],
    none = [],
  },
}]
actions = []

[[binding]]
id = "settings.save"
surface = "settings"
ui_target = "save_button"
placement = {
  kind = "fixed",
  rect = [1600, 920, 220, 90],
  action_point = [1710, 965],
}
variants = [{
  name = "enabled",
  detector = {
    all = [{ kind = "locator_present", locator = "save.template" }],
    any = [],
    none = [],
  },
}]
actions = [{ id = "activate", kind = "click", proof_locator = "save.template" }]

[[collection]]
id = "options"
surface = "settings"
placement = {
  kind = "detected",
  search_rect = [120, 300, 1680, 300],
  confidence_floor = 0.80,
  order = "left_to_right",
  slots = { origin = 631, pitch = 316, extent = 1080, tolerance = 2, maximum_slots = 8 },
}
actions = [{ id = "pick", kind = "click", proof_locator = "save.template", offset = [12, 18] }]
reads = [{ id = "label", offset = [0, -40], size = [320, 40], layout = "single_line", confidence_floor = 0.80, normalization = "trim" }]
predicate = { kind = "text_equals", read = "label", value = "Enabled" }

[[readout]]
id = "settings.title"
surface = "settings"
rect = [40, 30, 600, 80]
layout = "single_line"
confidence_floor = 0.80
normalization = "trim"

[[stitched_map]]
id = "route"
collection = "options"
colour_key = { red = 184, green = 92, blue = 31, tolerance = 12 }
minimum_ratio = 0.75

[[surface]]
id = "settings"
covers = []
identity = ["settings.header"]

[[transition]]
id = "settings.saved"
from_surfaces = ["settings"]
trigger = { collection = "options", action = "pick" }
to_surfaces = ["settings"]
]==]

local runtime = project.parse(valid)
assert(model.is_runtime_model(runtime))
assert(type(model.canonical_bytes(runtime)) == "string")
assert(#model.canonical_bytes(runtime) > 0)
local reformatted = project.parse("# canonical formatting must not matter\n" .. valid .. "\n")
assert(model.canonical_bytes(reformatted) == model.canonical_bytes(runtime))
assert(runtime.schema_version == 4)
assert(runtime.ui_target_by_id.save_button.kind == "control")
assert(runtime.ui_target_by_id.save_button.placement == nil)
assert(runtime.ui_target_by_id.save_button.variant == nil)
assert(runtime.binding_by_id["settings.save"].variants[1].name == "enabled")
assert(runtime.binding_by_id["settings.save"].placement.rect[1] == 1600)
assert(runtime.binding_by_id["settings.save"].placement.action_point[1] == 1710)
assert(
    runtime.collection_by_id.options.placement.order == "left_to_right",
    "T-002 collection order must survive TOML compilation"
)
assert(
    runtime.collection_by_id.options.placement.search_rect[3] == 1680,
    "T-002 collection search rectangle must survive TOML compilation"
)
assert(runtime.collection_by_id.options.placement.slots.origin == 631)
assert(runtime.collection_by_id.options.placement.slots.pitch == 316)
assert(runtime.collection_by_id.options.placement.slots.extent == 1080)
assert(runtime.collection_by_id.options.placement.slots.maximum_slots == 8)
assert(runtime.collection_by_id.options.placement.slots.tolerance == 2)
assert(runtime.collection_by_id.options.actions[1].id == "pick")
assert(runtime.collection_by_id.options.actions[1].offset[1] == 12)
assert(#runtime.collection_by_id.options.reads == 1)
assert(runtime.collection_by_id.options.reads[1].id == "label")
assert(runtime.collection_by_id.options.reads[1].offset[1] == 0)
assert(runtime.collection_by_id.options.reads[1].offset[2] == -40)
assert(runtime.collection_by_id.options.reads[1].size[1] == 320)
assert(runtime.collection_by_id.options.reads[1].size[2] == 40)
assert(runtime.collection_by_id.options.predicate.value == "Enabled")
assert(runtime.collection_by_id.options.predicate.read == "label")

-- T-009: text is evidence about a Surface, never its identity. The only legal
-- home of a text predicate is a Collection's filter, exercised above; a
-- Binding detector naming text_equals must fail to parse rather than let a
-- model put text on the identity path.
rejects(function()
    project.parse(string.gsub(
        valid,
        '{ kind = "locator_present", locator = "header.template" }',
        '{ kind = "locator_present", locator = "header.template" }, { kind = "text_equals", read = "label", value = "Settings" }'
    ))
end, "T-009 a Binding detector must not name text_equals")
assert(runtime.stitched_map_by_id.route.collection == "options")
assert(runtime.stitched_map_by_id.route.colour_key.red == 184)
assert(runtime.stitched_map_by_id.route.minimum_ratio == 0.75)
assert(runtime.locator_by_id["save.template"].asset_path == "assets/templates/save.png")
assert(runtime.locator_by_id["save.template"].sha256 == nil)
assert(runtime.asset_paths[1] == "assets/templates/header.png")
assert(runtime.asset_paths[2] == "assets/templates/save.png")

-- The vocabulary the Operator matches a workflow step's UI identifiers
-- against. It is published here because C++ may not read RuntimeModel
-- semantics: every id the model declares must appear, and nothing else may.
local function ids(values: { string }): string
    return table.concat(values, ",")
end
assert(ids(runtime.declared_surface_ids) == "settings")
assert(ids(runtime.declared_ui_target_ids) == "header,save_button")
assert(ids(runtime.declared_action_ids) == "activate,pick")
rejects(function() runtime.declared_surface_ids[1] = "forged" end)
assert(project.is_loaded(runtime) == false)
rejects(function() runtime.ui_targets[1].kind = "control" end)

-- No second document builder or caller identity attachment is accepted.
assert(project.build == nil)
rejects(function() project.parse(valid .. "\n[[forged_section]]\nid = \"forged\"\n") end)
rejects(function() project.parse(string.gsub(valid, "schema_version = 4", "schema_version = 1")) end)
rejects(function() project.parse(string.gsub(valid, "schema_version = 4", "schema_version = 3")) end)

-- Hash and placement have one authority each: Artifact owns hashes, Binding
-- owns all placement. Unsupported spellings fail instead of falling back.
rejects(function()
    project.parse(string.gsub(
        valid,
        'asset_path = "assets/templates/header.png"',
        'asset_path = "templates/header.png"'
    ))
end)
rejects(function()
    project.parse(string.gsub(
        valid,
        'threshold = 0.90',
        'threshold = 0.90\nsha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"',
        1
    ))
end)
rejects(function()
    project.parse(string.gsub(
        valid,
        'kind = "region"',
        'kind = "region"\nplacement = { kind = "fixed", rect = [0, 0, 1, 1] }',
        1
    ))
end)
rejects(function()
    project.parse(string.gsub(valid, 'name = "enabled"', 'name = "enabled"\n  variant = "other"'))
end)

-- Plain substring replacement rather than gsub: every model fragment below
-- contains a rectangle, and `[` opens a character class in a Lua pattern.
local function replace(text: string, from: string, to: string): string
    local at = string.find(text, from, 1, true)
    assert(at ~= nil)
    local start = at :: number
    return string.sub(text, 1, start - 1) .. to .. string.sub(text, start + #from)
end

-- R1: there is no Reader record. Every statement about how a rectangle is read
-- belongs to whoever drew that rectangle, so the section that used to hold
-- those statements is not a section this vocabulary has, and no reading site
-- may point at one.
assert(runtime.reader_by_id == nil, "R1 a compiled model publishes no Reader index")
assert(runtime.readers == nil, "R1 a compiled model has no Reader vocabulary")
rejects(function()
    project.parse(
        valid
            .. '\n[[reader]]\nid = "orphan"\nkind = "text"\nconfidence_floor = 0.5\n'
            .. 'layout = "block"\nnormalization = "trim"\n'
    )
end, "R1 [[reader]] is not a section this vocabulary has")
rejects(function()
    project.parse(replace(
        valid,
        '  confidence_floor = 0.80,\n  order = "left_to_right",',
        '  reader = "options.text",\n  order = "left_to_right",'
    ))
end, "R1 a Collection's detection names no Reader")
rejects(function()
    project.parse(replace(valid, '{ id = "label", offset', '{ reader = "label", offset'))
end, "R1 a Collection's read is named by its own id")

-- R2: a Readout is a fixed rectangle on one Surface, read on its own terms. It
-- names no UiTarget and carries no detector; its Surface is its whole gate.
assert(runtime.readout_by_id["settings.title"].surface == "settings")
assert(runtime.readout_by_id["settings.title"].rect[3] == 600)
assert(runtime.readout_by_id["settings.title"].layout == "single_line")
assert(runtime.readout_by_id["settings.title"].confidence_floor == 0.80)
assert(runtime.readout_by_id["settings.title"].normalization == "trim")
assert(runtime.readouts_by_surface.settings[1].id == "settings.title")
rejects(function()
    project.parse(replace(valid, 'id = "settings.title"\nsurface = "settings"', 'id = "settings.title"\nsurface = "absent"'))
end, "R2 a Readout must name a declared Surface")
rejects(function()
    project.parse(replace(valid, 'rect = [40, 30, 600, 80]\nlayout', 'rect = [40, 30, 600, 1080]\nlayout'))
end, "R2 a Readout rectangle must stay inside base_resolution")
rejects(function()
    project.parse(replace(valid, 'surface = "settings"\nrect = [40, 30, 600, 80]\n', 'surface = "settings"\n'))
end, "R2 a Readout must declare the rectangle it reads")
rejects(function()
    project.parse(replace(
        valid,
        'rect = [40, 30, 600, 80]\nlayout = "single_line"\n',
        'rect = [40, 30, 600, 80]\nui_target = "header"\nlayout = "single_line"\n'
    ))
end, "R2 a Readout attaches to no UiTarget")

-- R2's other half: a Binding measures and does not report text. The member that
-- made it report was `reads`, and it is gone rather than tolerated.
rejects(function()
    project.parse(replace(
        valid,
        'actions = [{ id = "activate", kind = "click", proof_locator = "save.template" }]',
        'actions = [{ id = "activate", kind = "click", proof_locator = "save.template" }]\nreads = ["settings.title"]'
    ))
end, "R2 a Binding declares no reads")

-- Every read states its own layout and inherits nothing. There is no default
-- because a single_line read of a rectangle holding several lines comes back as
-- one run of nonsense rather than failing, so a model that says nothing would
-- be given the answer that cannot report its own mistake.
rejects(function()
    project.parse(replace(valid, 'layout = "single_line"\nconfidence_floor = 0.80\n', 'confidence_floor = 0.80\n'))
end, "a Readout states its layout")
rejects(function()
    project.parse(replace(valid, 'layout = "single_line"\nconfidence_floor = 0.80\nnormalization', 'layout = "paragraph"\nconfidence_floor = 0.80\nnormalization'))
end, "a Readout layout comes from a closed vocabulary")
assert(
    project.parse(replace(
        valid,
        'layout = "single_line"\nconfidence_floor = 0.80\nnormalization',
        'layout = "block"\nconfidence_floor = 0.80\nnormalization'
    )).readout_by_id["settings.title"].layout == "block"
)
rejects(function()
    project.parse(replace(valid, ', layout = "single_line", confidence_floor = 0.80, normalization = "trim" }', ' }'))
end, "a Collection read states how its rectangle is read")

-- Six action kinds, and the placement rule follows the kinds present rather
-- than the count. A keystroke and a wheel scroll name no coordinate, so a
-- Binding granting only those carries no action_point and one that carries
-- anyway is refused; a click, drag, hold or move requires one.
local clickAction = '{ id = "activate", kind = "click", proof_locator = "save.template" }'
local keyAction = '{ id = "activate", kind = "key", key = "ENTER", proof_locator = "save.template" }'
local dragAction = '{ id = "pan", kind = "drag", offset = [-400, 0], duration_ms = 600, proof_locator = "save.template" }'
local holdAction = '{ id = "inspect", kind = "hold", hold_ms = 350, proof_locator = "save.template" }'
local moveAction = '{ id = "hover", kind = "move", proof_locator = "save.template" }'
local scrollAction = '{ id = "wheel", kind = "scroll", notches = -3, proof_locator = "save.template" }'
local actionPointLine = '  action_point = [1710, 965],\n'

-- A Collection's detection reads a region it must find the lines inside, so its
-- floor is the one thing it states about that read and cannot be left out.
rejects(function()
    project.parse(replace(valid, '  confidence_floor = 0.80,\n  order = "left_to_right",', '  order = "left_to_right",'))
end, "T-002 a Collection's detection must declare its confidence floor")
rejects(function()
    project.parse(replace(valid, '  slots = { origin = 631, pitch = 316, extent = 1080, tolerance = 2, maximum_slots = 8 },\n', ""))
end, "T-002 collection placement must require a slot layout")
-- A layout's extent is the bound its own pitch stops describing once the fan
-- is long enough. There is no absent reading of it: a declaration that omits
-- it is refused rather than treated as an unbounded fan.
rejects(function()
    project.parse(replace(valid, "extent = 1080, ", ""))
end, "T-002 collection slots must declare an extent")
-- The bound the fitted-layout search stops at. It is declared and not derived,
-- so there is no absent reading of it either: everything else in `slots` is
-- pixels, and a slot count computed from pixels is a cardinality bound the
-- framework would have invented.
rejects(function()
    project.parse(replace(valid, ", maximum_slots = 8", ""))
end, "T-002 collection slots must declare a maximum slot count")
rejects(function()
    project.parse(replace(valid, "maximum_slots = 8", "maximum_slots = 0"))
end, "T-002 a collection that can hold no slot at all is not a collection")
-- Integer pixels are now a property of each rounded slot, so an odd pitch is
-- an ordinary declaration rather than a refusal.
assert(
    project.parse(replace(valid, "pitch = 316", "pitch = 315"))
        .collection_by_id["options"].placement.slots.pitch == 315,
    "T-002 collection slot pitch carries no parity constraint"
)
rejects(function()
    project.parse(replace(valid, '  kind = "detected",\n', '  kind = "fixed",\n'))
end, "T-002 collection placement must have one destructive spelling")
rejects(function()
    project.parse(replace(
        valid,
        'trigger = { collection = "options", action = "pick" }',
        'trigger = { binding = "settings.save", collection = "options", action = "pick" }'
    ))
end, "a transition trigger naming both a Binding and Collection must be refused")
rejects(function()
    project.parse(replace(valid, 'offset = [12, 18]', ''))
end, "a Collection click must name one item-origin offset")

-- A Collection predicate points at a sibling read and at nothing else. There is
-- no vocabulary outside the Collection for it to reach into.
rejects(function()
    project.parse(replace(valid, 'read = "label", value = "Enabled"', 'read = "absent", value = "Enabled"'))
end, "a Collection predicate must name a read declared in collection.reads")

local keyed = project.parse(
    replace(replace(valid, clickAction, keyAction), actionPointLine, "")
)
assert(keyed.binding_by_id["settings.save"].actions[1].kind == "key")
assert(keyed.binding_by_id["settings.save"].actions[1].key == "ENTER")
assert(keyed.binding_by_id["settings.save"].placement.action_point == nil)

-- The keys the Host validates against its own single definition of the accepted
-- set. A model granting no keystroke publishes an empty list rather than none.
assert(ids(keyed.declared_key_names) == "ENTER")
assert(ids(runtime.declared_key_names) == "")

local dragging = project.parse(replace(valid, clickAction, dragAction))
assert(dragging.binding_by_id["settings.save"].actions[1].kind == "drag")
assert(dragging.binding_by_id["settings.save"].actions[1].offset[1] == -400)
assert(dragging.binding_by_id["settings.save"].actions[1].duration_ms == 600)
rejects(function()
    project.parse(replace(valid, clickAction, replace(dragAction, ", duration_ms = 600", "")))
end, "T-006 drag duration must be explicit")
rejects(function()
    project.parse(replace(valid, clickAction, replace(dragAction, "[-400, 0]", "[400, 400]")))
end, "T-006 drag endpoint must remain in base_resolution")

-- The three kinds the vocabulary gained, each parsed as declared. A hold and a
-- move aim at the same measured point a click does; a scroll names none, so its
-- Binding drops the action_point exactly as a keystroke's does.
local holding = project.parse(replace(valid, clickAction, holdAction))
assert(holding.binding_by_id["settings.save"].actions[1].kind == "hold")
assert(holding.binding_by_id["settings.save"].actions[1].hold_ms == 350)

local hovering = project.parse(replace(valid, clickAction, moveAction))
assert(hovering.binding_by_id["settings.save"].actions[1].kind == "move")
assert(hovering.binding_by_id["settings.save"].placement.action_point[1] == 1710)

local wheeling = project.parse(
    replace(replace(valid, clickAction, scrollAction), actionPointLine, "")
)
assert(wheeling.binding_by_id["settings.save"].actions[1].kind == "scroll")
assert(wheeling.binding_by_id["settings.save"].actions[1].notches == -3)
assert(wheeling.binding_by_id["settings.save"].placement.action_point == nil)

-- A hold has no default and answers to the same ceiling the delivery layer
-- holds a button under, so a declaration asking for a press the Host would
-- refuse to lift is refused at the artifact boundary instead.
rejects(function()
    project.parse(replace(valid, clickAction, replace(holdAction, ", hold_ms = 350", "")))
end, "a hold duration must be explicit")
rejects(function()
    project.parse(replace(valid, clickAction, replace(holdAction, "hold_ms = 350", "hold_ms = 5001")))
end, "a hold must stay inside the ceiling the delivery layer holds a button under")

-- A key carrying a coordinate, a click without one, a key with no name, a click
-- carrying one, a scroll carrying a coordinate, and a kind nothing delivers:
-- six refusals, one rule each.
rejects(function() project.parse(replace(valid, clickAction, keyAction)) end)
rejects(function() project.parse(replace(valid, actionPointLine, "")) end)
rejects(function()
    project.parse(replace(
        replace(
            valid,
            clickAction,
            '{ id = "activate", kind = "key", proof_locator = "save.template" }'
        ),
        actionPointLine,
        ""
    ))
end)
rejects(function()
    project.parse(replace(
        valid,
        clickAction,
        '{ id = "activate", kind = "click", key = "ENTER", proof_locator = "save.template" }'
    ))
end)
rejects(function() project.parse(replace(valid, clickAction, scrollAction)) end)
rejects(function()
    project.parse(replace(
        valid,
        clickAction,
        '{ id = "activate", kind = "tap", proof_locator = "save.template" }'
    ))
end)

-- R3: a Surface states what it covers and nothing beside it. Covering nothing
-- is the bottom of a stack, and there is no second member saying so.
assert(runtime.surface_by_id.settings.kind == nil, "R3 a Surface has no kind")
assert(#runtime.surface_by_id.settings.covers == 0)
rejects(function()
    project.parse(replace(valid, 'id = "settings"\ncovers = []', 'id = "settings"\nkind = "scene"\ncovers = []'))
end, "R3 `kind` is not a member a Surface has")
-- The cover graph is where a stack's shape is decided, and a cycle in it has no
-- bottom to resolve from.
rejects(function()
    project.parse(replace(
        valid,
        'id = "settings"\ncovers = []\nidentity = ["settings.header"]',
        'id = "settings"\ncovers = ["popup"]\nidentity = ["settings.header"]\n\n'
            .. '[[surface]]\nid = "popup"\ncovers = ["settings"]\nidentity = ["settings.header"]'
    ))
end, "R3 a cover cycle has no bottom and is refused")

-- R4: an absent list IS the empty list. Each member below says nothing when it
-- is written empty, so writing it is a second spelling of leaving it out.
local terseSource = replace(
    valid,
    '[[transition]]\nid = "settings.saved"\nfrom_surfaces = ["settings"]\n'
        .. 'trigger = { collection = "options", action = "pick" }\nto_surfaces = ["settings"]\n',
    ''
)
terseSource = replace(
    terseSource,
    'actions = []\n\n[[binding]]\nid = "settings.save"',
    '\n[[binding]]\nid = "settings.save"'
)
terseSource = replace(
    terseSource,
    'actions = [{ id = "pick", kind = "click", proof_locator = "save.template", offset = [12, 18] }]\n'
        .. 'reads = [{ id = "label", offset = [0, -40], size = [320, 40], layout = "single_line", confidence_floor = 0.80, normalization = "trim" }]\n'
        .. 'predicate = { kind = "text_equals", read = "label", value = "Enabled" }\n',
    ''
)
terseSource = replace(terseSource, 'covers = []\n', '')
terseSource = replace(terseSource, '    any = [],\n    none = [],\n', '')
local terseParse = { pcall(project.parse, terseSource) }
assert(
    terseParse[1],
    "R4 a row that says nothing by leaving a list out must parse: "
        .. tostring(terseParse[2])
)
local terse = terseParse[2]
assert(#terse.binding_by_id["settings.header"].actions == 0, "R4 an absent actions list is empty")
assert(#terse.collection_by_id.options.actions == 0)
assert(#terse.collection_by_id.options.reads == 0)
assert(terse.collection_by_id.options.predicate == nil)
assert(#terse.surface_by_id.settings.covers == 0, "R4 an absent covers list is empty")
assert(#terse.binding_by_id["settings.header"].variants[1].detector.any == 0)
assert(#terse.binding_by_id["settings.header"].variants[1].detector.none == 0)
-- The empty list is still the empty list, and still refused where something was
-- required: absent means empty, never absent means unconstrained.
rejects(function()
    project.parse(replace(valid, 'identity = ["settings.header"]', ''))
end, "R4 a Surface identity that says nothing is refused for being empty")
rejects(function()
    project.parse(replace(
        valid,
        '    all = [{ kind = "locator_present", locator = "header.template" }],\n',
        ''
    ))
end, "R4 a detector with no positive evidence is refused for being empty")

-- The genesis model: the document that declares nothing.
--
-- It compiles, and what it compiles to can do nothing -- no ui target, no
-- binding, no surface. There is no lower bound on those three collections,
-- because the only thing one protected was the tautology that a model
-- declaring nothing declares nothing, while making the state every project
-- starts from unrepresentable.
local genesis = project.parse([==[
schema_version = 4
base_resolution = [1, 1]
base_dpi = [96, 96]
]==])
assert(#genesis.ui_targets == 0)
assert(#genesis.bindings == 0)
assert(#genesis.readouts == 0)
assert(#genesis.surfaces == 0)
assert(#genesis.declared_surface_ids == 0)
assert(#genesis.declared_ui_target_ids == 0)
assert(#genesis.declared_action_ids == 0)
assert(genesis.base_resolution[1] == 1)
assert(genesis.base_dpi[1] == 96)

return 1
