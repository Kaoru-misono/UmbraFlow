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
-- THE TWO RULES THAT FIRE WITHOUT AN EXPECTATION are the reason this is more
-- than a table of recorded answers, and they are aimed at the failure
-- docs/pitfalls/colour-key-annotation.md documents -- a mark that hits every
-- state it was meant to tell apart, and therefore looks like the most reliable
-- mark in the project:
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
--
-- LOAD ORDER. `regress` sorts last of the framework modules, which is what lets
-- it name `model`, `observe` and `oracle`. It is the only module that walks a
-- whole model against a whole set of screens, and it holds no state of its own:
-- every verb takes the ctx it observes through and the model it walks.

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

local k_outcomeHit      = "hit"
local k_outcomeMiss     = "miss"
local k_outcomeNoPixels = "no_pixels"

local k_verdictExpected  = "expected"
local k_verdictUnclaimed = "unclaimed"
local k_verdictHole      = "hole"
local k_verdictMisfire   = "misfire"

local regress = {}

regress.separation_factor = k_separationFactor

-- One measured cell of the matrix.
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
    x: number?,
    y: number?,
    width: number?,
    height: number?,
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
export type Finding = {
    kind: string,
    screen: string,
    element: string,
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
-- IT SEARCHES THE ELEMENT'S OWN RECTANGLE AND NOT A PAGE'S REFINEMENT. A
-- reference may narrow the region for a click on one page, and narrowing is
-- what a click wants; the matrix wants the opposite. The question here is
-- whether these pixels can be found somewhere they do not belong, so the widest
-- region the model ever searches is the honest one, and it is also the region
-- an identify sweep uses -- which runs before any page is known and so has no
-- refinement available to it.
local function measureElement(ctx: any, ticket: any, element: any): { any }
    local measured: { any } = {}
    local rect = element.rect
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
    expected: string
): Cell
    local match: any = if entry ~= nil then entry.match else nil
    local hit        = entry ~= nil and entry.hit
    local cell: Cell = {
        screen     = screen.name,
        element    = element.name,
        appearance = appearance,
        subject    = subject,
        outcome    = if entry == nil
            then k_outcomeNoPixels
            elseif hit then k_outcomeHit
            else k_outcomeMiss,
        expected   = expected,
        verdict    = verdictFor(expected, hit),
        score      = if match ~= nil then match.score else nil,
        maximum    = if match ~= nil then match.maximum else nil,
        x          = if match ~= nil then match.x else nil,
        y          = if match ~= nil then match.y else nil,
        width      = if match ~= nil then match.width else nil,
        height     = if match ~= nil then match.height else nil,
    }
    table.insert(cells, cell)
    return cell
end

local function pushFinding(
    findings: { Finding },
    kind: string,
    screen: any,
    element: any,
    appearance: string?,
    rival: string?,
    detail: string
)
    table.insert(findings, {
        kind       = kind,
        screen     = screen.name,
        element    = element.name,
        appearance = appearance,
        rival      = rival,
        detail     = detail,
    })
end

-- Every appearance the claims name as matching this screen. More than one is
-- itself a contradiction in the file: two appearances of one element cannot
-- both be on one screen, because they are two ways ONE rectangle can look.
local function ownersOf(
    claims: any,
    screen: any,
    element: any
): { string }
    local owners: { string } = {}
    for index = 1, #element.appearances do
        local appearance = element.appearances[index]
        if appearance.name ~= nil then
            local state = oracle.Claims.state_for(
                claims,
                screen.name,
                element.name,
                appearance.name
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
    measured: { any },
    folded: any,
    owners: { string },
    separations: { Separation },
    findings: { Finding }
)
    if #measured < 2 then
        return
    end

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
            screen,
            element,
            hits[1],
            hits[2],
            "appearances "
                .. table.concat(hits, ", ")
                .. " all match this screen; they are two ways ONE rectangle can "
                .. "look, so a screen matching several of them tells nothing "
                .. "apart and the fold's answer is whichever scored better today"
        )
    end

    if #owners > 1 then
        pushFinding(
            findings,
            "contradictory_claims",
            screen,
            element,
            owners[1],
            owners[2],
            "the project file claims appearances "
                .. table.concat(owners, ", ")
                .. " both match this screen, and one rectangle wears one "
                .. "appearance at a time"
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
            screen,
            element,
            winnerName,
            owners[1],
            "the fold answered with '"
                .. winnerName
                .. "' on a screen the project file says '"
                .. owners[1]
                .. "' owns, so the click lands on that appearance's rectangle "
                .. "instead"
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
                    screen,
                    element,
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

-- Measures one screen: every element, then every appearance of the elements
-- that declare more than one.
local function walkScreen(
    ctx: any,
    ticket: any,
    built: any,
    screen: any,
    verdict: Verdict
)
    local claims = built.claims
    for _, element in built.elements do
        if #element.appearances == 0 then
            -- Nothing to search for. The row is emitted anyway, because a
            -- reader comparing two runs needs the inventory to be the same
            -- shape; `Expectation.new` already refuses to claim one of these,
            -- so its expectation is always "unclaimed" and its verdict always
            -- the same.
            pushCell(
                verdict.cells,
                screen,
                element,
                nil,
                k_subjectElement,
                nil,
                oracle.state_unclaimed
            )
        else
            local measured = measureElement(ctx, ticket, element)
            local folded   = foldMeasured(measured)

            local elementExpected = oracle.Claims.state_for(
                claims,
                screen.name,
                element.name,
                nil
            )
            local elementCell = pushCell(
                verdict.cells,
                screen,
                element,
                if folded ~= nil then folded.appearance.name else nil,
                k_subjectElement,
                folded or { match = nil, hit = false },
                elementExpected
            )
            if elementCell.verdict == k_verdictHole then
                pushFinding(
                    verdict.findings,
                    k_verdictHole,
                    screen,
                    element,
                    nil,
                    nil,
                    "the project file says this element matches here and "
                        .. "nothing did"
                )
            elseif elementCell.verdict == k_verdictMisfire then
                pushFinding(
                    verdict.findings,
                    k_verdictMisfire,
                    screen,
                    element,
                    elementCell.appearance,
                    nil,
                    "the project file says this element is absent here and it "
                        .. "matched"
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
                        name
                    )
                    local cell = pushCell(
                        verdict.cells,
                        screen,
                        element,
                        name,
                        k_subjectAppearance,
                        entry,
                        state
                    )
                    if cell.verdict == k_verdictHole then
                        pushFinding(
                            verdict.findings,
                            k_verdictHole,
                            screen,
                            element,
                            name,
                            nil,
                            "the project file says this appearance matches here "
                                .. "and it did not"
                        )
                    elseif cell.verdict == k_verdictMisfire then
                        pushFinding(
                            verdict.findings,
                            k_verdictMisfire,
                            screen,
                            element,
                            name,
                            nil,
                            "the project file says this appearance is absent "
                                .. "here and it matched"
                        )
                    end
                end
            end

            judgeAppearanceSet(
                screen,
                element,
                measured,
                folded,
                ownersOf(claims, screen, element),
                verdict.separations,
                verdict.findings
            )
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
            { "x", cell.x },
            { "y", cell.y },
            { "width", cell.width },
            { "height", cell.height },
        }))
    end

    return table.concat(lines, "\n")
end

return regress
