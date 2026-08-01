--!strict
-- reading: the text half of the falsification matrix.
--
-- WHAT IT IS FOR. An element with no templates identifies by the text its
-- rectangle reads (docs/plans/2026-08-01-three-layers-and-agent-operator.md
-- 5-4), so the matrix has a second kind of cell to measure and a second kind of
-- contradiction to look for. Neither has anything to do with a sum of absolute
-- differences, and putting them in `regress` beside the template walk would have
-- made one file hold two unrelated measurements, two verdict shapes and two sets
-- of rules. This is that half, and `regress` is the walk that spends it.
--
-- CONFIDENCE IS THE SCORE. That is the whole reason a text cell can be in the
-- matrix at all. A template cell is falsified against a distance, and reading
-- was long held to have no equivalent -- but the engine reports how sure it was,
-- an element declares the floor it has to clear (`model.Element.read_floor`),
-- and those two are exactly a score and a threshold. So a cell is a match when
-- the reading says the claimed text AND the engine was sure enough to be
-- believed, and a reading nobody is sure of is not evidence of anything.
--
-- IT OWNS NO COMPARISON OF ITS OWN. Equality is `observe.exact_text` and the
-- floor test is `observe.confident`, both reached rather than reimplemented. A
-- second copy of either is where the matrix and the runtime would come to
-- disagree about whether a page is the page it says it is -- and the whole point
-- of the matrix is that it measures what a run would do.
--
-- LOAD ORDER. `reading` sorts after `model`, `observe`, `oracle` and `project`
-- and before `regress`, which is what lets it name the three it uses and lets
-- `regress` name it. It holds no state: every verb takes the ctx it reads
-- through and the model it is asked about.

local reading = {}

-- What one text cell measured.
--
-- `text` and `confidence` are nil together, and only when the engine answered
-- with nothing at all. That is a `matched` of false like any other -- the region
-- does not read the claimed text -- and the pair being absent is what tells a
-- reader afterwards that there was no reading rather than a wrong one.
export type Measurement = {
    text: string?,
    confidence: number?,
    matched: boolean,
}

-- Two screens whose claims about one element cannot tell them apart.
--
-- `screen` is the first screen that claimed this text and `rival` is the one
-- that repeated it, in the order the project file lists them, so the same file
-- reports the same pair every time.
export type Confusion = {
    element: string,
    text: string,
    screen: string,
    rival: string,
}

-- reading.measure(ctx, ticket, element, expected) -> Measurement
--
-- Reads the element's own rectangle on the frame behind `ticket` and says
-- whether it read `expected`.
--
-- IT READS THE ELEMENT'S OWN RECTANGLE AND NOT A PAGE'S REFINEMENT, for the
-- reason `regress`'s template search does: the matrix asks whether this region
-- can be found saying something it should not, so the widest region the model
-- ever reads is the honest one. A page's `rect_override` narrows a click and is
-- refused on an identify row anyway.
--
-- THE READ IS NOT WRAPPED IN `pcall`. `cycle_read` raises when the cycle's read
-- budget is spent or the host has no engine bound, and a read that never
-- happened has established nothing -- exactly as a stopped search has not.
-- Turning either into "this region does not read that" would make the matrix's
-- verdict a function of a configuration number, which is the one thing a
-- falsification run must never be.
function reading.measure(
    ctx: any,
    ticket: any,
    element: any,
    expected: string
): Measurement
    local rect  = element.rect
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
        -- Both halves, and in this order because the cheap one settles most
        -- cells: a reading under the floor is a guess, and a guess that happens
        -- to spell the right word is still a guess.
        matched    = observe.confident(found, model.Element.read_floor(element))
            and observe.exact_text(found.text, expected),
    }
end

-- reading.confusions(claims) -> every pair of screens one element's claims
-- cannot tell apart.
--
-- THIS IS THE TEXT ANALOGUE OF "NO TWO APPEARANCES OF ONE ELEMENT MAY HIT ONE
-- SCREEN", and it is aimed at the same failure from the other side. That rule
-- catches one screen answering to two marks; this one catches one mark
-- answering to two screens: if the file says this region reads "battle" on two
-- different screens and means `match` both times, then whatever else those two
-- screens have, this element is not what separates them -- and a page signature
-- resting on it resolves on whichever of the two is in front of the run.
--
-- IT IS A FINDING AND NOT A REFUSAL, which is why it lives here rather than in
-- `Claims.new`. Two pages really can print one word, and the model may still be
-- correct because some other reference separates them. What is never correct is
-- believing this one does, so the matrix reports it and leaves the judgement
-- where a reader can see the evidence.
--
-- IT DOES NOT ASK WHETHER THE TWO SCREENS ARE TWO PAGES, and that judgement is
-- `regress`'s, through `recognition.same_page`. What is returned here is a fact
-- -- these claims cannot separate these two screens -- and it stays a fact when
-- the two are one page captured twice, at two scroll positions, with the page's
-- anchors outside the part that scrolled. What changes in that case is that
-- separating them was never this element's job, so nothing is reported. Filtering
-- here instead would make this verb answer a question about pages, which are not
-- in a `Claims` at all.
--
-- IT READS THE CLAIMS AND NOT THE SCREENS. Nothing here costs a capture: the
-- contradiction is already written down, and a run that had to measure it would
-- only be confirming what the file said twice.
function reading.confusions(claims: any): { Confusion }
    local found: { Confusion } = {}
    -- element and text, NUL separated because neither can contain one, so two
    -- different pairs cannot collide by concatenation.
    local firstClaim: { [string]: string } = {}

    for index = 1, #claims.expectations do
        local expectation = claims.expectations[index]
        if
            expectation.text ~= nil
            and expectation.state == oracle.state_match
        then
            local key     = expectation.element .. "\0" .. expectation.text
            local earlier = firstClaim[key]
            if earlier == nil then
                firstClaim[key] = expectation.screen
            else
                table.insert(found, {
                    element = expectation.element,
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
