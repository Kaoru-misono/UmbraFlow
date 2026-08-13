--!strict
-- Unique TOML parser and immutable RuntimeModel v2 field contract.

local function rejects(action: () -> any, message: string?)
    assert(not pcall(action), message)
end

local valid = [==[
schema_version = 2
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

[[reader]]
id = "header.text"
kind = "text"
confidence_floor = 0.80
layout = "single_line"
normalization = "trim"

[[reader]]
id = "options.text"
kind = "text"
confidence_floor = 0.80
layout = "block"
normalization = "trim"

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
    all = [
      { kind = "locator_present", locator = "header.template" },
      { kind = "text_equals", reader = "header.text", value = "Settings" },
    ],
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
  reader = "options.text",
  order = "left_to_right",
  slots = { origin = 631, pitch = 316, tolerance = 2 },
}
reads = [{ reader = "header.text", offset = [0, -40], size = [320, 40] }]

[[surface]]
id = "settings"
kind = "scene"
covers = []
identity = ["settings.header"]

[[transition]]
id = "settings.saved"
from_surfaces = ["settings"]
trigger = { binding = "settings.save", action = "activate" }
to_surfaces = ["settings"]
]==]

local runtime = project.parse(valid)
assert(model.is_runtime_model(runtime))
assert(type(model.canonical_bytes(runtime)) == "string")
assert(#model.canonical_bytes(runtime) > 0)
local reformatted = project.parse("# canonical formatting must not matter\n" .. valid .. "\n")
assert(model.canonical_bytes(reformatted) == model.canonical_bytes(runtime))
assert(runtime.schema_version == 2)
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
assert(runtime.collection_by_id.options.placement.slots.tolerance == 2)
assert(#runtime.collection_by_id.options.reads == 1)
assert(runtime.collection_by_id.options.reads[1].offset[1] == 0)
assert(runtime.collection_by_id.options.reads[1].offset[2] == -40)
assert(runtime.collection_by_id.options.reads[1].size[1] == 320)
assert(runtime.collection_by_id.options.reads[1].size[2] == 40)
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
assert(ids(runtime.declared_action_ids) == "activate")
rejects(function() runtime.declared_surface_ids[1] = "forged" end)
assert(project.is_loaded(runtime) == false)
rejects(function() runtime.ui_targets[1].kind = "control" end)

-- No second document builder or caller identity attachment is accepted.
assert(project.build == nil)
rejects(function() project.parse(valid .. "\n[[forged_section]]\nid = \"forged\"\n") end)
rejects(function() project.parse(string.gsub(valid, "schema_version = 2", "schema_version = 1")) end)

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

-- A Reader states its layout and inherits nothing. There is no default because
-- a single_line read of a rectangle holding several lines comes back as one run
-- of nonsense rather than failing, so a model that says nothing would be given
-- the answer that cannot report its own mistake.
assert(project.parse(valid).reader_by_id["header.text"].layout == "single_line")
rejects(function()
    project.parse(string.gsub(valid, 'layout = "single_line"\n', ""))
end)
rejects(function()
    project.parse(string.gsub(valid, 'layout = "single_line"', 'layout = "paragraph"'))
end)
assert(
    project.parse(string.gsub(valid, 'layout = "single_line"', 'layout = "block"'))
        .reader_by_id["header.text"].layout == "block"
)

-- Two action kinds, and the placement rule follows the kinds present rather
-- than the count. A keystroke names no coordinate, so a Binding granting only
-- keystrokes carries no action_point and one that carries anyway is refused.
--
-- Plain substring replacement rather than gsub: every model fragment below
-- contains a rectangle, and `[` opens a character class in a Lua pattern.
local function replace(text: string, from: string, to: string): string
    local at = string.find(text, from, 1, true)
    assert(at ~= nil)
    local start = at :: number
    return string.sub(text, 1, start - 1) .. to .. string.sub(text, start + #from)
end

local clickAction = '{ id = "activate", kind = "click", proof_locator = "save.template" }'
local keyAction = '{ id = "activate", kind = "key", key = "ENTER", proof_locator = "save.template" }'
local actionPointLine = '  action_point = [1710, 965],\n'

-- A collection's Reader is its item detector, so the declaration accepts only
-- block layout and cannot fall back to geometry or to a fixed placement.
rejects(function()
    project.parse(replace(
        valid,
        'id = "options.text"\nkind = "text"\nconfidence_floor = 0.80\nlayout = "block"',
        'id = "options.text"\nkind = "text"\nconfidence_floor = 0.80\nlayout = "single_line"'
    ))
end, "T-002 collection detector must require block layout")
rejects(function()
    project.parse(replace(valid, '  reader = "options.text",\n', ""))
end, "T-002 collection detector must require a Reader")
rejects(function()
    project.parse(replace(valid, '  slots = { origin = 631, pitch = 316, tolerance = 2 },\n', ""))
end, "T-002 collection placement must require a slot layout")
rejects(function()
    project.parse(replace(valid, "pitch = 316", "pitch = 315"))
end, "T-002 collection slot pitch must be even")
rejects(function()
    project.parse(replace(valid, '  kind = "detected",\n', '  kind = "fixed",\n'))
end, "T-002 collection placement must have one destructive spelling")

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

-- A key carrying a coordinate, a click without one, a key with no name, a click
-- carrying one, and a kind nothing delivers: five refusals, one rule each.
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
rejects(function()
    project.parse(replace(
        valid,
        clickAction,
        '{ id = "activate", kind = "scroll", proof_locator = "save.template" }'
    ))
end)

return 1
