--!strict
-- Strict JSON parsing, deterministic encoding, and immutable JSON values.
-- Runtime behavior is pure Luau: it reaches only release-owned Framework
-- modules and never delegates parsing or encoding to the host.

local jcsModule = jcs
if jcsModule == nil then
    jcsModule = require("@umbraflow/jcs")
end
local builtinUtf8 = utf8
local json = {}

local k_maximumInputBytes = 1024 * 1024
local k_maximumDepth = 128
local k_maximumValues = 100000

-- Lua tables do not carry an array/object distinction. Every table created by
-- this module is registered by identity before it is frozen, so even empty
-- parsed and constructed values retain their JSON shape.
local shapeByValue: { [any]: string } = {}
local valueCountByValue: { [any]: number } = {}

json.null = jcsModule.null

local function reject(message: string): never
    error("json " .. message, 3)
end

local function finiteNumber(value: any, where: string): number
    if type(value) ~= "number" then reject(where .. " must be a number") end
    if value ~= value or value == math.huge or value == -math.huge then
        reject(where .. " must be finite")
    end
    return value
end

local function checkedString(value: any, where: string): string
    if type(value) ~= "string" then reject(where .. " must be a string") end
    if #value > k_maximumInputBytes then reject(where .. " is too large") end
    local valid, length = pcall(builtinUtf8.len, value)
    if not valid or length == nil then reject(where .. " is not valid UTF-8") end
    return value
end

local function encodedStringBytes(value: string): number
    local size = 2
    for index = 1, #value do
        local byte = string.byte(value, index) :: number
        if byte == 0x22 or byte == 0x5C
            or byte == 0x08 or byte == 0x09 or byte == 0x0A
            or byte == 0x0C or byte == 0x0D
        then
            size += 2
        elseif byte < 0x20 then
            size += 6
        else
            size += 1
        end
        if size > k_maximumInputBytes then return size end
    end
    return size
end

local function denseLength(value: any, where: string): number
    local count = rawlen(value)
    for key in next, value do
        if type(key) ~= "number" then reject(where .. " mixes array and object keys") end
        local index = key :: number
        if index % 1 ~= 0 or index < 1 or index > count then
            reject(where .. " is sparse")
        end
    end
    for index = 1, count do
        if rawget(value, index) == nil then reject(where .. " is sparse") end
    end
    return count
end

local function inferredShape(value: any, where: string): string
    local registered = shapeByValue[value]
    if registered ~= nil then return registered end
    local first = next(value)
    if first == nil then
        reject(where .. " is an ambiguous empty table; use json.array or json.object")
    end
    local hasNumber = false
    local hasString = false
    for key in next, value do
        if type(key) == "number" then
            hasNumber = true
        elseif type(key) == "string" then
            hasString = true
        else
            reject(where .. " has an unsupported key")
        end
        if hasNumber and hasString then reject(where .. " mixes array and object keys") end
    end
    if hasNumber then
        denseLength(value, where)
        return "array"
    end
    return "object"
end

local immutableValue: (
    any,
    string,
    number,
    { [any]: boolean },
    { number },
    string?
) -> any

immutableValue = function(
    value: any,
    where: string,
    depth: number,
    active: { [any]: boolean },
    count: { number },
    forcedShape: string?
): any
    if depth > k_maximumDepth then reject(where .. " is too deep") end
    if type(value) == "table" and forcedShape == nil then
        local registeredCount = valueCountByValue[value]
        if registeredCount ~= nil then
            count[1] += registeredCount
            if count[1] > k_maximumValues then reject("value count exceeds limit") end
            return value
        end
    end
    local countBefore = count[1]
    count[1] += 1
    if count[1] > k_maximumValues then reject("value count exceeds limit") end
    if rawequal(value, jcsModule.null) then return jcsModule.null end
    local valueType = type(value)
    if valueType == "string" then return checkedString(value, where) end
    if valueType == "number" then return finiteNumber(value, where) end
    if valueType == "boolean" then return value end
    if valueType ~= "table" then reject(where .. " is not a JSON value") end

    if active[value] then reject(where .. " contains a cycle") end
    active[value] = true

    local shape = forcedShape or inferredShape(value, where)
    local copied: { [any]: any } = {}
    if shape == "array" then
        local length = denseLength(value, where)
        for index = 1, length do
            copied[index] = immutableValue(
                rawget(value, index),
                where .. "[" .. tostring(index) .. "]",
                depth + 1,
                active,
                count,
                nil
            )
        end
    else
        for key, child in next, value do
            if type(key) ~= "string" then reject(where .. " has a non-string object key") end
            local checkedKey = checkedString(key, where .. " key")
            copied[checkedKey] = immutableValue(
                child,
                where .. "." .. checkedKey,
                depth + 1,
                active,
                count,
                nil
            )
        end
    end
    active[value] = nil
    shapeByValue[copied] = shape
    valueCountByValue[copied] = count[1] - countBefore
    return table.freeze(copied)
end

local function parsedContainer(
    value: { [any]: any },
    shape: string,
    valueCount: number
): any
    shapeByValue[value] = shape
    valueCountByValue[value] = valueCount
    return table.freeze(value)
end

function json.parse(source: any): any
    local text = checkedString(source, "parse input")
    local size = #text
    local index = 1
    local valueCount = 0

    local function failAt(message: string): never
        error("json parse error at byte " .. tostring(index) .. ": " .. message, 2)
    end

    local function skipWhitespace()
        while index <= size do
            local byte = string.byte(text, index) :: number
            if byte ~= 0x20 and byte ~= 0x09 and byte ~= 0x0A and byte ~= 0x0D then return end
            index += 1
        end
    end

    local function parseString(): string
        if string.byte(text, index) ~= 0x22 then failAt("expected a string") end
        index += 1
        local parts: { string } = {}
        local runStart = index
        while index <= size do
            local byte = string.byte(text, index) :: number
            if byte == 0x22 then
                if index > runStart then table.insert(parts, string.sub(text, runStart, index - 1)) end
                index += 1
                local result = table.concat(parts)
                if #result > k_maximumInputBytes then failAt("string is too large") end
                return result
            end
            if byte < 0x20 then failAt("unescaped control character in string") end
            if byte ~= 0x5C then
                index += 1
                continue
            end
            if index > runStart then table.insert(parts, string.sub(text, runStart, index - 1)) end
            index += 1
            if index > size then failAt("unfinished escape") end
            local escape = string.sub(text, index, index)
            local simple = ({
                ['"'] = '"',
                ["\\"] = "\\",
                ["/"] = "/",
                ["b"] = "\b",
                ["f"] = "\f",
                ["n"] = "\n",
                ["r"] = "\r",
                ["t"] = "\t",
            } :: any)[escape]
            if simple ~= nil then
                table.insert(parts, simple)
                index += 1
                runStart = index
                continue
            end
            if escape ~= "u" then failAt("invalid escape") end

            local function hexUnit(at: number): number
                if at + 3 > size then failAt("unfinished Unicode escape") end
                local unit = 0
                for offset = 0, 3 do
                    local digit = string.byte(text, at + offset) :: number
                    local value: number
                    if digit >= 0x30 and digit <= 0x39 then
                        value = digit - 0x30
                    elseif digit >= 0x41 and digit <= 0x46 then
                        value = digit - 0x41 + 10
                    elseif digit >= 0x61 and digit <= 0x66 then
                        value = digit - 0x61 + 10
                    else
                        failAt("invalid Unicode escape")
                    end
                    unit = unit * 16 + value
                end
                return unit
            end

            local first = hexUnit(index + 1)
            index += 5
            local point = first
            if first >= 0xD800 and first <= 0xDBFF then
                if index + 5 > size or string.sub(text, index, index + 1) ~= "\\u" then
                    failAt("unpaired high surrogate")
                end
                local second = hexUnit(index + 2)
                if second < 0xDC00 or second > 0xDFFF then failAt("unpaired high surrogate") end
                point = 0x10000 + (first - 0xD800) * 0x400 + second - 0xDC00
                index += 6
            elseif first >= 0xDC00 and first <= 0xDFFF then
                failAt("unpaired low surrogate")
            end
            table.insert(parts, builtinUtf8.char(point))
            runStart = index
        end
        failAt("unterminated string")
    end

    local parseValue: (number) -> any

    local function parseNumber(): number
        local start = index
        if string.sub(text, index, index) == "-" then index += 1 end
        if index > size then failAt("unfinished number") end
        local first = string.byte(text, index) :: number
        if first == 0x30 then
            index += 1
            local following = string.byte(text, index)
            if following ~= nil and following >= 0x30 and following <= 0x39 then
                failAt("leading zero in number")
            end
        elseif first >= 0x31 and first <= 0x39 then
            repeat
                index += 1
                local digit = string.byte(text, index)
            until digit == nil or digit < 0x30 or digit > 0x39
        else
            failAt("invalid integer part")
        end
        if string.sub(text, index, index) == "." then
            index += 1
            local digit = string.byte(text, index)
            if digit == nil or digit < 0x30 or digit > 0x39 then
                failAt("fraction requires a digit")
            end
            repeat
                index += 1
                digit = string.byte(text, index)
            until digit == nil or digit < 0x30 or digit > 0x39
        end
        local marker = string.sub(text, index, index)
        if marker == "e" or marker == "E" then
            index += 1
            local sign = string.sub(text, index, index)
            if sign == "+" or sign == "-" then index += 1 end
            local digit = string.byte(text, index)
            if digit == nil or digit < 0x30 or digit > 0x39 then
                failAt("exponent requires a digit")
            end
            repeat
                index += 1
                digit = string.byte(text, index)
            until digit == nil or digit < 0x30 or digit > 0x39
        end
        local value = tonumber(string.sub(text, start, index - 1))
        if value == nil or value ~= value or value == math.huge or value == -math.huge then
            failAt("number is outside the finite JSON range")
        end
        return value
    end

    local function parseArray(depth: number): any
        local containerStart = valueCount
        index += 1
        local output: { any } = {}
        skipWhitespace()
        if string.sub(text, index, index) == "]" then
            index += 1
            return parsedContainer(output, "array", 1)
        end
        while true do
            table.insert(output, parseValue(depth + 1))
            skipWhitespace()
            local delimiter = string.sub(text, index, index)
            if delimiter == "]" then
                index += 1
                return parsedContainer(
                    output,
                    "array",
                    valueCount - containerStart + 1
                )
            end
            if delimiter ~= "," then failAt("expected ',' or ']' in array") end
            index += 1
            skipWhitespace()
        end
    end

    local function parseObject(depth: number): any
        local containerStart = valueCount
        index += 1
        local output: { [string]: any } = {}
        skipWhitespace()
        if string.sub(text, index, index) == "}" then
            index += 1
            return parsedContainer(output, "object", 1)
        end
        while true do
            if string.sub(text, index, index) ~= '"' then failAt("expected object key") end
            local key = parseString()
            if rawget(output, key) ~= nil then failAt("duplicate object key") end
            skipWhitespace()
            if string.sub(text, index, index) ~= ":" then failAt("expected ':' after object key") end
            index += 1
            output[key] = parseValue(depth + 1)
            skipWhitespace()
            local delimiter = string.sub(text, index, index)
            if delimiter == "}" then
                index += 1
                return parsedContainer(
                    output,
                    "object",
                    valueCount - containerStart + 1
                )
            end
            if delimiter ~= "," then failAt("expected ',' or '}' in object") end
            index += 1
            skipWhitespace()
        end
    end

    parseValue = function(depth: number): any
        if depth > k_maximumDepth then failAt("value is too deep") end
        valueCount += 1
        if valueCount > k_maximumValues then failAt("value count exceeds limit") end
        skipWhitespace()
        local byte = string.byte(text, index)
        if byte == 0x22 then return parseString() end
        if byte == 0x5B then return parseArray(depth) end
        if byte == 0x7B then return parseObject(depth) end
        if byte == 0x74 and string.sub(text, index, index + 3) == "true" then
            index += 4
            return true
        end
        if byte == 0x66 and string.sub(text, index, index + 4) == "false" then
            index += 5
            return false
        end
        if byte == 0x6E and string.sub(text, index, index + 3) == "null" then
            index += 4
            return jcsModule.null
        end
        if byte == 0x2D or (byte ~= nil and byte >= 0x30 and byte <= 0x39) then
            return parseNumber()
        end
        failAt("expected a JSON value")
    end

    skipWhitespace()
    if index > size then failAt("empty input") end
    local result = parseValue(0)
    skipWhitespace()
    if index <= size then failAt("trailing content") end
    return result
end

local encodeValue: (any, string, number, { [any]: boolean }, { number }) -> string

encodeValue = function(
    value: any,
    where: string,
    depth: number,
    active: { [any]: boolean },
    count: { number }
): string
    if depth > k_maximumDepth then reject(where .. " is too deep") end
    count[1] += 1
    if count[1] > k_maximumValues then reject("value count exceeds limit") end
    if rawequal(value, jcsModule.null) then return "null" end
    local valueType = type(value)
    if valueType == "string" then
        local text = checkedString(value, where)
        if encodedStringBytes(text) > k_maximumInputBytes then
            reject("encoded value is too large")
        end
        return jcsModule.encode(text)
    end
    if valueType == "number" then return jcsModule.encode(finiteNumber(value, where)) end
    if valueType == "boolean" then return if value then "true" else "false" end
    if valueType ~= "table" then reject(where .. " is not a JSON value") end
    if active[value] then reject(where .. " contains a cycle") end
    active[value] = true
    local shape = inferredShape(value, where)
    local parts: { string } = {}
    if shape == "array" then
        local length = denseLength(value, where)
        table.insert(parts, "[")
        for index = 1, length do
            if index > 1 then table.insert(parts, ",") end
            table.insert(parts, encodeValue(
                rawget(value, index),
                where .. "[" .. tostring(index) .. "]",
                depth + 1,
                active,
                count
            ))
        end
        table.insert(parts, "]")
    else
        local keys: { string } = {}
        for key in next, value do
            if type(key) ~= "string" then reject(where .. " has a non-string object key") end
            checkedString(key, where .. " key")
            table.insert(keys, key)
        end
        table.sort(keys)
        table.insert(parts, "{")
        for keyIndex, key in keys do
            if keyIndex > 1 then table.insert(parts, ",") end
            table.insert(parts, jcsModule.encode(key))
            table.insert(parts, ":")
            table.insert(parts, encodeValue(
                rawget(value, key),
                where .. "." .. key,
                depth + 1,
                active,
                count
            ))
        end
        table.insert(parts, "}")
    end
    active[value] = nil
    local result = table.concat(parts)
    if #result > k_maximumInputBytes then reject("encoded value is too large") end
    return result
end

function json.encode(value: any): string
    local encoded = encodeValue(value, "value", 0, {}, { 0 })
    if #encoded > k_maximumInputBytes then reject("encoded value is too large") end
    return encoded
end

function json.array(value: any?): any
    local input = if value == nil then {} else value
    if type(input) ~= "table" then reject("array input must be a table") end
    return immutableValue(input, "array input", 0, {}, { 0 }, "array")
end

function json.object(value: any?): any
    local input = if value == nil then {} else value
    if type(input) ~= "table" then reject("object input must be a table") end
    return immutableValue(input, "object input", 0, {}, { 0 }, "object")
end

function json.immutable(value: any): any
    return immutableValue(value, "value", 0, {}, { 0 }, nil)
end

function json.kind(value: any): string
    if rawequal(value, jcsModule.null) then return "null" end
    local valueType = type(value)
    if valueType == "table" then return inferredShape(value, "value") end
    if valueType == "string" then
        checkedString(value, "value")
        return "string"
    end
    if valueType == "number" then
        finiteNumber(value, "value")
        return "number"
    end
    if valueType == "boolean" then return "boolean" end
    reject("value is not a JSON value")
end

function json.is_null(value: any): boolean
    return rawequal(value, jcsModule.null)
end

function json.get(value: any, key: any): any
    if type(value) ~= "table" or rawequal(value, jcsModule.null) then
        reject("get input must be an array or object")
    end
    local shape = inferredShape(value, "get input")
    if shape == "array" then
        local length = denseLength(value, "get input")
        if type(key) ~= "number" or key % 1 ~= 0 or key < 1 or key > length then
            return nil
        end
    elseif type(key) ~= "string" then
        reject("object key must be a string")
    end
    return rawget(value, key)
end

function json.set(value: any, key: any, child: any): any
    if child == nil then reject("set value must not be nil") end
    if type(value) ~= "table" or rawequal(value, jcsModule.null) then
        reject("set input must be an array or object")
    end
    local shape = inferredShape(value, "set input")
    local copied: { [any]: any } = {}
    for existingKey, existingValue in next, value do copied[existingKey] = existingValue end
    if shape == "array" then
        local length = denseLength(value, "set input")
        if type(key) ~= "number" or key % 1 ~= 0 or key < 1 or key > length then
            reject("array index is outside the array")
        end
    elseif type(key) ~= "string" then
        reject("object key must be a string")
    else
        checkedString(key, "object key")
    end
    copied[key] = child
    return immutableValue(copied, "set result", 0, {}, { 0 }, shape)
end

function json.remove(value: any, key: any): any
    if type(value) ~= "table" or rawequal(value, jcsModule.null) then
        reject("remove input must be an array or object")
    end
    local shape = inferredShape(value, "remove input")
    local copied: { [any]: any } = {}
    if shape == "array" then
        local length = denseLength(value, "remove input")
        if type(key) ~= "number" or key % 1 ~= 0 or key < 1 or key > length then
            reject("array index is outside the array")
        end
        for index = 1, length do
            if index ~= key then table.insert(copied, rawget(value, index)) end
        end
    else
        if type(key) ~= "string" then reject("object key must be a string") end
        for existingKey, existingValue in next, value do
            if existingKey ~= key then copied[existingKey] = existingValue end
        end
    end
    return immutableValue(copied, "remove result", 0, {}, { 0 }, shape)
end

function json.append(value: any, child: any): any
    if child == nil then reject("append value must not be nil") end
    if type(value) ~= "table" or rawequal(value, jcsModule.null)
        or inferredShape(value, "append input") ~= "array"
    then
        reject("append input must be an array")
    end
    local length = denseLength(value, "append input")
    local copied: { any } = {}
    for index = 1, length do copied[index] = rawget(value, index) end
    copied[length + 1] = child
    return immutableValue(copied, "append result", 0, {}, { 0 }, "array")
end

return table.freeze(json)
