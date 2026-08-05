--!strict
-- project: the project file this layer owns, reads and writes.
--
-- The format is layer two's, including its schema version
-- (docs/plans/2026-07-31-script-owned-page-model.md 4 and 8). Layer one hands
-- over bytes for a name inside the project directory and nothing else, so
-- everything about what those bytes MEAN is here.
--
-- It is a TOML subset rather than a serialised Luau table because the file is
-- written by an agent and reviewed as a git diff: one field per line makes a
-- one-field change a one-line diff. The subset refuses what it does not support
-- by name rather than guessing.
--
-- Two rules carry forward compatibility, because a file outlives the build that
-- wrote it:
--
--   1. A project's own fields live under `extra`, so this layer can add a
--      top-level key next year without colliding with a project that invented
--      the same name.
--   2. An unknown key survives a rewrite BYTE FOR BYTE. Every line this parser
--      did not consume is carried on the object it belonged to as `residual`
--      and written back verbatim, because the alternative is silent deletion.
--
-- `save(load(bytes)) == bytes` holds for bytes already in canonical form. For
-- any other input the known keys come back canonical -- sections grouped and
-- sorted by name, fields in a fixed order -- while unknown lines keep their own
-- bytes, moved to the end of the section they were found in. Blank lines are
-- structural and regenerated rather than preserved.
--
-- ORDER IS NOT ALWAYS SORTED. Elements and pages are written sorted by name,
-- because their order means nothing. Appearances within an element and
-- references within a page keep their declared order, because declaration order
-- is what breaks a tie between two appearances -- sorting them would change
-- which pixels a click lands on.
--
-- `project` sorts after `mint`, `model`, `navigation`, `observe` and `oracle`.
-- It names the constructors in `model`, `navigation` and `oracle`, and nothing
-- from `observe`: parsing and writing a file costs no capture.

local k_schema   = "umbraflow-project/l2-v1"
local k_fileName = "page-model.toml"

local k_sectionKinds: { [string]: boolean } = {
    element    = true,
    appearance = true,
    page       = true,
    reference  = true,
    edge       = true,
    screen     = true,
    expect     = true,
}

-- Which section kinds carry a parsed `[<kind>.extra]` subtable. An appearance
-- does NOT, because the model has nowhere to hold one; leaving it out routes an
-- `[appearance.extra]` block down the raw residual path, so those bytes come
-- back instead of being parsed into a field nothing writes out.
local k_extraOwners: { [string]: boolean } = {
    element   = true,
    page      = true,
    reference = true,
    edge      = true,
    screen    = true,
    expect    = true,
}

local k_sectionFields: { [string]: { [string]: boolean } } = {
    element = {
        name          = true,
        capabilities  = true,
        rect          = true,
        expected_text = true,
        -- The other two spellings of what a region must read: ordered pieces
        -- with anything between them, and any confident reading at all. A row
        -- or an element states at most one of the three.
        expected_fragments = true,
        expected_presence  = true,
        read_floor    = true,
    },
    -- `key` is the colour key the template blob was cut under, written as
    -- [red, green, blue, tolerance] because it is one value with four
    -- components. The mask itself is in the PNG's alpha channel; this is the
    -- record of what carved it, which a later session needs to re-cut the glyph.
    --
    -- `key_removes` is that key's direction, and it is a SEPARATE line rather
    -- than a fifth component: the four are one measurement in one unit and a
    -- boolean among them would be neither. It is REQUIRED beside a key: a colour
    -- alone does not say whether the mask kept the pixels near it or dropped
    -- them, and those two cut complementary templates.
    appearance = {
        element     = true,
        name        = true,
        source      = true,
        threshold   = true,
        key         = true,
        key_removes = true,
    },
    -- `interrupt` and `catch_all` are the two ends of the order pages are asked
    -- in, and `state` / `over` say where a page belongs -- a mode for a scene, the
    -- modes it can cover for an overlay. All four are the page's own, so a build
    -- that does not know one keeps it as residual rather than dropping it.
    page = {
        name      = true,
        overlay   = true,
        interrupt = true,
        catch_all = true,
        state     = true,
        over      = true,
    },
    -- `expected_text` on a row is the per-page half of the element field of the
    -- same name: what the shared region reads on THIS page. An identify row on
    -- an element with no template is checked against it.
    reference = {
        page              = true,
        element           = true,
        holding           = true,
        exercised         = true,
        identify          = true,
        expected_text     = true,
        expected_fragments = true,
        expected_presence  = true,
        pinned_appearance = true,
        rect_override     = true,
    },
    -- An edge is a top-level section because it is a fact about a PAIR of pages
    -- and a trigger, and a `to` set spanning three pages has no single owner.
    -- `via_element` names an element on the from page and `via_key` names a key;
    -- a pop writes no `to`, because where it lands is the run's business.
    edge = {
        from        = true,
        to          = true,
        via         = true,
        via_element = true,
        via_key     = true,
        kind        = true,
    },
    -- A screen carries a name and the content hash of its PNG, and no path: the
    -- path is `assets/screens/<hash>.png` by construction, so a project cannot
    -- hold two facts about one picture that disagree. `page` is the optional
    -- third, which page this picture is OF, written last because it is the only
    -- one naming something else in this file.
    screen = {
        name = true,
        hash = true,
        page = true,
    },
    -- What the model states about one cell of the falsification matrix. It is a
    -- top-level section for the reason an edge is: it is a fact about a PAIR.
    --
    -- `rect` is WHERE the cell was measured and is part of the cell's identity
    -- rather than of the measurement: a shape with no rectangle of its own can
    -- be claimed several times on one screen, and the rectangle is what tells
    -- those claims apart. An element that draws its own writes none.
    --
    -- `appearance` and `text` name the two things a cell can be measured by --
    -- one template searched alone, or what the region reads on this screen --
    -- and a cell carries at most one of them.
    expect = {
        screen     = true,
        element    = true,
        rect       = true,
        appearance = true,
        text       = true,
        state      = true,
    },
}

-- Everything `load_project` hands back. The element and page entries are the
-- frozen tables `model` minted, spelled `any` because there is no module
-- resolver in this VM, only a global binding, so a chunk cannot name another
-- chunk's exported types.
export type Model = {
    schema: string,

    -- Where to poke a target that has stopped drawing anything this model can
    -- recognise, in target pixels, or nil when the project declares none. It is
    -- NOT a control: the whole point is a spot where a click does nothing even
    -- when the interface is fully drawn, so that waking a target and pressing
    -- one of its buttons cannot be the same act. `regress` refuses a project
    -- whose wake point lands inside any rectangle any page references, which is
    -- what makes "does nothing" a checked claim rather than an author's promise.
    wake_point: { x: number, y: number }?,

    elements: { any },
    element_by_name: { [string]: any },
    pages: { any },
    page_by_name: { [string]: any },
    graph: any,
    -- The screens this model is measured against and what it states about each
    -- cell, as one `oracle.Claims`, kept in this file rather than a second one
    -- (docs/plans/2026-07-31-annotation-model-capabilities.md 4-2.1).
    claims: any,
    residual: {
        preamble: { string },
        sections: { { string } },
    },
}

local project = {}

project.schema    = k_schema
project.file_name = k_fileName

local function trim(text: string): string
    return (string.gsub(text, "^%s*(.-)%s*$", "%1"))
end

-- ---------------------------------------------------------------- rendering

local function renderString(value: string): string
    local out = string.gsub(value, "[\\\"\n\t]", {
        ["\\"] = "\\\\",
        ['"']  = '\\"',
        ["\n"] = "\\n",
        ["\t"] = "\\t",
    })
    -- Any other control character would come back as a different byte or not at
    -- all, so it is refused while it can still be attributed to a field.
    if string.find(out, "%c") ~= nil then
        error(
            "a project file value contains a control character this format "
                .. "cannot carry",
            0
        )
    end
    return '"' .. out .. '"'
end

local function renderNumber(value: number): string
    if value % 1 ~= 0 then
        error("a project file value must be a whole number, got " .. tostring(value), 0)
    end
    return string.format("%d", value)
end

-- `field` is the key whose line is being written. Every refusal names it,
-- because the only values here this layer did not build itself are a project's
-- own under `extra`, and nothing else in the message would say which one.
--
-- A table is written as an array, so one whose keys are not 1..n is refused
-- instead: rendering it would put `[]` in the file and take every key with it,
-- and a save that stops is louder than a file that is quietly short.
local function renderValue(value: any, field: string): string
    local kind = type(value)
    if kind == "string" then
        return renderString(value)
    end
    if kind == "number" then
        return renderNumber(value)
    end
    if kind == "boolean" then
        return if value then "true" else "false"
    end
    if kind == "table" then
        local list  = value :: { any }
        local count = 0
        for _ in list do
            count += 1
        end
        if count ~= #list then
            error(
                'a project file cannot carry "'
                    .. field
                    .. '": a table is written as an array, and every key of '
                    .. "this one would be dropped",
                0
            )
        end
        local parts: { string } = {}
        for index = 1, #list do
            local entry = list[index]
            if type(entry) == "table" then
                error(
                    'a project file cannot carry "'
                        .. field
                        .. '": an array cannot contain another array',
                    0
                )
            end
            parts[index] = renderValue(entry, field)
        end
        return "[" .. table.concat(parts, ", ") .. "]"
    end
    error(
        'a project file cannot carry "'
            .. field
            .. '": it is a value of type '
            .. kind,
        0
    )
end

local function sortedKeys(map: { [string]: any }): { string }
    local keys: { string } = {}
    for key in map do
        if type(key) ~= "string" then
            error("an extra subtable may only have string keys", 0)
        end
        table.insert(keys, key)
    end
    table.sort(keys)
    return keys
end

local function pushExtra(lines: { string }, header: string, extra: { [string]: any })
    local keys = sortedKeys(extra)
    if #keys == 0 then
        return
    end
    table.insert(lines, "")
    table.insert(lines, header)
    for _, key in keys do
        table.insert(lines, key .. " = " .. renderValue(extra[key], key))
    end
end

local function pushResidual(lines: { string }, residual: { string })
    for _, line in residual do
        table.insert(lines, line)
    end
end

local function rectList(rect: any): { number }
    return { rect.x, rect.y, rect.width, rect.height }
end

-- The four components as the file writes them. The direction is not among them:
-- see `key_removes` in the known-key table above.
local function colourKeyList(key: any): { number }
    return { key.red, key.green, key.blue, key.tolerance }
end

-- project.encode(model) -> the file's text.
function project.encode(built: any): string
    if type(built) ~= "table" then
        error("project.encode needs the model to write", 2)
    end

    local lines: { string } = {}
    table.insert(lines, "schema = " .. renderString(k_schema))
    if built.wake_point ~= nil then
        table.insert(
            lines,
            "wake_point = "
                .. renderValue(
                    { built.wake_point.x, built.wake_point.y },
                    "wake_point"
                )
        )
    end
    if built.residual ~= nil then
        pushResidual(lines, built.residual.preamble or {})
    end

    local elements: { any } = table.clone(built.elements)
    table.sort(elements, function(left, right)
        return left.name < right.name
    end)

    for _, element in elements do
        table.insert(lines, "")
        table.insert(lines, "[[element]]")
        table.insert(lines, "name = " .. renderString(element.name))
        table.insert(
            lines,
            "capabilities = "
                .. renderValue(
                    model.capability_list(element.capabilities),
                    "capabilities"
                )
        )
        -- Written only when the element drew one. An element that leaves it out
        -- is placed by whoever uses it -- one confirm button reused on nine
        -- screens -- so materialising the rectangle it was cut from would put
        -- back the fact this model stopped stating.
        if element.rect ~= nil then
            table.insert(
                lines,
                "rect = " .. renderValue(rectList(element.rect), "rect")
            )
        end
        if element.expected_text ~= nil then
            table.insert(
                lines,
                "expected_text = " .. renderString(element.expected_text)
            )
        end
        if element.expected_fragments ~= nil then
            table.insert(
                lines,
                "expected_fragments = "
                    .. renderValue(element.expected_fragments, "expected_fragments")
            )
        end
        if element.expected_presence then
            table.insert(lines, "expected_presence = true")
        end
        -- Written only when the element stated one: the framework's own floor is
        -- the reading of an absent line, so materialising it would put a number
        -- nobody chose into every file and make it look chosen.
        if element.read_floor ~= nil then
            table.insert(
                lines,
                "read_floor = " .. renderNumber(element.read_floor)
            )
        end
        pushResidual(lines, element.residual)
        pushExtra(lines, "[element.extra]", element.extra)

        for _, appearance in element.appearances do
            table.insert(lines, "")
            table.insert(lines, "[[appearance]]")
            table.insert(lines, "element = " .. renderString(element.name))
            if appearance.name ~= nil then
                table.insert(lines, "name = " .. renderString(appearance.name))
            end
            table.insert(lines, "source = " .. renderString(appearance.source))
            table.insert(lines, "threshold = " .. renderNumber(appearance.threshold))
            if appearance.key ~= nil then
                table.insert(
                    lines,
                    "key = " .. renderValue(colourKeyList(appearance.key), "key")
                )
                -- Always written beside the key, never only when true. A key
                -- read the wrong way round selects the complement of its
                -- subject, so a file that leaves the direction to a default is
                -- a file whose masks cannot be reproduced from it.
                table.insert(
                    lines,
                    "key_removes = "
                        .. (if appearance.key.removes then "true" else "false")
                )
            end
            pushResidual(lines, appearance.residual)
        end
    end

    local pages: { any } = table.clone(built.pages)
    table.sort(pages, function(left, right)
        return left.name < right.name
    end)

    for _, page in pages do
        table.insert(lines, "")
        table.insert(lines, "[[page]]")
        table.insert(lines, "name = " .. renderString(page.name))
        -- A flag is written only when true: false is the absence of a refinement
        -- rather than a value with something to say.
        if page.overlay then
            table.insert(lines, "overlay = true")
        end
        if page.interrupt then
            table.insert(lines, "interrupt = true")
        end
        if page.catch_all then
            table.insert(lines, "catch_all = true")
        end
        -- After the flags and before the residual, in the order Page.new reads
        -- them: `state` and `over` are the two halves of one question -- where
        -- this page belongs -- and no page carries both.
        if page.state ~= nil then
            table.insert(lines, "state = " .. renderString(page.state))
        end
        if page.over ~= nil then
            table.insert(lines, "over = " .. renderValue(page.over, "over"))
        end
        pushResidual(lines, page.residual)
        pushExtra(lines, "[page.extra]", page.extra)

        for _, reference in page.references do
            table.insert(lines, "")
            table.insert(lines, "[[reference]]")
            table.insert(lines, "page = " .. renderString(page.name))
            table.insert(lines, "element = " .. renderString(reference.element.name))
            table.insert(lines, "holding = " .. renderString(reference.holding))
            table.insert(
                lines,
                "exercised = "
                    .. renderValue(
                        model.capability_list(reference.exercised),
                        "exercised"
                    )
            )
            if reference.identify ~= nil then
                table.insert(lines, "identify = " .. renderString(reference.identify))
            end
            if reference.expected_text ~= nil then
                table.insert(
                    lines,
                    "expected_text = " .. renderString(reference.expected_text)
                )
            end
            if reference.expected_fragments ~= nil then
                table.insert(
                    lines,
                    "expected_fragments = "
                        .. renderValue(
                            reference.expected_fragments,
                            "expected_fragments"
                        )
                )
            end
            if reference.expected_presence then
                table.insert(lines, "expected_presence = true")
            end
            if reference.pinned_appearance ~= nil then
                table.insert(
                    lines,
                    "pinned_appearance = " .. renderString(reference.pinned_appearance)
                )
            end
            if reference.rect_override ~= nil then
                table.insert(
                    lines,
                    "rect_override = "
                        .. renderValue(
                            rectList(reference.rect_override),
                            "rect_override"
                        )
                )
            end
            pushResidual(lines, reference.residual)
            pushExtra(lines, "[reference.extra]", reference.extra)
        end
    end

    -- Edges come after every page, because each names two of them. A model
    -- assembled by hand with no graph writes none.
    local edges: { any } = if built.graph ~= nil
        then table.clone(built.graph.edges)
        else {}
    table.sort(edges, function(left, right)
        if left.from.name ~= right.from.name then
            return left.from.name < right.from.name
        end
        return navigation.Edge.trigger_key(left) < navigation.Edge.trigger_key(right)
    end)

    for _, edge in edges do
        table.insert(lines, "")
        table.insert(lines, "[[edge]]")
        table.insert(lines, "from = " .. renderString(edge.from.name))
        if edge.to ~= nil then
            local names: { string } = {}
            for index, target in edge.to do
                names[index] = target.name
            end
            table.insert(lines, "to = " .. renderValue(names, "to"))
        end
        table.insert(lines, "via = " .. renderString(edge.via))
        if edge.via_element ~= nil then
            table.insert(
                lines,
                "via_element = " .. renderString(edge.via_element.name)
            )
        end
        if edge.via_key ~= nil then
            table.insert(lines, "via_key = " .. renderString(edge.via_key))
        end
        table.insert(lines, "kind = " .. renderString(edge.kind))
        pushResidual(lines, edge.residual)
        pushExtra(lines, "[edge.extra]", edge.extra)
    end

    -- The screens and the claims about them come last of the known sections,
    -- because an expectation names an element and a screen from above.
    local claims: any = built.claims
    local screens: { any } = if claims ~= nil then table.clone(claims.screens) else {}
    table.sort(screens, function(left, right)
        return left.name < right.name
    end)

    for _, screen in screens do
        table.insert(lines, "")
        table.insert(lines, "[[screen]]")
        table.insert(lines, "name = " .. renderString(screen.name))
        table.insert(lines, "hash = " .. renderString(screen.hash))
        -- Written only when the screen said one: an absent line is a screen
        -- nobody has annotated yet, and materialising a name would be this layer
        -- inventing a claim the matrix then measures.
        if screen.page ~= nil then
            table.insert(lines, "page = " .. renderString(screen.page))
        end
        pushResidual(lines, screen.residual)
        pushExtra(lines, "[screen.extra]", screen.extra)
    end

    local expectations: { any } = if claims ~= nil
        then table.clone(claims.expectations)
        else {}
    -- The rectangle is part of the sort key because one element can be claimed
    -- several times on one screen, and a sort stopping at the appearance would
    -- leave those rows in list order -- a file whose bytes depend on the order
    -- it was edited in. It compares as four numbers rather than as text, so
    -- [9, 0, 1, 1] sorts before [10, 0, 1, 1].
    table.sort(expectations, function(left, right)
        if left.screen ~= right.screen then
            return left.screen < right.screen
        end
        if left.element ~= right.element then
            return left.element < right.element
        end
        if (left.appearance or "") ~= (right.appearance or "") then
            return (left.appearance or "") < (right.appearance or "")
        end
        local here  = left.rect
        local there = right.rect
        if here == nil or there == nil then
            return here == nil and there ~= nil
        end
        for _, part in { "x", "y", "width", "height" } do
            if here[part] ~= there[part] then
                return here[part] < there[part]
            end
        end
        return false
    end)

    for _, expectation in expectations do
        table.insert(lines, "")
        table.insert(lines, "[[expect]]")
        table.insert(lines, "screen = " .. renderString(expectation.screen))
        table.insert(lines, "element = " .. renderString(expectation.element))
        if expectation.rect ~= nil then
            table.insert(
                lines,
                "rect = " .. renderValue(rectList(expectation.rect), "rect")
            )
        end
        if expectation.appearance ~= nil then
            table.insert(
                lines,
                "appearance = " .. renderString(expectation.appearance)
            )
        end
        if expectation.text ~= nil then
            table.insert(lines, "text = " .. renderString(expectation.text))
        end
        table.insert(lines, "state = " .. renderString(expectation.state))
        pushResidual(lines, expectation.residual)
        pushExtra(lines, "[expect.extra]", expectation.extra)
    end

    if built.residual ~= nil then
        for _, block in built.residual.sections or {} do
            table.insert(lines, "")
            pushResidual(lines, block)
        end
    end

    return table.concat(lines, "\n") .. "\n"
end

-- ------------------------------------------------------------------ parsing

local function parseString(text: string, lineNumber: number): (string, string)
    local out: { string } = {}
    local index = 2
    local length = #text
    while index <= length do
        local character = string.sub(text, index, index)
        if character == '"' then
            return table.concat(out), string.sub(text, index + 1)
        end
        if character == "\\" then
            local escaped = string.sub(text, index + 1, index + 1)
            if escaped == "n" then
                table.insert(out, "\n")
            elseif escaped == "t" then
                table.insert(out, "\t")
            elseif escaped == '"' then
                table.insert(out, '"')
            elseif escaped == "\\" then
                table.insert(out, "\\")
            else
                error(
                    "line "
                        .. tostring(lineNumber)
                        .. ': "\\'
                        .. escaped
                        .. '" is not an escape this format supports; it has '
                        .. '\\\\, \\", \\n and \\t',
                    0
                )
            end
            index += 2
        else
            table.insert(out, character)
            index += 1
        end
    end
    error(
        "line " .. tostring(lineNumber) .. ": a quoted value is never closed",
        0
    )
end

local parseValue

local function parseArray(text: string, lineNumber: number): (any, string)
    local list: { any } = {}
    local rest = trim(string.sub(text, 2))
    if string.sub(rest, 1, 1) == "]" then
        return list, string.sub(rest, 2)
    end
    while true do
        local value: any
        value, rest = parseValue(rest, lineNumber)
        table.insert(list, value)
        rest = trim(rest)
        local head = string.sub(rest, 1, 1)
        if head == "]" then
            return list, string.sub(rest, 2)
        end
        if head ~= "," then
            error(
                "line "
                    .. tostring(lineNumber)
                    .. ": an array needs a comma between values and a ] at the "
                    .. "end",
                0
            )
        end
        rest = trim(string.sub(rest, 2))
    end
end

function parseValue(text: string, lineNumber: number): (any, string)
    local body = trim(text)
    local head = string.sub(body, 1, 1)
    if head == '"' then
        return parseString(body, lineNumber)
    end
    if head == "[" then
        return parseArray(body, lineNumber)
    end
    local word = string.match(body, "^[%w%-%+%.]+")
    if word == nil then
        error(
            "line " .. tostring(lineNumber) .. ": there is no value after the =",
            0
        )
    end
    local rest = string.sub(body, #word + 1)
    if word == "true" then
        return true, rest
    end
    if word == "false" then
        return false, rest
    end
    if string.match(word, "^%-?%d+$") ~= nil then
        return tonumber(word), rest
    end
    error(
        "line "
            .. tostring(lineNumber)
            .. ': "'
            .. word
            .. '" is not a value this format supports; it has quoted strings, '
            .. "whole numbers, true, false, and arrays of those",
        0
    )
end

local function parseAssignment(line: string, lineNumber: number): (string?, any)
    local key, remainder = string.match(line, "^([%w_%-]+)%s*=%s*(.*)$")
    if key == nil then
        return nil, nil
    end
    local value, rest = parseValue(remainder :: string, lineNumber)
    if trim(rest) ~= "" then
        error(
            "line "
                .. tostring(lineNumber)
                .. ": there is more than one value on this line",
            0
        )
    end
    return key, value
end

-- project.parse(text) -> the document behind the text.
--
-- The result is deliberately NOT a model: it has no host handles, so it is pure
-- and a test can round-trip it without a VM that owns a session. `project.build`
-- turns one into a model by loading each appearance's template through ctx.
function project.parse(text: string): any
    if type(text) ~= "string" then
        error("project.parse needs the file's text", 2)
    end

    local document = {
        schema     = nil :: string?,
        wake_point = nil :: any,
        sections   = {} :: { any },
        preamble   = {} :: { string },
        blocks     = {} :: { { string } },
    }

    local current: any        = nil
    local rawTarget: { string }? = nil
    local inExtra             = false
    local lineNumber          = 0

    local function residualTarget(): { string }
        if current ~= nil then
            return current.residual
        end
        return document.preamble
    end

    for line in string.gmatch(text .. "\n", "([^\n]*)\n") do
        lineNumber += 1
        local body = trim(line)

        local sectionName = string.match(body, "^%[%[([%w_%-]+)%]%]$")
        if sectionName ~= nil then
            rawTarget = nil
            inExtra   = false
            if k_sectionKinds[sectionName] then
                current = {
                    kind     = sectionName,
                    fields   = {} :: { [string]: any },
                    extra    = {} :: { [string]: any },
                    residual = {} :: { string },
                    line     = lineNumber,
                }
                table.insert(document.sections, current)
            else
                -- A section kind this build does not know: kept as raw lines and
                -- written back at the end of the file.
                current   = nil
                local block: { string } = { line }
                rawTarget               = block
                table.insert(document.blocks, block)
            end
        else
            local subName = string.match(body, "^%[([%w_%-%.]+)%]$")
            if subName ~= nil then
                inExtra = false
                if
                    current ~= nil
                    and k_extraOwners[current.kind]
                    and subName == current.kind .. ".extra"
                then
                    inExtra   = true
                    rawTarget = nil
                else
                    -- An open raw block wins over the residual: a sub-table
                    -- inside a section kind this build does not know belongs to
                    -- that section's block, and appending it to the preamble
                    -- instead would hoist it above every header on rewrite.
                    local target = rawTarget or residualTarget()
                    table.insert(target, line)
                    rawTarget = target
                end
            elseif body == "" then
                -- Blank lines are structure, and the writer regenerates them.
            elseif string.sub(body, 1, 1) == "#" then
                table.insert(rawTarget or residualTarget(), line)
            elseif rawTarget ~= nil then
                table.insert(rawTarget, line)
            else
                local key, value = parseAssignment(body, lineNumber)
                if key == nil then
                    error(
                        "line "
                            .. tostring(lineNumber)
                            .. ": this is not a section header, a comment, or a "
                            .. "key = value line",
                        0
                    )
                elseif current == nil then
                    if key == "schema" then
                        document.schema = value
                    elseif key == "wake_point" then
                        document.wake_point = value
                    else
                        table.insert(document.preamble, line)
                    end
                elseif inExtra then
                    current.extra[key] = value
                elseif k_sectionFields[current.kind][key] then
                    current.fields[key] = value
                else
                    table.insert(current.residual, line)
                end
            end
        end
    end

    if document.schema == nil then
        error(
            'this project file has no schema line; the first line should be '
                .. "schema = "
                .. renderString(k_schema),
            2
        )
    end
    if document.schema ~= k_schema then
        error(
            "this project file is schema '"
                .. tostring(document.schema)
                .. "' and this build reads '"
                .. k_schema
                .. "'",
            2
        )
    end

    return document
end

-- ----------------------------------------------------------------- building

local function requireField(section: any, key: string): any
    local value = section.fields[key]
    if value == nil then
        error(
            "the [["
                .. section.kind
                .. "]] starting at line "
                .. tostring(section.line)
                .. " has no "
                .. key,
            0
        )
    end
    return value
end

local function rectOf(section: any, key: string, value: any): any
    if type(value) ~= "table" or #value ~= 4 then
        error(
            "the [["
                .. section.kind
                .. "]] starting at line "
                .. tostring(section.line)
                .. " needs "
                .. key
                .. " = [x, y, width, height]",
            0
        )
    end
    return {
        x      = value[1],
        y      = value[2],
        width  = value[3],
        height = value[4],
    }
end

-- The wake point the preamble declared, or nil when it declared none. The shape
-- is checked here rather than at the point of use, because by the time a run is
-- lost enough to need it, the file is not what anyone is looking at.
local function wakePointOf(value: any): any
    if value == nil then
        return nil
    end
    if type(value) ~= "table" or #value ~= 2 then
        error("this project's wake_point must be [x, y]", 0)
    end
    if not mint.whole_number(value[1]) or not mint.whole_number(value[2]) then
        error("this project's wake_point must be two whole numbers of pixels", 0)
    end
    if value[1] < 0 or value[2] < 0 then
        error("this project's wake_point must be inside the target", 0)
    end
    return table.freeze({ x = value[1], y = value[2] })
end

-- The colour key an appearance section wrote, or nil when it wrote none. Only
-- the LIST SHAPE is checked here, because by the time `Element.new` sees a
-- malformed key it no longer knows which line of which file it came from.
-- Whether the four numbers are a channel and a tolerance stays the constructor's.
local function colourKeyOf(section: any, value: any, removes: any): any
    if value == nil then
        if removes ~= nil then
            error(
                "the [["
                    .. section.kind
                    .. "]] starting at line "
                    .. tostring(section.line)
                    .. " states key_removes but no key; the flag says which way "
                    .. "round a colour was read, and there is no colour here",
                0
            )
        end
        return nil
    end
    if type(removes) ~= "boolean" then
        error(
            "the [["
                .. section.kind
                .. "]] starting at line "
                .. tostring(section.line)
                .. " states a key and no key_removes; a colour alone does not "
                .. "say whether the mask kept the pixels near it or dropped "
                .. "them, and the two cut complementary templates",
            0
        )
    end
    if type(value) ~= "table" or #value ~= 4 then
        error(
            "the [["
                .. section.kind
                .. "]] starting at line "
                .. tostring(section.line)
                .. " needs key = [red, green, blue, tolerance]",
            0
        )
    end
    return {
        red       = value[1],
        green     = value[2],
        blue      = value[3],
        tolerance = value[4],
        removes   = removes,
    }
end

-- project.build(ctx, document) -> the model.
--
-- Every appearance's template blob is read from the project directory and
-- decoded here, the only thing in this file that touches the host. Loading the
-- same blob twice returns the same handle, so the cache below saves a read
-- rather than changing an answer.
function project.build(ctx: any, document: any): any
    if type(ctx) ~= "table" then
        error("project.build needs the ctx it should load templates through", 2)
    end

    local elementSections: { any }            = {}
    local appearancesFor: { [string]: { any } } = {}
    local pageSections: { any }               = {}
    local referencesFor: { [string]: { any } } = {}
    local edgeSections: { any }               = {}
    local screenSections: { any }             = {}
    local expectSections: { any }             = {}

    for _, section in document.sections do
        if section.kind == "element" then
            table.insert(elementSections, section)
        elseif section.kind == "screen" then
            table.insert(screenSections, section)
        elseif section.kind == "expect" then
            table.insert(expectSections, section)
        elseif section.kind == "appearance" then
            local owner = requireField(section, "element")
            local list  = appearancesFor[owner]
            if list == nil then
                list = {}
                appearancesFor[owner] = list
            end
            table.insert(list, section)
        elseif section.kind == "page" then
            table.insert(pageSections, section)
        elseif section.kind == "edge" then
            table.insert(edgeSections, section)
        else
            local owner = requireField(section, "page")
            local list  = referencesFor[owner]
            if list == nil then
                list = {}
                referencesFor[owner] = list
            end
            table.insert(list, section)
        end
    end

    -- Every bucket key above has to name a section this file declares. A bucket
    -- nobody owns is read by neither loop below and is not raw residual either,
    -- so the next save would delete those lines silently. Refused with the line
    -- the section starts on, like every other cross-reference here; the lowest
    -- line wins, so two runs over one file name one section.
    local function requireOwners(
        buckets: { [string]: { any } },
        declared: { [string]: any },
        field: string
    )
        local orphan: any = nil
        for owner, sections in buckets do
            if declared[owner] == nil then
                for _, section in sections do
                    if orphan == nil or section.line < orphan.line then
                        orphan = section
                    end
                end
            end
        end
        if orphan ~= nil then
            error(
                "the [["
                    .. orphan.kind
                    .. "]] starting at line "
                    .. tostring(orphan.line)
                    .. " names "
                    .. field
                    .. " '"
                    .. tostring(orphan.fields[field])
                    .. "', which this project file does not declare",
                0
            )
        end
    end

    local templates: { [string]: any } = {}
    local function templateOf(source: string): any
        local handle = templates[source]
        if handle == nil then
            handle = ctx:template_load(ctx:project_read(source))
            templates[source] = handle
        end
        return handle
    end

    local elements: { any }             = {}
    local elementByName: { [string]: any } = {}
    for _, section in elementSections do
        local name        = requireField(section, "name")
        local appearances = {}
        local ownAppearances: { any } = appearancesFor[name] or {}
        for index, appearanceSection in ownAppearances do
            local source = requireField(appearanceSection, "source")
            appearances[index] = {
                name      = appearanceSection.fields.name,
                source    = source,
                template  = templateOf(source),
                threshold = requireField(appearanceSection, "threshold"),
                key       = colourKeyOf(
                    appearanceSection,
                    appearanceSection.fields.key,
                    appearanceSection.fields.key_removes
                ),
                residual  = appearanceSection.residual,
            }
        end

        -- `rect` is required nowhere: an element that omits it is placed by
        -- whoever uses it, and the rows and claims that do the placing are
        -- refused by their own constructors when they name no rectangle either.
        local rectValue = section.fields.rect
        local element   = model.Element.new({
            name          = name,
            capabilities  = requireField(section, "capabilities"),
            rect          = if rectValue ~= nil
                then rectOf(section, "rect", rectValue)
                else nil,
            appearances   = appearances,
            expected_text = section.fields.expected_text,
            expected_fragments = section.fields.expected_fragments,
            expected_presence  = section.fields.expected_presence,
            read_floor    = section.fields.read_floor,
            extra         = section.extra,
            residual      = section.residual,
        })
        if elementByName[name] ~= nil then
            error("this project file declares element '" .. name .. "' twice", 0)
        end
        elementByName[name] = element
        table.insert(elements, element)
    end

    requireOwners(appearancesFor, elementByName, "element")

    -- One element has AT MOST ONE owning page. Holding is not exclusivity --
    -- other pages may reference it freely -- it answers where the element lives,
    -- and two answers is a model with no home for its own pixels
    -- (docs/plans/2026-07-31-annotation-model-capabilities.md 5.4).
    local ownedBy: { [string]: string } = {}

    local pages: { any }              = {}
    local pageByName: { [string]: any } = {}
    for _, section in pageSections do
        local name       = requireField(section, "name")
        local references = {}
        local ownReferences: { any } = referencesFor[name] or {}
        for index, referenceSection in ownReferences do
            local elementName = requireField(referenceSection, "element")
            local element     = elementByName[elementName]
            if element == nil then
                error(
                    "page '"
                        .. name
                        .. "' references element '"
                        .. elementName
                        .. "', which this project file does not declare",
                    0
                )
            end
            local holding = requireField(referenceSection, "holding")
            if holding == "owned" then
                local previous = ownedBy[elementName]
                if previous ~= nil then
                    error(
                        "element '"
                            .. elementName
                            .. "' is owned by page '"
                            .. previous
                            .. "' and again by page '"
                            .. name
                            .. "'; an element has one home",
                        0
                    )
                end
                ownedBy[elementName] = name
            end

            local overrideValue = referenceSection.fields.rect_override
            references[index] = {
                element           = element,
                holding           = holding,
                exercised         = requireField(referenceSection, "exercised"),
                identify          = referenceSection.fields.identify,
                expected_text     = referenceSection.fields.expected_text,
                expected_fragments =
                    referenceSection.fields.expected_fragments,
                expected_presence =
                    referenceSection.fields.expected_presence,
                pinned_appearance = referenceSection.fields.pinned_appearance,
                rect_override     = if overrideValue ~= nil
                    then rectOf(referenceSection, "rect_override", overrideValue)
                    else nil,
                extra             = referenceSection.extra,
                residual          = referenceSection.residual,
            }
        end

        local page = model.Page.new({
            name       = name,
            overlay    = section.fields.overlay,
            interrupt  = section.fields.interrupt,
            catch_all  = section.fields.catch_all,
            state      = section.fields.state,
            over       = section.fields.over,
            references = references,
            extra      = section.extra,
            residual   = section.residual,
        })
        if pageByName[name] ~= nil then
            error("this project file declares page '" .. name .. "' twice", 0)
        end
        pageByName[name] = page
        table.insert(pages, page)
    end

    requireOwners(referencesFor, pageByName, "page")

    -- A page an edge names has to be one this file declares. Saying so here
    -- rather than inside Edge.new is what gives the message a LINE NUMBER.
    local function pageOf(section: any, key: string, name: any): any
        local page = pageByName[name]
        if page == nil then
            error(
                "the [[edge]] starting at line "
                    .. tostring(section.line)
                    .. " names page '"
                    .. tostring(name)
                    .. "' as its "
                    .. key
                    .. ", which this project file does not declare",
                0
            )
        end
        return page
    end

    local edges: { any } = {}
    for _, section in edgeSections do
        local from = pageOf(section, "from", requireField(section, "from"))

        local destinations: { any }? = nil
        local declared = section.fields.to
        if declared ~= nil then
            if type(declared) ~= "table" then
                error(
                    "the [[edge]] starting at line "
                        .. tostring(section.line)
                        .. ' needs to = ["page", ...]',
                    0
                )
            end
            destinations = {}
            for index, name in declared :: { any } do
                (destinations :: { any })[index] = pageOf(section, "to", name)
            end
        end

        local viaElement: any = nil
        local elementName     = section.fields.via_element
        if elementName ~= nil then
            viaElement = elementByName[elementName]
            if viaElement == nil then
                error(
                    "the [[edge]] starting at line "
                        .. tostring(section.line)
                        .. " is taken by clicking element '"
                        .. tostring(elementName)
                        .. "', which this project file does not declare",
                    0
                )
            end
        end

        table.insert(
            edges,
            navigation.Edge.new({
                from        = from,
                to          = destinations,
                via         = requireField(section, "via"),
                via_element = viaElement,
                via_key     = section.fields.via_key,
                kind        = requireField(section, "kind"),
                extra       = section.extra,
                residual    = section.residual,
            })
        )
    end

    local screens: { any }                = {}
    local screenByName: { [string]: any } = {}
    for _, section in screenSections do
        -- A page a screen names has to be one this file declares, checked here
        -- for the reason `pageOf` above is: `Screen.new` is handed one screen's
        -- own facts and can see no pages, and this loop knows the line number. It
        -- runs before the constructor, so such a file is refused rather than
        -- minted and thrown away.
        local declaredPage = section.fields.page
        if declaredPage ~= nil and pageByName[declaredPage] == nil then
            error(
                "the [[screen]] starting at line "
                    .. tostring(section.line)
                    .. " says it is page '"
                    .. tostring(declaredPage)
                    .. "', which this project file does not declare",
                0
            )
        end

        local screen = oracle.Screen.new({
            name     = requireField(section, "name"),
            hash     = requireField(section, "hash"),
            page     = declaredPage,
            extra    = section.extra,
            residual = section.residual,
        })
        screenByName[screen.name] = screen
        table.insert(screens, screen)
    end

    local expectations: { any } = {}
    for _, section in expectSections do
        local screenName = requireField(section, "screen")
        local screen     = screenByName[screenName]
        if screen == nil then
            error(
                "the [[expect]] starting at line "
                    .. tostring(section.line)
                    .. " names screen '"
                    .. tostring(screenName)
                    .. "', which this project file does not declare",
                0
            )
        end
        local elementName = requireField(section, "element")
        local element     = elementByName[elementName]
        if element == nil then
            error(
                "the [[expect]] starting at line "
                    .. tostring(section.line)
                    .. " names element '"
                    .. tostring(elementName)
                    .. "', which this project file does not declare",
                0
            )
        end
        local claimRect = section.fields.rect
        table.insert(
            expectations,
            oracle.Expectation.new({
                screen     = screen,
                element    = element,
                rect       = if claimRect ~= nil
                    then rectOf(section, "rect", claimRect)
                    else nil,
                appearance = section.fields.appearance,
                text       = section.fields.text,
                state      = requireField(section, "state"),
                extra      = section.extra,
                residual   = section.residual,
            })
        )
    end

    return {
        schema          = document.schema,
        wake_point      = wakePointOf(document.wake_point),
        elements        = elements,
        element_by_name = elementByName,
        pages           = pages,
        page_by_name    = pageByName,
        graph           = navigation.Graph.new({ pages = pages, edges = edges }),
        claims          = oracle.Claims.new({
            screens      = screens,
            expectations = expectations,
        }),
        residual        = {
            preamble = document.preamble,
            sections = document.blocks,
        },
    }
end

-- project.load_project(ctx) -> the model in the generation's project directory.
--
-- The file name is a constant of the schema rather than a parameter: a project
-- whose model lives under a name this layer does not know is one it cannot open.
-- It is exported as `project.file_name` so nothing spells it twice.
function project.load_project(ctx: any): any
    if type(ctx) ~= "table" then
        error("project.load_project needs the ctx to read the file through", 2)
    end
    return project.build(ctx, project.parse(ctx:project_read(k_fileName)))
end

-- project.save_project(ctx, model) -> ()
--
-- Writes the model back over the file it came from. Everything the parser did
-- not understand rides along on the objects and is written out again, so saving
-- a newer build's file loses nothing.
function project.save_project(ctx: any, built: any)
    if type(ctx) ~= "table" then
        error("project.save_project needs the ctx to write the file through", 2)
    end
    ctx:project_write(k_fileName, project.encode(built))
end

return project
