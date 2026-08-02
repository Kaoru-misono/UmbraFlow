--!strict
-- reading: the text half of the falsification matrix. An element with no
-- templates identifies by the text its rectangle reads
-- (docs/plans/2026-08-01-three-layers-and-agent-operator.md 5-4), so the matrix
-- has a second kind of cell and a second kind of contradiction. `regress` is the
-- walk that spends it.
--
-- CONFIDENCE IS THE SCORE, which is what lets a text cell be a matrix cell at
-- all: the engine reports how sure it was and an element declares the floor it
-- must clear (`model.Element.read_floor`), so a cell is a match when the reading
-- says the claimed text AND the engine was sure enough to be believed.
--
-- It owns no comparison of its own -- equality is `observe.exact_text` and the
-- floor test is `observe.confident` -- because a second copy is where the matrix
-- and the runtime would come to disagree about what a page is.
--
-- `reading` sorts after `model`, `observe`, `oracle` and `project` and before
-- `regress`. It holds no state: every verb takes the ctx it reads through and
-- the model it is asked about.

local reading = {}

-- What one text cell measured. `text` and `confidence` are nil together, and
-- only when the engine answered with nothing at all; that is a `matched` of
-- false like any other, and the absent pair is what says there was no reading
-- rather than a wrong one.
export type Measurement = {
    text: string?,
    confidence: number?,
    matched: boolean,
}

-- Two screens whose claims about one element cannot tell them apart. `screen` is
-- the first screen that claimed this text and `rival` the one that repeated it,
-- in the order the project file lists them, so the same file reports the same
-- pair every time. `rect` is the region the two claims share, nil for an element
-- that draws its own: the rule is about ONE REGION reading one text on two
-- screens, and a shape with no rectangle of its own has as many regions as the
-- file gives it coordinates.
export type Confusion = {
    element: string,
    rect: any,
    text: string,
    screen: string,
    rival: string,
}

-- reading.measure(ctx, ticket, element, rect, expected) -> Measurement
--
-- Reads `rect` on the frame behind `ticket` and says whether it read `expected`
-- well enough to be believed at `element`'s own floor.
--
-- The rectangle is a PARAMETER because only one of the two kinds of element
-- holds one. An element that draws its own is read there -- never at a page's
-- refinement, since the matrix asks whether this region can be caught saying
-- something it should not, so the widest region the model ever reads is the
-- honest one. An element that draws none is read where the CLAIM says, one
-- confirm button reused on nine screens sitting somewhere different on each.
-- `regress` picks between the two, once.
--
-- The read is NOT wrapped in `pcall`. `cycle_read` raises when the cycle's read
-- budget is spent or no engine is bound, and a read that never happened has
-- established nothing; turning that into "this region does not read that" would
-- make the matrix's verdict a function of a configuration number.
function reading.measure(
    ctx: any,
    ticket: any,
    element: any,
    rect: any,
    expected: string
): Measurement
    local found =
        ctx:cycle_read(ticket, rect.x, rect.y, rect.width, rect.height)
    if found == nil then
        return {
            text       = nil,
            confidence = nil,
            matched    = false,
        }
    end

    return {
        text       = found.text,
        confidence = found.confidence,
        -- Floor first: a reading under it is a guess, and a guess that happens
        -- to spell the right word is still a guess.
        matched    = observe.confident(found, model.Element.read_floor(element))
            and observe.exact_text(found.text, expected),
    }
end

-- reading.confusions(claims) -> every pair of screens one element's claims
-- cannot tell apart.
--
-- The text analogue of "no two appearances of one element may hit one screen",
-- from the other side: that rule catches one screen answering to two marks, this
-- one catches one mark answering to two screens. If the file says this region
-- reads "battle" on two screens and means `match` both times, a page signature
-- resting on it resolves on whichever of the two is in front of the run.
--
-- It is a FINDING and not a refusal, which is why it is not in `Claims.new`: two
-- pages really can print one word and some other reference may separate them.
--
-- It does not ask whether the two screens are two pages; that judgement is
-- `regress`'s, through `recognition.same_page`. Pages are not in a `Claims` at
-- all, and what is returned here stays a fact when the two screens turn out to
-- be one page captured at two scroll positions.
--
-- It reads the claims and not the screens, so nothing here costs a capture.
--
-- It is keyed by the REGION and not by the element. Nine confirm buttons are one
-- element and nine rectangles; keyed by the element the rule would fire on every
-- one of them and be switched off, taking with it the case it exists for.
function reading.confusions(claims: any): { Confusion }
    local found: { Confusion } = {}
    -- element, rectangle and text, NUL separated because none of the three can
    -- contain one, so two different triples cannot collide by concatenation.
    local firstClaim: { [string]: string } = {}

    for index = 1, #claims.expectations do
        local expectation = claims.expectations[index]
        if
            expectation.text ~= nil
            and expectation.state == oracle.state_match
        then
            local key = expectation.element
                .. "\0"
                .. oracle.rect_key(expectation.rect)
                .. "\0"
                .. expectation.text
            local earlier = firstClaim[key]
            if earlier == nil then
                firstClaim[key] = expectation.screen
            else
                table.insert(found, {
                    element = expectation.element,
                    rect    = expectation.rect,
                    text    = expectation.text,
                    screen  = earlier,
                    rival   = expectation.screen,
                })
            end
        end
    end

    return found
end

return reading
