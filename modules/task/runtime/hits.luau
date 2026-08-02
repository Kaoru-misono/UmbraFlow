--!strict
-- hits: the operations on what a capture already produced that cost no capture
-- of their own. They reach no ctx, open no cycle and spend no budget, so a test
-- can drive them with nothing bound -- which is why they are not in `observe`,
-- which owns everything that costs a capture.
--
-- They return a hit rather than a boolean, and a value that can end in a click
-- has to be minted; that is what separates them from the pure judgements that
-- stayed in `observe` (`matched`, `better`, `exact_text`, `confident`).
--
-- It loads after `evidence` and before every module that observes, names only
-- `evidence`, and holds no state.

local hits = {}

-- hits.matching(lines, expected, compare) -> Hit?
--
-- The first line in `lines` whose text the comparison accepts, or nil.
--
-- FIRST WINS is a contract, not a shortcut: the host returns lines top to bottom
-- then left to right over the same pixels every time, so "the first one that
-- matches" names the same line on every run.
--
-- The comparison is a parameter with no default, because whether full width
-- equals half width and whether traditional equals simplified is a rule a
-- project may replace. Pass `observe.exact_text` for equality.
--
-- It does NOT consult the confidence: the floor an element needs is data on the
-- element (`model.Element.read_floor`) and judging against it is
-- `observe.confident`, so a second floor here would be invisible policy.
function hits.matching(lines: { any }, expected: string, compare: any): any
    if type(compare) ~= "function" then
        error(
            "hits.matching needs the comparison to judge a line's text with; "
                .. "pass observe.exact_text for equality",
            2
        )
    end
    for index = 1, #lines do
        if compare(lines[index].text, expected) == true then
            return lines[index]
        end
    end
    return nil
end

-- hits.offset(hit, dx, dy) -> Hit
--
-- The same hit, clicked `dx` right and `dy` down of where it would have been.
--
-- The offset belongs to the TASK. "The card sits eighty pixels above its name"
-- is a fact about the target's layout, and this framework owns no facts about
-- the target, so no offset is computed, defaulted or remembered here; the caller
-- states it and this does the arithmetic.
--
-- The mechanism is the framework's because the alternative is a bare coordinate
-- click, which a business environment cannot reach
-- (docs/plans/2026-08-01-three-layers-and-agent-operator.md 2). The derived hit
-- is minted from a hit the framework minted and inherits that hit's ticket, so a
-- line located on one frame cannot be offset into a click on another.
--
-- THE RECTANGLE DOES NOT MOVE; only the click point does. `x, y, width, height`
-- stay where the frame found the thing, because nothing here located a card.
function hits.offset(hit: any, dx: number, dy: number): any
    local ticket = evidence.hit_ticket(hit)
    if ticket == nil then
        error(
            "hits.offset takes a hit that observe.find or observe.read_lines "
                .. "returned, and nothing else",
            2
        )
    end
    if
        type(dx) ~= "number"
        or type(dy) ~= "number"
        or dx ~= math.floor(dx)
        or dy ~= math.floor(dy)
    then
        error("hits.offset needs whole pixel offsets", 2)
    end

    local clickX = hit.click_x + dx
    local clickY = hit.click_y + dy
    if clickX < 0 or clickY < 0 then
        error(
            "hits.offset was asked for a click at ("
                .. tostring(clickX)
                .. ", "
                .. tostring(clickY)
                .. "), which is off the target",
            2
        )
    end

    return evidence.mint_hit({
        element       = hit.element,
        page          = hit.page,
        appearance    = hit.appearance,
        positioned_by = hit.positioned_by,
        interact      = hit.interact,
        -- No match handle, whatever the source hit carried: it is C++'s record
        -- of where a template landed, and this layer moved the click point away
        -- from it.
        match         = nil,
        x             = hit.x,
        y             = hit.y,
        width         = hit.width,
        height        = hit.height,
        click_x       = clickX,
        click_y       = clickY,
        score         = hit.score,
        maximum       = hit.maximum,
        text          = hit.text,
        confidence    = hit.confidence,
    }, ticket)
end

return hits
