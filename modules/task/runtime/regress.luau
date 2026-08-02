--!strict
-- regress: the falsification matrix, on the script-owned model.
--
-- WHAT IT IS FOR. Every template matches the picture it was cut from at a
-- distance of zero, so a mark measured only on its own screen has proved
-- nothing. This walks every declared mark against every declared screen --
-- including the cells an author would never think to measure -- and reports
-- where the model contradicts itself. It is the one guard in this repository
-- that has ever caught anything real
-- (docs/plans/2026-07-31-script-owned-page-model.md 7), and the multi-appearance
-- model is only sound while it exists.
--
-- THE SPLIT THE MOVE TO LUAU WAS FOR. Scores stay in layer one: a sum of
-- absolute differences over a rectangle of pixels is an image operation and the
-- host does it. JUDGING is here -- what counts as a hit, which outcome the model
-- asked for, how far ahead a winner has to be -- because every one of those is
-- an opinion, and the whole reason the page model moved up a layer is that C++
-- kept holding opinions the next game did not share (the same document, 7).
--
-- WHAT IT DOES NOT DO. It never writes its own results back into the project
-- file. The expectations in that file are what the matrix is judged AGAINST; a
-- run that recorded what it measured would be a check that agrees with itself
-- by construction, and the next drift would be recorded as the new truth
-- instead of reported as a defect. Recording the measurement is the trace's
-- job, and every `cycle_match` below already writes one line to it.
--
-- THE FOUR RULES THAT FIRE WITHOUT AN `[[expect]]` BEING CONTRADICTED are the
-- reason this is more than a table of recorded answers. The first three are
-- aimed at the failure docs/pitfalls/colour-key-annotation.md documents -- a
-- mark that hits every state it was meant to tell apart, and therefore looks
-- like the most reliable mark in the project:
--
--   * NO TWO APPEARANCES OF ONE ELEMENT MAY HIT ONE SCREEN. Three speed icons
--     differing by one digit, keyed a little too generously, match each other;
--     an author who claims only the diagonal cell of each one sees a green
--     matrix and owns a broken element
--     (docs/plans/2026-07-31-annotation-model-capabilities.md 4-2.4, and 2.3
--     correction (b): the habit of testing each appearance only on its own
--     screen skips exactly the half that matters).
--   * THE WINNER MUST BEAT EVERY RIVAL BY A FACTOR. Both answers can be correct
--     while the margin between them is a frame of drift wide.
--   * ONE REGION MAY NOT BE CLAIMED TO READ ONE TEXT ON TWO SCREENS. The same
--     failure from the other side, on the cells a template cannot answer; it is
--     `reading.confusions` and the reasoning is written there. It fires only
--     between two screens that are two PAGES, because a page captured twice
--     reads its own name in its own box both times and that is the property
--     rather than a defect (`recognition.same_page`).
--
-- The fourth is aimed at a different gap, and it is the one a screen's own
-- declaration buys:
--
--   * A SCREEN MUST BE THE PAGE IT SAYS IT IS. Every rule above measures ONE
--     element, while what a run actually asks is whether a whole page signature
--     holds -- a conjunction over several rows, which nothing walked end to end
--     until a screen could name the page it is of. It is `recognition.verify`,
--     it costs the screen's own observation and no capture of its own, and the
--     reasoning for both its direction and the direction it declines to ask is
--     written there.
--
-- TWO KINDS OF CELL, BECAUSE THE MODEL HAS TWO KINDS OF EVIDENCE. An element
-- with appearances is measured by what its templates score, which is every cell
-- this matrix has ever held. An element with none is measured by what its
-- rectangle READS, against the text the project file claims it reads on that
-- screen and the confidence floor the element declares. Both halves are judged
-- the same way -- hit or miss against what was written down -- and the text half
-- is measured by `reading`, next door.
--
-- AND A SECOND AXIS, BECAUSE NOT EVERY ELEMENT HOLDS A RECTANGLE. One that does
-- is measured there on every screen, claimed or not, which is what every
-- unclaimed cell of this matrix rests on. One that does not -- a minimap shape
-- searched at coordinates the script works out per frame, a confirm button drawn
-- once and placed on nine screens -- is measured at the rectangles its CLAIMS
-- name, which is the only place that position can be written down and
-- contradicted at all. So one screen can hold several rows for one element, each
-- carrying the region it looked in, and "it matches here and stays away from
-- there" becomes two cells rather than an argument. Which rectangles a screen
-- offers is `searchRects`, and it is the one place the choice is made.
--
-- LOAD ORDER. `regress` sorts last of the framework modules, which is what lets
-- it name `model`, `observe`, `oracle`, `reading` and `recognition`. It is the
-- only module that walks a whole model against a whole set of screens, and it
-- holds no state of its own: every verb takes the ctx it observes through and
-- the model it walks.

-- How far ahead the appearance that owns a screen must be of every other
-- appearance of the same element there.
--
-- Two appearances are NOT comparable by raw score: each carries its own
-- template size and its own threshold, so its ceiling is its own. The factor is
-- therefore applied to an exact integer cross product and never to a ratio.
--
-- CALIBRATION, and why it is a constant rather than a setting: the repository's
-- own measured cross-screen misses sat between 2.85x and 4.15x their thresholds
-- (docs/pitfalls/page-modeling-and-multi-step.md), so four is inside what a
-- healthy mark has already been observed to deliver and outside what drift can
-- close in one frame. Tunable per project it would slide to one, and at one this
-- check degenerates into "the right appearance won", which the fold rule below
-- already asserts.
local k_separationFactor = 4

-- What one row's subject is: the folded element, which is what a page signature
-- and a click actually read, or one appearance searched alone, which no runtime
-- path ever does.
--
-- An element declaring ONE appearance gets no appearance row: the folded row IS
-- that appearance's row, and searching it twice would double the cost of every
-- project authored so far to restate a measurement already present.
local k_subjectElement    = "element"
local k_subjectAppearance = "appearance"

local k_outcomeHit  = "hit"
local k_outcomeMiss = "miss"

-- Nothing was measured here at all. Two rows carry it, and both are cells with no
-- question in them rather than questions this declined to answer: an element with
-- no templates on a screen whose file claims no text for it -- there are no
-- pixels to search for, and a reading needs something to be read AGAINST, which
-- only a claim supplies -- and an element that draws no rectangle of its own on a
-- screen no claim gives it one, where there is not even a region to look in. The
-- row is still emitted, because a reader comparing two runs needs the inventory
-- to be the same shape.
local k_outcomeNoPixels = "no_pixels"

local k_verdictExpected  = "expected"
local k_verdictUnclaimed = "unclaimed"
local k_verdictHole      = "hole"
local k_verdictMisfire   = "misfire"

local regress = {}

regress.separation_factor = k_separationFactor

-- One measured cell of the matrix.
--
-- THE MEASUREMENT FIELDS COME IN TWO SETS AND ONE ROW CARRIES ONE OF THEM,
-- because there are two kinds of evidence. A template row names the appearance
-- that answered and reports `score` against `maximum` plus the rectangle it
-- landed on. A text row names no appearance and reports the reading -- `text`
-- and `confidence` -- beside the `expected_text` it was measured against.
--
-- `expected_text` is on the row rather than left in the project file for the
-- reason `appearance` is: it is what makes the row legible ALONE. A template row
-- says which of several marks answered; a text row has no such name, so without
-- the claimed text a reader looking at a miss cannot tell what was expected
-- without opening the file and finding the cell again.
--
-- `search_*` IS WHERE THIS ROW LOOKED, and it is the same argument one step
-- further. An element that draws no rectangle of its own is measured wherever its
-- claims put it, so one screen can carry several rows naming ONE element -- "it
-- matches here" beside "it stays away from there" -- and without the searched
-- rectangle on the row those are two lines a reader cannot tell apart. It is on
-- every row rather than only those, because "where did this measurement look" is
-- worth the same four numbers on a row that inherited them.
--
-- It is a separate quartet from `x`/`y`/`width`/`height` because the two answer
-- opposite questions: this is the region that was SEARCHED, and those are where
-- the template LANDED -- which is nil on a miss, exactly when a reader most needs
-- to know where the search happened.
export type Cell = {
    screen: string,
    element: string,
    appearance: string?,
    subject: string,
    outcome: string,
    expected: string,
    verdict: string,
    score: number?,
    maximum: number?,
    expected_text: string?,
    text: string?,
    confidence: number?,
    x: number?,
    y: number?,
    width: number?,
    height: number?,
    search_x: number?,
    search_y: number?,
    search_width: number?,
    search_height: number?,
}

-- How far the appearance that answered on one screen beat one rival there.
--
-- `clears` is the exact integer comparison and is the only thing judged.
-- `factor` is the same quantity as a number for a reader, and is nil when the
-- winner scored zero, because an exact match is ahead of every rival by an
-- amount no ratio can express.
export type Separation = {
    screen: string,
    element: string,
    winner: string,
    rival: string,
    winner_score: number,
    winner_maximum: number,
    rival_score: number,
    rival_maximum: number,
    factor: number?,
    clears: boolean,
}

-- One thing the measurement says is wrong with the model. A verdict with no
-- findings is the verdict "accepted".
--
-- `rival` is the OTHER side of whatever collided, and what that other side is
-- follows the kind: another appearance of the same element for every rule about
-- a set of marks, and another SCREEN for `ambiguous_text`, where the two things
-- one region fails to tell apart are two screens rather than two templates. It
-- is one field rather than two because a finding names one collision, and a
-- reader who has the kind already knows what it collided with.
--
-- `element` IS ABSENT ON EXACTLY ONE KIND, and `page` is set on that same one.
-- Every rule but `unresolved_page` is about a CELL, and a cell is a screen and an
-- element; that one is about a whole page signature on a screen. Naming the page
-- in a field of its own is what keeps `element` meaning the thing every other row
-- means by it -- a borrowed field would make a reader filtering the report by
-- element pick up a page.
export type Finding = {
    kind: string,
    screen: string,
    element: string?,
    page: string?,
    appearance: string?,
    rival: string?,
    detail: string,
}

export type Verdict = {
    accepted: boolean,
    screens: number,
    elements: number,
    cells: { Cell },
    separations: { Separation },
    findings: { Finding },
    claimed: number,
    unclaimed: number,
    matches: number,
}

-- ------------------------------------------------------------- measurement

-- Searches one element's rectangle for each of its appearances and reports what
-- each one scored, whether it cleared its own threshold, and how many times the
-- host boundary was crossed doing it.
--
-- IT NEVER SEARCHES A PAGE'S REFINEMENT. A reference may narrow the region for a
-- click on one page, and narrowing is what a click wants; the matrix wants the
-- opposite. The question here is whether these pixels can be found somewhere
-- they do not belong, so the widest region the model ever searches is the honest
-- one, and it is also the region an identify sweep uses -- which runs before any
-- page is known and so has no refinement available to it.
--
-- The rectangle is a parameter because a shape with no rectangle of its own is
-- searched where its CLAIM says, and `searchRects` is the one place that choice
-- is made.
local function measureElement(
    ctx: any,
    ticket: any,
    element: any,
    rect: any
): { any }
    local measured: { any } = {}
    for index = 1, #element.appearances do
        local appearance = element.appearances[index]
        -- Not wrapped in pcall: `cycle_match` raises when a budget, a deadline
        -- or a cancel stopped the search, and a search that stopped looking has
        -- established nothing. Reading one as "absent" would make the matrix's
        -- verdict a function of a configuration number.
        local found = ctx:cycle_match(
            ticket,
            appearance.template,
            rect.x,
            rect.y,
            rect.width,
            rect.height
        )
        measured[index] = {
            appearance = appearance,
            match      = found,
            hit        = found ~= nil
                and observe.matched(found, appearance.threshold),
        }
    end
    return measured
end

-- The measurement the fold settles on: the best normalised margin among those
-- that cleared their threshold, with declaration order breaking an exact tie.
-- This is `observe.find`'s own rule, reached through `observe.better` so the two
-- cannot come to disagree about which appearance a click would land on.
local function foldMeasured(measured: { any }): any
    local best: any = nil
    for index = 1, #measured do
        local entry = measured[index]
        if entry.hit then
            if best == nil or observe.better(entry.match, best.match) then
                best = entry
            end
        end
    end
    return best
end

-- --------------------------------------------------------------- judging

local function verdictFor(expected: string, hit: boolean): string
    if expected == oracle.state_match then
        return if hit then k_verdictExpected else k_verdictHole
    end
    if expected == oracle.state_absent then
        return if hit then k_verdictMisfire else k_verdictExpected
    end
    return k_verdictUnclaimed
end

local function pushCell(
    cells: { Cell },
    screen: any,
    element: any,
    appearance: string?,
    subject: string,
    entry: any,
    expected: string,
    rect: any
): Cell
    local match: any = if entry ~= nil then entry.match else nil
    local hit        = entry ~= nil and entry.hit
    local cell: Cell = {
        screen        = screen.name,
        element       = element.name,
        appearance    = appearance,
        subject       = subject,
        outcome       = if entry == nil
            then k_outcomeNoPixels
            elseif hit then k_outcomeHit
            else k_outcomeMiss,
        expected      = expected,
        verdict       = verdictFor(expected, hit),
        score         = if match ~= nil then match.score else nil,
        maximum       = if match ~= nil then match.maximum else nil,
        expected_text = nil,
        text          = nil,
        confidence    = nil,
        x             = if match ~= nil then match.x else nil,
        y             = if match ~= nil then match.y else nil,
        width         = if match ~= nil then match.width else nil,
        height        = if match ~= nil then match.height else nil,
        search_x      = if rect ~= nil then rect.x else nil,
        search_y      = if rect ~= nil then rect.y else nil,
        search_width  = if rect ~= nil then rect.width else nil,
        search_height = if rect ~= nil then rect.height else nil,
    }
    table.insert(cells, cell)
    return cell
end

-- The same row for the other kind of evidence: what the region was claimed to
-- read, what it read, and how sure the engine was.
--
-- It is a second function rather than a branch inside `pushCell`, because the
-- two share only the four fields that say WHICH cell this is. Folding them
-- together would mean one signature carrying a match handle and a reading, with
-- every call site passing nil for half of it.
--
-- The verdict is `verdictFor`'s, unshared and unconditional, so "the file said
-- match and it did not" means the same thing on both halves of the matrix.
local function pushTextCell(
    cells: { Cell },
    screen: any,
    element: any,
    expectedText: string,
    measured: any,
    expected: string,
    rect: any
): Cell
    local cell: Cell = {
        screen        = screen.name,
        element       = element.name,
        appearance    = nil,
        subject       = k_subjectElement,
        outcome       = if measured.matched then k_outcomeHit else k_outcomeMiss,
        expected      = expected,
        verdict       = verdictFor(expected, measured.matched),
        score         = nil,
        maximum       = nil,
        expected_text = expectedText,
        text          = measured.text,
        confidence    = measured.confidence,
        x             = nil,
        y             = nil,
        width         = nil,
        height        = nil,
        search_x      = rect.x,
        search_y      = rect.y,
        search_width  = rect.width,
        search_height = rect.height,
    }
    table.insert(cells, cell)
    return cell
end

-- The phrase a finding about a reading uses to say what came back instead.
--
-- The confidence is in it even when the text was right, because that is the case
-- a reader most needs the number for: a cell that read the claimed words and
-- still missed missed on the floor, and a detail line saying only "it read
-- battle" would look like a defect in the matrix rather than in the model.
local function readingPhrase(measured: any): string
    if measured.text == nil then
        return "nothing at all"
    end
    return '"'
        .. measured.text
        .. '" at '
        .. tostring(measured.confidence)
        .. " basis points of confidence"
end

-- The phrase every finding about a shape with no rectangle of its own ends with,
-- and the empty string for every other element.
--
-- It is conditional rather than always present because the two cases differ in
-- what a reader needs: an element that draws its own rectangle has exactly one
-- row per screen, so naming the rectangle in the sentence would repeat what the
-- model already says. An element that draws none can have several rows on one
-- screen, and then the rectangle is the ONLY thing separating one finding from
-- the next.
local function whereIn(element: any, rect: any): string
    if element.rect ~= nil then
        return ""
    end
    return " at [" .. oracle.rect_key(rect) .. "]"
end

-- The rectangle a claim about this measurement would have written down, which is
-- what a cell is filed under.
--
-- An element that draws its own rectangle is claimed with none of its own --
-- `Expectation.new` refuses the second answer -- so a lookup for one has to ask
-- for the cell filed under no rectangle at all, whatever region the walk happens
-- to be standing in. One function rather than the same conditional at each of the
-- four lookups, because a walk that asked for the wrong key would find no cell
-- and report a claimed row as unclaimed, which reads as a quiet model rather than
-- as a bug.
local function claimedAt(element: any, rect: any): any
    if element.rect ~= nil then
        return nil
    end
    return rect
end

-- Findings carry NAMES rather than the objects they were read off, because one
-- of them is about a pair of screens and is raised after the walk, where no
-- screen is being stood on.
local function pushFinding(
    findings: { Finding },
    kind: string,
    screen: string,
    element: string,
    appearance: string?,
    rival: string?,
    detail: string
)
    table.insert(findings, {
        kind       = kind,
        screen     = screen,
        element    = element,
        appearance = appearance,
        rival      = rival,
        detail     = detail,
    })
end

-- The one finding that is not about a cell: the screen said which page it is,
-- and that page did not resolve on its own pixels.
--
-- It is a second function rather than four nils in the first, for the reason
-- `pushTextCell` is a second function beside `pushCell`: the two share `kind` and
-- `detail` and nothing else. One names an element, an appearance and the rival it
-- collided with, and this one names a page and has no collision in it at all.
local function pushPageFinding(
    findings: { Finding },
    screen: string,
    page: string,
    detail: string
)
    table.insert(findings, {
        kind   = "unresolved_page",
        screen = screen,
        page   = page,
        detail = detail,
    })
end

-- Every appearance the claims name as matching this screen. More than one is
-- itself a contradiction in the file: two appearances of one element cannot
-- both be on one screen, because they are two ways ONE rectangle can look.
local function ownersOf(
    claims: any,
    screen: any,
    element: any,
    rect: any
): { string }
    local owners: { string } = {}
    for index = 1, #element.appearances do
        local appearance = element.appearances[index]
        if appearance.name ~= nil then
            local state = oracle.Claims.state_for(
                claims,
                screen.name,
                element.name,
                appearance.name,
                claimedAt(element, rect)
            )
            if state == oracle.state_match then
                table.insert(owners, appearance.name)
            end
        end
    end
    return owners
end

-- The two rules that hold whatever the file claims, plus the fold's own duty to
-- answer with the appearance the file says owns the screen.
local function judgeAppearanceSet(
    screen: any,
    element: any,
    rect: any,
    measured: { any },
    folded: any,
    owners: { string },
    separations: { Separation },
    findings: { Finding }
)
    if #measured < 2 then
        return
    end

    local screenName  = screen.name
    local elementName = element.name
    local where       = whereIn(element, rect)

    -- Rule one: at most one appearance may hit. It fires on an unclaimed screen
    -- too, which is the point -- that is where an author's own testing habit
    -- leaves the hole.
    local hits: { string } = {}
    for index = 1, #measured do
        local entry = measured[index]
        if entry.hit then
            table.insert(hits, entry.appearance.name or "unnamed")
        end
    end
    if #hits > 1 then
        pushFinding(
            findings,
            "ambiguous_appearances",
            screenName,
            elementName,
            hits[1],
            hits[2],
            "appearances "
                .. table.concat(hits, ", ")
                .. " all match this screen"
                .. where
                .. "; they are two ways ONE rectangle can look, so a screen "
                .. "matching several of them tells nothing apart and the fold's "
                .. "answer is whichever scored better today"
        )
    end

    if #owners > 1 then
        pushFinding(
            findings,
            "contradictory_claims",
            screenName,
            elementName,
            owners[1],
            owners[2],
            "the project file claims appearances "
                .. table.concat(owners, ", ")
                .. " both match this screen"
                .. where
                .. ", and one rectangle wears one appearance at a time"
        )
    end

    if folded == nil then
        return
    end
    local winnerName = folded.appearance.name or "unnamed"

    -- Rule two: the fold must answer with the appearance the file says owns the
    -- screen. This is the one "first past the threshold wins" would violate: an
    -- over-wide early appearance answers first and returns ITS rectangle, and
    -- the click lands on pixels belonging to another appearance with nothing
    -- downstream noticing.
    if #owners == 1 and owners[1] ~= winnerName then
        pushFinding(
            findings,
            "wrong_appearance",
            screenName,
            elementName,
            winnerName,
            owners[1],
            "the fold answered with '"
                .. winnerName
                .. "' on a screen the project file says '"
                .. owners[1]
                .. "' owns"
                .. where
                .. ", so the click lands on that appearance's rectangle instead"
        )
    end

    -- Rule three: separation. Measured against every other appearance, whether
    -- or not it cleared its own threshold, because the question is how close the
    -- runner-up came rather than whether it would have been accepted.
    for index = 1, #measured do
        local entry = measured[index]
        if entry ~= folded and entry.match ~= nil then
            local rivalName = entry.appearance.name or "unnamed"
            local winner    = folded.match
            local rival     = entry.match
            -- The exact integer form of "the winner's normalised distance,
            -- multiplied by the factor, is still no worse than the rival's".
            -- Every operand is a whole number well inside what a double holds
            -- exactly: a score is at most 255 times the template's pixel count.
            local clears = winner.score * rival.maximum * k_separationFactor
                <= rival.score * winner.maximum
            local factor: number? = nil
            if winner.score > 0 then
                factor = (rival.score * winner.maximum)
                    / (winner.score * rival.maximum)
            end
            table.insert(separations, {
                screen         = screen.name,
                element        = element.name,
                winner         = winnerName,
                rival          = rivalName,
                winner_score   = winner.score,
                winner_maximum = winner.maximum,
                rival_score    = rival.score,
                rival_maximum  = rival.maximum,
                factor         = factor,
                clears         = clears,
            })
            if not clears then
                pushFinding(
                    findings,
                    "thin_separation",
                    screenName,
                    elementName,
                    winnerName,
                    rivalName,
                    "'"
                        .. winnerName
                        .. "' beat '"
                        .. rivalName
                        .. "' by less than "
                        .. tostring(k_separationFactor)
                        .. "x; both answers are still correct here, which is "
                        .. "what makes it worth reporting rather than waiting "
                        .. "for"
                )
            end
        end
    end
end

-- ------------------------------------------------------------------- walk

-- Measures one element that has no templates: it reads the region and judges
-- what came back against the text this screen's claim says belongs there.
--
-- THE READ HAPPENS ONLY WHERE A CLAIM ASKED FOR IT, and that asymmetry with the
-- template half is the point rather than an economy. A template search is
-- defined by the element alone, so the matrix can run it on every screen and
-- catch a mark answering where nobody thought to look. A reading is not: there
-- is no such thing as reading a region "in general", only reading it and
-- comparing against something, and the only thing that says what to compare
-- against on THIS screen is the claim. An unclaimed text cell is therefore not a
-- cell the matrix declined to measure, it is a cell with no question in it -- and
-- the falsification pressure the template half gets from unclaimed cells, the
-- text half gets from `reading.confusions` instead.
local function walkTextElement(
    ctx: any,
    ticket: any,
    claims: any,
    screen: any,
    element: any,
    rect: any,
    verdict: Verdict
)
    local where        = whereIn(element, rect)
    local claimRect    = claimedAt(element, rect)
    local expectedText =
        oracle.Claims.text_for(claims, screen.name, element.name, claimRect)
    if expectedText == nil then
        -- Nothing to search for and nothing claimed to read. The row is emitted
        -- anyway, because a reader comparing two runs needs the inventory to be
        -- the same shape.
        pushCell(
            verdict.cells,
            screen,
            element,
            nil,
            k_subjectElement,
            nil,
            oracle.state_unclaimed,
            rect
        )
        return
    end

    local expected = oracle.Claims.state_for(
        claims,
        screen.name,
        element.name,
        nil,
        claimRect
    )
    local measured = reading.measure(ctx, ticket, element, rect, expectedText)
    local cell     = pushTextCell(
        verdict.cells,
        screen,
        element,
        expectedText,
        measured,
        expected,
        rect
    )

    if cell.verdict == k_verdictHole then
        pushFinding(
            verdict.findings,
            k_verdictHole,
            screen.name,
            element.name,
            nil,
            nil,
            'the project file says this region reads "'
                .. expectedText
                .. '" here'
                .. where
                .. " and it read "
                .. readingPhrase(measured)
        )
    elseif cell.verdict == k_verdictMisfire then
        -- The misfire that matters most on this half. A region reading the same
        -- words on a screen the file says it does not is one page's signature
        -- answering for another, and it is invisible to every template rule
        -- because there is no template in it.
        pushFinding(
            verdict.findings,
            k_verdictMisfire,
            screen.name,
            element.name,
            nil,
            nil,
            'the project file says this region does not read "'
                .. expectedText
                .. '" here'
                .. where
                .. " and it read exactly that, at "
                .. tostring(measured.confidence)
                .. " basis points of confidence"
        )
    end
end

-- Measures one element that has templates, in one region: the folded answer, a
-- row per appearance when it declares more than one, and the three rules that
-- hold whatever the file claims.
--
-- It is a function of a RECTANGLE rather than of an element, which is what lets
-- one screen carry several rows for a shape drawn once and placed many times: the
-- caller walks the rectangles and this measures each. For every element that
-- draws its own the caller walks exactly one, so this is the whole of the old
-- body with a parameter in front of it.
local function walkPixelElement(
    ctx: any,
    ticket: any,
    claims: any,
    screen: any,
    element: any,
    rect: any,
    verdict: Verdict
)
    local where     = whereIn(element, rect)
    local claimRect = claimedAt(element, rect)
    local measured  = measureElement(ctx, ticket, element, rect)
    local folded    = foldMeasured(measured)

    local elementExpected = oracle.Claims.state_for(
        claims,
        screen.name,
        element.name,
        nil,
        claimRect
    )
    local elementCell = pushCell(
        verdict.cells,
        screen,
        element,
        if folded ~= nil then folded.appearance.name else nil,
        k_subjectElement,
        folded or { match = nil, hit = false },
        elementExpected,
        rect
    )
    if elementCell.verdict == k_verdictHole then
        pushFinding(
            verdict.findings,
            k_verdictHole,
            screen.name,
            element.name,
            nil,
            nil,
            "the project file says this element matches here"
                .. where
                .. " and nothing did"
        )
    elseif elementCell.verdict == k_verdictMisfire then
        pushFinding(
            verdict.findings,
            k_verdictMisfire,
            screen.name,
            element.name,
            elementCell.appearance,
            nil,
            "the project file says this element is absent here"
                .. where
                .. " and it matched"
        )
    end

    if #measured > 1 then
        for index = 1, #measured do
            local entry = measured[index]
            local name  = entry.appearance.name
            local state = oracle.Claims.state_for(
                claims,
                screen.name,
                element.name,
                name,
                claimRect
            )
            local cell = pushCell(
                verdict.cells,
                screen,
                element,
                name,
                k_subjectAppearance,
                entry,
                state,
                rect
            )
            if cell.verdict == k_verdictHole then
                pushFinding(
                    verdict.findings,
                    k_verdictHole,
                    screen.name,
                    element.name,
                    name,
                    nil,
                    "the project file says this appearance matches here"
                        .. where
                        .. " and it did not"
                )
            elseif cell.verdict == k_verdictMisfire then
                pushFinding(
                    verdict.findings,
                    k_verdictMisfire,
                    screen.name,
                    element.name,
                    name,
                    nil,
                    "the project file says this appearance is absent here"
                        .. where
                        .. " and it matched"
                )
            end
        end
    end

    judgeAppearanceSet(
        screen,
        element,
        rect,
        measured,
        folded,
        ownersOf(claims, screen, element, rect),
        verdict.separations,
        verdict.findings
    )
end

-- The regions one element is measured in on one screen.
--
-- AN ELEMENT THAT DRAWS ITS OWN RECTANGLE IS MEASURED THERE, ALWAYS, and that is
-- the property every unclaimed cell of this matrix rests on: the walk searches it
-- on every screen whether or not anybody thought to claim it, which is where a
-- mark answering somewhere it does not belong is caught.
--
-- AN ELEMENT THAT DRAWS NONE IS MEASURED WHERE ITS CLAIMS SAY, AND NOWHERE ELSE.
-- There is no such thing as searching for it "in general" -- a minimap cell's
-- position is a fact about the frame, so a rectangle from one screen means
-- nothing on the next -- and the answer to that is not for this walk to invent.
-- It is the same asymmetry the text half already stands on: the pressure comes
-- from the author writing the rectangle it must match AND the ones it must miss,
-- which is exactly the measurement this matrix is for.
local function searchRects(claims: any, screen: any, element: any): { any }
    if element.rect ~= nil then
        return { element.rect }
    end
    return oracle.Claims.rects_for(claims, screen.name, element.name)
end

-- Measures one screen: what it says it IS, then every element, then every
-- appearance of the elements that declare more than one.
--
-- THE SCREEN'S OWN CLAIM GOES FIRST because it is about the whole frame the
-- cells are then read off, and because it is the only row here a reader can act
-- on without reading any other: a page that does not resolve on the screen that
-- says it is that page explains most of the cells underneath it.
--
-- Its reason is `observe.resolve_page`'s own sentence, spent on THIS ticket, so
-- the finding names the same clause a run would have failed on.
local function walkScreen(
    ctx: any,
    ticket: any,
    built: any,
    screen: any,
    verdict: Verdict
)
    local unresolved = recognition.verify(ctx, ticket, built, screen)
    if unresolved ~= nil then
        pushPageFinding(
            verdict.findings,
            screen.name,
            screen.page,
            "the project file says screen '"
                .. screen.name
                .. "' is page '"
                .. screen.page
                .. "', and that page does not resolve on its own pixels: "
                .. unresolved
        )
    end

    local claims = built.claims
    for _, element in built.elements do
        local rects = searchRects(claims, screen, element)
        if #rects == 0 then
            -- A shape drawn once and placed by whoever uses it, on a screen no
            -- claim places it. There is no region to search and no reading to
            -- take, so the row says nothing was measured rather than reporting a
            -- miss nobody looked for.
            pushCell(
                verdict.cells,
                screen,
                element,
                nil,
                k_subjectElement,
                nil,
                oracle.state_unclaimed,
                nil
            )
        else
            for _, rect in rects do
                if #element.appearances == 0 then
                    walkTextElement(
                        ctx,
                        ticket,
                        claims,
                        screen,
                        element,
                        rect,
                        verdict
                    )
                else
                    walkPixelElement(
                        ctx,
                        ticket,
                        claims,
                        screen,
                        element,
                        rect,
                        verdict
                    )
                end
            end
        end
    end
end

-- regress.check(ctx, built) -> Verdict
--
-- Walks every declared screen against every declared element and reports what
-- the model got wrong.
--
-- ONE OBSERVATION PER SCREEN, IN CONTENT-HASH ORDER. The host serves this run's
-- frames from files rather than from a target, one file per capture in file-name
-- order, and a screen's file name is its content hash -- so the order
-- `oracle.Claims.screens_in_frame_order` returns is what pairs the Nth capture
-- with the Nth screen. Every declared screen's file is READ before its
-- observation is opened, which is what turns "this project declares a screen
-- whose PNG is not there" into a failure naming that screen instead of a
-- capture that ran out of frames three screens later. It also puts each screen's
-- content hash into the trace, so a verdict is attributable to exact bytes.
--
-- A TEXT CELL SPENDS THAT SAME OBSERVATION'S READ BUDGET, one read per claimed
-- cell, and the host refuses the next read once the budget is out. That refusal
-- is a raise and travels out of here as one: a matrix that reported the cells it
-- could not afford as misses would be a green report with a hole in it whose
-- size is a configuration number.
--
-- SO DOES THE PAGE A SCREEN DECLARES, one read per identify row of that page
-- whose element has no template. It is the same observation and the same budget,
-- which is why one screen can cost twice the element count and no more: each
-- half walks the same list once.
--
-- WHICH IS WHY THE CEILING IS NOT THIS WALK'S TO WORK AROUND. The walk reads
-- each element at most once per screen for its cell and at most once more for
-- the declared page, so what a cycle here needs is a fact about the FILE -- and
-- the caller sizes the budget from that file before the run starts
-- (entry/cli/check.cpp). Splitting a screen across two observations
-- to fit under a smaller ceiling is not available and would be wrong if it
-- were: the frames come from a directory served one file per capture, so a
-- re-opened cycle is the NEXT screen's pixels, and half the cells of screen
-- three would be judged against screen four.
--
-- It also means a check over a project claiming text needs an OCR engine bound;
-- whoever assembles the run asks `oracle.Claims.reads_text` and
-- `recognition.needs_engine` first and refuses in its own words, because a
-- refusal naming the missing flag is worth more than a read failing on screen
-- three. Two questions rather than one, because a page identified by what its
-- title box reads is read on every screen that declares it, whether or not that
-- screen claims a text of its own.
--
-- The observation is closed on every path, including a raise from the walk, and
-- the walk's own failure outranks a failure from the close.
function regress.check(ctx: any, built: any): Verdict
    if type(ctx) ~= "table" then
        error("regress.check needs the ctx it should observe through", 2)
    end
    if type(built) ~= "table" or not oracle.is_claims(built.claims) then
        error(
            "regress.check needs the model project.load_project returned; the "
                .. "screens and the expectations it walks are part of that file",
            2
        )
    end

    local verdict: Verdict = {
        accepted    = true,
        screens     = 0,
        elements    = #built.elements,
        cells       = {},
        separations = {},
        findings    = {},
        claimed     = #built.claims.expectations,
        unclaimed   = 0,
        matches     = 0,
    }

    local screens = oracle.Claims.screens_in_frame_order(built.claims)
    verdict.screens = #screens

    for _, screen in screens do
        -- The existence proof, and the trace line that names these bytes. The
        -- blob itself is not kept: the pixels reach the matcher through the
        -- frame the host serves, never through this read.
        ctx:project_read(screen.path)

        local ticket             = ctx:cycle_open()
        local ok, walkError      = pcall(walkScreen, ctx, ticket, built, screen, verdict)
        local closed, closeError = pcall(ctx.cycle_close, ctx, ticket)
        if not ok then
            error(walkError, 0)
        end
        if not closed then
            error(closeError, 0)
        end
    end

    -- The rule that reads the FILE rather than a screen, so it is raised once
    -- here rather than inside the walk: it is about a pair of screens, and the
    -- walk stands on one at a time. It costs no capture, which is why it runs
    -- last instead of being used to refuse the run -- a reader gets the measured
    -- evidence above it either way.
    for _, confusion in reading.confusions(built.claims) do
        -- A pair of screens that are two views of ONE page is not a defect and
        -- never was: the region reads that page's own name on both because that
        -- is what the page prints, and a signature resting on it resolving on
        -- either of them is the right answer twice. The rule is unchanged for
        -- every pair that is not -- including a pair where one screen or both
        -- say nothing about which page they are, because silence is not a fact
        -- (`recognition.same_page`).
        if
            not recognition.same_page(
                built.claims,
                confusion.screen,
                confusion.rival
            )
        then
            pushFinding(
                verdict.findings,
                "ambiguous_text",
                confusion.screen,
                confusion.element,
                nil,
                confusion.rival,
                "the project file says this region"
                    .. (
                        if confusion.rect ~= nil
                            then " at ["
                                .. oracle.rect_key(confusion.rect)
                                .. "]"
                            else ""
                    )
                    .. ' reads "'
                    .. confusion.text
                    .. "\" on screens '"
                    .. confusion.screen
                    .. "' and '"
                    .. confusion.rival
                    .. "', and does not declare those two to be one page -- so "
                    .. "it cannot tell those two apart and a page signature "
                    .. "resting on it resolves on whichever one is in front of "
                    .. "the run"
            )
        end
    end

    for _, cell in verdict.cells do
        if cell.verdict == k_verdictUnclaimed then
            verdict.unclaimed += 1
        end
        if cell.outcome == k_outcomeHit then
            verdict.matches += 1
        end
    end
    verdict.accepted = #verdict.findings == 0
    return verdict
end

-- ----------------------------------------------------------------- report

local k_jsonEscapes: { [string]: string } = {
    ['"']    = '\\"',
    ["\\"]   = "\\\\",
    ["\n"]   = "\\n",
    ["\r"]   = "\\r",
    ["\t"]   = "\\t",
    ["\b"]   = "\\b",
    ["\f"]   = "\\f",
}

local function jsonString(value: string): string
    local escaped = string.gsub(value, '[%c"\\]', function(character: string): string
        local known = k_jsonEscapes[character]
        if known ~= nil then
            return known
        end
        return string.format("\\u%04x", string.byte(character))
    end)
    return '"' .. escaped .. '"'
end

local function jsonNumber(value: number): string
    if value % 1 == 0 then
        return string.format("%d", value)
    end
    -- Enough digits to reproduce a double, and the same text on every host.
    return string.format("%.6g", value)
end

-- One JSON object from an ordered list of key/value pairs. The order is the
-- caller's, because a report a human reads in a terminal is easier to scan when
-- the same field is in the same place on every line, and a sorted object would
-- put `element` after `cells`.
local function jsonObject(fields: { { any } }): string
    local parts: { string } = {}
    for _, field in fields do
        local key   = field[1] :: string
        local value = field[2]
        if value ~= nil then
            local rendered: string
            if type(value) == "string" then
                rendered = jsonString(value)
            elseif type(value) == "number" then
                rendered = jsonNumber(value)
            elseif type(value) == "boolean" then
                rendered = if value then "true" else "false"
            else
                rendered = jsonString(tostring(value))
            end
            table.insert(parts, jsonString(key) .. ":" .. rendered)
        end
    end
    return "{" .. table.concat(parts, ",") .. "}"
end

-- regress.render(verdict) -> the verdict as JSON lines, newline separated and
-- with no trailing newline.
--
-- The summary comes FIRST so a reader piping this into a pager sees the answer
-- before the evidence, and the findings come before the cells for the same
-- reason: a rejected model is read for its findings, and the cells are what
-- explains them.
function regress.render(verdict: Verdict): string
    local lines: { string } = {}

    table.insert(lines, jsonObject({
        { "check", "summary" },
        { "accepted", verdict.accepted },
        { "screens", verdict.screens },
        { "elements", verdict.elements },
        { "cells", #verdict.cells },
        { "claimed", verdict.claimed },
        { "unclaimed", verdict.unclaimed },
        { "matches", verdict.matches },
        { "findings", #verdict.findings },
        { "separation_factor", k_separationFactor },
    }))

    for _, finding in verdict.findings do
        table.insert(lines, jsonObject({
            { "check", "finding" },
            { "kind", finding.kind },
            { "screen", finding.screen },
            { "element", finding.element },
            { "page", finding.page },
            { "appearance", finding.appearance },
            { "rival", finding.rival },
            { "detail", finding.detail },
        }))
    end

    for _, separation in verdict.separations do
        table.insert(lines, jsonObject({
            { "check", "separation" },
            { "screen", separation.screen },
            { "element", separation.element },
            { "winner", separation.winner },
            { "rival", separation.rival },
            { "winner_score", separation.winner_score },
            { "winner_maximum", separation.winner_maximum },
            { "rival_score", separation.rival_score },
            { "rival_maximum", separation.rival_maximum },
            { "factor", separation.factor },
            { "clears", separation.clears },
        }))
    end

    for _, cell in verdict.cells do
        table.insert(lines, jsonObject({
            { "check", "cell" },
            { "screen", cell.screen },
            { "element", cell.element },
            { "appearance", cell.appearance },
            { "subject", cell.subject },
            { "outcome", cell.outcome },
            { "expected", cell.expected },
            { "verdict", cell.verdict },
            { "score", cell.score },
            { "maximum", cell.maximum },
            { "expected_text", cell.expected_text },
            { "text", cell.text },
            { "confidence", cell.confidence },
            { "x", cell.x },
            { "y", cell.y },
            { "width", cell.width },
            { "height", cell.height },
            -- Last, because the searched rectangle is what a reader looks up
            -- when two rows share a name rather than what they scan a report
            -- for. Every row above it answers "what happened here"; these four
            -- answer "which here".
            { "search_x", cell.search_x },
            { "search_y", cell.search_y },
            { "search_width", cell.search_width },
            { "search_height", cell.search_height },
        }))
    end

    return table.concat(lines, "\n")
end

return regress
