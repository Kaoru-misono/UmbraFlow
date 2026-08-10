--!strict
-- RFC 8785 JSON Canonicalization Scheme.
--
-- These bytes are hashed on the C++ side, so every rule here is a wire-format
-- constraint, not a formatting preference: one value must always produce one
-- byte string, and that byte string must be the one every other RFC 8785
-- implementation produces for the same value.  Anything this cannot represent
-- exactly is refused rather than approximated -- a plausible-looking encoding of
-- a value we could not represent would be certified by a content hash and never
-- questioned again.
--
-- Not to be confused with `model.canonical_bytes`.  That is a private
-- length-prefixed type-tagged encoding for one schema-owned tree; this is the
-- interoperable JSON one.  They are different bytes for the same value on
-- purpose and neither may be substituted for the other.
--
-- The string rule below is the same transform as `appendJsonString` in
-- core/text/json-text.hpp, and the whole encoding must agree byte for byte with
-- tools/annotate/jcs.py.  A second spelling of any of this cannot fail a test --
-- it produces bytes that merely disagree.

local jcs = {}

-- JSON null.  A Lua table cannot hold nil -- storing nil erases the member --
-- so null has to be a value, and identity is the only thing that separates this
-- sentinel from an empty table.
local null = table.freeze({})
jcs.null = null

-- The exact escapes RFC 8785 section 3.2.2.2 requires.  Every other code point,
-- including DEL (0x7F) and the C1 controls, is emitted verbatim as UTF-8;
-- escaping more than this produces different bytes and therefore a different
-- content hash.
local k_shortEscape: { [number]: string } = {
    [0x08] = "\\b",
    [0x09] = "\\t",
    [0x0A] = "\\n",
    [0x0C] = "\\f",
    [0x0D] = "\\r",
    [0x22] = '\\"',
    [0x5C] = "\\\\",
}

-- Raised with level 0: the offending value is the subject, and a framework
-- source position would point at this file rather than at the caller who built
-- the value.  `jcs.encode` re-raises at the caller's frame.
local function reject(where: string, complaint: string): never
    error("jcs cannot encode " .. where .. ": " .. complaint, 0)
end

-- Decodes UTF-8 into code points, refusing every sequence that is not the one
-- shortest encoding of one Unicode scalar value.  Overlong forms, surrogates and
-- anything above U+10FFFF are rejected here, which is what lets the escape pass
-- below make its decisions one byte at a time.
local function codePoints(text: string, where: string): { number }
    local points: { number } = {}
    local size = #text
    local index = 1
    while index <= size do
        local lead = string.byte(text, index) :: number
        local length: number
        local point: number
        if lead < 0x80 then
            length, point = 1, lead
        elseif lead < 0xC2 then
            -- A continuation byte in lead position, or a two-byte overlong.
            reject(where, "is not valid UTF-8")
        elseif lead < 0xE0 then
            length, point = 2, lead - 0xC0
        elseif lead < 0xF0 then
            length, point = 3, lead - 0xE0
        elseif lead < 0xF5 then
            length, point = 4, lead - 0xF0
        else
            reject(where, "is not valid UTF-8")
        end

        for offset = 1, length - 1 do
            local continuation = string.byte(text, index + offset)
            if continuation == nil or continuation < 0x80 or continuation > 0xBF then
                reject(where, "is not valid UTF-8")
            end
            point = point * 0x40 + ((continuation :: number) - 0x80)
        end

        -- One range check per length covers every remaining malformation:
        -- a three-byte overlong lands below 0x800, a four-byte overlong below
        -- 0x10000, an encoded surrogate inside D800..DFFF.
        if length == 3 and (point < 0x800 or (point >= 0xD800 and point <= 0xDFFF)) then
            reject(where, "is not valid UTF-8")
        end
        if length == 4 and (point < 0x10000 or point > 0x10FFFF) then
            reject(where, "is not valid UTF-8")
        end

        table.insert(points, point)
        index += length
    end
    return points
end

-- Member names are ordered by their UTF-16 code-unit sequence, which is neither
-- byte order nor code-point order: a supplementary code point becomes a
-- surrogate pair starting in D800..DBFF and therefore sorts BEFORE every
-- unpaired code point in E000..FFFF, the opposite of both other orderings.
local function utf16Units(text: string, where: string): { number }
    local units: { number } = {}
    for _, point in codePoints(text, where) do
        if point < 0x10000 then
            table.insert(units, point)
        else
            local rest = point - 0x10000
            table.insert(units, 0xD800 + rest // 0x400)
            table.insert(units, 0xDC00 + rest % 0x400)
        end
    end
    return units
end

local function unitsLess(left: { number }, right: { number }): boolean
    local shared = math.min(#left, #right)
    for index = 1, shared do
        if left[index] ~= right[index] then
            return left[index] < right[index]
        end
    end
    return #left < #right
end

local function quote(text: string, where: string): string
    codePoints(text, where)

    local parts: { string } = { '"' }
    local size = #text
    local runStart = 1
    for index = 1, size do
        local byte = string.byte(text, index) :: number
        local short = k_shortEscape[byte]
        local escape: string
        if short ~= nil then
            escape = short
        elseif byte < 0x20 then
            escape = string.format("\\u%04x", byte)
        else
            continue
        end
        if index > runStart then
            table.insert(parts, string.sub(text, runStart, index - 1))
        end
        table.insert(parts, escape)
        runStart = index + 1
    end
    if runStart <= size then
        table.insert(parts, string.sub(text, runStart))
    end
    table.insert(parts, '"')
    return table.concat(parts)
end

-- The shortest decimal digit string that reads back as exactly this magnitude,
-- with the decimal exponent ECMAScript calls `n` (the value is 0.<digits> x
-- 10^n).  Trying `%.<p>e` for widening p and keeping the first that round-trips
-- is what "shortest" means here; 17 significant digits always suffice for a
-- double, so the loop is bounded.
local function shortestDigits(magnitude: number): (string, number)
    for precision = 0, 16 do
        local formatted = string.format("%." .. tostring(precision) .. "e", magnitude)
        if tonumber(formatted) == magnitude then
            -- `%e` always yields `d.ddd e sign digits`, or `d e sign digits` at
            -- precision 0.  A Lua pattern cannot spell "an optional group", so
            -- one character class covers both spellings instead.
            local mantissa, exponent = string.match(formatted, "^(%d[%d%.]*)[eE]([-+]%d+)$")
            if mantissa == nil or exponent == nil then
                reject("a number", "could not be formatted as a decimal")
            end
            -- No trailing zero can survive here: if the last digit were 0, the
            -- previous precision would have produced the same value in one digit
            -- fewer and the loop would already have stopped.
            local digits = string.gsub(mantissa :: string, "%.", "")
            return digits, (tonumber(exponent) :: number) + 1
        end
    end
    reject("a number", "has no shortest round-trip decimal form")
end

-- ECMAScript Number::toString, which RFC 8785 section 3.2.2.3 adopts verbatim.
-- Its layout is not printf's: no exponent below 1e21 down to 1e-6, no zero
-- padding in the exponent, and `-0` prints as `0`.
local function numberText(value: number, where: string): string
    if value ~= value then
        reject(where, "is NaN, which JSON cannot express")
    end
    if value == math.huge or value == -math.huge then
        reject(where, "is an infinity, which JSON cannot express")
    end
    -- Covers -0, which ECMAScript prints as "0".
    if value == 0 then
        return "0"
    end

    local sign = ""
    local magnitude = value
    if magnitude < 0 then
        sign = "-"
        magnitude = -magnitude
    end

    local digits, n = shortestDigits(magnitude)
    local k = #digits
    if k <= n and n <= 21 then
        return sign .. digits .. string.rep("0", n - k)
    end
    if 0 < n and n <= 21 then
        return sign .. string.sub(digits, 1, n) .. "." .. string.sub(digits, n + 1)
    end
    if -6 < n and n <= 0 then
        return sign .. "0." .. string.rep("0", -n) .. digits
    end

    local power = n - 1
    local mantissa = if k == 1
        then digits
        else string.sub(digits, 1, 1) .. "." .. string.sub(digits, 2)
    local marker = if power < 0 then "e-" else "e+"
    return sign .. mantissa .. marker .. string.format("%d", math.abs(power))
end

local encodeValue: (any, string, { [any]: boolean }) -> string

-- An empty table encodes as `[]`, matching `model.canonical`: Lua cannot tell an
-- empty array from an empty object, and inventing a tag for the difference would
-- freeze a shape the consumers have not agreed on.  The consequence is that an
-- empty JSON object is not expressible in this value model.
local function encodeTable(value: any, where: string, seen: { [any]: boolean }): string
    if seen[value] then
        reject(where, "is a cycle")
    end
    seen[value] = true

    -- `next` rather than generalized iteration, and `rawget` rather than
    -- indexing, throughout: the encoding must depend on the table's raw contents
    -- alone.  A caller-supplied `__iter` or `__index` would otherwise be able to
    -- decide which members a content hash covers.
    local indexCount = 0
    local nameCount = 0
    for key in next, value do
        local kind = type(key)
        if kind == "number" then
            if key % 1 ~= 0 or key < 1 then
                reject(where, "has a numeric key that is not an array position")
            end
            indexCount += 1
        elseif kind == "string" then
            nameCount += 1
        else
            reject(where, "has a key that is neither an array position nor a name")
        end
    end
    if indexCount > 0 and nameCount > 0 then
        reject(where, "mixes array positions with object names")
    end

    local parts: { string } = {}
    local opening: string
    local closing: string
    if nameCount > 0 then
        opening, closing = "{", "}"
        local names: { string } = {}
        for key in next, value do
            table.insert(names, key :: string)
        end
        local order: { [string]: { number } } = {}
        for _, name in names do
            order[name] = utf16Units(name, where .. " name")
        end
        table.sort(names, function(left: string, right: string): boolean
            return unitsLess(order[left], order[right])
        end)
        for _, name in names do
            table.insert(
                parts,
                quote(name, where .. " name")
                    .. ":"
                    .. encodeValue(rawget(value, name), where .. "." .. name, seen)
            )
        end
    else
        opening, closing = "[", "]"
        for index = 1, indexCount do
            local child = rawget(value, index)
            if child == nil then
                reject(where, "is a sparse array")
            end
            table.insert(parts, encodeValue(child, where .. "[" .. tostring(index) .. "]", seen))
        end
    end

    seen[value] = nil
    return opening .. table.concat(parts, ",") .. closing
end

function encodeValue(value: any, where: string, seen: { [any]: boolean }): string
    if rawequal(value, null) then
        return "null"
    end
    local kind = type(value)
    if kind == "boolean" then
        return if value then "true" else "false"
    end
    if kind == "number" then
        return numberText(value, where)
    end
    if kind == "string" then
        return quote(value, where)
    end
    if kind == "table" then
        return encodeTable(value, where, seen)
    end
    if kind == "nil" then
        reject(where, "is nil; JSON null is the value jcs.null")
    end
    reject(where, "is a " .. kind .. ", which is not a JSON value")
end

-- The exact RFC 8785 bytes for `value`: UTF-8, no BOM, no trailing newline and
-- no insignificant whitespace anywhere.  Raises on anything JSON cannot express
-- exactly.
--
-- Numbers are IEEE-754 doubles and nothing else, so an integer literal above
-- 2^53 was already rounded before it reached here; this returns -- and a content
-- hash then certifies -- the double that is actually held, not the literal that
-- was written.  Bounding such fields belongs in the producer's schema.
function jcs.encode(value: any): string
    local encoded, result = pcall(encodeValue, value, "the value", {})
    if not encoded then
        error(result, 2)
    end
    return result
end

return table.freeze(jcs)
