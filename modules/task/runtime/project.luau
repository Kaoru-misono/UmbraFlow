--!strict
-- project: parse and compile the v1 runtime page-model.toml.
--
-- This is intentionally a strict, purpose-built TOML subset.  It accepts the
-- runtime schema's record tables and inline tables, and rejects every unknown
-- section and field.  Annotation corpus data is not a second input format:
-- screenshots, observations and review records belong to the offline tool.

local model = model

local project = {}
project.schema = "umbraflow-runtime/v1"
project.file_name = "page-model.toml"

local sectionKinds: { [string]: boolean } = {
    context = true,
    target = true,
    locator = true,
    locator_variant = true,
    reader = true,
    surface = true,
    binding = true,
    transition = true,
}

local rootFields = {
    schema_version = true,
    base_resolution = true,
    base_dpi = true,
}

local nestedTables: { [string]: boolean } = {
    ["target.geometry"] = true,
    ["locator.asset"] = true,
    ["locator_variant.asset"] = true,
    ["binding.placement"] = true,
    ["binding.identity"] = true,
    ["transition.from"] = true,
    ["transition.trigger"] = true,
    ["transition.to"] = true,
    ["transition.verification"] = true,
}

local nestedFields: { [string]: { [string]: boolean } } = {
    ["target.geometry"] = { kind = true, rect = true, axis = true, item_size = true, spacing = true, max_items = true, anchor = true, offset = true },
    ["locator.asset"] = { path = true, sha256 = true },
    ["locator_variant.asset"] = { path = true, sha256 = true },
    ["binding.placement"] = { kind = true, rect = true, anchor = true, offset = true },
    ["binding.identity"] = { all = true, any = true, none = true },
    ["transition.from"] = { context = true, surfaces = true },
    ["transition.trigger"] = { kind = true, binding = true, action = true, milliseconds = true, name = true },
    ["transition.to"] = { context = true, surfaces = true },
    ["transition.verification"] = { kind = true, timeout_ms = true, attempts = true },
}

local sectionFields: { [string]: { [string]: boolean } } = {
    context = { id = true },
    target = { id = true, kind = true, geometry = true, locators = true, readers = true },
    locator = {
        id = true, target = true, kind = true, asset = true, threshold = true,
        search_rect = true, variants = true, reader = true, rect = true,
        anchor = true, offset = true, item_locator = true, axis = true,
        spacing = true, max_items = true,
    },
    locator_variant = { id = true, locator = true, asset = true, threshold = true, search_rect = true },
    reader = {
        id = true, target = true, kind = true, confidence_floor = true,
        normalization = true, join = true, item_kind = true, min_items = true,
        max_items = true,
    },
    surface = { id = true, kind = true, contexts = true, covers = true },
    binding = { id = true, surface = true, target = true, placement = true, identity = true, readers = true, actions = true },
    transition = { id = true, from = true, trigger = true, to = true, verification = true },
}

local function trim(text: string): string
    return (string.gsub(text, "^%s*(.-)%s*$", "%1"))
end

local function stripComment(text: string): string
    local quoted = false
    local escaped = false
    for index = 1, #text do
        local character = string.sub(text, index, index)
        if quoted then
            if escaped then escaped = false
            elseif character == "\\" then escaped = true
            elseif character == '"' then quoted = false end
        elseif character == '"' then
            quoted = true
        elseif character == "#" then
            return string.sub(text, 1, index - 1)
        end
    end
    return text
end

local function logicalStatements(text: string): { any }
    local result: { any } = {}
    local pending = ""
    local pendingLine = 0
    local lineNumber = 0
    for raw in string.gmatch(text .. "\n", "([^\n]*)\n") do
        lineNumber += 1
        local line = trim(stripComment(raw))
        if line ~= "" then
            if pending == "" then
                pending = line
                pendingLine = lineNumber
            else
                pending ..= " " .. line
            end
            local balance = 0
            local quoted = false
            local escaped = false
            for index = 1, #pending do
                local character = string.sub(pending, index, index)
                if quoted then
                    if escaped then escaped = false
                    elseif character == "\\" then escaped = true
                    elseif character == '"' then quoted = false end
                elseif character == '"' then
                    quoted = true
                elseif character == "[" or character == "{" then
                    balance += 1
                elseif character == "]" or character == "}" then
                    balance -= 1
                end
            end
            if balance < 0 then error("line " .. tostring(lineNumber) .. " has an unmatched closing delimiter", 0) end
            if balance == 0 then
                if quoted then error("line " .. tostring(lineNumber) .. " has an unterminated string", 0) end
                table.insert(result, { text = pending, line = pendingLine })
                pending = ""
                pendingLine = 0
            end
        end
    end
    if pending ~= "" then error("line " .. tostring(pendingLine) .. " has an unterminated value", 0) end
    return result
end

local function parseString(text: string, line: number): (string, string)
    local result: { string } = {}
    local index = 2
    while index <= #text do
        local character = string.sub(text, index, index)
        if character == '"' then return table.concat(result), string.sub(text, index + 1) end
        if character == "\\" then
            local escaped = string.sub(text, index + 1, index + 1)
            if escaped == "n" then table.insert(result, "\n")
            elseif escaped == "t" then table.insert(result, "\t")
            elseif escaped == '"' then table.insert(result, '"')
            elseif escaped == "\\" then table.insert(result, "\\")
            else error("line " .. tostring(line) .. " has an unsupported string escape", 0) end
            index += 2
        else
            table.insert(result, character)
            index += 1
        end
    end
    error("line " .. tostring(line) .. " has an unterminated string", 0)
end

local parseValue

local function parseArray(text: string, line: number): (any, string)
    local result: { any } = {}
    local rest = trim(string.sub(text, 2))
    if string.sub(rest, 1, 1) == "]" then return result, string.sub(rest, 2) end
    while true do
        local value: any
        value, rest = parseValue(rest, line)
        table.insert(result, value)
        rest = trim(rest)
        local head = string.sub(rest, 1, 1)
        if head == "]" then return result, string.sub(rest, 2) end
        if head ~= "," then error("line " .. tostring(line) .. " needs a comma or ] in an array", 0) end
        rest = trim(string.sub(rest, 2))
        if string.sub(rest, 1, 1) == "]" then return result, string.sub(rest, 2) end
    end
end

local function parseInlineTable(text: string, line: number): (any, string)
    local result: { [string]: any } = {}
    local rest = trim(string.sub(text, 2))
    if string.sub(rest, 1, 1) == "}" then return result, string.sub(rest, 2) end
    while true do
        local key, remainder = string.match(rest, "^([%w_%-]+)%s*=%s*(.*)$")
        if key == nil then error("line " .. tostring(line) .. " needs key = value inside an inline table", 0) end
        if result[key] ~= nil then error("line " .. tostring(line) .. " repeats inline key '" .. key .. "'", 0) end
        local value: any
        value, rest = parseValue(remainder :: string, line)
        result[key] = value
        rest = trim(rest)
        local head = string.sub(rest, 1, 1)
        if head == "}" then return result, string.sub(rest, 2) end
        if head ~= "," then error("line " .. tostring(line) .. " needs a comma or } in an inline table", 0) end
        rest = trim(string.sub(rest, 2))
        if string.sub(rest, 1, 1) == "}" then return result, string.sub(rest, 2) end
    end
end

function parseValue(text: string, line: number): (any, string)
    local body = trim(text)
    local head = string.sub(body, 1, 1)
    if head == '"' then return parseString(body, line) end
    if head == "[" then return parseArray(body, line) end
    if head == "{" then return parseInlineTable(body, line) end
    local word = string.match(body, "^[%w%+%-%.]+")
    if word == nil then error("line " .. tostring(line) .. " has no value after =", 0) end
    local rest = string.sub(body, #word + 1)
    if word == "true" then return true, rest end
    if word == "false" then return false, rest end
    if string.match(word, "^[%+%-]?%d+%.?%d*[eE][%+%-]?%d+$") ~= nil
        or string.match(word, "^[%+%-]?%d+%.?%d*$") ~= nil then
        local number = tonumber(word)
        if number ~= nil then return number, rest end
    end
    error("line " .. tostring(line) .. ' has unsupported value "' .. word .. '"', 0)
end

local function assignment(line: string, lineNumber: number): (string?, any)
    local key, remainder = string.match(line, "^([%w_%-]+)%s*=%s*(.*)$")
    if key == nil then return nil, nil end
    local value, rest = parseValue(remainder :: string, lineNumber)
    if trim(rest) ~= "" then error("line " .. tostring(lineNumber) .. " has more than one value", 0) end
    return key, value
end

local function newSection(kind: string, line: number): any
    return { kind = kind, line = line, fields = {}, tables = {}, actionChildren = {} }
end

local function addField(target: { [string]: any }, key: string, value: any, line: number): ()
    if target[key] ~= nil then error("line " .. tostring(line) .. ' repeats field "' .. key .. '"', 0) end
    target[key] = value
end

function project.parse(text: string): any
    if type(text) ~= "string" then error("project.parse needs the model text", 2) end
    local document: any = {
        schema_version = nil,
        base_resolution = nil,
        base_dpi = nil,
        sections = {},
    }
    local current: any = nil
    local activeTable: { [string]: any }? = nil
    local activeTableName: string? = nil
    local activeAction: any = nil
    for _, statement in logicalStatements(text) do
        local lineNumber = statement.line
        local body = statement.text
        local arrayName = string.match(body, "^%[%[([%w_%-%.]+)%]%]$")
        if arrayName ~= nil then
            activeTable = nil
            activeTableName = nil
            activeAction = nil
            if arrayName == "binding.actions" then
                if current == nil or current.kind ~= "binding" then error("line " .. tostring(lineNumber) .. " binding.actions needs a preceding [[binding]]", 0) end
                local child = { line = lineNumber, fields = {} }
                table.insert(current.actionChildren, child)
                activeAction = child
            elseif not sectionKinds[arrayName :: string] then
                error("line " .. tostring(lineNumber) .. ' has unsupported array section "' .. arrayName .. '"', 0)
            else
                current = newSection(arrayName :: string, lineNumber)
                table.insert(document.sections, current)
            end
            continue
        end
        local tableName = string.match(body, "^%[([%w_%-%.]+)%]$")
        if tableName ~= nil then
            activeAction = nil
            if current == nil then error("line " .. tostring(lineNumber) .. " has a subtable without a parent", 0) end
            if not nestedTables[tableName :: string] then error("line " .. tostring(lineNumber) .. ' has unsupported subtable "' .. tableName .. '"', 0) end
            local owner = string.match(tableName :: string, "^([%w_%-]+)\\.")
            if owner ~= current.kind then error("line " .. tostring(lineNumber) .. " subtable belongs to another section", 0) end
            local name = string.match(tableName :: string, "^[%w_%-]+\\.([%w_%-]+)$")
            if name == nil then error("line " .. tostring(lineNumber) .. " has an invalid subtable", 0) end
            if current.fields[name :: string] ~= nil then
                error("line " .. tostring(lineNumber) .. ' repeats field "' .. name .. '" as a subtable', 0)
            end
            if current.tables[name] ~= nil then error("line " .. tostring(lineNumber) .. ' repeats subtable "' .. name .. '"', 0) end
            current.tables[name] = {}
            activeTable = current.tables[name]
            activeTableName = tableName :: string
            continue
        end
        local key, value = assignment(body, lineNumber)
        if key == nil then error("line " .. tostring(lineNumber) .. " is not a supported TOML statement", 0) end
        if current == nil then
            if not rootFields[key :: string] then error("line " .. tostring(lineNumber) .. ' has unsupported root field "' .. key .. '"', 0) end
            addField(document, key :: string, value, lineNumber)
        elseif activeAction ~= nil then
            local allowed = { id = true, kind = true, locator = true, preconditions = true, key = true, axis = true, delta = true, destination = true }
            if not allowed[key :: string] then error("line " .. tostring(lineNumber) .. ' has unsupported action field "' .. key .. '"', 0) end
            addField(activeAction.fields, key :: string, value, lineNumber)
        elseif activeTable ~= nil then
            local allowed = nestedFields[activeTableName :: string]
            if allowed == nil or not allowed[key :: string] then
                error("line " .. tostring(lineNumber) .. ' has unsupported field "' .. key .. '" in [' .. (activeTableName :: string) .. "]", 0)
            end
            addField(activeTable, key :: string, value, lineNumber)
        else
            if current.tables[key :: string] ~= nil then
                error("line " .. tostring(lineNumber) .. ' repeats subtable "' .. key .. '" as a field', 0)
            end
            if not sectionFields[current.kind][key :: string] then error("line " .. tostring(lineNumber) .. ' has unsupported field "' .. key .. '" in [[' .. current.kind .. ']]', 0) end
            addField(current.fields, key :: string, value, lineNumber)
        end
    end
    if document.schema_version ~= 1 then error("this runtime model requires schema_version = 1", 2) end
    if document.base_resolution == nil then error("this runtime model needs base_resolution", 2) end
    if document.base_dpi == nil then error("this runtime model needs base_dpi", 2) end
    if #document.sections == 0 then error("this runtime model needs at least one record", 2) end
    return document
end

function project.build(ctx: any, document: any, contentHash: string?): any
    if type(document) ~= "table" then error("project.build needs project.parse output", 2) end
    local built: any = {
        schema_version = document.schema_version,
        base_resolution = document.base_resolution,
        base_dpi = document.base_dpi,
        contexts = {}, targets = {}, locators = {}, locator_variants = {}, readers = {},
        surfaces = {}, bindings = {}, transitions = {},
    }
    for _, section in document.sections do
        local fields: any = table.clone(section.fields)
        for key, tableValue in section.tables do fields[key] = tableValue end
        if section.kind == "context" then table.insert(built.contexts, model.Context.new(fields))
        elseif section.kind == "target" then table.insert(built.targets, model.Target.new(fields))
        elseif section.kind == "locator" then table.insert(built.locators, model.Locator.new(fields))
        elseif section.kind == "locator_variant" then table.insert(built.locator_variants, model.LocatorVariant.new(fields))
        elseif section.kind == "reader" then table.insert(built.readers, model.Reader.new(fields))
        elseif section.kind == "surface" then table.insert(built.surfaces, model.Surface.new(fields))
        elseif section.kind == "binding" then
            if #section.actionChildren > 0 and fields.actions ~= nil then
                error("a binding cannot declare actions both inline and as [[binding.actions]]", 2)
            end
            if #section.actionChildren > 0 then
                fields.actions = {}
                for _, child in section.actionChildren do table.insert(fields.actions, child.fields) end
            elseif fields.actions == nil then
                fields.actions = {}
            end
            table.insert(built.bindings, model.Binding.new(fields))
        elseif section.kind == "transition" then table.insert(built.transitions, model.Transition.new(fields))
        else error("unsupported section " .. tostring(section.kind), 2) end
    end

    local compiled = model.RuntimeModel.new(built)
    if contentHash ~= nil then model.attach_model_hash(compiled, contentHash) end
    return compiled
end

local function renderString(value: string): string
    local result = string.gsub(value, "[\\\"\n\t]", { ["\\"] = "\\\\", ['"'] = '\\"', ["\n"] = "\\n", ["\t"] = "\\t" })
    return '"' .. result .. '"'
end

local function renderNumber(value: number): string
    if value % 1 == 0 then return string.format("%d", value) end
    return string.format("%.17g", value)
end

local function renderValue(value: any): string
    if type(value) == "string" then return renderString(value) end
    if type(value) == "number" then return renderNumber(value) end
    if type(value) == "boolean" then return if value then "true" else "false" end
    error("project.encode cannot render a value of type " .. type(value), 0)
end

local renderInline

local function renderArray(values: { any }): string
    local parts: { string } = {}
    for index = 1, #values do
        local value = values[index]
        if type(value) == "table" then parts[index] = renderInline(value)
        else parts[index] = renderValue(value) end
    end
    return "[" .. table.concat(parts, ", ") .. "]"
end

function renderInline(value: { [string]: any }, order: { string }?): string
    local keys: { string } = if order ~= nil then table.clone(order :: { string }) else {}
    if order == nil then for key in value do table.insert(keys, key) end table.sort(keys) end
    local parts: { string } = {}
    for index, key in keys do
        if value[key] ~= nil then
            local child = value[key]
            local rendered = if type(child) == "table" then (if #child > 0 then renderArray(child) else renderInline(child)) else renderValue(child)
            parts[index] = key .. " = " .. rendered
        end
    end
    return "{ " .. table.concat(parts, ", ") .. " }"
end

local function renderRef(ref: any): string return renderInline(ref, { "locator", "variant" }) end
local function renderAsset(value: any): string return renderInline(value, { "path", "sha256" }) end
local function renderPredicate(value: any): string
    local order = if value.locator ~= nil then { "kind", "locator" } elseif value.value ~= nil then { "kind", "reader", "operator", "value" } else { "kind", "reader", "operator" }
    local copy: any = table.clone(value)
    if copy.locator ~= nil then copy.locator = value.locator end
    return renderInline(copy, order)
end

local function renderPredicates(values: { any }): string
    local parts: { string } = {}
    for index = 1, #values do parts[index] = renderPredicate(values[index]) end
    return "[" .. table.concat(parts, ", ") .. "]"
end

local function renderIdentity(value: any): string
    return "{ all = " .. renderPredicates(value.all)
        .. ", any = " .. renderPredicates(value.any)
        .. ", none = " .. renderPredicates(value.none) .. " }"
end

local function renderGeometry(value: any): string
    if value.kind == "fixed" then
        return "{ kind = " .. renderValue(value.kind)
            .. ", rect = " .. renderArray(value.rect) .. " }"
    end
    if value.kind == "collection" then
        local result = "{ kind = " .. renderValue(value.kind)
            .. ", rect = " .. renderArray(value.rect)
            .. ", axis = " .. renderValue(value.axis)
            .. ", item_size = " .. renderArray(value.item_size)
            .. ", spacing = " .. renderArray(value.spacing)
        if value.max_items ~= nil then result ..= ", max_items = " .. renderNumber(value.max_items) end
        return result .. " }"
    end
    return "{ kind = " .. renderValue(value.kind)
        .. ", anchor = " .. renderRef(value.anchor)
        .. ", offset = " .. renderArray(value.offset) .. " }"
end

local function renderPlacement(value: any): string
    if value.kind == "target_geometry" then return "{ kind = " .. renderValue(value.kind) .. " }" end
    if value.kind == "rect" then return "{ kind = " .. renderValue(value.kind) .. ", rect = " .. renderArray(value.rect) .. " }" end
    return "{ kind = " .. renderValue(value.kind)
        .. ", anchor = " .. renderRef(value.anchor)
        .. ", offset = " .. renderArray(value.offset) .. " }"
end

local function renderAction(value: any): string
    local fields: any = table.clone(value)
    if fields.locator ~= nil then fields.locator = renderRef(fields.locator) end
    if fields.destination ~= nil then
        if fields.destination.kind == "point" then fields.destination = renderInline({ kind = "point", point = fields.destination.point }, { "kind", "point" })
        else fields.destination = renderInline({ kind = "locator", locator = renderRef(fields.destination.locator) }, { "kind", "locator" }) end
    end
    fields.preconditions = renderPredicates(value.preconditions)
    local order = if value.kind == "click" then { "id", "kind", "locator", "preconditions" }
        elseif value.kind == "key" then { "id", "kind", "key", "locator", "preconditions" }
        elseif value.kind == "scroll" then { "id", "kind", "axis", "delta", "locator", "preconditions" }
        else { "id", "kind", "locator", "destination", "preconditions" }
    return renderInline(fields, order)
end

local function copyById(list: { any }): { any }
    local result = table.clone(list)
    table.sort(result, function(left: any, right: any): boolean return left.id < right.id end)
    return result
end

function project.encode(built: any): string
    if not model.is_runtime_model(built) then error("project.encode needs a framework-minted RuntimeModel", 2) end
    local lines: { string } = {
        "schema_version = 1",
        "base_resolution = " .. renderArray(built.base_resolution),
        "base_dpi = " .. renderArray(built.base_dpi),
    }
    local function line(text: string) table.insert(lines, text) end
    for _, value in copyById(built.contexts) do line("\n[[context]]"); line("id = " .. renderValue(value.id)) end
    for _, value in copyById(built.targets) do
        line("\n[[target]]"); line("id = " .. renderValue(value.id)); line("kind = " .. renderValue(value.kind))
        if value.geometry ~= nil then line("geometry = " .. renderGeometry(value.geometry)) end
        line("locators = " .. renderArray(value.locators)); line("readers = " .. renderArray(value.readers))
    end
    for _, value in copyById(built.locators) do
        line("\n[[locator]]"); line("id = " .. renderValue(value.id)); line("target = " .. renderValue(value.target)); line("kind = " .. renderValue(value.kind))
        if value.kind == "template" then
            if value.asset ~= nil then line("asset = " .. renderAsset(value.asset)) end
            if value.threshold ~= nil then line("threshold = " .. renderNumber(value.threshold)) end
            if value.search_rect ~= nil then line("search_rect = " .. renderArray(value.search_rect)) end
            if value.variants ~= nil then line("variants = " .. renderArray(value.variants)) end
        elseif value.kind == "text" then line("reader = " .. renderValue(value.reader)); if value.search_rect ~= nil then line("search_rect = " .. renderArray(value.search_rect)) end
        elseif value.kind == "geometry" then line("rect = " .. renderArray(value.rect))
        elseif value.kind == "relative" then line("anchor = " .. renderRef(value.anchor)); line("offset = " .. renderArray(value.offset))
        else line("item_locator = " .. renderRef(value.item_locator)); line("axis = " .. renderValue(value.axis)); line("spacing = " .. renderArray(value.spacing)); if value.max_items ~= nil then line("max_items = " .. renderNumber(value.max_items)) end end
    end
    for _, value in copyById(built.locator_variants) do
        line("\n[[locator_variant]]"); line("id = " .. renderValue(value.id)); line("locator = " .. renderValue(value.locator)); line("asset = " .. renderAsset(value.asset)); if value.threshold ~= nil then line("threshold = " .. renderNumber(value.threshold)) end; if value.search_rect ~= nil then line("search_rect = " .. renderArray(value.search_rect)) end
    end
    for _, value in copyById(built.readers) do
        line("\n[[reader]]"); line("id = " .. renderValue(value.id)); line("target = " .. renderValue(value.target)); line("kind = " .. renderValue(value.kind)); line("confidence_floor = " .. renderNumber(value.confidence_floor)); if value.normalization ~= nil then line("normalization = " .. renderValue(value.normalization)) end; if value.join ~= nil then line("join = " .. renderValue(value.join)) end; if value.item_kind ~= nil then line("item_kind = " .. renderValue(value.item_kind)) end; if value.min_items ~= nil then line("min_items = " .. renderNumber(value.min_items)) end; if value.max_items ~= nil then line("max_items = " .. renderNumber(value.max_items)) end
    end
    for _, value in copyById(built.surfaces) do
        line("\n[[surface]]"); line("id = " .. renderValue(value.id)); line("kind = " .. renderValue(value.kind)); line("contexts = " .. renderArray(value.contexts)); if value.covers ~= nil then line("covers = " .. renderArray(value.covers)) end
    end
    for _, value in copyById(built.bindings) do
        line("\n[[binding]]"); line("id = " .. renderValue(value.id)); line("surface = " .. renderValue(value.surface)); line("target = " .. renderValue(value.target)); line("placement = " .. renderPlacement(value.placement)); line("identity = " .. renderIdentity(value.identity)); line("readers = " .. renderArray(value.readers))
        for _, action in copyById(value.actions) do
            line("\n[[binding.actions]]")
            line("id = " .. renderValue(action.id))
            line("kind = " .. renderValue(action.kind))
            if action.locator ~= nil then line("locator = " .. renderRef(action.locator)) end
            if action.key ~= nil then line("key = " .. renderValue(action.key)) end
            if action.axis ~= nil then line("axis = " .. renderValue(action.axis)) end
            if action.delta ~= nil then line("delta = " .. renderNumber(action.delta)) end
            if action.destination ~= nil then
                line("destination = " .. (if action.destination.kind == "point"
                    then renderInline({ kind = "point", point = action.destination.point }, { "kind", "point" })
                    else renderInline({ kind = "locator", locator = action.destination.locator }, { "kind", "locator" })))
            end
            line("preconditions = " .. renderPredicates(action.preconditions))
        end
    end
    for _, value in copyById(built.transitions) do
        line("\n[[transition]]"); line("id = " .. renderValue(value.id)); line("from = { context = " .. renderValue(value.from.context) .. ", surfaces = " .. renderArray(value.from.surfaces) .. " }"); line("trigger = " .. renderInline(value.trigger, { "kind", "binding", "action", "milliseconds", "name" })); line("to = { context = " .. renderValue(value.to.context) .. ", surfaces = " .. renderArray(value.to.surfaces) .. " }"); line("verification = " .. renderInline(value.verification, { "kind", "timeout_ms", "attempts" }))
    end
    return table.concat(lines, "\n") .. "\n"
end

function project.load_project(ctx: any, contentHash: string?): any
    if type(ctx) ~= "table" or type(ctx.project_read) ~= "function" then error("project.load_project needs a context with project_read", 2) end
    return project.build(ctx, project.parse(ctx:project_read(project.file_name)), contentHash)
end

function project.save_project(ctx: any, built: any)
    if type(ctx) ~= "table" or type(ctx.project_write) ~= "function" then error("project.save_project needs a context with project_write", 2) end
    ctx:project_write(project.file_name, project.encode(built))
end

return project
