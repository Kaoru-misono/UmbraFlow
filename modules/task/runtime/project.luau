--!strict
-- The only RuntimeModel v2 TOML parser and Host artifact binding boundary.

local native = ...
local model = model

local project = {}
project.schema = "umbraflow-runtime/v2"
project.file_name = "runtime-model.toml"

local hostBindings: { [any]: any } = setmetatable({}, { __mode = "k" }) :: any
local loadedModel: any = nil

local sectionLists: { [string]: string } = {
    ui_target = "ui_targets",
    locator = "locators",
    reader = "readers",
    binding = "bindings",
    surface = "surfaces",
    transition = "transitions",
}

local sectionFields: { [string]: { [string]: boolean } } = {
    ui_target = { id = true, kind = true },
    locator = { id = true, kind = true, asset_path = true, threshold = true },
    reader = {
        id = true,
        kind = true,
        confidence_floor = true,
        layout = true,
        normalization = true,
    },
    binding = {
        id = true,
        surface = true,
        ui_target = true,
        variant = true,
        placement = true,
        detector = true,
        actions = true,
        reads = true,
    },
    surface = { id = true, kind = true, covers = true, identity = true },
    transition = { id = true, from_surfaces = true, trigger = true, to_surfaces = true },
}

local rootFields = {
    schema_version = true,
    base_resolution = true,
    base_dpi = true,
}

local function parseToml(text: string): any
    local index = 1
    local line = 1
    local length = #text

    local function peek(offset: number?): string
        local at = index + (offset or 0)
        if at > length then return "" end
        return string.sub(text, at, at)
    end

    local function take(): string
        local value = peek()
        if value == "" then return value end
        index += 1
        if value == "\n" then line += 1 end
        return value
    end

    local function failure(message: string): never
        error("line " .. tostring(line) .. " " .. message, 0)
    end

    local function horizontal()
        while peek() == " " or peek() == "\t" or peek() == "\r" do take() end
    end

    local function comment()
        if peek() ~= "#" then return end
        while peek() ~= "" and peek() ~= "\n" do take() end
    end

    local function valueTrivia()
        while true do
            horizontal()
            if peek() == "#" then comment() end
            if peek() ~= "\n" then return end
            take()
        end
    end

    local function statementTrivia()
        while true do
            horizontal()
            if peek() == "#" then comment() end
            if peek() ~= "\n" then return end
            take()
        end
    end

    local function finishStatement()
        horizontal()
        if peek() == "#" then comment() end
        if peek() == "\n" then
            take()
        elseif peek() ~= "" then
            failure("has trailing data")
        end
    end

    local function key(): string
        local start = index
        while string.match(peek(), "[A-Za-z0-9_-]") ~= nil do take() end
        if index == start then failure("needs a bare key") end
        return string.sub(text, start, index - 1)
    end

    local function expect(value: string)
        for position = 1, #value do
            if take() ~= string.sub(value, position, position) then
                failure("needs '" .. value .. "'")
            end
        end
    end

    local parseValue

    local function quotedString(): string
        expect('"')
        local result: { string } = {}
        while true do
            local child = take()
            if child == "" or child == "\n" then failure("has an unterminated string") end
            if child == '"' then return table.concat(result) end
            if child ~= "\\" then
                table.insert(result, child)
                continue
            end
            local escaped = take()
            if escaped == "b" then table.insert(result, "\b")
            elseif escaped == "t" then table.insert(result, "\t")
            elseif escaped == "n" then table.insert(result, "\n")
            elseif escaped == "f" then table.insert(result, "\f")
            elseif escaped == "r" then table.insert(result, "\r")
            elseif escaped == '"' then table.insert(result, '"')
            elseif escaped == "\\" then table.insert(result, "\\")
            else failure("has an unsupported string escape") end
        end
    end

    local function array(): any
        expect("[")
        local result = {}
        valueTrivia()
        if peek() == "]" then take() return result end
        while true do
            table.insert(result, parseValue())
            valueTrivia()
            if peek() == "]" then take() return result end
            if take() ~= "," then failure("needs ',' or ']' in an array") end
            valueTrivia()
            if peek() == "]" then take() return result end
        end
    end

    local function inlineTable(): any
        expect("{")
        local result = {}
        valueTrivia()
        if peek() == "}" then take() return result end
        while true do
            local name = key()
            if result[name] ~= nil then failure("repeats inline key '" .. name .. "'") end
            horizontal()
            expect("=")
            horizontal()
            result[name] = parseValue()
            valueTrivia()
            if peek() == "}" then take() return result end
            if take() ~= "," then failure("needs ',' or '}' in an inline table") end
            valueTrivia()
            if peek() == "}" then take() return result end
        end
    end

    local function scalar(): any
        local start = index
        while true do
            local child = peek()
            if child == "" or child == " " or child == "\t" or child == "\r"
                or child == "\n" or child == "," or child == "]" or child == "}"
                or child == "#"
            then
                break
            end
            take()
        end
        if index == start then failure("needs a value") end
        local token = string.sub(text, start, index - 1)
        if token == "true" then return true end
        if token == "false" then return false end
        -- The four accepted spellings are listed separately because a Lua
        -- pattern has no optional group: `?` applies to one character class,
        -- so a `(...)?` borrowed from regex never matches and every decimal
        -- would be refused as a bare value.
        if string.match(token, "^[+-]?%d+$") == nil
            and string.match(token, "^[+-]?%d+%.%d+$") == nil
            and string.match(token, "^[+-]?%d+%.%d+[eE][+-]?%d+$") == nil
            and string.match(token, "^[+-]?%d+[eE][+-]?%d+$") == nil
        then
            failure("has unsupported bare value '" .. token .. "'")
        end
        local number = tonumber(token)
        if number == nil or number ~= number or number == math.huge or number == -math.huge then
            failure("has invalid number '" .. token .. "'")
        end
        return number
    end

    function parseValue(): any
        local child = peek()
        if child == '"' then return quotedString() end
        if child == "[" then return array() end
        if child == "{" then return inlineTable() end
        return scalar()
    end

    local document: any = {
        ui_targets = {},
        locators = {},
        readers = {},
        bindings = {},
        surfaces = {},
        transitions = {},
    }
    local active: any = nil
    local activeSection: string? = nil

    while true do
        statementTrivia()
        if peek() == "" then break end

        if peek() == "[" then
            expect("[[")
            local name = key()
            expect("]]" )
            local listName = sectionLists[name]
            if listName == nil then failure("has unsupported section [[" .. name .. "]]") end
            active = {}
            activeSection = name
            table.insert(document[listName], active)
            finishStatement()
            continue
        end

        local name = key()
        horizontal()
        expect("=")
        horizontal()
        local value = parseValue()
        finishStatement()

        if active == nil then
            if not rootFields[name] then failure("has unsupported root field '" .. name .. "'") end
            if document[name] ~= nil then failure("repeats root field '" .. name .. "'") end
            document[name] = value
        else
            if activeSection == nil or not sectionFields[activeSection][name] then
                failure("has unsupported [[" .. tostring(activeSection) .. "]] field '" .. name .. "'")
            end
            if active[name] ~= nil then failure("repeats field '" .. name .. "'") end
            active[name] = value
        end
    end

    return document
end

function project.parse(text: any): any
    if type(text) ~= "string" then error("project.parse needs TOML text", 2) end
    return model.compile(parseToml(text))
end

local function requireNative(name: string): any
    if type(native) ~= "table" or type(native[name]) ~= "function" then
        error("Host does not provide private capability '" .. name .. "'", 3)
    end
    return native[name]
end

function project.load_project(): any
    if loadedModel ~= nil then return loadedModel end
    local runtimeModel = project.parse(requireNative("runtime_model_bytes")())
    local semanticHash = requireNative("runtime_semantic_hash")(model.canonical_bytes(runtimeModel))
    local binding = requireNative("runtime_model_finalize")(
        model.schema_hash,
        semanticHash,
        runtimeModel.asset_paths,
        runtimeModel.declared_surface_ids,
        runtimeModel.declared_ui_target_ids,
        runtimeModel.declared_action_ids,
        runtimeModel.base_resolution,
        runtimeModel.base_dpi,
        runtimeModel.declared_key_names
    )
    if binding == nil then error("Host rejected RuntimeArtifact binding", 2) end
    hostBindings[runtimeModel] = binding
    loadedModel = runtimeModel
    return runtimeModel
end

function project.host_binding(runtimeModel: any): any?
    return hostBindings[runtimeModel]
end

function project.is_loaded(runtimeModel: any): boolean
    return hostBindings[runtimeModel] ~= nil
end

return table.freeze(project)
