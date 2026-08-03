--!strict
-- regress: the falsification matrix, on the script-owned model.
--
-- Every template matches the picture it was cut from at a distance of zero, so a
-- mark measured only on its own screen has proved nothing. This walks every
-- declared mark against every declared screen -- including the cells an author
-- would never think to measure -- and reports where the model contradicts itself
-- (docs/plans/2026-07-31-script-owned-page-model.md 7). The multi-appearance
-- model is only sound while it exists.
--
-- Scores stay in layer one, because a sum of absolute differences over a
-- rectangle of pixels is an image operation. JUDGING is here -- what counts as a
-- hit, which outcome the model asked for, how far ahead a winner has to be --
-- because each of those is an opinion the next game may not share.
--
-- It never writes its own results back into the project file: the expectations
-- there are what the matrix is judged AGAINST, so a run that recorded what it
-- measured would agree with itself by construction. Recording the measurement is
-- the trace's job, and every `cycle_match` below already writes one line to it.
--
-- FOUR RULES FIRE WITHOUT AN `[[expect]]` BEING CONTRADICTED; the first three
-- are aimed at the failure docs/pitfalls/colour-key-annotation.md documents --
-- a mark that hits every state it was meant to tell apart.
--
-- The two rules that read a CLAIM -- `contradictory_claims` and
-- `wrong_appearance` -- are unscoped, because an author who wrote down which
-- appearance owns a screen is the consumer that sentence needed.
--
-- The fourth is what a screen's own declaration buys:
--
--   * A screen must be the page it says it is. Every rule above measures ONE
--     element, while a page signature is a conjunction over several rows that
--     nothing walked end to end until a screen could name its page. It is
--     `recognition.verify` and costs the screen's own observation.
--
-- TWO KINDS OF CELL. An element with appearances is measured by what its
-- templates score; an element with none by what its rectangle READS, against the
-- text the project file claims and the confidence floor the element declares.
-- The text half is measured by `reading`, next door.
--
-- Which rectangles a screen offers is `searchRects`, and it is the one place
-- that choice is made.
--
-- `regress` sorts last of the framework modules, which is what lets it name
-- `model`, `observe`, `oracle`, `reading` and `recognition`. It holds no state:
-- every verb takes the ctx it observes through and the model it walks.

-- How far ahead the appearance that owns a screen must be of every other
-- appearance of the same element there.
--
-- Two appearances are NOT comparable by raw score -- each carries its own
-- template size and threshold, so its ceiling is its own -- which is why the
-- factor is applied to an exact integer cross product and never to a ratio.
--
-- Measured cross-screen misses in this repository sat between 2.85x and 4.15x
-- their thresholds (docs/pitfalls/page-modeling-and-multi-step.md), so four is
-- inside what a healthy mark delivers and outside what drift closes in one
-- frame. A constant rather than a setting: at one this check degenerates into
-- "the right appearance won", which the fold rule below already asserts.
local k_separationFactor = 4

-- What one row's subject is: the folded element, which is what a page signature
-- and a click actually read, or one appearance searched alone, which no runtime
-- path ever does. An element declaring ONE appearance gets no appearance row --
-- the folded row IS that appearance's row.
local k_subjectElement    = "element"
local k_subjectAppearance = "appearance"

local k_outcomeHit  = "hit"
local k_outcomeMiss = "miss"

-- Nothing was measured here at all: an element with no templates on a screen
-- whose file claims no text for it -- a reading needs something to be read
-- AGAINST, which only a claim supplies -- or an element that draws no rectangle
-- of its own on a screen no claim gives it one. Both are cells with no question
-- in them rather than questions this declined to answer, and the row is still
-- emitted so that two runs have the same inventory shape.
local k_outcomeNoPixels = "no_pixels"

local k_verdictExpected  = "expected"
local k_verdictUnclaimed = "unclaimed"
local k_verdictHole      = "hole"
local k_verdictMisfire   = "misfire"

local regress = {}

regress.separation_factor = k_separationFactor

-- One measured cell of the matrix. The measurement fields come in two sets and
-- one row carries one of them: a template row names the appearance that answered
-- and reports `score` against `maximum` plus the rectangle it landed on, while a
-- text row names no appearance and reports `text` and `confidence` beside the
-- `expected_text` it was measured against.
--
-- `expected_text` and `search_*` are on the row rather than left in the project
-- file so that the row is legible ALONE. One screen can carry several rows naming
-- one element -- "it matches here" beside "it stays away from there" -- and
-- without the searched rectangle those are two lines a reader cannot tell apart.
--
-- `search_*` is where this row LOOKED and `x`/`y`/`width`/`height` is where the
-- template LANDED, which is nil on a miss -- exactly when a reader most needs to
-- know where the search happened.
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
-- `clears` is the exact integer comparison and the only thing judged; `factor`
-- is the same quantity as a number for a reader, nil when the winner scored zero
-- because an exact match is ahead by an amount no ratio can express.
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
-- `rival` is the OTHER side of whatever collided, and which side that is follows
-- the kind: another appearance of the same element for every rule about a set of
-- marks, and another SCREEN for `ambiguous_text`.
--
-- `element` is absent on exactly one kind and `page` is set on that same one.
-- Every rule but `unresolved_page` is about a cell, which is a screen and an
-- element; that one is about a whole page signature on a screen, and it names the
-- page in a field of its own so a reader filtering by element does not pick up a
-- page.
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
-- each one scored and whether it cleared its own threshold.
--
-- It never searches a page's REFINEMENT. Narrowing is what a click wants; the
-- matrix asks whether these pixels can be found somewhere they do not belong, so
-- the widest region the model ever searches is the honest one -- and it is the
-- region an identify sweep uses, which runs before any page is known.
--
-- The rectangle is a parameter because a shape with no rectangle of its own is
-- searched where its CLAIM says; `searchRects` makes that choice.
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
        -- or a cancel stopped the search, and reading a search that never
        -- finished as "absent" would make the verdict a function of a
        -- configuration number.
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
-- read, what it read, and how sure the engine was. The verdict is `verdictFor`'s,
-- unshared and unconditional, so "the file said match and it did not" means the
-- same thing on both halves of the matrix.
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

-- The phrase a finding about a reading uses to say what came back instead. The
-- confidence is in it even when the text was right, because a cell that read the
-- claimed words and still missed, missed on the floor.
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
-- and the empty string for every other element -- which has exactly one row per
-- screen, so naming its rectangle would repeat what the model already says.
local function whereIn(element: any, rect: any): string
    if element.rect ~= nil then
        return ""
    end
    return " at [" .. oracle.rect_key(rect) .. "]"
end

-- The rectangle a claim about this measurement would have written down, which is
-- what a cell is filed under. An element that draws its own rectangle is claimed
-- with none of its own -- `Expectation.new` refuses the second answer -- so a
-- lookup for one asks for the cell filed under no rectangle, whatever region the
-- walk is standing in. One function rather than the same conditional at each of
-- the four lookups: a walk that asked for the wrong key would find no cell and
-- report a claimed row as unclaimed, which reads as a quiet model, not a bug.
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
-- and that page did not resolve on its own pixels. It names a page and carries no
-- collision, so it shares only `kind` and `detail` with `pushFinding`.
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

-- `distinguishes` is whether any page in this model exercises `identify` on the
-- element. The two rules that need no claim ask whether the appearances can be
-- told apart, so without it they have nothing to be wrong about; the ones that
-- read a claim do not consult it, and neither does the separation measurement.
local function judgeAppearanceSet(
    screen: any,
    element: any,
    rect: any,
    measured: { any },
    folded: any,
    owners: { string },
    distinguishes: boolean,
    separations: { Separation },
    findings: { Finding }
)
    if #measured < 2 then
        return
    end

    local screenName  = screen.name
    local elementName = element.name
    local where       = whereIn(element, rect)

    -- Rule one: at most one appearance may hit, on an element some page
    -- identifies by. It fires on an unclaimed screen too, which is where an
    -- author's own testing habit leaves the hole.
    local hits: { string } = {}
    for index = 1, #measured do
        local entry = measured[index]
        if entry.hit then
            table.insert(hits, entry.appearance.name or "unnamed")
        end
    end
    if distinguishes and #hits > 1 then
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
    -- screen. "First past the threshold wins" violates it -- an over-wide early
    -- appearance answers first and returns ITS rectangle, so the click lands on
    -- another appearance's pixels with nothing downstream noticing.
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
    -- runner-up came rather than whether it would have been accepted. Every
    -- element is measured; the finding is the half that asks whether the two
    -- answers had to be told apart.
    for index = 1, #measured do
        local entry = measured[index]
        if entry ~= folded and entry.match ~= nil then
            local rivalName = entry.appearance.name or "unnamed"
            local winner    = folded.match
            local rival     = entry.match
            -- The exact integer form of "the winner's normalised distance,
            -- multiplied by the factor, is still no worse than the rival's".
            -- Every operand is inside what a double holds exactly: a score is at
            -- most 255 times the template's pixel count.
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
            if distinguishes and not clears then
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
-- template half is deliberate. A template search is defined by the element alone,
-- so the matrix can run it on every screen and catch a mark answering where
-- nobody thought to look; there is no reading a region "in general", only reading
-- it and comparing against something, and only the claim says what. The
-- falsification pressure the template half gets from unclaimed cells, the text
-- half gets from `reading.confusions` instead.
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
        -- Nothing to search for and nothing claimed to read.
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
        -- A region reading the claimed words on a screen the file says it does
        -- not is one page's signature answering for another, and no template
        -- rule can see it.
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

-- It is a function of a RECTANGLE rather than of an element, which is what lets
-- one screen carry several rows for a shape drawn once and placed many times: the
-- caller walks the rectangles and this measures each.
local function walkPixelElement(
    ctx: any,
    ticket: any,
    claims: any,
    screen: any,
    element: any,
    rect: any,
    distinguishes: boolean,
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
            -- An unnamed appearance can own no cell: `Expectation.new` takes
            -- only a name `Element.appearance_named` resolves, and a nil one
            -- keys the FOLDED element's own cell, so asking would judge this
            -- row against the element's claim. `ownersOf` skips it likewise.
            local state = oracle.state_unclaimed
            if name ~= nil then
                state = oracle.Claims.state_for(
                    claims,
                    screen.name,
                    element.name,
                    name,
                    claimRect
                )
            end
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
        distinguishes,
        verdict.separations,
        verdict.findings
    )
end

-- The regions one element is measured in on one screen.
--
-- An element that draws its own rectangle is measured there ALWAYS, on every
-- screen whether or not anybody thought to claim it, which is the property every
-- unclaimed cell of this matrix rests on.
--
-- An element that draws none is measured where its claims say and nowhere else:
-- a minimap cell's position is a fact about the frame, so a rectangle from one
-- screen means nothing on the next. The pressure comes instead from the author
-- writing the rectangle it must match AND the ones it must miss.
local function searchRects(claims: any, screen: any, element: any): { any }
    if element.rect ~= nil then
        return { element.rect }
    end
    return oracle.Claims.rects_for(claims, screen.name, element.name)
end

-- Every element some page in this model exercises `identify` on, by name. It is
-- the scope of the two rules that ask whether an appearance set can be told
-- apart, and it is asked of the whole model rather than of one page: an element
-- that anchors one page and is only clicked on the next still has the consumer,
-- so one page exercising identify is enough.
--
-- `exercised` on the reference and not `capabilities` on the element. The
-- element's set says what it MAY do, and an element declaring identify that no
-- page identifies by has no consumer either; `Page.new` refuses a row
-- exercising more than the element declares, so the reference side is the
-- tighter of the two questions.
--
-- Names rather than tables, for the reason `Page.reference_for` compares by
-- name: a layer-three script may derive its own element from the file's.
local function identifiedElements(built: any): { [string]: boolean }
    local identified: { [string]: boolean } = {}
    for _, page in built.pages do
        for _, reference in page.references do
            if reference.exercised.identify then
                identified[reference.element.name] = true
            end
        end
    end
    return identified
end

-- Measures one screen: what it says it IS, then every element, then every
-- appearance of the elements that declare more than one.
--
-- The screen's own claim goes FIRST because it is about the whole frame the cells
-- are then read off, and a page that does not resolve on the screen that says it
-- is that page explains most of the cells underneath it. Its reason is
-- `observe.resolve_page`'s own sentence, spent on THIS ticket, so the finding
-- names the same clause a run would have failed on.
local function walkScreen(
    ctx: any,
    ticket: any,
    built: any,
    screen: any,
    identified: { [string]: boolean },
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
            -- claim places it: no region to search and no reading to take, so
            -- the row says nothing was measured rather than reporting a miss
            -- nobody looked for.
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
                        identified[element.name] == true,
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
-- frames from files, one file per capture in file-name order, and a screen's file
-- name is its content hash -- so `oracle.Claims.screens_in_frame_order` is what
-- pairs the Nth capture with the Nth screen. Every declared screen's file is READ
-- before its observation is opened, which turns "this project declares a screen
-- whose PNG is not there" into a failure naming that screen rather than a capture
-- that ran out of frames three screens later, and puts each screen's content hash
-- into the trace.
--
-- A text cell spends that same observation's READ BUDGET, one read per claimed
-- cell, and the host's refusal once it is out travels out of here as a raise: a
-- matrix that reported the cells it could not afford as misses would be a green
-- report with a hole whose size is a configuration number.
--
-- So does the page a screen declares, one read per identify row of that page
-- whose element has no template. One screen therefore costs twice the element
-- count ONLY WHILE EVERY ELEMENT DRAWS ITS OWN RECTANGLE: each half walks the
-- same list once, and each walk searches every rectangle the claims place the
-- element at. An element that draws none is placed by its claims and can be
-- claimed several times on one screen, so it costs one read per claim.
--
-- WHICH IS WHY THE CEILING IS NOT THIS WALK'S TO WORK AROUND. The exact number is
-- `oracle.Claims.most_reads_on_one_screen`, a fact about the FILE, and the caller
-- sizes the budget from that file before the run starts and refuses in its own
-- words when the budget is short of it (entry/cli/check.cpp). Splitting a screen
-- across two observations is not available and would be wrong: the frames are
-- served one file per capture, so a re-opened cycle is the NEXT screen's pixels
-- and half the cells of screen three would be judged against screen four.
--
-- A check over a project claiming text also needs an OCR engine bound. Whoever
-- assembles the run asks `oracle.Claims.reads_text` and
-- `recognition.needs_engine` first -- two questions, because a page identified by
-- what its title box reads is read on every screen that declares it whether or
-- not that screen claims a text of its own -- and refuses naming the missing flag
-- rather than letting a read fail on screen three.
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
    -- Asked for rather than defaulted to an empty list: the appearance rules are
    -- scoped by which elements the PAGES identify by, so a model handed over
    -- without them would report a quiet matrix instead of a missing field.
    if type(built.pages) ~= "table" then
        error(
            "regress.check needs the pages of the model it walks; whether an "
                .. "appearance set has to be told apart is a question about "
                .. "which elements a page identifies by, and a model with no "
                .. "pages would answer 'none' rather than refuse",
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

    -- Once for the walk: the answer is a fact about the model and the same on
    -- every screen.
    local identified = identifiedElements(built)

    for _, screen in screens do
        -- The existence proof, and the trace line that names these bytes. The
        -- blob is not kept: the pixels reach the matcher through the frame the
        -- host serves, never through this read.
        ctx:project_read(screen.path)

        local ticket             = ctx:cycle_open()
        local ok, walkError      =
            pcall(walkScreen, ctx, ticket, built, screen, identified, verdict)
        local closed, closeError = pcall(ctx.cycle_close, ctx, ticket)
        if not ok then
            error(walkError, 0)
        end
        if not closed then
            error(closeError, 0)
        end
    end

    -- The rule that reads the FILE rather than a screen, raised once here because
    -- it is about a pair of screens and the walk stands on one at a time. It
    -- costs no capture, so it runs last and a reader gets the measured evidence
    -- above it either way.
    for _, confusion in reading.confusions(built.claims) do
        -- A pair of screens that are two views of ONE page is not a defect: the
        -- region reads that page's own name on both, and a signature resting on
        -- it is right either time. The rule holds for every other pair, including
        -- one where a screen says nothing about which page it is, because silence
        -- is not a fact (`recognition.same_page`).
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
-- caller's, so the same field is in the same place on every line of a report read
-- in a terminal; sorting would put `element` after `cells`.
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
-- before the evidence, and the findings before the cells that explain them.
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
            -- Last: every field above answers "what happened here", and these
            -- four answer "which here" -- looked up when two rows share a name
            -- rather than scanned for.
            { "search_x", cell.search_x },
            { "search_y", cell.search_y },
            { "search_width", cell.search_width },
            { "search_height", cell.search_height },
        }))
    end

    return table.concat(lines, "\n")
end

return regress
