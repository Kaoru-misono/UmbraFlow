--!strict
-- model: the trusted framework's v1 runtime model.
--
-- This module deliberately has no Page, Element, Reference, CapabilitySet or
-- holding vocabulary.  The file is the semantic boundary between the runtime
-- model compiler and the future resolver: constructors validate local shape,
-- RuntimeModel.new validates references and builds the indexes every consumer
-- needs.  Screens, observations and review data are offline concerns and do
-- not enter this module.

export type Identifier = string
export type Rect = { number }
export type Size = { number }
export type Offset = { number }
export type Point = { number }

export type AssetRef = {
    path: string,
    sha256: string,
}

export type LocatorRef = {
    locator: Identifier,
    variant: Identifier?,
}

export type Context = { id: Identifier }
export type RuntimeState = {
    context: Identifier,
    surfaces: { Identifier },
}

export type Surface = {
    id: Identifier,
    kind: string,
    contexts: { Identifier },
    covers: { Identifier }?,
}

export type Target = {
    id: Identifier,
    kind: string,
    geometry: any,
    locators: { Identifier },
    readers: { Identifier },
}

export type Locator = {
    id: Identifier,
    target: Identifier,
    kind: string,
    asset: AssetRef?,
    threshold: number?,
    search_rect: Rect?,
    variants: { Identifier }?,
    reader: Identifier?,
    rect: Rect?,
    anchor: LocatorRef?,
    offset: Offset?,
    item_locator: LocatorRef?,
    axis: string?,
    spacing: Size?,
    max_items: number?,
}

export type LocatorVariant = {
    id: Identifier,
    locator: Identifier,
    asset: AssetRef,
    threshold: number?,
    search_rect: Rect?,
}

export type Reader = {
    id: Identifier,
    target: Identifier,
    kind: string,
    confidence_floor: number,
    normalization: string?,
    join: string?,
    item_kind: string?,
    min_items: number?,
    max_items: number?,
}

export type IdentityPredicate = { [string]: any }
export type Identity = {
    all: { IdentityPredicate },
    any: { IdentityPredicate },
    none: { IdentityPredicate },
}

export type Placement = { [string]: any }
export type Action = { [string]: any }

export type Binding = {
    id: Identifier,
    surface: Identifier,
    target: Identifier,
    placement: Placement,
    identity: Identity,
    readers: { Identifier },
    actions: { Action },
}

export type Transition = {
    id: Identifier,
    from: any,
    trigger: any,
    to: any,
    verification: any,
}

export type RuntimeModel = {
    schema_version: number,
    base_resolution: Size,
    base_dpi: Size,
    contexts: { Context },
    targets: { Target },
    locators: { Locator },
    locator_variants: { LocatorVariant },
    readers: { Reader },
    surfaces: { Surface },
    bindings: { Binding },
    transitions: { Transition },
    context_by_id: { [Identifier]: Context },
    target_by_id: { [Identifier]: Target },
    locator_by_id: { [Identifier]: Locator },
    locator_variant_by_id: { [Identifier]: LocatorVariant },
    reader_by_id: { [Identifier]: Reader },
    surface_by_id: { [Identifier]: Surface },
    binding_by_id: { [Identifier]: Binding },
    transition_by_id: { [Identifier]: Transition },
    bindings_by_surface: { [Identifier]: { Binding } },
    bindings_by_target: { [Identifier]: { Binding } },
    locator_variants_by_locator: { [Identifier]: { LocatorVariant } },
    readers_by_target: { [Identifier]: { Reader } },
    surfaces_by_kind: { [string]: { any } },
}

local model = {}

local function weakRegistry(): { [any]: boolean }
    return setmetatable({}, { __mode = "k" }) :: any
end

local minted = {
    context = weakRegistry(),
    state = weakRegistry(),
    target = weakRegistry(),
    locator = weakRegistry(),
    locator_variant = weakRegistry(),
    reader = weakRegistry(),
    surface = weakRegistry(),
    binding = weakRegistry(),
    action = weakRegistry(),
    transition = weakRegistry(),
    runtime_model = weakRegistry(),
}

local function derives(value: any, registry: { [any]: boolean }): boolean
    -- A runtime object is a framework-owned immutable value.  Do not accept a
    -- look-alike or a user-derived table: a resolver must be able to trust the
    -- identity test without following an author-controlled metatable.
    return type(value) == "table" and registry[value] == true
end

local modelHashes: { [any]: string } = setmetatable({}, { __mode = "k" }) :: any

local function unknownKey(
    what: string,
    spec: { [string]: any },
    known: { [string]: boolean }
): string?
    for key in spec do
        if type(key) ~= "string" or not known[key] then
            return what .. ' does not have a field named "' .. tostring(key) .. '"'
        end
    end
    return nil
end

local function requireTable(what: string, value: any): { [any]: any }
    if type(value) ~= "table" then
        error(what .. " needs a table", 3)
    end
    return value :: { [any]: any }
end

local function requireString(what: string, value: any): string
    if type(value) ~= "string" or value == "" then
        error(what .. " needs a non-empty string", 3)
    end
    return value :: string
end

local function isWhole(value: any): boolean
    return type(value) == "number" and value % 1 == 0
end

local function requireWhole(what: string, value: any): number
    if not isWhole(value) then
        error(what .. " needs a whole number", 3)
    end
    return value :: number
end

local function requireIdentifier(what: string, value: any): string
    local id = requireString(what, value)
    if #id > 128 then
        error(what .. " must be at most 128 bytes", 3)
    end
    local previousSeparator = false
    for index = 1, #id do
        local character = string.sub(id, index, index)
        local lower = string.byte(character) >= string.byte("a")
            and string.byte(character) <= string.byte("z")
        local digit = string.byte(character) >= string.byte("0")
            and string.byte(character) <= string.byte("9")
        local separator = character == "." or character == "_" or character == "-"
        if index == 1 and not lower then
            error(what .. ' must start with a lowercase letter: "' .. id .. '"', 3)
        end
        if not lower and not digit and not separator then
            error(
                what .. ' contains an invalid identifier character in "' .. id .. '"',
                3
            )
        end
        if separator and (index == #id or previousSeparator) then
            error(what .. ' may not contain a trailing or repeated separator', 3)
        end
        previousSeparator = separator
    end
    return id
end

local function denseList(what: string, value: any, allowEmpty: boolean): { any }
    if type(value) ~= "table" then
        error(what .. " needs an array", 3)
    end
    local list = value :: { any }
    if not allowEmpty and #list == 0 then
        error(what .. " may not be empty", 3)
    end
    for key in list do
        if type(key) ~= "number" or key % 1 ~= 0 or key < 1 or key > #list then
            error(what .. " needs a dense array", 3)
        end
    end
    return list
end

local function stringList(
    what: string,
    value: any,
    allowEmpty: boolean,
    identifiers: boolean
): { string }
    local list = denseList(what, value, allowEmpty)
    local copy: { string } = {}
    local seen: { [string]: boolean } = {}
    for index = 1, #list do
        local entry = if identifiers
            then requireIdentifier(what .. "[" .. tostring(index) .. "]", list[index])
            else requireString(what .. "[" .. tostring(index) .. "]", list[index])
        if seen[entry] then
            error(what .. ' repeats "' .. entry .. '"', 3)
        end
        seen[entry] = true
        copy[index] = entry
    end
    return table.freeze(copy)
end

local function frozenArray(what: string, value: any, count: number, check: any): { number }
    local list = denseList(what, value, false)
    if #list ~= count then
        error(what .. " needs exactly " .. tostring(count) .. " numbers", 3)
    end
    local copy: { number } = {}
    for index = 1, count do
        local number = check(what .. "[" .. tostring(index) .. "]", list[index])
        copy[index] = number
    end
    return table.freeze(copy)
end

local function positive(what: string, value: any): number
    local number = requireWhole(what, value)
    if number < 1 then
        error(what .. " must be positive", 3)
    end
    return number
end

local function nonNegative(what: string, value: any): number
    local number = requireWhole(what, value)
    if number < 0 then
        error(what .. " may not be negative", 3)
    end
    return number
end

local function rect(what: string, value: any): Rect
    local list = denseList(what, value, false)
    if #list ~= 4 then
        error(what .. " needs [x, y, width, height]", 3)
    end
    return table.freeze({
        nonNegative(what .. "[1]", list[1]),
        nonNegative(what .. "[2]", list[2]),
        positive(what .. "[3]", list[3]),
        positive(what .. "[4]", list[4]),
    })
end

local function size(what: string, value: any): Size
    return frozenArray(what, value, 2, positive)
end

local function offset(what: string, value: any): Offset
    return frozenArray(what, value, 4, requireWhole)
end

local function point(what: string, value: any): Point
    return frozenArray(what, value, 2, requireWhole)
end

local function confidence(what: string, value: any): number
    if type(value) ~= "number" or value ~= value or value < 0 or value > 1 then
        error(what .. " must be a number between 0 and 1", 3)
    end
    return value :: number
end

local function sha256(what: string, value: any): string
    local hash = requireString(what, value)
    if #hash ~= 64 or string.match(hash, "^[0-9a-f]+$") == nil then
        error(what .. " must be a lowercase SHA-256 hex digest", 3)
    end
    return hash
end

local function asset(what: string, value: any): AssetRef
    local tableValue = requireTable(what, value)
    local unknown = unknownKey(what, tableValue, { path = true, sha256 = true })
    if unknown ~= nil then
        error(unknown, 3)
    end
    local path = requireString(what .. ".path", tableValue.path)
    if
        string.sub(path, 1, 1) == "/"
        or string.sub(path, 1, 1) == "\\"
        or string.match(path, "^[A-Za-z]:") ~= nil
        or string.find(path, "\\", 1, true) ~= nil
        or string.find(path, "//", 1, true) ~= nil
        or string.sub(path, -1) == "/"
    then
        error(what .. ".path must be a relative path using '/' separators", 3)
    end
    for component in string.gmatch(path, "[^/]+") do
        if component == ".." or component == "." or component == "" then
            error(what .. ".path contains an invalid path component", 3)
        end
    end
    return table.freeze({
        path = path,
        sha256 = sha256(what .. ".sha256", tableValue.sha256),
    })
end

local function locatorRef(what: string, value: any): LocatorRef
    local tableValue = requireTable(what, value)
    local unknown = unknownKey(what, tableValue, { locator = true, variant = true })
    if unknown ~= nil then
        error(unknown, 3)
    end
    local result: { [string]: any } = {
        locator = requireIdentifier(what .. ".locator", tableValue.locator),
    }
    if tableValue.variant ~= nil then
        result.variant = requireIdentifier(what .. ".variant", tableValue.variant)
    end
    return table.freeze(result) :: any
end

local function placement(what: string, value: any): Placement
    local tableValue = requireTable(what, value)
    local kind = requireString(what .. ".kind", tableValue.kind)
    if kind == "target_geometry" then
        local unknown = unknownKey(what, tableValue, { kind = true })
        if unknown ~= nil then error(unknown, 3) end
        return table.freeze({ kind = kind })
    elseif kind == "rect" then
        local unknown = unknownKey(what, tableValue, { kind = true, rect = true })
        if unknown ~= nil then error(unknown, 3) end
        return table.freeze({ kind = kind, rect = rect(what .. ".rect", tableValue.rect) })
    elseif kind == "relative" then
        local unknown = unknownKey(what, tableValue, { kind = true, anchor = true, offset = true })
        if unknown ~= nil then error(unknown, 3) end
        return table.freeze({
            kind = kind,
            anchor = locatorRef(what .. ".anchor", tableValue.anchor),
            offset = offset(what .. ".offset", tableValue.offset),
        })
    end
    error(what .. '.kind must be "target_geometry", "rect" or "relative"', 3)
end

local function geometry(what: string, value: any): any
    local tableValue = requireTable(what, value)
    local kind = requireString(what .. ".kind", tableValue.kind)
    if kind == "fixed" then
        local unknown = unknownKey(what, tableValue, { kind = true, rect = true })
        if unknown ~= nil then error(unknown, 3) end
        return table.freeze({ kind = kind, rect = rect(what .. ".rect", tableValue.rect) })
    elseif kind == "collection" then
        local unknown = unknownKey(what, tableValue, {
            kind = true, rect = true, axis = true, item_size = true,
            spacing = true, max_items = true,
        })
        if unknown ~= nil then error(unknown, 3) end
        local result: { [string]: any } = {
            kind = kind,
            rect = rect(what .. ".rect", tableValue.rect),
            axis = requireString(what .. ".axis", tableValue.axis),
            item_size = size(what .. ".item_size", tableValue.item_size),
            spacing = size(what .. ".spacing", tableValue.spacing),
        }
        if result.axis ~= "horizontal" and result.axis ~= "vertical" then
            error(what .. '.axis must be "horizontal" or "vertical"', 3)
        end
        if tableValue.max_items ~= nil then
            result.max_items = positive(what .. ".max_items", tableValue.max_items)
        end
        return table.freeze(result)
    elseif kind == "relative" then
        local unknown = unknownKey(what, tableValue, { kind = true, anchor = true, offset = true })
        if unknown ~= nil then error(unknown, 3) end
        return table.freeze({
            kind = kind,
            anchor = locatorRef(what .. ".anchor", tableValue.anchor),
            offset = offset(what .. ".offset", tableValue.offset),
        })
    end
    error(what .. '.kind must be "fixed", "collection" or "relative"', 3)
end

local function textPredicate(what: string, value: any): any
    local tableValue = requireTable(what, value)
    local kind = requireString(what .. ".kind", tableValue.kind)
    if kind == "locator_present" then
        local unknown = unknownKey(what, tableValue, { kind = true, locator = true })
        if unknown ~= nil then error(unknown, 3) end
        return table.freeze({ kind = kind, locator = locatorRef(what .. ".locator", tableValue.locator) })
    elseif kind == "text_match" then
        local unknown = unknownKey(what, tableValue, {
            kind = true, reader = true, operator = true, value = true,
        })
        if unknown ~= nil then error(unknown, 3) end
        local operator = requireString(what .. ".operator", tableValue.operator)
        local result: { [string]: any } = {
            kind = kind,
            reader = requireIdentifier(what .. ".reader", tableValue.reader),
            operator = operator,
        }
        if operator == "equals" or operator == "present" then
            if operator == "present" then
                if tableValue.value ~= nil then
                    error(what .. ' with operator "present" has no value', 3)
                end
            else
                result.value = requireString(what .. ".value", tableValue.value)
            end
        elseif operator == "contains_all" then
            result.value = table.freeze(stringList(what .. ".value", tableValue.value, false, false))
        else
            error(what .. '.operator must be "equals", "contains_all" or "present"', 3)
        end
        return table.freeze(result)
    end
    error(what .. '.kind must be "locator_present" or "text_match"', 3)
end

local function predicateKey(value: any): string
    local result = tostring(value.kind)
    if value.locator ~= nil then
        result ..= ":locator=" .. value.locator.locator
        if value.locator.variant ~= nil then result ..= ":variant=" .. value.locator.variant end
    end
    if value.reader ~= nil then result ..= ":reader=" .. value.reader end
    if value.operator ~= nil then result ..= ":operator=" .. value.operator end
    if type(value.value) == "table" then
        result ..= ":value=" .. table.concat(value.value, "\31")
    elseif value.value ~= nil then
        result ..= ":value=" .. tostring(value.value)
    end
    return result
end

local function predicates(what: string, value: any): { IdentityPredicate }
    local list = denseList(what, value, true)
    local result: { IdentityPredicate } = {}
    local seen: { [string]: boolean } = {}
    for index = 1, #list do
        local predicate = textPredicate(what .. "[" .. tostring(index) .. "]", list[index])
        local key = predicateKey(predicate)
        if seen[key] then error(what .. " repeats a predicate", 3) end
        seen[key] = true
        result[index] = predicate
    end
    return table.freeze(result)
end

local function identity(what: string, value: any): Identity
    local tableValue = requireTable(what, value)
    local unknown = unknownKey(what, tableValue, { all = true, any = true, none = true })
    if unknown ~= nil then error(unknown, 3) end
    return table.freeze({
        all = predicates(what .. ".all", tableValue.all),
        any = predicates(what .. ".any", tableValue.any),
        none = predicates(what .. ".none", tableValue.none),
    }) :: any
end

local function preconditions(what: string, value: any): { any }
    local list = denseList(what, value, true)
    local result: { any } = {}
    local seen: { [string]: boolean } = {}
    for index = 1, #list do
        local predicate = textPredicate(what .. "[" .. tostring(index) .. "]", list[index])
        local key = predicateKey(predicate)
        if seen[key] then error(what .. " repeats a predicate", 3) end
        seen[key] = true
        result[index] = predicate
    end
    return table.freeze(result)
end

local function statePattern(what: string, value: any): any
    local tableValue = requireTable(what, value)
    local unknown = unknownKey(what, tableValue, { context = true, surfaces = true })
    if unknown ~= nil then error(unknown, 3) end
    return table.freeze({
        context = requireIdentifier(what .. ".context", tableValue.context),
        surfaces = stringList(what .. ".surfaces", tableValue.surfaces, false, true),
    })
end

function model.is_context(value: any): boolean return derives(value, minted.context) end
function model.is_runtime_state(value: any): boolean return derives(value, minted.state) end
function model.is_target(value: any): boolean return derives(value, minted.target) end
function model.is_locator(value: any): boolean return derives(value, minted.locator) end
function model.is_locator_variant(value: any): boolean return derives(value, minted.locator_variant) end
function model.is_reader(value: any): boolean return derives(value, minted.reader) end
function model.is_surface(value: any): boolean return derives(value, minted.surface) end
function model.is_binding(value: any): boolean return derives(value, minted.binding) end
function model.is_action(value: any): boolean return derives(value, minted.action) end
function model.is_transition(value: any): boolean return derives(value, minted.transition) end
function model.is_runtime_model(value: any): boolean return derives(value, minted.runtime_model) end

local Context = {}
model.Context = Context
function Context.new(spec: { [string]: any }): Context
    local value = requireTable("Context.new", spec)
    local unknown = unknownKey("Context.new", value, { id = true })
    if unknown ~= nil then error(unknown, 2) end
    local result = table.freeze({ id = requireIdentifier("context id", value.id) })
    minted.context[result] = true
    return result :: any
end

local RuntimeState = {}
model.RuntimeState = RuntimeState
function RuntimeState.new(spec: { [string]: any }): RuntimeState
    local value = requireTable("RuntimeState.new", spec)
    local unknown = unknownKey("RuntimeState.new", value, { context = true, surfaces = true })
    if unknown ~= nil then error(unknown, 2) end
    local result = table.freeze({
        context = requireIdentifier("runtime state context", value.context),
        surfaces = stringList("runtime state surfaces", value.surfaces, false, true),
    })
    minted.state[result] = true
    return result :: any
end

local Target = {}
model.Target = Target
function Target.new(spec: { [string]: any }): Target
    local value = requireTable("Target.new", spec)
    local unknown = unknownKey("Target.new", value, {
        id = true, kind = true, geometry = true, locators = true, readers = true,
    })
    if unknown ~= nil then error(unknown, 2) end
    local kind = requireString("target kind", value.kind)
    if kind ~= "control" and kind ~= "label" and kind ~= "indicator"
        and kind ~= "collection" and kind ~= "region" then
        error("target kind must be control, label, indicator, collection or region", 2)
    end
    local result = table.freeze({
        id = requireIdentifier("target id", value.id),
        kind = kind,
        geometry = if value.geometry ~= nil then geometry("target geometry", value.geometry) else nil,
        locators = stringList("target locators", value.locators, false, true),
        readers = stringList("target readers", value.readers, true, true),
    })
    minted.target[result] = true
    return result :: any
end

local Locator = {}
model.Locator = Locator
function Locator.new(spec: { [string]: any }): Locator
    local value = requireTable("Locator.new", spec)
    local kind = requireString("locator kind", value.kind)
    local common = { id = true, target = true, kind = true }
    local result: { [string]: any } = {
        id = requireIdentifier("locator id", value.id),
        target = requireIdentifier("locator target", value.target),
        kind = kind,
    }
    if kind == "template" then
        local unknown = unknownKey("template locator", value, {
            id = true, target = true, kind = true, asset = true, threshold = true,
            search_rect = true, variants = true,
        })
        if unknown ~= nil then error(unknown, 2) end
        if value.asset == nil and value.variants == nil then
            error("a template locator needs asset or variants", 2)
        end
        if value.asset ~= nil then result.asset = asset("locator asset", value.asset) end
        if value.threshold ~= nil then result.threshold = confidence("locator threshold", value.threshold) end
        if value.search_rect ~= nil then result.search_rect = rect("locator search_rect", value.search_rect) end
        if value.variants ~= nil then result.variants = stringList("locator variants", value.variants, false, true) end
    elseif kind == "text" then
        local unknown = unknownKey("text locator", value, {
            id = true, target = true, kind = true, reader = true, search_rect = true,
        })
        if unknown ~= nil then error(unknown, 2) end
        result.reader = requireIdentifier("text locator reader", value.reader)
        if value.search_rect ~= nil then result.search_rect = rect("text locator search_rect", value.search_rect) end
    elseif kind == "geometry" then
        local unknown = unknownKey("geometry locator", value, { id = true, target = true, kind = true, rect = true })
        if unknown ~= nil then error(unknown, 2) end
        result.rect = rect("geometry locator rect", value.rect)
    elseif kind == "relative" then
        local unknown = unknownKey("relative locator", value, { id = true, target = true, kind = true, anchor = true, offset = true })
        if unknown ~= nil then error(unknown, 2) end
        result.anchor = locatorRef("relative locator anchor", value.anchor)
        result.offset = offset("relative locator offset", value.offset)
    elseif kind == "collection" then
        local unknown = unknownKey("collection locator", value, {
            id = true, target = true, kind = true, item_locator = true,
            axis = true, spacing = true, max_items = true,
        })
        if unknown ~= nil then error(unknown, 2) end
        result.item_locator = locatorRef("collection item_locator", value.item_locator)
        result.axis = requireString("collection axis", value.axis)
        if result.axis ~= "horizontal" and result.axis ~= "vertical" then
            error('collection axis must be "horizontal" or "vertical"', 2)
        end
        result.spacing = size("collection spacing", value.spacing)
        if value.max_items ~= nil then result.max_items = positive("collection max_items", value.max_items) end
    else
        error("locator kind must be template, text, geometry, relative or collection", 2)
    end
    local frozen = table.freeze(result)
    minted.locator[frozen] = true
    return frozen :: any
end

local LocatorVariant = {}
model.LocatorVariant = LocatorVariant
function LocatorVariant.new(spec: { [string]: any }): LocatorVariant
    local value = requireTable("LocatorVariant.new", spec)
    local unknown = unknownKey("LocatorVariant.new", value, {
        id = true, locator = true, asset = true, threshold = true, search_rect = true,
    })
    if unknown ~= nil then error(unknown, 2) end
    local result: { [string]: any } = {
        id = requireIdentifier("locator variant id", value.id),
        locator = requireIdentifier("locator variant locator", value.locator),
        asset = asset("locator variant asset", value.asset),
    }
    if value.threshold ~= nil then result.threshold = confidence("locator variant threshold", value.threshold) end
    if value.search_rect ~= nil then result.search_rect = rect("locator variant search_rect", value.search_rect) end
    local frozen = table.freeze(result)
    minted.locator_variant[frozen] = true
    return frozen :: any
end

local Reader = {}
model.Reader = Reader
function Reader.new(spec: { [string]: any }): Reader
    local value = requireTable("Reader.new", spec)
    local kind = requireString("reader kind", value.kind)
    local result: { [string]: any } = {
        id = requireIdentifier("reader id", value.id),
        target = requireIdentifier("reader target", value.target),
        kind = kind,
        confidence_floor = confidence("reader confidence_floor", value.confidence_floor),
    }
    if kind == "line" or kind == "block" then
        local known = { id = true, target = true, kind = true, confidence_floor = true, normalization = true, join = true }
        local unknown = unknownKey(kind .. " reader", value, known)
        if unknown ~= nil then error(unknown, 2) end
        result.normalization = requireString(kind .. " reader normalization", value.normalization)
        if result.normalization ~= "raw" and result.normalization ~= "trim" and result.normalization ~= "collapse_whitespace" then
            error('reader normalization must be "raw", "trim" or "collapse_whitespace"', 2)
        end
        if kind == "block" then
            result.join = requireString("block reader join", value.join)
            if result.join ~= "newline" and result.join ~= "space" then
                error('block reader join must be "newline" or "space"', 2)
            end
        elseif value.join ~= nil then
            error("line reader does not have join", 2)
        end
    elseif kind == "items" then
        local unknown = unknownKey("items reader", value, {
            id = true, target = true, kind = true, confidence_floor = true,
            item_kind = true, min_items = true, max_items = true,
        })
        if unknown ~= nil then error(unknown, 2) end
        result.item_kind = requireString("items reader item_kind", value.item_kind)
        if result.item_kind ~= "line" and result.item_kind ~= "block" then
            error('items reader item_kind must be "line" or "block"', 2)
        end
        if value.min_items ~= nil then result.min_items = nonNegative("items reader min_items", value.min_items) end
        if value.max_items ~= nil then result.max_items = positive("items reader max_items", value.max_items) end
        if result.min_items ~= nil and result.max_items ~= nil and result.min_items > result.max_items then
            error("items reader min_items may not exceed max_items", 2)
        end
    elseif kind == "presence" then
        local unknown = unknownKey("presence reader", value, { id = true, target = true, kind = true, confidence_floor = true })
        if unknown ~= nil then error(unknown, 2) end
    else
        error("reader kind must be line, block, items or presence", 2)
    end
    local frozen = table.freeze(result)
    minted.reader[frozen] = true
    return frozen :: any
end

local Surface = {}
model.Surface = Surface
function Surface.new(spec: { [string]: any }): Surface
    local value = requireTable("Surface.new", spec)
    local kind = requireString("surface kind", value.kind)
    local known = { id = true, kind = true, contexts = true, covers = true }
    local unknown = unknownKey("Surface.new", value, known)
    if unknown ~= nil then error(unknown, 2) end
    local contexts = stringList("surface contexts", value.contexts, kind == "interrupt", true)
    local result: { [string]: any } = {
        id = requireIdentifier("surface id", value.id),
        kind = kind,
        contexts = contexts,
    }
    if kind == "scene" then
        if #contexts == 0 then error("scene surface contexts may not be empty", 2) end
        if value.covers ~= nil then error("scene surface may not declare covers", 2) end
    elseif kind == "overlay" then
        if #contexts == 0 then error("overlay surface contexts may not be empty", 2) end
        result.covers = stringList("overlay surface covers", value.covers, false, true)
    elseif kind == "interrupt" then
        if value.covers ~= nil then error("interrupt surface may not declare covers", 2) end
    else
        error('surface kind must be "scene", "overlay" or "interrupt"', 2)
    end
    local frozen = table.freeze(result)
    minted.surface[frozen] = true
    return frozen
end

local Action = {}
model.Action = Action
function Action.new(spec: { [string]: any }): Action
    local value = requireTable("Action.new", spec)
    local kind = requireString("action kind", value.kind)
    local result: { [string]: any } = {
        id = requireIdentifier("action id", value.id),
        kind = kind,
    }
    if kind == "click" then
        local unknown = unknownKey("click action", value, { id = true, kind = true, locator = true, preconditions = true })
        if unknown ~= nil then error(unknown, 2) end
        result.locator = locatorRef("click action locator", value.locator)
    elseif kind == "key" then
        local unknown = unknownKey("key action", value, { id = true, kind = true, key = true, locator = true, preconditions = true })
        if unknown ~= nil then error(unknown, 2) end
        result.key = requireString("key action key", value.key)
        if #result.key > 64 then error("key action key must be at most 64 bytes", 2) end
        if value.locator ~= nil then result.locator = locatorRef("key action locator", value.locator) end
    elseif kind == "scroll" then
        local unknown = unknownKey("scroll action", value, { id = true, kind = true, axis = true, delta = true, locator = true, preconditions = true })
        if unknown ~= nil then error(unknown, 2) end
        result.axis = requireString("scroll action axis", value.axis)
        if result.axis ~= "horizontal" and result.axis ~= "vertical" then error("scroll action axis must be horizontal or vertical", 2) end
        result.delta = requireWhole("scroll action delta", value.delta)
        if result.delta == 0 then error("scroll action delta may not be zero", 2) end
        if value.locator ~= nil then result.locator = locatorRef("scroll action locator", value.locator) end
    elseif kind == "drag" then
        local unknown = unknownKey("drag action", value, { id = true, kind = true, locator = true, destination = true, preconditions = true })
        if unknown ~= nil then error(unknown, 2) end
        result.locator = locatorRef("drag action locator", value.locator)
        local destination = requireTable("drag action destination", value.destination)
        local destinationKind = requireString("drag action destination.kind", destination.kind)
        if destinationKind == "point" then
            local bad = unknownKey("drag action point destination", destination, { kind = true, point = true })
            if bad ~= nil then error(bad, 2) end
            result.destination = table.freeze({ kind = destinationKind, point = point("drag action destination.point", destination.point) })
        elseif destinationKind == "locator" then
            local bad = unknownKey("drag action locator destination", destination, { kind = true, locator = true })
            if bad ~= nil then error(bad, 2) end
            result.destination = table.freeze({ kind = destinationKind, locator = locatorRef("drag action destination.locator", destination.locator) })
        else
            error('drag action destination.kind must be "point" or "locator"', 2)
        end
    else
        error("action kind must be click, key, scroll or drag", 2)
    end
    result.preconditions = preconditions("action preconditions", value.preconditions)
    local frozen = table.freeze(result)
    minted.action[frozen] = true
    return frozen
end

local Binding = {}
model.Binding = Binding
function Binding.new(spec: { [string]: any }): Binding
    local value = requireTable("Binding.new", spec)
    local unknown = unknownKey("Binding.new", value, {
        id = true, surface = true, target = true, placement = true,
        identity = true, readers = true, actions = true,
    })
    if unknown ~= nil then error(unknown, 2) end
    local actions = denseList("binding actions", value.actions, true)
    local actionCopies: { Action } = {}
    local actionIds: { [string]: boolean } = {}
    for index = 1, #actions do
        local action = Action.new(actions[index])
        if actionIds[action.id] then error("binding repeats action '" .. action.id .. "'", 2) end
        actionIds[action.id] = true
        actionCopies[index] = action
    end
    local result = table.freeze({
        id = requireIdentifier("binding id", value.id),
        surface = requireIdentifier("binding surface", value.surface),
        target = requireIdentifier("binding target", value.target),
        placement = placement("binding placement", value.placement),
        identity = identity("binding identity", value.identity),
        readers = stringList("binding readers", value.readers, true, true),
        actions = table.freeze(actionCopies),
    })
    minted.binding[result] = true
    return result :: any
end

local Transition = {}
model.Transition = Transition
function Transition.new(spec: { [string]: any }): Transition
    local value = requireTable("Transition.new", spec)
    local unknown = unknownKey("Transition.new", value, { id = true, from = true, trigger = true, to = true, verification = true })
    if unknown ~= nil then error(unknown, 2) end
    local triggerValue = requireTable("transition trigger", value.trigger)
    local triggerKind = requireString("transition trigger.kind", triggerValue.kind)
    local trigger: { [string]: any } = { kind = triggerKind }
    if triggerKind == "action" then
        local bad = unknownKey("action transition trigger", triggerValue, { kind = true, binding = true, action = true })
        if bad ~= nil then error(bad, 2) end
        trigger.binding = requireIdentifier("transition trigger binding", triggerValue.binding)
        trigger.action = requireIdentifier("transition trigger action", triggerValue.action)
    elseif triggerKind == "timeout" then
        local bad = unknownKey("timeout transition trigger", triggerValue, { kind = true, milliseconds = true })
        if bad ~= nil then error(bad, 2) end
        trigger.milliseconds = nonNegative("transition timeout milliseconds", triggerValue.milliseconds)
    elseif triggerKind == "external" then
        local bad = unknownKey("external transition trigger", triggerValue, { kind = true, name = true })
        if bad ~= nil then error(bad, 2) end
        trigger.name = requireIdentifier("transition external name", triggerValue.name)
    else
        error('transition trigger.kind must be "action", "timeout" or "external"', 2)
    end
    local verificationValue = requireTable("transition verification", value.verification)
    local verificationUnknown = unknownKey("transition verification", verificationValue, { kind = true, timeout_ms = true, attempts = true })
    if verificationUnknown ~= nil then error(verificationUnknown, 2) end
    if verificationValue.kind ~= "resolve" then error('transition verification.kind must be "resolve"', 2) end
    local result = table.freeze({
        id = requireIdentifier("transition id", value.id),
        from = statePattern("transition from", value.from),
        trigger = table.freeze(trigger),
        to = statePattern("transition to", value.to),
        verification = table.freeze({
            kind = "resolve",
            timeout_ms = nonNegative("transition verification timeout_ms", verificationValue.timeout_ms),
            attempts = positive("transition verification attempts", verificationValue.attempts),
        }),
    })
    minted.transition[result] = true
    return result :: any
end

local function mapById<T>(what: string, list: { T }, kind: string): { [string]: T }
    local map: { [string]: T } = {}
    for index = 1, #list do
        local object: any = list[index]
        if type(object) ~= "table" or type(object.id) ~= "string" then
            error(what .. "[" .. tostring(index) .. "] is not a framework object", 3)
        end
        if map[object.id] ~= nil then error(kind .. " '" .. object.id .. "' is declared twice", 3) end
        map[object.id] = object
    end
    return map
end

local function contains(list: { string }, value: string): boolean
    for index = 1, #list do if list[index] == value then return true end end
    return false
end

local function requireMapObject(what: string, map: { [string]: any }, id: string): any
    local object = map[id]
    if object == nil then error(what .. ' names unknown id "' .. id .. '"', 3) end
    return object
end

local function validateRef(
    what: string,
    ref: LocatorRef,
    targetId: string,
    locators: { [string]: Locator },
    variants: { [string]: LocatorVariant }
)
    local locator = requireMapObject(what .. ".locator", locators, ref.locator) :: Locator
    if locator.target ~= targetId then error(what .. ' locator belongs to target "' .. locator.target .. '", not "' .. targetId .. '"', 3) end
    if ref.variant ~= nil then
        local variant = requireMapObject(what .. ".variant", variants, ref.variant) :: LocatorVariant
        if variant.locator ~= locator.id then error(what .. " variant does not belong to its locator", 3) end
    end
end

local function validatePredicate(
    what: string,
    predicate: any,
    targetId: string,
    locators: { [string]: Locator },
    variants: { [string]: LocatorVariant },
    readers: { [string]: Reader }
)
    if predicate.locator ~= nil then
        validateRef(what, predicate.locator, targetId, locators, variants)
    else
        requireMapObject(what .. ".reader", readers, predicate.reader)
        if readers[predicate.reader].target ~= targetId then error(what .. " reader is not owned by the binding target", 3) end
    end
end

local function validateState(
    what: string,
    state: any,
    contextById: { [string]: Context },
    surfaceById: { [string]: any }
)
    requireMapObject(what .. ".context", contextById, state.context)
    if #state.surfaces == 0 then error(what .. ".surfaces may not be empty", 3) end
    local previous: any = nil
    for index = 1, #state.surfaces do
        local surface = requireMapObject(what .. ".surfaces", surfaceById, state.surfaces[index])
        if index == 1 then
            if surface.kind ~= "scene" then error(what .. " must start with a scene surface", 3) end
            if not contains(surface.contexts, state.context) then error(what .. " scene does not support its context", 3) end
        elseif surface.kind == "scene" then
            error(what .. " may contain only one scene at the bottom", 3)
        elseif surface.kind == "overlay" then
            if previous.kind == "interrupt" then error(what .. " may not place an overlay above an interrupt", 3) end
            if not contains(surface.covers, previous.id) then error(what .. " overlay does not cover the preceding surface", 3) end
            if not contains(surface.contexts, state.context) then error(what .. " overlay does not support its context", 3) end
        elseif surface.kind == "interrupt" then
            if index ~= #state.surfaces then error(what .. " interrupt must be the top surface", 3) end
            if #surface.contexts > 0 and not contains(surface.contexts, state.context) then error(what .. " interrupt does not support its context", 3) end
        end
        previous = surface
    end
end

local RuntimeModel = {}
model.RuntimeModel = RuntimeModel
function RuntimeModel.new(spec: { [string]: any }): RuntimeModel
    local value = requireTable("RuntimeModel.new", spec)
    local unknown = unknownKey("RuntimeModel.new", value, {
        schema_version = true, base_resolution = true, base_dpi = true,
        contexts = true, targets = true, locators = true, locator_variants = true,
        readers = true, surfaces = true, bindings = true, transitions = true,
    })
    if unknown ~= nil then error(unknown, 2) end
    if value.schema_version ~= 1 then error("runtime model schema_version must be 1", 2) end
    local contexts: { Context } = {}
    for index, entry in denseList("runtime model contexts", value.contexts, false) do contexts[index] = if model.is_context(entry) then entry else Context.new(entry) end end
    local targets: { Target } = {}
    for index, entry in denseList("runtime model targets", value.targets, false) do targets[index] = if model.is_target(entry) then entry else Target.new(entry) end end
    local locators: { Locator } = {}
    for index, entry in denseList("runtime model locators", value.locators, false) do locators[index] = if model.is_locator(entry) then entry else Locator.new(entry) end end
    local variants: { LocatorVariant } = {}
    for index, entry in denseList("runtime model locator_variants", value.locator_variants, true) do variants[index] = if model.is_locator_variant(entry) then entry else LocatorVariant.new(entry) end end
    local readers: { Reader } = {}
    for index, entry in denseList("runtime model readers", value.readers, true) do readers[index] = if model.is_reader(entry) then entry else Reader.new(entry) end end
    local surfaces: { any } = {}
    for index, entry in denseList("runtime model surfaces", value.surfaces, false) do surfaces[index] = if model.is_surface(entry) then entry else Surface.new(entry) end end
    local bindings: { Binding } = {}
    for index, entry in denseList("runtime model bindings", value.bindings, false) do bindings[index] = if model.is_binding(entry) then entry else Binding.new(entry) end end
    local transitions: { Transition } = {}
    for index, entry in denseList("runtime model transitions", value.transitions, true) do transitions[index] = if model.is_transition(entry) then entry else Transition.new(entry) end end

    local contextById = mapById("contexts", contexts, "context")
    local targetById = mapById("targets", targets, "target")
    local locatorById = mapById("locators", locators, "locator")
    local variantById = mapById("locator variants", variants, "locator variant")
    local readerById = mapById("readers", readers, "reader")
    local surfaceById = mapById("surfaces", surfaces, "surface")
    local bindingById = mapById("bindings", bindings, "binding")
    local transitionById = mapById("transitions", transitions, "transition")

    local result: any = {
        schema_version = 1,
        base_resolution = size("runtime model base_resolution", value.base_resolution),
        base_dpi = size("runtime model base_dpi", value.base_dpi),
        contexts = table.freeze(contexts), targets = table.freeze(targets),
        locators = table.freeze(locators), locator_variants = table.freeze(variants),
        readers = table.freeze(readers), surfaces = table.freeze(surfaces),
        bindings = table.freeze(bindings), transitions = table.freeze(transitions),
        context_by_id = table.freeze(contextById), target_by_id = table.freeze(targetById),
        locator_by_id = table.freeze(locatorById), locator_variant_by_id = table.freeze(variantById),
        reader_by_id = table.freeze(readerById), surface_by_id = table.freeze(surfaceById),
        binding_by_id = table.freeze(bindingById), transition_by_id = table.freeze(transitionById),
    }

    for _, target in targets do
        if target.geometry ~= nil and target.geometry.kind == "relative" then
            validateRef(
                "target '" .. target.id .. "' geometry anchor",
                target.geometry.anchor,
                target.id,
                locatorById,
                variantById
            )
        end
        for _, locatorId in target.locators do
            local locator = requireMapObject("target locators", locatorById, locatorId) :: Locator
            if locator.target ~= target.id then error("target '" .. target.id .. "' lists a locator owned by '" .. locator.target .. "'", 2) end
        end
        for _, readerId in target.readers do
            local reader = requireMapObject("target readers", readerById, readerId) :: Reader
            if reader.target ~= target.id then error("target '" .. target.id .. "' lists a reader owned by '" .. reader.target .. "'", 2) end
        end
    end
    for _, locator in locators do
        local target = requireMapObject("locator target", targetById, locator.target) :: Target
        if not contains(target.locators, locator.id) then error("locator '" .. locator.id .. "' is not listed by its target", 2) end
        if locator.kind == "text" then
            local reader = requireMapObject("text locator reader", readerById, locator.reader :: string) :: Reader
            if reader.target ~= target.id then error("text locator '" .. locator.id .. "' uses a reader from another target", 2) end
        elseif locator.kind == "template" and locator.variants ~= nil then
            for _, variantId in locator.variants do
                local variant = requireMapObject("locator variants", variantById, variantId) :: LocatorVariant
                if variant.locator ~= locator.id then error("locator '" .. locator.id .. "' lists a variant owned by another locator", 2) end
            end
        elseif locator.kind == "relative" then
            validateRef("relative locator anchor", locator.anchor :: LocatorRef, target.id, locatorById, variantById)
        elseif locator.kind == "collection" then
            validateRef(
                "collection item_locator",
                locator.item_locator :: LocatorRef,
                target.id,
                locatorById,
                variantById
            )
        end
    end
    for _, variant in variants do
        local locator = requireMapObject("locator variant locator", locatorById, variant.locator) :: Locator
        if locator.kind ~= "template" then error("locator variant '" .. variant.id .. "' belongs to a non-template locator", 2) end
        if locator.variants == nil or not contains(locator.variants, variant.id) then error("locator variant '" .. variant.id .. "' is not listed by its locator", 2) end
    end
    for _, reader in readers do
        local target = requireMapObject("reader target", targetById, reader.target) :: Target
        if not contains(target.readers, reader.id) then error("reader '" .. reader.id .. "' is not listed by its target", 2) end
    end
    for _, surface in surfaces do
        for _, contextId in surface.contexts do requireMapObject("surface context", contextById, contextId) end
        if surface.kind == "overlay" then
            for _, coveredId in surface.covers do
                local covered = requireMapObject("surface covers", surfaceById, coveredId)
                if covered.kind ~= "scene" and covered.kind ~= "overlay" then error("overlay '" .. surface.id .. "' may cover only scene or overlay surfaces", 2) end
                if covered.id == surface.id then error("overlay '" .. surface.id .. "' may not cover itself", 2) end
            end
        end
    end

    local coverVisit: { [string]: number } = {}
    local function visitSurface(id: string)
        local state = coverVisit[id]
        if state == 1 then
            error("surface covers relation contains a cycle at '" .. id .. "'", 2)
        end
        if state == 2 then return end
        coverVisit[id] = 1
        for _, coveredId in surfaceById[id].covers or {} do
            visitSurface(coveredId)
        end
        coverVisit[id] = 2
    end
    for _, surface in surfaces do visitSurface(surface.id) end

    local bindingsBySurface: { [string]: { Binding } } = {}
    local bindingsByTarget: { [string]: { Binding } } = {}
    for _, binding in bindings do
        requireMapObject("binding surface", surfaceById, binding.surface)
        local target = requireMapObject("binding target", targetById, binding.target) :: Target
        if binding.placement.kind == "target_geometry" and target.geometry == nil then error("binding '" .. binding.id .. "' uses target_geometry but its target has no geometry", 2) end
        if binding.placement.kind == "relative" then validateRef("binding placement anchor", binding.placement.anchor, target.id, locatorById, variantById) end
        local readerIds = target.readers
        for _, readerId in binding.readers do
            local reader = requireMapObject("binding reader", readerById, readerId) :: Reader
            if reader.target ~= target.id or not contains(readerIds, readerId) then error("binding '" .. binding.id .. "' exposes a reader outside its target", 2) end
        end
        local positiveIdentity = #binding.identity.all > 0 or #binding.identity.any > 0
        if positiveIdentity then
            for _, predicate in binding.identity.all do validatePredicate("binding identity", predicate, target.id, locatorById, variantById, readerById) end
            for _, predicate in binding.identity.any do validatePredicate("binding identity", predicate, target.id, locatorById, variantById, readerById) end
        end
        for _, predicate in binding.identity.none do validatePredicate("binding identity", predicate, target.id, locatorById, variantById, readerById) end
        for _, action in binding.actions do
            if action.locator ~= nil then validateRef("binding action locator", action.locator, target.id, locatorById, variantById) end
            for _, predicate in action.preconditions do validatePredicate("binding action precondition", predicate, target.id, locatorById, variantById, readerById) end
            if action.destination ~= nil and action.destination.kind == "locator" then validateRef("binding drag destination", action.destination.locator, target.id, locatorById, variantById) end
        end
        if bindingsBySurface[binding.surface] == nil then bindingsBySurface[binding.surface] = {} end
        table.insert(bindingsBySurface[binding.surface], binding)
        if bindingsByTarget[binding.target] == nil then bindingsByTarget[binding.target] = {} end
        table.insert(bindingsByTarget[binding.target], binding)
    end
    for _, surface in surfaces do
        local rows = bindingsBySurface[surface.id] or {}
        local positive = false
        for _, binding in rows do if #binding.identity.all > 0 or #binding.identity.any > 0 then positive = true end end
        if not positive then error("surface '" .. surface.id .. "' has no positive identity binding", 2) end
    end

    for _, transition in transitions do
        validateState("transition '" .. transition.id .. "' from", transition.from, contextById, surfaceById)
        validateState("transition '" .. transition.id .. "' to", transition.to, contextById, surfaceById)
        if transition.trigger.kind == "action" then
            local binding = requireMapObject("transition trigger binding", bindingById, transition.trigger.binding) :: Binding
            local actionFound = false
            for _, action in binding.actions do if action.id == transition.trigger.action then actionFound = true end end
            if not actionFound then error("transition '" .. transition.id .. "' names an action not owned by its binding", 2) end
            if not contains(transition.from.surfaces, binding.surface) then error("transition '" .. transition.id .. "' action binding is not present in its from state", 2) end
        end
    end

    local surfacesByKind: { [string]: { any } } = { scene = {}, overlay = {}, interrupt = {} }
    for _, surface in surfaces do table.insert(surfacesByKind[surface.kind], surface) end
    for key, list in surfacesByKind do surfacesByKind[key] = table.freeze(list) end
    for key, list in bindingsBySurface do bindingsBySurface[key] = table.freeze(list) end
    for key, list in bindingsByTarget do bindingsByTarget[key] = table.freeze(list) end
    local locatorVariantsByLocator: { [string]: { LocatorVariant } } = {}
    for _, variant in variants do
        local list = locatorVariantsByLocator[variant.locator]
        if list == nil then
            list = {}
            locatorVariantsByLocator[variant.locator] = list
        end
        table.insert(list, variant)
    end
    local readersByTarget: { [string]: { Reader } } = {}
    for _, reader in readers do
        local list = readersByTarget[reader.target]
        if list == nil then
            list = {}
            readersByTarget[reader.target] = list
        end
        table.insert(list, reader)
    end
    for key, list in locatorVariantsByLocator do locatorVariantsByLocator[key] = table.freeze(list) end
    for key, list in readersByTarget do readersByTarget[key] = table.freeze(list) end
    result.bindings_by_surface = table.freeze(bindingsBySurface)
    result.bindings_by_target = table.freeze(bindingsByTarget)
    result.locator_variants_by_locator = table.freeze(locatorVariantsByLocator)
    result.readers_by_target = table.freeze(readersByTarget)
    result.surfaces_by_kind = table.freeze(surfacesByKind)
    local frozen = table.freeze(result)
    minted.runtime_model[frozen] = true
    return frozen :: any
end

model.compile = RuntimeModel.new
model.LocatorRef = table.freeze({ new = locatorRef })

function model.attach_model_hash(value: RuntimeModel, hash: any): RuntimeModel
    if not model.is_runtime_model(value) then
        error("attach_model_hash needs a framework-minted RuntimeModel", 2)
    end
    modelHashes[value] = sha256("model hash", hash)
    return value
end

function model.model_hash(value: RuntimeModel): string?
    if not model.is_runtime_model(value) then
        error("model_hash needs a framework-minted RuntimeModel", 2)
    end
    return modelHashes[value]
end

return model
