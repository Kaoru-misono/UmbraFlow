--!strict
-- hits: the operations on what a capture already produced that cost no capture
-- of their own.
--
-- WHY IT IS NOT IN `observe`. That file's own first paragraph says it owns
-- everything that COSTS A CAPTURE, and neither verb here does: one picks a line
-- out of a list the caller is already holding, and the other does arithmetic on
-- a hit's click point. They reach no ctx, open no cycle and spend no budget, so
-- a test can drive them with nothing bound at all -- which is exactly the
-- property that made it worth taking them out.
--
-- They are separate from the pure judgements that stayed behind (`matched`,
-- `better`, `exact_text`, `confident`) on a line the two sides can be read off:
-- those four answer a question ABOUT one value and return a boolean, while these
-- two return a hit -- and a value that can end in a click has to be minted, so
-- these need `evidence` and those need nothing.
--
-- It loads after `evidence` and before every module that observes, names only
-- `evidence`, and holds no state.

local hits = {}

-- hits.matching(lines, expected, compare) -> Hit?
--
-- The first line in `lines` whose text the comparison accepts, or nil.
--
-- FIRST WINS, and that is a rule rather than a shortcut, because the order is a
-- contract: the host returns lines top to bottom then left to right over the
-- same pixels every time, so "the first one that matches" names the same line on
-- every run. A verb that returned all of them and left the choice to the caller
-- would be handing back a list whose order the caller has to be told about
-- anyway.
--
-- The comparison is a PARAMETER with no default, for the reason
-- `observe.read_element` takes one: whether full width equals half width and
-- whether traditional equals simplified is a rule a project may replace. Pass
-- `observe.exact_text` for equality.
--
-- It does NOT consult the confidence. A reading nobody is sure of is not
-- evidence, but the floor an element needs is data on the element
-- (`model.Element.read_floor`) and judging against it is `observe.confident`; a
-- second, silent floor here would be a policy the caller cannot see.
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
-- WHOSE NUMBER THAT IS, which is the whole question this verb answers. The
-- offset belongs to the TASK, never to this layer. "The card sits eighty pixels
-- above its name" is a fact about the target's layout, and this framework owns
-- no facts about the target -- every one of them lives where it can be falsified
-- and replayed, which is the same reason expected text is data and the
-- comparison is policy. So no offset is computed here, defaulted here, or
-- remembered here; the caller states it at the call site and this does the
-- arithmetic.
--
-- WHY THE MECHANISM IS STILL THE FRAMEWORK'S. Because the alternative is a bare
-- coordinate click, and a business environment cannot reach one -- deliberately
-- (docs/plans/2026-08-01-three-layers-and-agent-operator.md 2). Without this a
-- task wanting the card its name sits in would have to click the name instead,
-- or be handed a privilege it must not have. This keeps the delivery on
-- `observe.click` with every check intact: the derived hit is minted from a hit
-- the framework minted, and it inherits that hit's ticket, so a line located on
-- one frame cannot be offset into a click on another.
--
-- THE RECTANGLE DOES NOT MOVE. Only the click point does. `x, y, width, height`
-- stay the evidence's -- where the frame found the thing -- because nothing here
-- located a card and nothing here may claim a rectangle for one. A reader of the
-- trace then sees a click delivered at a point beside the line that authorised
-- it, which is exactly what happened.
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
        -- No match handle, whatever the source hit carried. A match handle is
        -- C++'s own record of where a template landed, and delivering at a point
        -- this layer moved would make that record describe somewhere the click
        -- did not go.
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
