--!strict
-- RFC 8785 JCS behaviour.  Expected bytes were produced by an ECMAScript
-- reference (JSON.stringify IS the required string form, String(number) IS
-- Number::toString, and JS `<` on strings IS UTF-16 code-unit order) and
-- cross-checked against tools/annotate/jcs.py on every value that tool accepts.
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

local CR = "\r"
local DEL = "\u{7F}" -- not escaped by RFC 8785
local C1 = "\u{80}" -- a C1 control, also not escaped
local ODIAERESIS = "\u{F6}"
local EACUTE = "\u{E9}"
local EURO = "\u{20AC}"
local DALET = "\u{FB33}" -- last-but-one BMP sample
local PRIVATE = "\u{E000}" -- BMP, one UTF-16 unit E000
local LINEAR_B = "\u{10000}" -- supplementary, UTF-16 units D800 DC00
local GRIN = "\u{1F600}" -- supplementary, UTF-16 units D83D DE00

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

-- MEMBER ORDERING.
--
-- Ordering is by UTF-16 code-unit sequence.  It is not byte order and not
-- code-point order: a supplementary code point encodes as a surrogate pair
-- starting in D800..DBFF, so it sorts BEFORE every BMP code point in
-- E000..FFFF, while both UTF-8 byte order and code-point order put the BMP one
-- first.  U+E000 against U+10000 is the minimal case that separates the three.
do
    local discriminator = {}
    discriminator[PRIVATE] = 1
    discriminator[LINEAR_B] = 2
    same(
        jcs.encode(discriminator),
        [==[{"]==] .. LINEAR_B .. [==[":2,"]==] .. PRIVATE .. [==[":1}]==],
        "UTF-16 order puts U+10000 before U+E000"
    )
end

-- RFC 8785 section 3.2.3's own ordering example, which exercises the same rule
-- through U+FB33 against U+1F600 and pins the short escape for CR, the
-- unescaped C1 control, and a digit name.
do
    local rfc = {}
    rfc[EURO] = "Euro Sign"
    rfc[CR] = "Carriage Return"
    rfc[DALET] = "Hebrew Letter Dalet With Dagesh"
    rfc["1"] = "One"
    rfc[GRIN] = "Emoji: Grinning Face"
    rfc[C1] = "Control"
    rfc[ODIAERESIS] = "Latin Small Letter O With Diaeresis"
    same(
        jcs.encode(rfc),
        [==[{"\r":"Carriage Return","1":"One","]==]
            .. C1
            .. [==[":"Control","]==]
            .. ODIAERESIS
            .. [==[":"Latin Small Letter O With Diaeresis","]==]
            .. EURO
            .. [==[":"Euro Sign","]==]
            .. GRIN
            .. [==[":"Emoji: Grinning Face","]==]
            .. DALET
            .. [==[":"Hebrew Letter Dalet With Dagesh"}]==],
        "RFC 8785 3.2.3 property ordering"
    )
end

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

-- NUMBERS: ECMAScript Number::toString, which is not printf and not tostring.
--
-- Luau's own tostring uses %.14g, so it disagrees with the required form on
-- several of these; the assertions below pin the required form, and the two
-- checks after them prove the disagreement is real rather than assumed.
same(
    jcs.encode({
        1e15,
        1e20,
        1e21,
        1e-6,
        1e-7,
        0.1,
        1 / 3,
        5e-324,
        1.7976931348623157e308,
        100,
        1.5,
        -2.5e-8,
        123.456,
        1152921504606846976,
        0.1 + 0.2,
    }),
    [==[[1000000000000000,100000000000000000000,1e+21,0.000001,1e-7,0.1,]==]
        .. [==[0.3333333333333333,5e-324,1.7976931348623157e+308,100,1.5,]==]
        .. [==[-2.5e-8,123.456,1152921504606847000,0.30000000000000004]]==],
    "ECMAScript Number::toString forms"
)

-- Luau's tostring is itself a shortest-round-trip formatter and agrees with the
-- required form on most values, which is exactly what makes its one
-- disagreement easy to ship: it zero-pads the exponent to two digits the way
-- printf does, and ECMAScript never pads.  Pinning the disagreement here stops
-- the vector above from quietly degenerating into "whatever tostring says".
assert(tostring(1e-7) == "1e-07", "Luau tostring no longer pads the exponent")
assert(tostring(-2.5e-8) == "-2.5e-08", "Luau tostring no longer pads the exponent")

-- Negative zero prints as "0".  The guard proves the value really is -0 and not
-- a constant-folded +0, without which this case would assert nothing.
do
    local negativeZero = -1 / math.huge
    assert(negativeZero == 0, "negative zero must compare equal to zero")
    assert(1 / negativeZero == -math.huge, "the value under test is not -0")
    same(jcs.encode({ negativeZero }), [==[[0]]==], "negative zero")
end

-- Luau numbers are IEEE-754 doubles, so an integer literal above 2^53 has
-- already been rounded before the serializer sees it.  The encoding certifies
-- the double that is held, not the literal that was written.
do
    local beyondExact = 9007199254740993
    assert(beyondExact == 9007199254740992, "2^53+1 is expected to round down")
    same(jcs.encode({ beyondExact }), [==[[9007199254740992]]==], "integer beyond 2^53")
end

-- STRING ESCAPING: only what RFC 8785 requires, with lowercase \u00XX for the
-- remaining controls.  DEL and the C1 controls are NOT escaped.
same(
    jcs.encode({ "\b\t\n\f\r" .. '"' .. "\\" .. "\0" .. "\u{1F}" .. DEL .. C1 }),
    [==[["\b\t\n\f\r\"\\\u0000\u001f]==] .. DEL .. C1 .. [==["]]==],
    "required escapes only"
)
same(
    jcs.encode({ EACUTE .. EURO .. GRIN }),
    [==[["]==] .. EACUTE .. EURO .. GRIN .. [==["]]==],
    "non-ASCII passes through as UTF-8"
)
same(jcs.encode({ "/" }), [==[["/"]]==], "solidus is not escaped")

-- REJECTION: refuse rather than emit something plausible.
do
    local nan = 0 / 0
    assert(nan ~= nan, "the value under test is not NaN")
    rejects("NaN", function()
        return jcs.encode({ nan })
    end)
end
rejects("positive infinity", function()
    return jcs.encode({ math.huge })
end)
rejects("negative infinity", function()
    return jcs.encode({ -math.huge })
end)
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

-- Lua cannot distinguish an empty array from an empty object, so an empty table
-- encodes as [], matching model.canonical.  Stated here so the limitation is
-- visible rather than discovered.
same(jcs.encode({}), [==[[]]==], "an empty table is an empty array")

return 1
