--!strict
-- observe: the verbs that turn a model plus one observation into a fact or an
-- action.
--
-- `model` owns the nouns and `navigation` owns the page graph and the belief
-- rules for the page stack; this file owns everything that COSTS A CAPTURE.
-- `walk_edge` is here because it delivers a trigger and waits on frames; the
-- stack it updates and the depth cap it honours are there, because neither looks
-- at a pixel. Both halves used to be the C++ verbs `cycle_page` and `cycle_find`
-- (docs/plans/2026-07-31-script-owned-page-model.md 4).
--
-- Every verb takes `ctx` first and reaches the host only through it, so a test
-- can drive these verbs against any ctx and a signature names every host call a
-- verb can make.
--
-- No policy is implied anywhere: every timeout, poll interval, streak length and
-- text comparison is a parameter with no default.
--
-- A stopped search is never a miss. `cycle_match` raises when a budget, a
-- deadline or a cancel stopped it, and no verb here wraps that in `pcall`:
-- turning one into "the element is not on screen" would make the model's answer
-- a function of a configuration number. A region that read nothing is the same
-- mistake one layer up, and `observe.empty_is_absence` is where it is answered.
--
-- Three things come from `native` rather than from `ctx`: `wait_until`'s Tier B
-- timeout, the read verbs' `recognition_incomplete`, and the bare-coordinate
-- delivery `observe.click` and `observe.long_press` need. Minting a host error
-- has no `ctx` forward, and a `ctx` forward for bare-coordinate input would put
-- it within reach of every business task. `native` is nil on a VM with no bound
-- session, and a method then fails at the call rather than at load.
local native = ...

local observe = {}

-- One thing this module located on one frame, and what a click on it would be
-- authorised by. `evidence` owns which frame it is about and who may mint one.
--
-- `positioned_by`: "pixels" is a template match, whose rectangle C++ can still
-- re-check at the click edge; "page" is an element with no template, whose whole
-- claim to a position is that the page resolved; "text" is a line `read_lines`
-- found inside an annotated region, where only the frame ever said where it is.
-- `text` and `confidence` are present exactly on a "text" hit, the only kind
-- whose identity is what it says.
export type Hit = {
    element: any,
    page: string,
    appearance: string?,
    positioned_by: string,
    interact: boolean,
    match: any,
    x: number,
    y: number,
    width: number,
    height: number,
    click_x: number,
    click_y: number,
    score: number?,
    maximum: number?,
    text: string?,
    confidence: number?,
}

-- What `observe.resolve_page` hands back when the frame behind a ticket IS the
-- page it was asked about; `evidence.mint_receipt` is where it comes from.
export type Receipt = {
    page: any,
    ticket: any,
}

export type Reading = {
    element: string,
    text: string,
    confidence: number,
    x: number,
    y: number,
    width: number,
    height: number,
    expected: string?,
    matches: boolean?,
}

-- What `observe.walk_edge` returns. "arrived" carries WHICH page resolved,
-- because an edge's `to` is a set. "interrupted" carries the interrupt page that
-- resolved instead; dismissing it is the caller's policy and never this layer's,
-- so the framework's job is to name it rather than to time out with nothing to
-- say.
export type WalkOutcome = {
    outcome: string,
    page: any,
}

-- Whether a match clears an appearance's threshold. The score is a sum of
-- absolute differences and the maximum is that template's ceiling, both handed
-- over by C++ without judgement
-- (docs/plans/2026-07-31-script-owned-page-model.md 7). The threshold is in
-- basis points of similarity, written multiplied out so no division rounds it.
function observe.matched(match: any, threshold: number): boolean
    return match.score * 10000 <= match.maximum * (10000 - threshold)
end

-- Whether `candidate` beat `incumbent`, comparing two appearances of one
-- element. Raw scores are not comparable across appearances -- each template has
-- its own ceiling -- so the test is the cross product, exact because both
-- operands are whole numbers well inside what a double represents exactly.
--
-- A tie loses: declaration order breaks ties and decides nothing else, so an
-- over-wide early appearance cannot answer first and return ITS rectangle while
-- the click lands on another appearance's pixels
-- (docs/plans/2026-07-31-annotation-model-capabilities.md 2.3, correction 1).
function observe.better(candidate: any, incumbent: any): boolean
    return candidate.score * incumbent.maximum
        < incumbent.score * candidate.maximum
end

-- Leading and trailing whitespace, removed. It belongs to the ENGINE and not to
-- the screen: a project file that had to spell " 出擊 " to match would record
-- where the rectangle was drawn instead of a fact about the UI. `%s` is the
-- ASCII class the engine emits around a line; a full-width space inside the text
-- is content and is left alone.
local function trimmed(text: string): string
    return (string.gsub(text, "^%s*(.-)%s*$", "%1"))
end

-- Whether a reading counts as the text the model expects.
--
-- It is equality and not containment: every screen on this target prints its own
-- name in the same top-left box and "出擊" is a prefix of "出擊紀錄", so a
-- contains rule resolves the second screen as the first and clicks the first
-- one's buttons on it. Whitespace at the ends is forgiven, for the reason above.
--
-- Callers still PASS it -- `read_element` takes it with no default -- because
-- whether full width equals half width and traditional equals simplified are
-- rules a project may replace. Identify-by-text calls this one itself: a page
-- signature is not a place to relax the rule per call site.
function observe.exact_text(actual: string, expected: string): boolean
    return trimmed(actual) == trimmed(expected)
end

-- Whether a reading is sure enough to be evidence, judged against the floor its
-- element declared (`model.Element.read_floor`). The number is DATA and lives in
-- the model where it can be argued with; the comparison is a judgement and lives
-- here. Reading is the one capability that fails open -- a rectangle aimed at
-- the wrong place returns plausible text -- so this is the only thing standing
-- between a guess and a page identity.
function observe.confident(reading: any, floor: number): boolean
    return reading.confidence >= floor
end

-- What a region that read NOTHING means to the caller who asked. Both read verbs
-- take one of these two and neither has a default.
--
-- The engine reports the same nothing whether the region is blank or whether
-- what belongs in it has not been drawn yet, so one frame cannot separate them
-- and the QUESTION has to. `empty_is_absence` is asked by a caller who wants to
-- know whether the region is clear -- the shape a `forbidden` clause has -- and
-- for it a region reading nothing is the answer. `empty_is_unknown` is asked by
-- a caller who wants what is IN the region, and for it the read verb raises
-- `uf.errors.recognition_incomplete`, which classifies as Retry so the caller
-- observes again instead of branching on a blank.
--
-- It raises rather than returning a second value, because a caller holding an
-- empty list walks its whole choosing loop and prints a sentence shaped like a
-- decision: this framework watched one skip a card reward whose cards had not
-- finished drawing.
observe.empty_is_absence = "absence"
observe.empty_is_unknown = "unknown"

-- The closed set, so a misspelling picks no policy rather than the wrong one.
local k_emptyMeaningKnown: { [string]: boolean } = table.freeze({
    [observe.empty_is_absence] = true,
    [observe.empty_is_unknown] = true,
})

-- The refusal blames level 3, as `searchRect` and `readTarget` below do: level 1
-- is this function, level 2 is the read verb that called it, and the caller that
-- left the policy out is one further again.
local function emptyMeaning(verb: string, value: any): string
    if type(value) ~= "string" or not k_emptyMeaningKnown[value] then
        error(
            verb
                .. " has to be told what an empty region means before it reads "
                .. "one: pass observe.empty_is_absence when a region holding "
                .. "nothing is the answer you came for, and "
                .. "observe.empty_is_unknown when you came for what is in it. "
                .. "There is no default, because the engine reports the same "
                .. "nothing for a blank region and for one the target has not "
                .. "drawn yet, and only the caller knows which of the two it "
                .. "would be willing to act on",
            3
        )
    end
    return value
end

-- What a read verb raises when it found nothing and the caller said nothing is
-- not an answer. It is a host-minted Tier B error rather than a Luau string, so
-- a project can catch it and a retry policy can name it; `recognition_incomplete`
-- means the caller learned nothing about the screen and must observe again.
local function raiseUnread(verb: string, element: any, rect: any)
    native.raise(
        "recognition_incomplete",
        verb
            .. " found no text at all in the region element '"
            .. element.name
            .. "' reads at ["
            .. tostring(rect.x)
            .. ","
            .. tostring(rect.y)
            .. " "
            .. tostring(rect.width)
            .. "x"
            .. tostring(rect.height)
            .. "], and this call passed observe.empty_is_unknown; a region "
            .. "reads nothing both when it is blank and when what belongs in it "
            .. "has not been drawn, so this frame established neither and the "
            .. "answer is another observation rather than a branch"
    )
end

-- Level 3, not 2: this is a helper, so the frames are caller -> public verb ->
-- here, and 2 blames the public verb's own line inside this file rather than the
-- project script that passed the wrong thing. The other helpers here already
-- use 3; this one was the exception.
local function requireCtx(ctx: any, verb: string)
    if type(ctx) ~= "table" then
        error(verb .. " needs the ctx it should reach the host through", 3)
    end
end

-- Where a verb looks: the row's rectangle when the page refined one, and the
-- element's own otherwise.
--
-- The last branch is unreachable through a page and reachable without one, which
-- is why it is a sentence rather than a nil. `Page.new` refuses a row that
-- neither inherits a rectangle nor states one, so every path through `find`
-- arrives with one; the read verbs also take a BARE element, and a caller
-- reaching this with one has asked to read a region nothing has placed yet.
local function searchRect(reference: any, element: any): any
    if reference ~= nil and reference.rect_override ~= nil then
        return reference.rect_override
    end
    if element.rect == nil then
        error(
            "element '"
                .. element.name
                .. "' draws no rectangle of its own, so there is nothing to look "
                .. "at until a page says where it sits; pass the page's own "
                .. "reference row rather than the element",
            3
        )
    end
    return element.rect
end

-- What a read verb was pointed at: the page row when the caller passed one, and
-- the element either way. Both read verbs take the same either-shape and refuse
-- on the same two grounds, and two copies of "which of these did I get, and may
-- it be read" is where the two would come to disagree about what `read` means.
local function readTarget(verb: string, value: any): (any, any)
    local reference: any = nil
    local element: any   = value
    if model.is_reference(value) then
        reference = value
        element   = reference.element
    elseif not model.is_element(value) then
        error(
            verb
                .. " needs an element built by Element.new, or the page "
                .. "reference row that says what that element reads on one page",
            3
        )
    end
    if not element.capabilities.read then
        error(
            "element '"
                .. element.name
                .. "' does not declare the read capability, so nothing may ask "
                .. "it for text",
            3
        )
    end
    return reference, element
end

-- The pixel a rectangle is clicked at when nothing matched inside it: the
-- middle, rounded down. It is the only deterministic answer for an element with
-- no template, whose whole claim to a position is that the page was recognised
-- (docs/plans/2026-07-31-annotation-model-capabilities.md 4-2.3).
local function centrePixel(rect: any): (number, number)
    return rect.x + (rect.width - 1) // 2, rect.y + (rect.height - 1) // 2
end

-- Searches one element's appearances inside `rect` and returns the best one, or
-- nil twice. `only` restricts the search to a single appearance name, which is
-- what a page reference's pin does for a click; with `only` nil every appearance
-- is searched and the best normalised margin wins.
local function foldAppearances(
    ctx: any,
    ticket: any,
    element: any,
    rect: any,
    only: string?
): (any, any)
    local bestAppearance: any = nil
    local bestMatch: any      = nil

    for index = 1, #element.appearances do
        local appearance = element.appearances[index]
        if only == nil or appearance.name == only then
            -- Not wrapped: a stopped search is a control error, not an absence.
            local found = ctx:cycle_match(
                ticket,
                appearance.template,
                rect.x,
                rect.y,
                rect.width,
                rect.height
            )
            if found ~= nil and observe.matched(found, appearance.threshold) then
                if bestMatch == nil or observe.better(found, bestMatch) then
                    bestAppearance = appearance
                    bestMatch      = found
                end
            end
        end
    end

    return bestAppearance, bestMatch
end

-- Reads one rectangle and says whether it read the text the model expects, plus
-- the phrase a refusal sentence uses to report what it read instead.
--
-- It costs a read out of the cycle's budget, which is why it is only ever
-- reached for an element with no appearances.
--
-- A reading below the floor is not a match and not an error either: the
-- commonest reason a title reads badly is that the screen in front of the run is
-- not the screen being asked about, which is an answer and not a fault. So it
-- comes back as "did not match", and a `forbidden` clause is satisfied by it,
-- because that clause asks for the absence of this evidence rather than for
-- evidence of an absence.
--
-- The read is not wrapped, for the reason no read here is.
local function readsExpected(
    ctx: any,
    ticket: any,
    element: any,
    rect: any,
    expected: string
): (boolean, string)
    local reading =
        ctx:cycle_read(ticket, rect.x, rect.y, rect.width, rect.height)
    if reading == nil then
        return false, "nothing at all"
    end

    local floor = model.Element.read_floor(element)
    if not observe.confident(reading, floor) then
        return false,
            '"'
                .. reading.text
                .. '" at only '
                .. tostring(reading.confidence)
                .. " basis points of confidence, under the "
                .. tostring(floor)
                .. " this element needs before a reading counts as evidence"
    end

    return observe.exact_text(reading.text, expected), '"' .. reading.text .. '"'
end

-- Whether `value` is a receipt this framework minted for `ticket`. Published
-- here because `evidence` is not a name any script can reach, and this is the
-- question a layer-three caller asks before committing to a branch that clicks.
observe.is_receipt_for = evidence.is_receipt_for

-- observe.resolve_page(ctx, ticket, page) -> (Receipt?, string?)
--
-- Whether the frame behind `ticket` is this page, and when it is not, the one
-- sentence saying why. Only references exercising identify are consulted; the
-- rest of the page describes what a script may DO once it is here. Success is a
-- receipt rather than a boolean because `observe.click` demands evidence that
-- this page resolved on THIS ticket.
--
-- The signature is strictly conjunctive -- every required reference must match
-- and every forbidden one must not -- and evaluation stops at the first clause
-- that fails, so rejecting a page costs one search rather than all of them.
--
-- The element decides which evidence is read. One with appearances identifies by
-- its pixels; one with none identifies by the TEXT its rectangle reads, checked
-- against what this page said it should read there
-- (docs/plans/2026-08-01-three-layers-and-agent-operator.md, open question 8),
-- which is what the target printing its own name in one box on every screen
-- costs one element instead of a template per page. Both polarities work on both
-- kinds, and a reading the engine was not sure enough about is an absence rather
-- than a match (see `readsExpected`).
--
-- Identify folds across every appearance, INCLUDING a pinned one: the sweep runs
-- before the page is known, so it cannot ask a page which appearance to expect
-- (docs/plans/2026-07-31-annotation-model-capabilities.md 4-2.4 correction).
--
-- It resolves ONE page. Asking "which page am I on" is a layer-three loop over
-- the pages it cares about, and that is the intended shape.
function observe.resolve_page(ctx: any, ticket: any, page: any): (Receipt?, string?)
    requireCtx(ctx, "observe.resolve_page")
    if not model.is_page(page) then
        error("observe.resolve_page needs a page built by Page.new", 2)
    end

    for index = 1, #page.references do
        local reference = page.references[index]
        if reference.exercised.identify then
            local element = reference.element
            if #element.appearances == 0 then
                -- The text clause. It reads the ELEMENT's rectangle, like the
                -- fold below: the sweep runs before the page is known, so a
                -- page's refinement is not available to it. `Page.new` refuses
                -- an identify row with nothing to check, one that refines the
                -- search rectangle, and one on an element that draws none, which
                -- is what makes the expected text and `element.rect` present
                -- here by construction.
                local expected =
                    model.Reference.expected_text(reference) :: string
                local matched, actual = readsExpected(
                    ctx,
                    ticket,
                    element,
                    element.rect,
                    expected
                )
                if reference.identify == "required" and not matched then
                    return nil,
                        "page '"
                            .. page.name
                            .. "' requires element '"
                            .. element.name
                            .. '\' to read "'
                            .. expected
                            .. '" and the region read '
                            .. actual
                end
                if reference.identify == "forbidden" and matched then
                    return nil,
                        "page '"
                            .. page.name
                            .. "' forbids element '"
                            .. element.name
                            .. '\' from reading "'
                            .. expected
                            .. '" and the region read exactly that'
                end
            else
                local appearance, found = foldAppearances(
                    ctx,
                    ticket,
                    element,
                    element.rect,
                    nil
                )
                if reference.identify == "required" and found == nil then
                    return nil,
                        "page '"
                            .. page.name
                            .. "' requires element '"
                            .. element.name
                            .. "' and none of its appearances matched"
                end
                if reference.identify == "forbidden" and found ~= nil then
                    return nil,
                        "page '"
                            .. page.name
                            .. "' forbids element '"
                            .. element.name
                            .. "' and appearance '"
                            .. (appearance.name or "unnamed")
                            .. "' matched"
                end
            end
        end
    end

    return evidence.mint_receipt(page, ticket), nil
end

-- observe.find(ctx, ticket, page_or_reference, element) -> Hit?
--
-- Locates one element the page references. `page_or_reference` may be the page
-- (with `element` naming which one) or the page's reference row directly.
--
-- Find is read-only and does not require interact: a state element that only
-- ever identifies still has to be queryable
-- (docs/plans/2026-08-01-three-layers-and-agent-operator.md 5-3). The interact
-- check happens in `observe.click`, and the authorisation travels on the hit so
-- the two cannot disagree.
--
-- An element with no appearances is positioned by the page: the hit is its
-- rectangle, `positioned_by` says "page", and `match` is nil. Such a hit has had
-- NO pixel-level re-verification, so reading the rectangle and checking its text
-- first is the recommended default
-- (docs/plans/2026-07-31-annotation-model-capabilities.md 4-2.6).
--
-- It does not check that the page resolved on this ticket. The caller names the
-- page, so a find is a question about pixels and answers it without a
-- resolution; looking is free and ACTING is not, and `observe.click` demands the
-- receipt `resolve_page` mints on the same ticket. A hit on a page-positioned
-- element is worth exactly that receipt and nothing without one.
function observe.find(
    ctx: any,
    ticket: any,
    pageOrReference: any,
    element: any
): Hit?
    requireCtx(ctx, "observe.find")

    local reference: any = nil
    if model.is_reference(pageOrReference) then
        reference = pageOrReference
        if
            element ~= nil
            and model.is_element(element)
            and element.name ~= reference.element.name
        then
            error(
                "observe.find was given a reference to element '"
                    .. reference.element.name
                    .. "' and asked for element '"
                    .. tostring(element.name)
                    .. "'",
                2
            )
        end
    elseif model.is_page(pageOrReference) then
        if not model.is_element(element) then
            error(
                "observe.find needs the element to look for on page '"
                    .. pageOrReference.name
                    .. "'",
                2
            )
        end
        reference = model.Page.reference_for(pageOrReference, element)
        if reference == nil then
            error(
                "page '"
                    .. pageOrReference.name
                    .. "' does not reference element '"
                    .. element.name
                    .. "'; a page can only find what it declared it uses",
                2
            )
        end
    else
        error(
            "observe.find needs a page or a page reference to search within",
            2
        )
    end

    local target = reference.element
    local rect   = searchRect(reference, target)

    if #target.appearances == 0 then
        local clickX, clickY = centrePixel(rect)
        local hit = evidence.mint_hit({
            element       = target,
            page          = reference.page,
            appearance    = nil,
            positioned_by = "page",
            interact      = reference.exercised.interact,
            match         = nil,
            x             = rect.x,
            y             = rect.y,
            width         = rect.width,
            height        = rect.height,
            click_x       = clickX,
            click_y       = clickY,
            score         = nil,
            maximum       = nil,
        }, ticket)
        return hit
    end

    -- The pin is consulted only for a reference that exercises interact, which
    -- Page.new already guarantees is the only place a pin may appear.
    local appearance, found =
        foldAppearances(ctx, ticket, target, rect, reference.pinned_appearance)
    if found == nil then
        return nil
    end

    local hit = evidence.mint_hit({
        element       = target,
        page          = reference.page,
        appearance    = appearance.name,
        positioned_by = "pixels",
        interact      = reference.exercised.interact,
        match         = found,
        x             = found.x,
        y             = found.y,
        width         = found.width,
        height        = found.height,
        click_x       = found.click_x,
        click_y       = found.click_y,
        score         = found.score,
        maximum       = found.maximum,
    }, ticket)
    return hit
end

-- observe.read_element(ctx, ticket, element_or_reference, on_empty, compare)
--     -> Reading?
--
-- Reads the element's rectangle and, when there is expected text, says whether
-- the reading counts as that text.
--
-- It takes either an element or a page's row, and the answer can differ. Handed
-- an element it reads that element's own rectangle against that element's own
-- expected text; handed a reference it reads what THIS page says about the same
-- element -- the row's expected text (`model.Reference.expected_text`) and the
-- row's `rect_override` when it carries them. There is no page-plus-element
-- form: a caller holding a page reaches the row through
-- `model.Page.reference_for`.
--
-- `on_empty` comes before `compare` because it is required on every call and the
-- comparison is required only when there is expected text to judge. What each
-- value means is on `observe.empty_is_absence`: a nil reading under
-- `empty_is_absence`, a raise under `empty_is_unknown`.
--
-- The expected text is DATA and the comparison is POLICY. "This cell says 出击"
-- is a fact about the UI and lives in the project file; whether full width
-- equals half width is a rule this layer owns, which is why `cycle_read` takes
-- no expected text at all
-- (docs/plans/2026-08-01-three-layers-and-agent-operator.md 5-2). So `compare`
-- is required whenever there is expected text and has no default -- pass
-- `observe.exact_text` for equality -- and `matches` comes back nil when there
-- is none. `confidence` travels on every reading, because reading fails open.
function observe.read_element(
    ctx: any,
    ticket: any,
    elementOrReference: any,
    onEmpty: any,
    compare: any
): Reading?
    requireCtx(ctx, "observe.read_element")
    -- Named before the generic refusal, because a comparison landing in the
    -- empty-region slot is the one mistake this signature makes easy.
    if type(onEmpty) == "function" then
        error(
            "observe.read_element takes the empty-region policy before the "
                .. "comparison: read_element(ctx, ticket, target, "
                .. "observe.empty_is_absence, observe.exact_text). Both are "
                .. "policies with no default, so their order is the only thing "
                .. "saying which is which",
            2
        )
    end
    local meaning = emptyMeaning("observe.read_element", onEmpty)

    local reference, element = readTarget("observe.read_element", elementOrReference)
    local expected: string? = if reference ~= nil
        then model.Reference.expected_text(reference)
        else element.expected_text
    if expected ~= nil and type(compare) ~= "function" then
        error(
            "reading element '"
                .. element.name
                .. "' needs a comparison function, because there is expected "
                .. "text to judge the reading against; pass observe.exact_text "
                .. "for equality",
            2
        )
    end

    local rect = searchRect(reference, element)
    local reading =
        ctx:cycle_read(ticket, rect.x, rect.y, rect.width, rect.height)
    if reading == nil then
        if meaning == observe.empty_is_unknown then
            raiseUnread("observe.read_element", element, rect)
        end
        return nil
    end

    -- The element's name travels with the reading: which region was read, what
    -- came back, and how sure the engine was are the whole of the evidence a
    -- wrong read can be found in afterwards
    -- (docs/plans/2026-07-31-annotation-model-capabilities.md 4-2.7).
    return table.freeze({
        element    = element.name,
        text       = reading.text,
        confidence = reading.confidence,
        x          = reading.x,
        y          = reading.y,
        width      = reading.width,
        height     = reading.height,
        expected   = expected,
        matches    = if expected ~= nil
            then compare(reading.text, expected) == true
            else nil,
    })
end

-- observe.read_lines(ctx, ticket, element_or_reference, on_empty) -> { Hit }
--
-- Reads the element's whole region and returns one hit per line of text the
-- FRAME found inside it, top to bottom then left to right, each carrying its own
-- rectangle in target pixels. What an empty list means is `on_empty`'s: under
-- `empty_is_unknown` there is no list to walk because the verb raises, and under
-- `empty_is_absence` the empty list comes back and means what the caller said.
--
-- It exists because a rectangle per line models one frame and not a region that
-- SCROLLS: this target's character grid has columns at fixed x and moves
-- continuously in y, so dragging it once moved every name by two hundred pixels.
--
-- What comes back is the `Hit` `find` mints, with `positioned_by` "text" and the
-- decoded string beside it, so a line reaches `observe.click` through the same
-- door a template match does and every check there applies to it unchanged. It
-- is authorised on the same terms too: its position is evidence read off the
-- very frame the click will be authorised against, and that evidence does not
-- travel -- `observe.click` refuses a line located on any other frame.
--
-- `read` is what lets the region be read; `interact` on the page's own row is
-- what lets a line in it be clicked. Reading is allowed from a bare element too,
-- and such a hit carries no page and no interact. An empty-text line is kept: a
-- region the detector found and the recogniser could not read is a frame saying
-- "there is something here I cannot spell".
--
-- It costs one read to locate plus one per line found and does NOT consume the
-- cycle, so the same frame goes on to click; a region holding more lines than
-- the cycle can pay for raises rather than returning the first few. Picking one
-- line out and aiming beside it are `hits.matching` and `hits.offset`, which
-- cost no capture and are therefore not here.
function observe.read_lines(
    ctx: any,
    ticket: any,
    elementOrReference: any,
    onEmpty: any
): { Hit }
    requireCtx(ctx, "observe.read_lines")
    local meaning = emptyMeaning("observe.read_lines", onEmpty)

    local reference, element = readTarget("observe.read_lines", elementOrReference)
    local rect  = searchRect(reference, element)
    local found =
        ctx:cycle_read_lines(ticket, rect.x, rect.y, rect.width, rect.height)

    if #found == 0 and meaning == observe.empty_is_unknown then
        raiseUnread("observe.read_lines", element, rect)
    end

    -- `located` rather than `hits`, which is the name of the published module
    -- whose verbs operate on these results.
    local located: { Hit } = {}
    for index = 1, #found do
        local line = found[index]
        local lineRect = {
            x      = line.x,
            y      = line.y,
            width  = line.width,
            height = line.height,
        }
        local clickX, clickY = centrePixel(lineRect)
        local hit = evidence.mint_hit({
            element       = element,
            page          = if reference ~= nil then reference.page else nil,
            appearance    = nil,
            positioned_by = "text",
            interact      = if reference ~= nil
                then reference.exercised.interact
                else false,
            match         = nil,
            x             = line.x,
            y             = line.y,
            width         = line.width,
            height        = line.height,
            click_x       = clickX,
            click_y       = clickY,
            score         = nil,
            maximum       = nil,
            text          = line.text,
            confidence    = line.confidence,
        }, ticket)
        located[index] = hit
    end
    return table.freeze(located)
end

-- requireAuthorisedHit(verb, ticket, receipt, hit) -> ()
--
-- The check every verb that delivers pointer input at a hit passes through
-- first. It reaches no host and delivers nothing; the verbs below it do that.
--
-- This is where authorisation is enforced. C++ still keeps four guarantees: the
-- match came from THIS ticket's own match on THIS frame, the observation lease
-- is fresh, the fingerprint is compatible, and a ticket delivers at most one
-- input. "Only click an annotated element" is no longer among them, because the
-- annotation lives in Luau -- so the trusted framework enforces it here, and the
-- bare-point primitive a page-positioned element needs is reachable only from
-- this layer (docs/plans/2026-08-01-three-layers-and-agent-operator.md 2).
--
-- The receipt is the fifth requisite: the page authorising the element must have
-- resolved on the SAME observation, which `cycle_click` could enforce while the
-- ledger held the resolution and cannot now that a page is a Luau object. Three
-- things are checked against it:
--
--   * it is a receipt this module minted, so a table shaped like one is not
--     evidence;
--   * its ticket is THIS ticket, so a page resolved on an earlier frame cannot
--     authorise a click on a later one;
--   * its page is the page the hit's own reference row belongs to, so proving
--     you are on page A cannot authorise a click page B declared.
--
-- A failed resolution mints nothing at all, which is what makes "resolve_page
-- said no" and "resolve_page was never called" the same refusal here.
--
-- One function serves both delivering verbs, so a verb that delivered at a hit
-- without passing through here would be visible as such.
--
-- `verb` names the caller because that is the only part of each sentence that
-- differs; the refusals blame level 3, the caller of the verb that called this.
local function requireAuthorisedHit(
    verb: string,
    ticket: any,
    receipt: any,
    hit: any
)
    if not evidence.is_receipt(receipt) then
        error(
            verb
                .. " needs the receipt observe.resolve_page returned for the "
                .. "page this element belongs to; a resolution that failed "
                .. "returns none, so there is nothing to pass and nothing to "
                .. "act on",
            3
        )
    end
    if not rawequal(receipt.ticket, ticket) then
        error(
            verb
                .. " was given a receipt for page '"
                .. receipt.page.name
                .. "' minted on another observation cycle; an action is "
                .. "authorised by the page THIS frame resolved, and evidence "
                .. "from an earlier frame describes a screen that has moved on",
            3
        )
    end
    if evidence.hit_ticket(hit) == nil then
        error(
            verb
                .. " takes a hit that observe.find returned on this cycle, and "
                .. "nothing else",
            3
        )
    end
    -- The sixth requisite, and the one a located line made necessary. A hit is
    -- evidence about the frame it was minted on and not a claim about the next.
    -- C++ enforces this for a template hit through the match handle's cycle
    -- ordinal and cannot enforce it for a line, which reaches the host as a bare
    -- coordinate -- so it is enforced here, for every kind of hit alike.
    if not rawequal(evidence.hit_ticket(hit), ticket) then
        error(
            verb
                .. " was given a hit located on another observation cycle; "
                .. "where a thing was on an earlier frame is not where it is on "
                .. "this one, and an action authorised by this frame has to be "
                .. "aimed by it too",
            3
        )
    end
    if receipt.page.name ~= hit.page then
        error(
            verb
                .. " holds a receipt for page '"
                .. receipt.page.name
                .. "' and a hit page '"
                .. tostring(hit.page)
                .. "' authorised; one element referenced by two pages is "
                .. "interactive on the page that was recognised and on no other",
            3
        )
    end
    if not hit.interact then
        error(
            "this page does not exercise interact on element '"
                .. hit.element.name
                .. "'; finding it is allowed and acting on it is not",
            3
        )
    end
end

-- observe.click(ctx, ticket, receipt, hit) -> ()
--
-- Delivers the click the hit describes and spends the observation cycle.
-- Authorisation is `requireAuthorisedHit`'s, above.
function observe.click(ctx: any, ticket: any, receipt: any, hit: any)
    requireCtx(ctx, "observe.click")
    requireAuthorisedHit("observe.click", ticket, receipt, hit)

    if hit.match ~= nil then
        ctx:cycle_click(ticket, hit.match)
        return
    end

    -- The bare-point delivery for an element with no template: it has a
    -- rectangle and no match handle, so the click lands where the page model
    -- says the element is
    -- (docs/plans/2026-08-01-three-layers-and-agent-operator.md 5, decision 4).
    -- It goes through `native` and not through `ctx`, which is the whole
    -- confinement: `ctx` is published into every project environment, so a
    -- forward there would be a bare click a business task could call directly,
    -- skipping every check above.
    if native == nil then
        error(
            "observe.click needs the host's private surface to deliver a click "
                .. "at an element with no template; this VM has no bound session",
            2
        )
    end
    native.cycle_click_point(ticket, hit.click_x, hit.click_y)
end

-- observe.long_press(ctx, ticket, receipt, hit, hold_ms) -> ()
--
-- Presses where the hit is, holds the button down for `hold_ms`, releases it,
-- and spends the observation cycle.
--
-- Some targets say more when pressed than when looked at. The one this framework
-- was built for fans its battle cards so each name is clipped by the card in
-- front of it, and a press magnifies the card; matching the fragment instead
-- read `萬人的英雄`, then `人的`, then `人的英` across three turns and once
-- nearly matched the wrong card.
--
-- It is authorised exactly as `observe.click` is, through the same function, and
-- goes through `native` rather than `ctx` for the reason stated there.
--
-- Every hit is delivered at its POINT, including one that carries a match: there
-- is no match-handle spelling of a long press. Nothing is relaxed by that,
-- because the same-frame rule is enforced in `requireAuthorisedHit` for every
-- kind of hit alike.
--
-- It spends the cycle, like every delivered input. The magnified card is on the
-- NEXT frame by definition, so reading it means opening another observation.
--
-- `hold_ms` has no default: how long this target wants a press held is a fact
-- the author measured. The host caps it, because a ceiling stops a script
-- leaving the target mid-press while a default would merely choose for them.
function observe.long_press(
    ctx: any,
    ticket: any,
    receipt: any,
    hit: any,
    holdMillis: number
)
    requireCtx(ctx, "observe.long_press")
    requireAuthorisedHit("observe.long_press", ticket, receipt, hit)
    if type(holdMillis) ~= "number" or holdMillis % 1 ~= 0 or holdMillis < 0 then
        error(
            "observe.long_press needs hold_ms = a whole number of milliseconds "
                .. "to hold the button down; it has no default, because how long "
                .. "a target wants a press held is a fact about that target and "
                .. "not something this framework can know",
            2
        )
    end

    if native == nil then
        error(
            "observe.long_press needs the host's private surface to deliver a "
                .. "press; this VM has no bound session",
            2
        )
    end
    native.cycle_long_press(ticket, hit.click_x, hit.click_y, holdMillis)
end

local function requireWholeAtLeast(
    verb: string,
    options: any,
    key: string,
    floor: number,
    what: string
): number
    local value = options[key]
    if type(value) ~= "number" or value % 1 ~= 0 or value < floor then
        error(
            verb
                .. " needs "
                .. key
                .. " = "
                .. what
                .. "; it has no default, because a wait nobody sized is a wait "
                .. "nobody can read off the script",
            3
        )
    end
    return value
end

-- One turn of the wait: does the target hold on the frame behind `ticket`.
local function holdsNow(ctx: any, ticket: any, page: any, element: any): boolean
    local receipt = observe.resolve_page(ctx, ticket, page)
    if receipt == nil then
        return false
    end
    if element == nil then
        return true
    end
    return observe.find(ctx, ticket, page, element) ~= nil
end

-- One observation, opened and closed whatever `probe` did. The observation's own
-- failure outranks a failure from the close, so the caller is told why the work
-- failed rather than the second-order consequence of it. Both waits below go
-- through here, so neither can leave a cycle open on a raise.
local function observeOnce(ctx: any, probe: any): any
    local ticket             = ctx:cycle_open()
    local ok, found          = pcall(probe, ctx, ticket)
    local closed, closeError = pcall(ctx.cycle_close, ctx, ticket)

    if not ok then
        error(found, 0)
    end
    if not closed then
        error(closeError, 0)
    end
    return found
end

-- Observes until `probe` returns the SAME page `consecutive` times in a row, and
-- returns it; returns nil when the budget ran out first.
--
-- Waiting for a still frame cannot work on this target: a resting-point border
-- animates forever, so a whole-frame difference never reaches zero and the verb
-- either deadlocks or lies. K consecutive agreeing observations of a SEMANTIC
-- fact say the same thing about the part of the screen that matters, and say it
-- about something a trace can show
-- (docs/plans/2026-08-01-three-layers-and-agent-operator.md 7).
--
-- A streak is broken by one disagreeing observation rather than decremented, and
-- it is per-page rather than per-hit: an edge whose `to` names three pages must
-- not settle by seeing each of them once.
--
-- Returning nil rather than raising leaves the policy to the callers:
-- `wait_until` turns an exhausted budget into a Tier B timeout and `walk_edge`
-- looks for an interrupt page first.
local function waitForStreak(
    ctx: any,
    probe: any,
    consecutive: number,
    timeoutMillis: number,
    intervalMillis: number
): any
    local deadline = ctx:deadline(timeoutMillis)
    local held: any = nil
    local streak    = 0

    while true do
        local found = observeOnce(ctx, probe)

        if found == nil then
            held   = nil
            streak = 0
        elseif held ~= nil and held.name == found.name then
            streak += 1
        else
            held   = found
            streak = 1
        end

        if streak >= consecutive then
            return held
        end

        if not ctx:wait(deadline, intervalMillis) then
            return nil
        end
    end
end

-- observe.wait_until(ctx, options) -> true
--
-- Observes until the target holds on `consecutive` observations in a row, or
-- raises `uf.errors.timeout` when the budget runs out. The timeout is a
-- host-minted Tier B error rather than a Luau string, so a project can catch it
-- and a retry policy can name it. It is a control error and never a "no":
-- nothing that ran out of time learned anything about the screen.
--
-- options.page       the page that must resolve (required)
-- options.element    an element on that page that must also be found (optional)
-- options.consecutive  how many agreeing observations in a row (required, >= 1)
-- options.timeout_ms   the whole budget (required, >= 0)
-- options.interval_ms  the pause between observations (required, >= 0; the host
--                      clamps it up to its own floor so a zero cannot spin)
function observe.wait_until(ctx: any, options: any): boolean
    requireCtx(ctx, "observe.wait_until")
    if type(options) ~= "table" then
        error("observe.wait_until needs an options table", 2)
    end
    if not model.is_page(options.page) then
        error(
            "observe.wait_until needs page = the page to wait for; waiting for "
                .. "an element still needs the page that references it, because "
                .. "the search region and the pinned appearance live on that "
                .. "page's reference row",
            2
        )
    end
    if options.element ~= nil and not model.is_element(options.element) then
        error("observe.wait_until element must be built by Element.new", 2)
    end

    local verb = "observe.wait_until"
    local consecutive = requireWholeAtLeast(
        verb,
        options,
        "consecutive",
        1,
        "a whole number of agreeing observations, at least one"
    )
    local timeoutMillis = requireWholeAtLeast(
        verb,
        options,
        "timeout_ms",
        0,
        "a whole number of milliseconds"
    )
    local intervalMillis = requireWholeAtLeast(
        verb,
        options,
        "interval_ms",
        0,
        "a whole number of milliseconds"
    )

    local page    = options.page
    local element = options.element
    local reached = waitForStreak(ctx, function(inner: any, ticket: any): any
        if holdsNow(inner, ticket, page, element) then
            return page
        end
        return nil
    end, consecutive, timeoutMillis, intervalMillis)

    if reached == nil then
        native.raise(
            "timeout",
            "waiting for page '"
                .. page.name
                .. "' to hold "
                .. tostring(consecutive)
                .. " observations in a row ran out of budget"
        )
    end
    return true
end

-- The first of `pages` that resolves on the frame behind `ticket`, or nil.
local function resolveAny(ctx: any, ticket: any, pages: any): any
    for index = 1, #pages do
        if observe.resolve_page(ctx, ticket, pages[index]) ~= nil then
            return pages[index]
        end
    end
    return nil
end

-- observe.walk_edge(ctx, stack, edge, options) -> WalkOutcome
--
-- Performs the edge's trigger, waits for somewhere it can land, and updates what
-- the run believes about the pages underneath. It does not pathfind and it does
-- not choose: which edge to walk is strategy and lives in layer three.
--
-- A click edge resolves the page it LEAVES, on the frame that delivers the
-- trigger, because `observe.click` is authorised by a receipt from this ticket.
-- It does not consult the page stack for that receipt -- the stack is belief and
-- this is evidence -- and walks nothing if the resolution fails. The cost is the
-- intended one: with a card-detail overlay open, an edge leaving the battle page
-- can no longer be walked, because that page's forbidden clause fires and
-- nothing on the frame proves the page authorising the click is the page in
-- front of the run. Reaching it means popping the overlay, or modelling the
-- overlaid screen as its own page
-- (docs/plans/2026-07-31-annotation-model-capabilities.md 4-2.6).
--
-- The click goes through `observe.click`, so interact and the receipt are
-- enforced in the one place that enforces them. A key goes through the ctx
-- forward and needs no receipt, because a keystroke names no position and no
-- page declares anything about keys. A spontaneous edge does nothing at all.
--
-- A timeout looks for an interrupt page before it gives up and hands it over
-- rather than dismissing it: what to do about a popup is the caller's policy.
--
-- options.consecutive  how many agreeing observations in a row (required, >= 1)
-- options.timeout_ms   the whole budget for the destination (required, >= 0)
-- options.interval_ms  the pause between observations (required, >= 0)
function observe.walk_edge(
    ctx: any,
    stack: any,
    edge: any,
    options: any
): WalkOutcome
    requireCtx(ctx, "observe.walk_edge")
    if not navigation.is_stack(stack) then
        error(
            "observe.walk_edge needs the page stack navigation.stack_new "
                .. "returned; a table nobody minted carries no depth cap and no "
                .. "graph, so the runaway push the cap exists to catch would go "
                .. "uncaught",
            2
        )
    end
    if not navigation.is_edge(edge) then
        error("observe.walk_edge needs an edge built by Edge.new", 2)
    end
    if not navigation.Graph.has_edge(stack.graph, edge) then
        error(
            "observe.walk_edge was given an edge that is not in the graph this "
                .. "stack was built over; the interrupt pages a timed-out walk "
                .. "falls back on come from that graph, so walking a stranger's "
                .. "edge would fall back on the wrong list",
            2
        )
    end
    if type(options) ~= "table" then
        error("observe.walk_edge needs an options table", 2)
    end

    local verb = "observe.walk_edge"
    local consecutive = requireWholeAtLeast(
        verb,
        options,
        "consecutive",
        1,
        "a whole number of agreeing observations, at least one"
    )
    local timeoutMillis = requireWholeAtLeast(
        verb,
        options,
        "timeout_ms",
        0,
        "a whole number of milliseconds"
    )
    local intervalMillis = requireWholeAtLeast(
        verb,
        options,
        "interval_ms",
        0,
        "a whole number of milliseconds"
    )

    -- Both read the stack and neither costs a capture, so both live in
    -- `navigation`. The ORDER matters: a push whose belief has nowhere to go is
    -- refused before its trigger is delivered, because a click that landed on a
    -- screen this run then refuses to reason about is worse than no click.
    local destinations: { any } = if edge.kind == "pop"
        then { navigation.pop_destination(stack, edge) }
        else edge.to
    navigation.require_push_room(stack, edge)

    if edge.via == "click" then
        local element = edge.via_element
        observeOnce(ctx, function(inner: any, ticket: any)
            local receipt, why = observe.resolve_page(inner, ticket, edge.from)
            if receipt == nil then
                error(
                    "walking the edge leaving page '"
                        .. edge.from.name
                        .. "' means clicking an element that page authorises, "
                        .. "and this frame is not that page: "
                        .. tostring(why),
                    0
                )
            end
            local hit = observe.find(inner, ticket, edge.from, element)
            if hit == nil then
                error(
                    "walking the edge leaving page '"
                        .. edge.from.name
                        .. "' means clicking element '"
                        .. element.name
                        .. "', and nothing on this frame matched it",
                    0
                )
            end
            observe.click(inner, ticket, receipt, hit)
            return nil
        end)
    elseif edge.via == "key" then
        observeOnce(ctx, function(inner: any, ticket: any)
            inner:key(ticket, edge.via_key)
            return nil
        end)
    end

    local reached = waitForStreak(ctx, function(inner: any, ticket: any): any
        return resolveAny(inner, ticket, destinations)
    end, consecutive, timeoutMillis, intervalMillis)

    if reached ~= nil then
        navigation.believe_arrival(stack, reached, edge.kind == "pop")
        return table.freeze({ outcome = "arrived", page = reached })
    end

    local interrupts = stack.graph.interrupts
    if #interrupts > 0 then
        local found = observeOnce(ctx, function(inner: any, ticket: any): any
            return resolveAny(inner, ticket, interrupts)
        end)
        if found ~= nil then
            navigation.believe_arrival(stack, found, false)
            return table.freeze({ outcome = "interrupted", page = found })
        end
    end

    local names: { string } = {}
    for index = 1, #destinations do
        names[index] = "'" .. destinations[index].name .. "'"
    end
    native.raise(
        "timeout",
        "walking the edge leaving page '"
            .. edge.from.name
            .. "' ran out of budget waiting for "
            .. table.concat(names, " or ")
            .. ", and no interrupt page was on screen either"
    )
    -- Unreachable: native.raise never returns. The return keeps the strict-mode
    -- reading of this function honest.
    return table.freeze({ outcome = "interrupted", page = nil })
end

return observe
