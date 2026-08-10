--!strict
-- RFC 8785 JCS behaviour.
--
-- Member ordering, number formatting, string escaping and the empty container
-- are checked against tests/vectors/jcs-vectors.txt, embedded below and read
-- from that same file by the C++ and Python spellings of this scheme.  The
-- shared block runs FIRST: a hand-written case failing ahead of it would hide
-- whether the shared rows have any teeth.  What follows it is what the vector
-- file cannot express -- this value model, the refusals, and the fact that
-- Luau tostring is not this format.
--
-- This source is deliberately pure ASCII: every non-ASCII character is a
-- \u{...} escape, so no editor, terminal or transport can silently alter a test
-- vector.  Expected JSON uses [==[ ]==] long strings because JSON is dense in
-- backslashes and in ]] pairs.

local function same(actual: string, expected: string, label: string)
    if actual ~= expected then
        error(
            label .. "\n  actual   " .. actual .. "\n  expected " .. expected,
            2
        )
    end
end

-- A refusal only counts if it is this module's own refusal.  Without the message
-- check a guard could be deleted and the case would stay green on an incidental
-- blow-up -- a dropped cycle check, for instance, still "fails", as a stack
-- overflow.
local function rejects(label: string, action: () -> any)
    local finished, message = pcall(action)
    if finished then
        error("expected jcs to reject " .. label, 2)
    end
    if type(message) ~= "string" or not string.find(message, "jcs cannot encode", 1, true) then
        error("jcs refused " .. label .. " by accident: " .. tostring(message), 2)
    end
end

local EURO = "\u{20AC}"
local DALET = "\u{FB33}" -- BMP, one UTF-16 unit FB33
local PRIVATE = "\u{E000}" -- BMP, one UTF-16 unit E000
local GRIN = "\u{1F600}" -- supplementary, UTF-16 units D83D DE00

-- SHARED VECTORS
--
-- The rows below are tests/vectors/jcs-vectors.txt verbatim: the same
-- expectations the C++ and Python spellings of RFC 8785 are held to, so that a
-- disagreement between the three fails a test instead of producing bytes that
-- merely differ.  The file states its own format.
--
-- They are copied rather than read because this sandbox has no way to open a
-- file.  tools/annotate/tests/test_backend.py fails when this copy and the file
-- disagree, which is the only thing keeping one contract from becoming two.
-- BEGIN SHARED VECTORS tests/vectors/jcs-vectors.txt
local vectorLines: { string } = {
    "count 47",
    "string x x2222 # empty string",
    "string x616263 x2261626322 # plain ASCII",
    "string x22 x225c2222 # quotation mark takes the two-character escape",
    "string x5c x225c5c22 # reverse solidus takes the two-character escape",
    "string x2f x222f22 # solidus is NOT escaped",
    "string x08090a0c0d x225c625c745c6e5c665c7222 # the five short control escapes",
    "string x00 x225c753030303022 # NUL takes lowercase u0000",
    "string x0b x225c753030306222 # vertical tab has no short escape",
    "string x1f x225c753030316622 # u001f in LOWERCASE hex; uppercase would hash differently",
    "string x7f x227f22 # DEL is NOT escaped",
    "string xc280 x22c28022 # the C1 control U+0080 is NOT escaped",
    "string xc3a9e282acf09f9880 x22c3a9e282acf09f988022 # non-ASCII passes through as UTF-8",
    "string xff x22ff22 py=reject luau=reject # 0xff is not UTF-8",
    "string xeda080 x22eda08022 py=reject luau=reject # an encoded lone surrogate is not UTF-8",
    "order x7b22f0908080223a312c22ee8080223a327d xf0908080 xee8080 # U+10000 sorts BEFORE U+E000: UTF-16 order is neither byte nor code-point order",
    "order x7b225c72223a312c2231223a322c22c280223a332c22c3b6223a342c22e282ac223a352c22f09f9880223a362c22efacb3223a377d x0d x31 xc280 xc3b6 xe282ac xf09f9880 xefacb3 # RFC 8785 section 3.2.3 property ordering example",
    "order x7b2261223a312c226162223a322c2262223a337d x61 x6162 x62 # a shorter name that is a prefix sorts first",
    "order x7b22223a312c2230223a322c2241223a332c225a223a342c2261223a352c227a223a367d x x30 x41 x5a x61 x7a # empty name first, then ASCII by code unit",
    "order x7b22f0908080223a312c22f09f9880223a322c22ee8080223a332c22efacb3223a347d xf0908080 xf09f9880 xee8080 xefacb3 # two supplementary names against two BMP names",
    "order - x61 xff py=reject luau=reject # an invalid UTF-8 name sorts after every valid one in c++; the others refuse",
    "integer x30 x30 cpp=absent # zero",
    "integer x31 x31 cpp=absent # one",
    "integer x2d31 x2d31 cpp=absent # minus one",
    "integer x313030 x313030 cpp=absent # no exponent and no trailing point",
    "integer x39303037313939323534373430393931 x39303037313939323534373430393931 cpp=absent # 2^53-1, the largest exact integer",
    "integer x39303037313939323534373430393932 x39303037313939323534373430393932 cpp=absent py=reject # 2^53 is exact as a double, but python refuses it",
    "integer x39303037313939323534373430393933 x39303037313939323534373430393932 cpp=absent py=reject # 2^53+1 has already rounded down before luau sees it",
    "integer x31313532393231353034363036383436393736 x31313532393231353034363036383437303030 cpp=absent py=reject # 2^60 renders with trailing zeros as a double",
    "double x302e31 x302e31 cpp=absent # shortest round-trip, not 0.1000000000000000055511151231257827",
    "double x312e35 x312e35 cpp=absent # exact half",
    "double x3132332e343536 x3132332e343536 cpp=absent # ordinary fraction",
    "double x302e3330303030303030303030303030303034 x302e3330303030303030303030303030303034 cpp=absent # 0.1 plus 0.2",
    "double x31652d36 x302e303030303031 cpp=absent # 0.000001 is the last magnitude written without an exponent",
    "double x31652d37 x31652d37 cpp=absent # 1e-7, NOT the printf spelling 1e-07",
    "double x2d322e35652d38 x2d322e35652d38 cpp=absent # negative, with an unpadded exponent",
    "double x31653135 x31303030303030303030303030303030 cpp=absent # a large power of ten still written out",
    "double x302e33333333333333333333333333333333 x302e33333333333333333333333333333333 cpp=absent # one third, the shortest form that reads back exactly",
    "double x31653230 x313030303030303030303030303030303030303030 cpp=absent # written out in full",
    "double x31653231 x31652b3231 cpp=absent # the boundary where the exponent form begins",
    "double x35652d333234 x35652d333234 cpp=absent # smallest subnormal double",
    "double x312e3739373639333133343836323331353765333038 x312e37393736393331333438363233313537652b333038 cpp=absent # largest finite double",
    "double x2d30 x30 cpp=absent # negative zero prints as 0",
    "double x6e616e - cpp=absent py=reject luau=reject # NaN has no JSON form",
    "double x696e66 - cpp=absent py=reject luau=reject # infinity has no JSON form",
    "double x2d696e66 - cpp=absent py=reject luau=reject # negative infinity has no JSON form",
    "empty-object - cpp=absent py=x7b7d luau=x5b5d # lua cannot tell an empty object from an empty array, so it emits []",
    "empty-array x5b5d cpp=absent # the empty array is the one empty container all of them agree on",
}
-- END SHARED VECTORS

type VectorOutcome = { kind: string, bytes: string? }
-- A field is absent when its token is `-`, and a Lua array cannot hold nil, so
-- each field is a one-member table rather than the byte string itself.
type VectorField = { bytes: string? }
type VectorRow = {
    kind: string,
    fields: { VectorField },
    luau: VectorOutcome?,
    label: string,
}

local function vectorTokens(row: string): { string }
    local tokens: { string } = {}
    for token in string.gmatch(row, "[^ ]+") do
        table.insert(tokens, token)
    end
    return tokens
end

local function decodeVectorField(token: string): string?
    if token == "-" then
        return nil
    end
    if string.sub(token, 1, 1) ~= "x" then
        error("not a vector field: " .. token, 2)
    end
    local digits = string.sub(token, 2)
    if #digits % 2 ~= 0 then
        error("a vector field has an odd hex digit count: " .. token, 2)
    end
    if #digits > 0 and string.match(digits, "^[0-9a-f]+$") == nil then
        error("a vector field is not lowercase hex: " .. token, 2)
    end
    local bytes: { string } = {}
    for index = 1, #digits, 2 do
        local pair = string.sub(digits, index, index + 1)
        table.insert(bytes, string.char(tonumber(pair, 16) :: number))
    end
    return table.concat(bytes)
end

local function parseVectorRows(lines: { string }): { VectorRow }
    local rows: { VectorRow } = {}
    local declared: number? = nil
    for _, line in lines do
        local tokens = vectorTokens(line)
        if tokens[1] == "count" then
            if declared ~= nil then
                error("the vector block declares count twice", 2)
            end
            declared = tonumber(tokens[2])
            continue
        end

        local row: VectorRow = { kind = tokens[1], fields = {}, luau = nil, label = "" }
        local labelled = false
        for index = 2, #tokens do
            local token = tokens[index]
            if labelled then
                row.label = if row.label == "" then token else row.label .. " " .. token
                continue
            end
            if token == "#" then
                labelled = true
                continue
            end
            local at = string.find(token, "=", 1, true)
            if at == nil then
                table.insert(row.fields, { bytes = decodeVectorField(token) })
                continue
            end
            -- An unknown implementation name must fail rather than be ignored:
            -- a typo would otherwise silently drop the only expectation the row
            -- carries for one of the three.
            local name = string.sub(token, 1, at - 1)
            local value = string.sub(token, at + 1)
            if name ~= "cpp" and name ~= "py" and name ~= "luau" then
                error("unknown implementation in a vector row: " .. name, 2)
            end
            if name == "luau" then
                if value == "reject" or value == "absent" then
                    row.luau = { kind = value, bytes = nil }
                else
                    row.luau = { kind = "bytes", bytes = decodeVectorField(value) }
                end
            end
        end
        table.insert(rows, row)
    end

    -- A vector block that failed to load, or loaded short, would make every
    -- assertion built on it pass by having nothing to assert.
    if declared == nil then
        error("the vector block declares no count", 2)
    end
    if #rows ~= declared then
        error(
            "the vector block declares "
                .. tostring(declared)
                .. " rows and carries "
                .. tostring(#rows),
            2
        )
    end
    return rows
end

-- Written rather than parsed, so that no tonumber detail decides the sign of a
-- zero or whether a spelling of NaN is recognised at all.
local k_specialDoubles: { [string]: number } = {
    nan = 0 / 0,
    inf = math.huge,
    ["-inf"] = -math.huge,
    ["-0"] = -1 / math.huge,
}

local function vectorInput(row: VectorRow): any
    if row.kind == "empty-object" or row.kind == "empty-array" then
        return {}
    end
    local first = row.fields[1].bytes :: string
    if row.kind == "string" then
        return first
    end
    if row.kind == "integer" then
        return tonumber(first) :: number
    end
    if row.kind == "double" then
        local special = k_specialDoubles[first]
        if special ~= nil then
            return special
        end
        return tonumber(first) :: number
    end
    if row.kind == "order" then
        -- Built back to front, so that an encoder which merely preserved
        -- insertion order could not pass this row.
        local document: { [string]: number } = {}
        for position = #row.fields - 1, 1, -1 do
            document[row.fields[position + 1].bytes :: string] = position
        end
        return document
    end
    error("unknown vector row kind: " .. row.kind, 2)
end

do
    local checked: { [string]: number } = {}
    for _, row in parseVectorRows(vectorLines) do
        local outcome = row.luau
        local kind = if outcome == nil then "bytes" else outcome.kind
        if kind ~= "absent" then
            local input = vectorInput(row)
            if kind == "reject" then
                rejects(row.label, function()
                    return jcs.encode(input)
                end)
            else
                local expected: string?
                if outcome ~= nil then
                    expected = outcome.bytes
                elseif
                    row.kind == "order"
                    or row.kind == "empty-object"
                    or row.kind == "empty-array"
                then
                    expected = row.fields[1].bytes
                else
                    expected = row.fields[2].bytes
                end
                if expected == nil then
                    error("a vector row expects bytes it does not carry: " .. row.label, 2)
                end
                same(jcs.encode(input), expected :: string, row.label)
            end
            checked[row.kind] = (checked[row.kind] or 0) + 1
        end
    end
    -- Every kind the file can carry, so that a row this consumer silently
    -- skipped could not be mistaken for a row it passed.
    for _, kind in { "string", "order", "integer", "double", "empty-object", "empty-array" } do
        if (checked[kind] or 0) == 0 then
            error("the shared vectors exercised no " .. kind .. " row", 2)
        end
    end
end

-- Scalars and the null sentinel.
same(jcs.encode(jcs.null), "null", "null sentinel")
same(jcs.encode(true), "true", "true")
same(jcs.encode(false), "false", "false")
same(jcs.encode("x"), [==["x"]==], "bare string")
same(jcs.encode({ jcs.null, true, false, 0, -1 }), [==[[null,true,false,0,-1]]==], "scalars")

-- No insignificant whitespace anywhere, at any nesting depth.
same(
    jcs.encode({ a = 1, b = { 2, { c = 3 } } }),
    [==[{"a":1,"b":[2,{"c":3}]}]==],
    "no insignificant whitespace"
)

-- The same rule must hold in a nested object, and a name that is a prefix of
-- another sorts first.
do
    local inner = {}
    inner[DALET] = 1
    inner[GRIN] = 2
    same(
        jcs.encode({ b = inner, a = 1, ab = 2 }),
        [==[{"a":1,"ab":2,"b":{"]==]
            .. GRIN
            .. [==[":2,"]==]
            .. DALET
            .. [==[":1}}]==],
        "nested ordering and prefix tiebreak"
    )
end

-- Luau's tostring is itself a shortest-round-trip formatter and agrees with the
-- required form on most values, which is exactly what makes its one
-- disagreement easy to ship: it zero-pads the exponent to two digits the way
-- printf does, and ECMAScript never pads.  Pinning the disagreement here stops
-- the shared rows from quietly degenerating into "whatever tostring says".
assert(tostring(1e-7) == "1e-07", "Luau tostring no longer pads the exponent")
assert(tostring(-2.5e-8) == "-2.5e-08", "Luau tostring no longer pads the exponent")

-- REJECTION: refuse rather than emit something plausible.
rejects("a function", function()
    return jcs.encode({ print })
end)
rejects("nil", function()
    return jcs.encode(nil)
end)
rejects("a boolean key", function()
    local mixed = {}
    mixed[true] = 1
    return jcs.encode(mixed)
end)
rejects("names mixed with positions", function()
    local mixed = { 1, 2 }
    mixed.a = 3
    return jcs.encode(mixed)
end)
rejects("a sparse array", function()
    local sparse = {}
    sparse[1] = 1
    sparse[3] = 3
    return jcs.encode(sparse)
end)
rejects("a cycle", function()
    local cycle = {}
    cycle.self = cycle
    return jcs.encode(cycle)
end)

-- Invalid UTF-8 cannot be emitted: the output must be well-formed UTF-8 for the
-- C++ side to hash, and a lone surrogate has no UTF-8 encoding at all.
rejects("a lone continuation byte", function()
    return jcs.encode({ string.char(0x80) })
end)
rejects("an overlong two-byte NUL", function()
    return jcs.encode({ string.char(0xC0, 0x80) })
end)
rejects("an encoded surrogate", function()
    return jcs.encode({ string.char(0xED, 0xA0, 0x80) })
end)
rejects("a truncated sequence", function()
    return jcs.encode({ string.char(0xE2, 0x82) })
end)
rejects("a code point above U+10FFFF", function()
    return jcs.encode({ string.char(0xF4, 0x90, 0x80, 0x80) })
end)
rejects("an invalid UTF-8 member name", function()
    local named = {}
    named[string.char(0xFF)] = 1
    return jcs.encode(named)
end)

-- Determinism: the same value always produces the same bytes, and construction
-- order never shows through.
do
    local first = {}
    first[EURO] = 1
    first[PRIVATE] = 2
    first.a = 3
    local second = {}
    second.a = 3
    second[PRIVATE] = 2
    second[EURO] = 1
    same(jcs.encode(first), jcs.encode(second), "insertion order must not show through")
    same(jcs.encode(first), jcs.encode(first), "encoding must be stable")
end

-- A metatable must not get to decide which members a content hash covers, so
-- the encoding reads raw contents only.  Under generalized iteration this object
-- would encode its __iter members instead of its own.
do
    -- __iter yields a positional key while the raw contents are name-keyed, so
    -- generalized iteration would misread this as an array in either of the two
    -- places the encoder walks a table.
    local disguised = setmetatable({}, {
        __iter = function()
            return next, { "ghost" }
        end,
        __index = function()
            return "ghost"
        end,
    })
    disguised.real = 1
    same(jcs.encode(disguised), [==[{"real":1}]==], "a metatable cannot redirect the encoding")
end

-- The hole a metatable could open in an array is worse than a wrong order: an
-- __index that answers for the missing position turns a sparse array into a
-- dense one, and the fabricated element would be hashed as if it were data.
rejects("a sparse array whose gap an __index would fill", function()
    local fabricating = setmetatable({}, {
        __index = function()
            return "ghost"
        end,
    })
    rawset(fabricating, 1, 1)
    rawset(fabricating, 3, 3)
    return jcs.encode(fabricating)
end)

return 1
