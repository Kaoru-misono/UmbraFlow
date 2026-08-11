--!strict
-- Immutable RuntimeModel v2 compiler.
--
-- UiTarget is semantic identity only.  Every visual variant and every pixel
-- coordinate belongs to one Binding.  A Binding still needs positive measured
-- evidence: its placement is never evidence by itself.

local model = {}
model.schema = "umbraflow-runtime/v2"
model.schema_hash = "4e8a8bb5199823f15021a2e7800c454f9d9287a9900e247a0e96b6c16fbd78c8"

local runtimeModels: { [any]: boolean } = setmetatable({}, { __mode = "k" }) :: any
local semanticBytes: { [any]: string } = setmetatable({}, { __mode = "k" }) :: any

local function fail(what: string, message: string): never
    error(what .. " " .. message, 3)
end

local function object(what: string, value: any): { [any]: any }
    if type(value) ~= "table" then fail(what, "must be a table") end
    return value
end

local function only(what: string, value: { [any]: any }, allowed: { [string]: boolean })
    for key in value do
        if type(key) ~= "string" or not allowed[key] then
            fail(what, "has unsupported field '" .. tostring(key) .. "'")
        end
    end
end

local function dense(what: string, value: any): { any }
    local result = object(what, value)
    local count = #result
    for key in result do
        if type(key) ~= "number" or key % 1 ~= 0 or key < 1 or key > count then
            fail(what, "must be a dense array")
        end
    end
    return result :: { any }
end

local function finite(what: string, value: any): number
    if type(value) ~= "number" or value ~= value or value == math.huge or value == -math.huge then
        fail(what, "must be a finite number")
    end
    return value
end

local function integer(what: string, value: any): number
    local result = finite(what, value)
    if result % 1 ~= 0 then fail(what, "must be an integer") end
    return result
end

local function positiveInteger(what: string, value: any): number
    local result = integer(what, value)
    if result <= 0 then fail(what, "must be positive") end
    return result
end

local function unit(what: string, value: any): number
    local result = finite(what, value)
    if result < 0 or result > 1 then fail(what, "must be between 0 and 1") end
    return result
end

local function identifier(what: string, value: any): string
    if type(value) ~= "string" or string.match(value, "^[a-z][a-z0-9%._:%-]*$") == nil then
        fail(what, "must be an identifier")
    end
    return value
end

local function enum(what: string, value: any, allowed: { [string]: boolean }): string
    if type(value) ~= "string" or not allowed[value] then
        fail(what, "has unsupported value '" .. tostring(value) .. "'")
    end
    return value
end

local function stringList(what: string, value: any, nonEmpty: boolean?): { string }
    local result: { string } = {}
    local seen: { [string]: boolean } = {}
    for index, child in dense(what, value) do
        local item = identifier(what .. "[" .. tostring(index) .. "]", child)
        if seen[item] then fail(what, "contains duplicate '" .. item .. "'") end
        seen[item] = true
        result[index] = item
    end
    if nonEmpty and #result == 0 then fail(what, "must not be empty") end
    return table.freeze(result)
end

local function size(what: string, value: any): { number }
    local values = dense(what, value)
    if #values ~= 2 then fail(what, "must contain exactly two integers") end
    return table.freeze({
        positiveInteger(what .. "[1]", values[1]),
        positiveInteger(what .. "[2]", values[2]),
    })
end

local function rect(what: string, value: any, bounds: { number }): { number }
    local values = dense(what, value)
    if #values ~= 4 then fail(what, "must contain exactly four integers") end
    local x = integer(what .. "[1]", values[1])
    local y = integer(what .. "[2]", values[2])
    local width = positiveInteger(what .. "[3]", values[3])
    local height = positiveInteger(what .. "[4]", values[4])
    if x < 0 or y < 0 or x + width > bounds[1] or y + height > bounds[2] then
        fail(what, "must stay inside base_resolution")
    end
    return table.freeze({ x, y, width, height })
end

local function point(what: string, value: any, bounds: { number }): { number }
    local values = dense(what, value)
    if #values ~= 2 then fail(what, "must contain exactly two integers") end
    local x = integer(what .. "[1]", values[1])
    local y = integer(what .. "[2]", values[2])
    if x < 0 or y < 0 or x >= bounds[1] or y >= bounds[2] then
        fail(what, "must stay inside base_resolution")
    end
    return table.freeze({ x, y })
end

local function assetPath(what: string, value: any): string
    if type(value) ~= "string" or string.sub(value, 1, 7) ~= "assets/" then
        fail(what, "must be a path below assets/")
    end
    if string.find(value, "\\", 1, true) ~= nil or string.find(value, "//", 1, true) ~= nil then
        fail(what, "must be a confined forward-slash path")
    end
    for segment in string.gmatch(value .. "/", "([^/]*)/") do
        if segment == "" or segment == "." or segment == ".." then
            fail(what, "must not contain empty, '.' or '..' segments")
        end
        if string.match(segment, "^[A-Za-z0-9_%-][A-Za-z0-9%._%-]*$") == nil then
            fail(what, "contains an unsupported path segment")
        end
    end
    return value
end

local function records(what: string, value: any, build: (string, any) -> any): { any }
    local result = {}
    for index, child in dense(what, value) do
        result[index] = build(what .. "[" .. tostring(index) .. "]", child)
    end
    return table.freeze(result)
end

-- Canonical, type-tagged encoding of the schema-owned tree. Length prefixes
-- make it unambiguous without depending on table iteration or JSON formatting.
local function canonical(value: any): string
    local kind = type(value)
    if kind == "string" then return "s" .. tostring(#value) .. ":" .. value end
    if kind == "number" then
        if value == 0 then return "n0;" end
        return "n" .. string.format("%.17g", value) .. ";"
    end
    if kind == "boolean" then return if value then "b1" else "b0" end
    if kind ~= "table" then fail("RuntimeModel", "contains a non-canonical value") end

    local count = #value
    local array = count > 0
    if count == 0 then
        array = next(value) == nil
    end
    if array then
        local result = { "a", tostring(count), ":" }
        for index = 1, count do table.insert(result, canonical(value[index])) end
        return table.concat(result)
    end

    local keys: { string } = {}
    for key in value do
        if type(key) ~= "string" then fail("RuntimeModel", "contains a non-string object key") end
        table.insert(keys, key)
    end
    table.sort(keys)
    local result = { "o", tostring(#keys), ":" }
    for _, key in keys do
        table.insert(result, canonical(key))
        table.insert(result, canonical(value[key]))
    end
    return table.concat(result)
end

local function mapById(what: string, values: { any }): { [string]: any }
    local result: { [string]: any } = {}
    for _, value in values do
        if result[value.id] ~= nil then fail(what, "contains duplicate id '" .. value.id .. "'") end
        result[value.id] = value
    end
    return table.freeze(result)
end

local function requireRef(what: string, index: { [string]: any }, id: string): any
    local result = index[id]
    if result == nil then fail(what, "references unknown id '" .. id .. "'") end
    return result
end

-- One declared vocabulary, in the order the Host will compare it in. Sorted and
-- deduplicated here because the Host holds these as opaque strings and cannot
-- re-derive them: two orderings of one model would be two spellings of one fact.
local function sortedUnique(values: { string }): { string }
    local seen: { [string]: boolean } = {}
    local result = {}
    for _, value in values do
        if not seen[value] then
            seen[value] = true
            table.insert(result, value)
        end
    end
    table.sort(result)
    return table.freeze(result)
end

function model.predicate_key(value: any): string
    local source = object("predicate", value)
    if source.kind == "locator_present" then
        return "locator:" .. identifier("predicate.locator", source.locator)
    end
    if source.kind == "text_equals" then
        if type(source.value) ~= "string" then fail("predicate.value", "must be a string") end
        return "text:" .. identifier("predicate.reader", source.reader) .. ":" .. source.value
    end
    fail("predicate.kind", "must be locator_present or text_equals")
end

function model.measurement_key(bindingId: any, value: any): string
    return "binding:" .. identifier("binding_id", bindingId) .. ":" .. model.predicate_key(value)
end

-- The key one reported reading is cached and looked up under.  It shares the
-- `binding:` prefix with measurement_key and can never collide with one: a
-- measurement key continues with `locator:` or `text:`, a reading key with
-- `read:`, and neither is a legal identifier so no id can spell the other.
function model.reading_key(bindingId: any, readerId: any): string
    return "binding:"
        .. identifier("binding_id", bindingId)
        .. ":read:"
        .. identifier("reader_id", readerId)
end

local function predicate(what: string, value: any): any
    local source = object(what, value)
    if source.kind == "locator_present" then
        only(what, source, { kind = true, locator = true })
        return table.freeze({
            kind = "locator_present",
            locator = identifier(what .. ".locator", source.locator),
        })
    end
    if source.kind == "text_equals" then
        only(what, source, { kind = true, reader = true, value = true })
        if type(source.value) ~= "string" then fail(what .. ".value", "must be a string") end
        return table.freeze({
            kind = "text_equals",
            reader = identifier(what .. ".reader", source.reader),
            value = source.value,
        })
    end
    fail(what .. ".kind", "must be locator_present or text_equals")
end

local function predicates(what: string, value: any, seen: { [string]: boolean }): { any }
    local result = {}
    for index, child in dense(what, value) do
        local item = predicate(what .. "[" .. tostring(index) .. "]", child)
        local key = model.predicate_key(item)
        if seen[key] then fail(what, "repeats predicate '" .. key .. "'") end
        seen[key] = true
        result[index] = item
    end
    return table.freeze(result)
end

local function detector(what: string, value: any): any
    local source = object(what, value)
    only(what, source, { all = true, any = true, none = true })
    local seen: { [string]: boolean } = {}
    local result = table.freeze({
        all = predicates(what .. ".all", source.all, seen),
        any = predicates(what .. ".any", source.any, seen),
        none = predicates(what .. ".none", source.none, seen),
    })
    if #result.all + #result.any == 0 then
        fail(what, "needs positive measured evidence in all or any")
    end
    return result
end

local function identity(what: string, value: any): any
    local source = object(what, value)
    only(what, source, { all = true, any = true, none = true })
    local result = table.freeze({
        all = stringList(what .. ".all", source.all),
        any = stringList(what .. ".any", source.any),
        none = stringList(what .. ".none", source.none),
    })
    if #result.all + #result.any == 0 then
        fail(what, "needs a positive Binding in all or any")
    end
    local seen: { [string]: boolean } = {}
    for _, group in { result.all, result.any, result.none } do
        for _, id in group do
            if seen[id] then fail(what, "repeats Binding '" .. id .. "'") end
            seen[id] = true
        end
    end
    return result
end

local function uiTarget(what: string, value: any): any
    local source = object(what, value)
    only(what, source, { id = true, kind = true })
    return table.freeze({
        id = identifier(what .. ".id", source.id),
        kind = enum(what .. ".kind", source.kind, { control = true, region = true }),
    })
end

local function locator(what: string, value: any): any
    local source = object(what, value)
    only(what, source, { id = true, kind = true, asset_path = true, threshold = true })
    if source.kind ~= "template" then fail(what .. ".kind", "must be template") end
    return table.freeze({
        id = identifier(what .. ".id", source.id),
        kind = "template",
        asset_path = assetPath(what .. ".asset_path", source.asset_path),
        threshold = unit(what .. ".threshold", source.threshold),
    })
end

local function reader(what: string, value: any): any
    local source = object(what, value)
    only(what, source, { id = true, kind = true, confidence_floor = true, normalization = true })
    if source.kind ~= "text" then fail(what .. ".kind", "must be text") end
    return table.freeze({
        id = identifier(what .. ".id", source.id),
        kind = "text",
        confidence_floor = unit(what .. ".confidence_floor", source.confidence_floor),
        normalization = enum(what .. ".normalization", source.normalization, {
            raw = true,
            trim = true,
            collapse_whitespace = true,
        }),
    })
end

local function placementBuilder(bounds: { number }): (string, any) -> any
    return function(what: string, value: any): any
        local source = object(what, value)
        only(what, source, { kind = true, rect = true, action_point = true })
        if source.kind ~= "fixed" then fail(what .. ".kind", "must be fixed") end
        local area = rect(what .. ".rect", source.rect, bounds)
        local actionPoint = if source.action_point == nil
            then nil
            else point(what .. ".action_point", source.action_point, bounds)
        if actionPoint ~= nil then
            if actionPoint[1] < area[1] or actionPoint[1] >= area[1] + area[3]
                or actionPoint[2] < area[2] or actionPoint[2] >= area[2] + area[4]
            then
                fail(what .. ".action_point", "must lie inside placement.rect")
            end
        end
        return table.freeze({ kind = "fixed", rect = area, action_point = actionPoint })
    end
end

-- One action a Binding grants, in one of the two kinds a target offers.
--
-- A click names a coordinate and a keystroke names a key, and neither invents
-- the other: a key action carries no point, which is why placement.action_point
-- below is conditional on the kinds present rather than on there being actions
-- at all.
--
-- WHICH key names exist is deliberately not decided here. That vocabulary is
-- closed and has one definition, in the trusted C++ KeyName, which refuses a
-- name outside it and prints the whole set it refused against; the Host applies
-- it to every key this model declares before the artifact is bound. A second
-- copy of the set in Luau would be two vocabularies agreeing by test rather than
-- by construction.
local function action(what: string, value: any): any
    local source = object(what, value)
    if source.kind == "click" then
        only(what, source, { id = true, kind = true, proof_locator = true })
        return table.freeze({
            id = identifier(what .. ".id", source.id),
            kind = "click",
            proof_locator = identifier(what .. ".proof_locator", source.proof_locator),
        })
    end
    if source.kind == "key" then
        only(what, source, { id = true, kind = true, key = true, proof_locator = true })
        if type(source.key) ~= "string" or #source.key == 0 then
            fail(what .. ".key", "must name a key")
        end
        return table.freeze({
            id = identifier(what .. ".id", source.id),
            kind = "key",
            key = source.key,
            proof_locator = identifier(what .. ".proof_locator", source.proof_locator),
        })
    end
    fail(what .. ".kind", "must be click or key")
end

local function bindingBuilder(bounds: { number }): (string, any) -> any
    local buildPlacement = placementBuilder(bounds)
    return function(what: string, value: any): any
        local source = object(what, value)
        only(what, source, {
            id = true,
            surface = true,
            ui_target = true,
            variant = true,
            placement = true,
            detector = true,
            actions = true,
            reads = true,
        })
        local actions = records(what .. ".actions", source.actions, action)
        mapById(what .. ".actions", actions)
        local placement = buildPlacement(what .. ".placement", source.placement)
        -- The point exists exactly when something aims at it. A Binding that
        -- grants only keystrokes is in the same position as one granting no
        -- action: nothing here is a coordinate, so carrying one would put a
        -- number in the model that nothing measures and nothing delivers.
        local aimed = false
        for _, valueAction in actions do
            if valueAction.kind == "click" then aimed = true end
        end
        if aimed and placement.action_point == nil then
            fail(what .. ".placement", "needs action_point when a click is granted")
        end
        if not aimed and placement.action_point ~= nil then
            fail(what .. ".placement.action_point", "is not allowed without a click")
        end
        -- Absent reads is the empty list and not a second shape: a Binding that
        -- reports nothing has nothing to say here, and requiring every Binding
        -- to write `reads = []` would put a field on every row to record its
        -- own absence.
        local reads = if source.reads == nil
            then table.freeze({})
            else stringList(what .. ".reads", source.reads)
        return table.freeze({
            id = identifier(what .. ".id", source.id),
            surface = identifier(what .. ".surface", source.surface),
            ui_target = identifier(what .. ".ui_target", source.ui_target),
            variant = identifier(what .. ".variant", source.variant),
            placement = placement,
            detector = detector(what .. ".detector", source.detector),
            actions = actions,
            reads = reads,
        })
    end
end

local function surface(what: string, value: any): any
    local source = object(what, value)
    only(what, source, { id = true, kind = true, covers = true, identity = true })
    local kind = enum(what .. ".kind", source.kind, { scene = true, overlay = true, interrupt = true })
    local covers = stringList(what .. ".covers", source.covers)
    if kind == "scene" and #covers ~= 0 then fail(what .. ".covers", "must be empty for a scene") end
    if kind ~= "scene" and #covers == 0 then fail(what .. ".covers", "must name a lower Surface") end
    return table.freeze({
        id = identifier(what .. ".id", source.id),
        kind = kind,
        covers = covers,
        identity = identity(what .. ".identity", source.identity),
    })
end

local function transition(what: string, value: any): any
    local source = object(what, value)
    only(what, source, { id = true, from_surfaces = true, trigger = true, to_surfaces = true })
    local trigger = object(what .. ".trigger", source.trigger)
    only(what .. ".trigger", trigger, { binding = true, action = true })
    return table.freeze({
        id = identifier(what .. ".id", source.id),
        from_surfaces = stringList(what .. ".from_surfaces", source.from_surfaces, true),
        trigger = table.freeze({
            binding = identifier(what .. ".trigger.binding", trigger.binding),
            action = identifier(what .. ".trigger.action", trigger.action),
        }),
        to_surfaces = stringList(what .. ".to_surfaces", source.to_surfaces, true),
    })
end

local function detectorRefs(
    what: string,
    value: any,
    locators: { [string]: any },
    readers: { [string]: any }
)
    for _, group in { value.all, value.any, value.none } do
        for _, item in group do
            if item.kind == "locator_present" then
                requireRef(what, locators, item.locator)
            else
                requireRef(what, readers, item.reader)
            end
        end
    end
end

function model.surface_covers(runtimeModel: any, upperId: string, lowerId: string): boolean
    if not runtimeModels[runtimeModel] then fail("surface_covers", "needs RuntimeModel v2") end
    local visited: { [string]: boolean } = {}
    local function visit(id: string): boolean
        if id == lowerId then return true end
        if visited[id] then return false end
        visited[id] = true
        local current = runtimeModel.surface_by_id[id]
        if current == nil then return false end
        for _, covered in current.covers do
            if visit(covered) then return true end
        end
        return false
    end
    return upperId ~= lowerId and visit(upperId)
end

function model.valid_surface_stack(runtimeModel: any, value: any): boolean
    if not runtimeModels[runtimeModel] or type(value) ~= "table" or #value == 0 then return false end
    local seen: { [string]: boolean } = {}
    for index, id in value do
        local current = runtimeModel.surface_by_id[id]
        if current == nil or seen[id] then return false end
        seen[id] = true
        if index == 1 then
            if current.kind ~= "scene" then return false end
        else
            if current.kind == "scene" then return false end
            local directlyCovers = false
            for _, covered in current.covers do
                if covered == value[index - 1] then directlyCovers = true end
            end
            if not directlyCovers then return false end
        end
    end
    return true
end

function model.compile(value: any): any
    local source = object("RuntimeModel", value)
    only("RuntimeModel", source, {
        schema_version = true,
        base_resolution = true,
        base_dpi = true,
        ui_targets = true,
        locators = true,
        readers = true,
        bindings = true,
        surfaces = true,
        transitions = true,
    })
    if source.schema_version ~= 2 then fail("RuntimeModel.schema_version", "must be 2") end

    local baseResolution = size("RuntimeModel.base_resolution", source.base_resolution)
    local baseDpi = size("RuntimeModel.base_dpi", source.base_dpi)
    local uiTargets = records("RuntimeModel.ui_targets", source.ui_targets, uiTarget)
    local locators = records("RuntimeModel.locators", source.locators, locator)
    local readers = records("RuntimeModel.readers", source.readers, reader)
    local bindings = records("RuntimeModel.bindings", source.bindings, bindingBuilder(baseResolution))
    local surfaces = records("RuntimeModel.surfaces", source.surfaces, surface)
    local transitions = records("RuntimeModel.transitions", source.transitions, transition)
    if #uiTargets == 0 then fail("RuntimeModel.ui_targets", "must not be empty") end
    if #surfaces == 0 then fail("RuntimeModel.surfaces", "must not be empty") end

    local targetById = mapById("RuntimeModel.ui_targets", uiTargets)
    local locatorById = mapById("RuntimeModel.locators", locators)
    local readerById = mapById("RuntimeModel.readers", readers)
    local bindingById = mapById("RuntimeModel.bindings", bindings)
    local surfaceById = mapById("RuntimeModel.surfaces", surfaces)
    local transitionById = mapById("RuntimeModel.transitions", transitions)

    local assetPaths = {}
    local seenAssetPath: { [string]: boolean } = {}
    for _, valueLocator in locators do
        if not seenAssetPath[valueLocator.asset_path] then
            seenAssetPath[valueLocator.asset_path] = true
            table.insert(assetPaths, valueLocator.asset_path)
        end
    end
    table.sort(assetPaths)
    table.freeze(assetPaths)

    -- The identifiers this model defines, as three flat vocabularies. They exist
    -- because the Operator has to refuse a workflow step naming UI no model
    -- declares, and C++ may not read RuntimeModel semantics to find out: the
    -- compiler that already cross-references these ids publishes them, and the
    -- Operator only asks whether a string is in a list.
    local surfaceIds = {}
    for _, valueSurface in surfaces do table.insert(surfaceIds, valueSurface.id) end
    local uiTargetIds = {}
    for _, valueTarget in uiTargets do table.insert(uiTargetIds, valueTarget.id) end
    local actionIds = {}
    -- Every key this model would ever press, published for one purpose: the
    -- Host holds the single definition of which key names exist and refuses the
    -- artifact here rather than at the moment an action is taken. A deployment
    -- that can never press its key is a broken deployment, and the artifact
    -- boundary is where a model is checked.
    local keyNames = {}
    for _, valueBinding in bindings do
        for _, valueAction in valueBinding.actions do
            table.insert(actionIds, valueAction.id)
            if valueAction.kind == "key" then
                table.insert(keyNames, valueAction.key)
            end
        end
    end

    local bindingsBySurface: { [string]: { any } } = {}
    for _, valueBinding in bindings do
        requireRef("binding '" .. valueBinding.id .. "'", surfaceById, valueBinding.surface)
        local target = requireRef("binding '" .. valueBinding.id .. "'", targetById, valueBinding.ui_target)
        detectorRefs("binding '" .. valueBinding.id .. "' detector", valueBinding.detector, locatorById, readerById)
        for _, readerId in valueBinding.reads do
            requireRef("binding '" .. valueBinding.id .. "' reads", readerById, readerId)
        end
        if #valueBinding.actions > 0 and target.kind ~= "control" then
            fail("binding '" .. valueBinding.id .. "'", "cannot grant actions to a region UiTarget")
        end
        local positiveLocators: { [string]: boolean } = {}
        for _, group in { valueBinding.detector.all, valueBinding.detector.any } do
            for _, valuePredicate in group do
                if valuePredicate.kind == "locator_present" then
                    positiveLocators[valuePredicate.locator] = true
                end
            end
        end
        for _, valueAction in valueBinding.actions do
            requireRef("action '" .. valueAction.id .. "'", locatorById, valueAction.proof_locator)
            if not positiveLocators[valueAction.proof_locator] then
                fail("action '" .. valueAction.id .. "'", "proof_locator must be positive Binding evidence")
            end
        end
        local rows = bindingsBySurface[valueBinding.surface]
        if rows == nil then
            rows = {}
            bindingsBySurface[valueBinding.surface] = rows
        end
        table.insert(rows, valueBinding)
    end
    for id, rows in bindingsBySurface do bindingsBySurface[id] = table.freeze(rows) end

    for _, valueSurface in surfaces do
        for _, covered in valueSurface.covers do
            if covered == valueSurface.id then fail("surface '" .. valueSurface.id .. "'", "cannot cover itself") end
            requireRef("surface '" .. valueSurface.id .. "'", surfaceById, covered)
        end
        for _, group in { valueSurface.identity.all, valueSurface.identity.any, valueSurface.identity.none } do
            for _, bindingId in group do
                local valueBinding = requireRef("surface '" .. valueSurface.id .. "' identity", bindingById, bindingId)
                if valueBinding.surface ~= valueSurface.id then
                    fail("surface '" .. valueSurface.id .. "' identity", "must use a Binding on that Surface")
                end
            end
        end
    end

    local visiting: { [string]: boolean } = {}
    local visited: { [string]: boolean } = {}
    local function visitSurface(id: string)
        if visiting[id] then fail("RuntimeModel.surfaces", "cover graph contains a cycle at '" .. id .. "'") end
        if visited[id] then return end
        visiting[id] = true
        for _, covered in surfaceById[id].covers do visitSurface(covered) end
        visiting[id] = nil
        visited[id] = true
    end
    for _, valueSurface in surfaces do visitSurface(valueSurface.id) end

    local semanticModel = table.freeze({
        schema_version = 2,
        base_resolution = baseResolution,
        base_dpi = baseDpi,
        ui_targets = uiTargets,
        locators = locators,
        readers = readers,
        bindings = bindings,
        surfaces = surfaces,
        transitions = transitions,
    })
    local runtimeModel: any = table.freeze({
        schema_version = semanticModel.schema_version,
        base_resolution = semanticModel.base_resolution,
        base_dpi = semanticModel.base_dpi,
        ui_targets = semanticModel.ui_targets,
        locators = semanticModel.locators,
        readers = semanticModel.readers,
        bindings = semanticModel.bindings,
        surfaces = semanticModel.surfaces,
        transitions = semanticModel.transitions,
        asset_paths = assetPaths,
        declared_surface_ids = sortedUnique(surfaceIds),
        declared_ui_target_ids = sortedUnique(uiTargetIds),
        declared_action_ids = sortedUnique(actionIds),
        declared_key_names = sortedUnique(keyNames),
        ui_target_by_id = targetById,
        locator_by_id = locatorById,
        reader_by_id = readerById,
        binding_by_id = bindingById,
        surface_by_id = surfaceById,
        transition_by_id = transitionById,
        bindings_by_surface = table.freeze(bindingsBySurface),
    })
    runtimeModels[runtimeModel] = true
    semanticBytes[runtimeModel] = canonical(semanticModel)

    local sceneCount = 0
    for _, valueSurface in surfaces do
        if valueSurface.kind == "scene" then sceneCount += 1 end
    end
    if sceneCount == 0 then fail("RuntimeModel.surfaces", "needs at least one scene") end

    for _, valueTransition in transitions do
        if not model.valid_surface_stack(runtimeModel, valueTransition.from_surfaces) then
            fail("transition '" .. valueTransition.id .. ".from_surfaces", "is not a valid Surface stack")
        end
        if not model.valid_surface_stack(runtimeModel, valueTransition.to_surfaces) then
            fail("transition '" .. valueTransition.id .. ".to_surfaces", "is not a valid Surface stack")
        end
        local triggerBinding = requireRef(
            "transition '" .. valueTransition.id .. "' trigger",
            bindingById,
            valueTransition.trigger.binding
        )
        local found = false
        for _, valueAction in triggerBinding.actions do
            if valueAction.id == valueTransition.trigger.action then found = true end
        end
        if not found then
            fail("transition '" .. valueTransition.id .. "' trigger", "references an unknown Binding action")
        end
    end

    return runtimeModel
end

function model.is_runtime_model(value: any): boolean
    return runtimeModels[value] == true
end

function model.canonical_bytes(value: any): string
    local bytes = semanticBytes[value]
    if bytes == nil then fail("canonical_bytes", "needs RuntimeModel v2") end
    return bytes
end

return table.freeze(model)
