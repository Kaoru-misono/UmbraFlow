-- UTF-8 utilities use only the bundled, release-hashed Unicode 15.0 data
-- module. Runtime behavior never consults Python or the host locale.
--!strict

local builtinUtf8 = utf8
local data = require("@umbraflow/internal/unicode-utf8-data")
local utf8 = {}

utf8.unicode_version = data.unicode_version

local categoryRanges = data.category_ranges
local whiteSpaceRanges = data.white_space_ranges

local function reject(message: string): never
    error("utf8 " .. message, 3)
end

local function checkedText(value: any, where: string): (string, number)
    if type(value) ~= "string" then reject(where .. " must be a string") end
    local ok, length = pcall(builtinUtf8.len, value)
    if not ok or length == nil then reject(where .. " is not valid UTF-8") end
    return value, length
end

local function checkedCodepoint(value: any): number
    if type(value) == "string" then
        local text, length = checkedText(value, "classify input")
        if length ~= 1 then reject("classify string must hold one code point") end
        return builtinUtf8.codepoint(text)
    end
    if type(value) ~= "number" or value % 1 ~= 0 or value < 0
        or value > 0x10FFFF or (value >= 0xD800 and value <= 0xDFFF)
    then
        reject("code point is not a Unicode scalar value")
    end
    return value
end

local function rangeValue(ranges: any, codepoint: number): any?
    local low = 1
    local high = #ranges
    while low <= high do
        local middle = math.floor((low + high) / 2)
        local row = ranges[middle]
        if codepoint < row[1] then
            high = middle - 1
        elseif codepoint > row[2] then
            low = middle + 1
        else
            return row[3] or true
        end
    end
    return nil
end

function utf8.is_valid(value: any): boolean
    if type(value) ~= "string" then return false end
    local ok, length = pcall(builtinUtf8.len, value)
    return ok and length ~= nil
end

function utf8.validate(value: any): string
    local text = checkedText(value, "value")
    return text
end

function utf8.length(value: any): number
    local _, length = checkedText(value, "value")
    return length
end

function utf8.codepoints(value: any): any
    local text = checkedText(value, "value")
    local output: { number } = {}
    for _, codepoint in builtinUtf8.codes(text) do
        table.insert(output, codepoint)
    end
    return table.freeze(output)
end

function utf8.slice(value: any, first: any, last: any?): string
    local text, length = checkedText(value, "value")
    if type(first) ~= "number" or first % 1 ~= 0 or first < 1
        or first > length + 1
    then
        reject("slice first index is outside the code-point sequence")
    end
    local final = if last == nil then length else last
    if type(final) ~= "number" or final % 1 ~= 0 or final < 0 or final > length then
        reject("slice last index is outside the code-point sequence")
    end
    if final < first then return "" end
    local firstByte = builtinUtf8.offset(text, first)
    local afterByte = if final == length
        then #text + 1
        else builtinUtf8.offset(text, final + 1)
    return string.sub(text, firstByte, afterByte - 1)
end

function utf8.classify(value: any): string
    local codepoint = checkedCodepoint(value)
    return rangeValue(categoryRanges, codepoint) or "other"
end

function utf8.is_whitespace(value: any): boolean
    return rangeValue(whiteSpaceRanges, checkedCodepoint(value)) == true
end

return table.freeze(utf8)
