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
--
-- `rect` is the region the two claims share, and it is nil for an element that
-- draws its own -- which is the only kind that existed when this rule was
-- written. It is here because the rule is about ONE REGION reading one text on
-- two screens, and a shape with no rectangle of its own has as many regions as
-- the file gives it coordinates.
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
-- THE RECTANGLE IS A PARAMETER AND NOT READ OFF THE ELEMENT, because there are
-- now two kinds of element and only one of them holds one. An element that draws
-- its own rectangle is read there -- never at a page's refinement, for the reason
-- `regress`'s template search is not: the matrix asks whether this region can be
-- caught saying something it should not, so the widest region the model ever
-- reads is the honest one. An element that draws none is read where the CLAIM
-- says, which is the only thing that can say: one confirm button reused on nine
-- screens sits somewhere different on each. `regress` is what picks between the
-- two, once, so this verb never has to.
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
-- IT IS KEYED BY THE REGION AND NOT BY THE ELEMENT, and on a shape drawn once
-- and placed many times those are different things. Nine confirm buttons are one
-- element and nine rectangles: each one reads "確認", and saying so nine times is
-- nine facts about nine regions rather than one region that cannot tell nine
-- screens apart. Keyed by the element alone the rule would fire on every one of
-- them and be switched off, taking with it the case it exists for -- ONE
-- rectangle claimed to read one text on two screens, where a page signature
-- resting on it really does resolve on whichever screen is in front.
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
