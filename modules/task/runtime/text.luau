-- Unicode normalization and full case folding use only the bundled,
-- release-hashed Unicode 15.0 data module. No host locale is consulted.
--!strict

local builtinUtf8 = utf8
local unicode = require("@umbraflow/utf8")
local data = require("@umbraflow/internal/unicode-text-data")
local text = {}

text.unicode_version = data.unicode_version

local canonicalData = data.canonical
local compatibilityData = data.compatibility
local combiningClassData = data.combining_classes
local compositionData = data.compositions
local caseFoldData = data.case_folds

local function reject(message: string): never
    error("text " .. message, 3)
end

local function parseMappings(data: string): any
    local output = {}
    for row in string.gmatch(data, "[^\n]+") do
        local separator = string.find(row, ":", 1, true)
        if separator == nil then reject("has corrupt generated mapping data") end
        local codepoint = tonumber(string.sub(row, 1, separator - 1))
        local values = {}
        for value in string.gmatch(string.sub(row, separator + 1), "%d+") do
            table.insert(values, tonumber(value))
        end
        output[codepoint] = values
    end
    return output
end

local function parsePairs(data: string): any
    local output = {}
    for row in string.gmatch(data, "[^\n]+") do
        local separator = string.find(row, ":", 1, true)
        if separator == nil then reject("has corrupt generated pair data") end
        output[tonumber(string.sub(row, 1, separator - 1))] =
            tonumber(string.sub(row, separator + 1))
    end
    return output
end

local function parseCompositions(data: string): any
    local output = {}
    for row in string.gmatch(data, "[^\n]+") do
        local comma = string.find(row, ",", 1, true)
        local colon = string.find(row, ":", 1, true)
        if comma == nil or colon == nil then
            reject("has corrupt generated composition data")
        end
        local starter = tonumber(string.sub(row, 1, comma - 1))
        local combining = tonumber(string.sub(row, comma + 1, colon - 1))
        local byCombining = output[starter]
        if byCombining == nil then
            byCombining = {}
            output[starter] = byCombining
        end
        byCombining[combining] = tonumber(string.sub(row, colon + 1))
    end
    return output
end

local canonical = parseMappings(canonicalData)
local compatibility = parseMappings(compatibilityData)
local combiningClasses = parsePairs(combiningClassData)
local compositions = parseCompositions(compositionData)
local caseFolds = parseMappings(caseFoldData)

local SBase = 0xAC00
local LBase = 0x1100
local VBase = 0x1161
local TBase = 0x11A7
local LCount = 19
local VCount = 21
local TCount = 28
local NCount = VCount * TCount
local SCount = LCount * NCount

local function checked(value: any, where: string): string
    if type(value) ~= "string" then reject(where .. " must be a string") end
    return unicode.validate(value)
end

local function encodeCodepoints(points: any, first: number?, last: number?): string
    local begin = first or 1
    local final = last or #points
    local output = {}
    for index = begin, final do
        table.insert(output, builtinUtf8.char(points[index]))
    end
    return table.concat(output)
end

local function appendDecomposition(output: any, codepoint: number, useCompatibility: boolean)
    local syllableIndex = codepoint - SBase
    if syllableIndex >= 0 and syllableIndex < SCount then
        local leading = LBase + math.floor(syllableIndex / NCount)
        local vowel = VBase + math.floor((syllableIndex % NCount) / TCount)
        local trailingIndex = syllableIndex % TCount
        table.insert(output, leading)
        table.insert(output, vowel)
        if trailingIndex ~= 0 then table.insert(output, TBase + trailingIndex) end
        return
    end

    local mapping = if useCompatibility then compatibility[codepoint] else nil
    if mapping == nil then mapping = canonical[codepoint] end
    if mapping == nil then
        table.insert(output, codepoint)
        return
    end
    for _, value in ipairs(mapping) do
        appendDecomposition(output, value, useCompatibility)
    end
end

local function canonicalOrder(points: any)
    for index = 2, #points do
        local currentClass = combiningClasses[points[index]] or 0
        if currentClass ~= 0 then
            local position = index
            while position > 1 do
                local previousClass = combiningClasses[points[position - 1]] or 0
                if previousClass == 0 or previousClass <= currentClass then break end
                points[position], points[position - 1] =
                    points[position - 1], points[position]
                position -= 1
            end
        end
    end
end

local function hangulComposition(starter: number, combining: number): number?
    local leadingIndex = starter - LBase
    if leadingIndex >= 0 and leadingIndex < LCount then
        local vowelIndex = combining - VBase
        if vowelIndex >= 0 and vowelIndex < VCount then
            return SBase + (leadingIndex * VCount + vowelIndex) * TCount
        end
    end
    local syllableIndex = starter - SBase
    local trailingIndex = combining - TBase
    if syllableIndex >= 0 and syllableIndex < SCount
        and syllableIndex % TCount == 0
        and trailingIndex > 0 and trailingIndex < TCount
    then
        return starter + trailingIndex
    end
    return nil
end

local function canonicalComposition(points: any): any
    if #points == 0 then return points end
    local output = { points[1] }
    local starterPosition = 1
    local starter = points[1]
    local previousClass = 0
    for index = 2, #points do
        local codepoint = points[index]
        local currentClass = combiningClasses[codepoint] or 0
        local byCombining = compositions[starter]
        local composite = if byCombining == nil then nil else byCombining[codepoint]
        if composite == nil then composite = hangulComposition(starter, codepoint) end
        if composite ~= nil and (previousClass == 0 or previousClass < currentClass) then
            output[starterPosition] = composite
            starter = composite
        else
            table.insert(output, codepoint)
            if currentClass == 0 then
                starterPosition = #output
                starter = codepoint
            end
            previousClass = currentClass
        end
    end
    return output
end

function text.normalize(value: any, form: any?): string
    local source = checked(value, "normalize value")
    local selected = if form == nil then "NFC" else form
    if selected ~= "NFC" and selected ~= "NFD"
        and selected ~= "NFKC" and selected ~= "NFKD"
    then
        reject("normalization form must be NFC, NFD, NFKC, or NFKD")
    end
    local decomposed = {}
    local useCompatibility = selected == "NFKC" or selected == "NFKD"
    for _, codepoint in ipairs(unicode.codepoints(source)) do
        appendDecomposition(decomposed, codepoint, useCompatibility)
    end
    canonicalOrder(decomposed)
    if selected == "NFC" or selected == "NFKC" then
        decomposed = canonicalComposition(decomposed)
    end
    return encodeCodepoints(decomposed)
end

function text.case_fold(value: any): string
    local source = checked(value, "case_fold value")
    local output = {}
    for _, codepoint in ipairs(unicode.codepoints(source)) do
        local mapping = caseFolds[codepoint]
        if mapping == nil then
            table.insert(output, codepoint)
        else
            for _, folded in ipairs(mapping) do table.insert(output, folded) end
        end
    end
    return encodeCodepoints(output)
end

function text.trim(value: any): string
    local points = unicode.codepoints(checked(value, "trim value"))
    local first = 1
    while first <= #points and unicode.is_whitespace(points[first]) do first += 1 end
    local last = #points
    while last >= first and unicode.is_whitespace(points[last]) do last -= 1 end
    return encodeCodepoints(points, first, last)
end

function text.collapse_whitespace(value: any): string
    local points = unicode.codepoints(checked(value, "collapse_whitespace value"))
    local output = {}
    local pendingSpace = false
    for _, codepoint in ipairs(points) do
        if unicode.is_whitespace(codepoint) then
            pendingSpace = #output > 0
        else
            if pendingSpace then table.insert(output, 0x20) end
            pendingSpace = false
            table.insert(output, codepoint)
        end
    end
    return encodeCodepoints(output)
end

function text.split(value: any, separator: any, keep_empty: any?): any
    local source = checked(value, "split value")
    local delimiter = checked(separator, "split separator")
    if delimiter == "" then reject("split separator must not be empty") end
    local keepEmpty = if keep_empty == nil then true else keep_empty
    if type(keepEmpty) ~= "boolean" then
        reject("split keep_empty must be a boolean")
    end
    local output = {}
    local start = 1
    while true do
        local first, last = string.find(source, delimiter, start, true)
        local part = if first == nil then string.sub(source, start)
            else string.sub(source, start, first - 1)
        if keepEmpty or part ~= "" then table.insert(output, part) end
        if first == nil then break end
        start = last + 1
    end
    return table.freeze(output)
end

function text.tokens(value: any): any
    local points = unicode.codepoints(checked(value, "tokens value"))
    local output = {}
    local word = {}
    local function flushWord()
        if #word == 0 then return end
        table.insert(output, encodeCodepoints(word))
        word = {}
    end
    for _, codepoint in ipairs(points) do
        local category = unicode.classify(codepoint)
        if category == "letter" or category == "mark" or category == "number" then
            table.insert(word, codepoint)
        elseif unicode.is_whitespace(codepoint) or category == "separator" then
            flushWord()
        else
            flushWord()
            table.insert(output, builtinUtf8.char(codepoint))
        end
    end
    flushWord()
    return table.freeze(output)
end

local function prepared(value: any, options: any?): string
    local source = checked(value, "match value")
    if options == nil then return text.normalize(source, "NFC") end
    if type(options) ~= "table" then reject("match options must be a table") end
    for key in pairs(options) do
        if key ~= "normalization" and key ~= "case_fold"
            and key ~= "collapse_whitespace"
        then
            reject("match options contain unknown field " .. tostring(key))
        end
    end
    local normalizationValue = rawget(options, "normalization")
    local foldValue = rawget(options, "case_fold")
    local collapseValue = rawget(options, "collapse_whitespace")
    local normalization = if normalizationValue == nil then "NFC"
        else normalizationValue
    local fold = if foldValue == nil then false else foldValue
    local collapse = if collapseValue == nil then false else collapseValue
    if type(fold) ~= "boolean" or type(collapse) ~= "boolean" then
        reject("match boolean options must be booleans")
    end
    local output = text.normalize(source, normalization)
    if fold then
        output = text.case_fold(output)
        output = text.normalize(output, normalization)
    end
    if collapse then output = text.collapse_whitespace(output) end
    return output
end

function text.equals(left: any, right: any, options: any?): boolean
    return prepared(left, options) == prepared(right, options)
end

function text.contains(value: any, candidate: any, options: any?): boolean
    local source = prepared(value, options)
    local needle = prepared(candidate, options)
    return string.find(source, needle, 1, true) ~= nil
end

function text.starts_with(value: any, candidate: any, options: any?): boolean
    local source = prepared(value, options)
    local prefix = prepared(candidate, options)
    return string.sub(source, 1, #prefix) == prefix
end

function text.ends_with(value: any, candidate: any, options: any?): boolean
    local source = prepared(value, options)
    local suffix = prepared(candidate, options)
    return suffix == "" or string.sub(source, -#suffix) == suffix
end

return table.freeze(text)
