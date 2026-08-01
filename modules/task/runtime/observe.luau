--!strict
-- observe: the verbs that turn a model plus one observation into a fact or an
-- action.
--
-- `model` owns the nouns and `navigation` owns the page graph and the belief
-- rules for the page stack; this file owns everything that COSTS A CAPTURE.
-- Both halves used to be C++ verbs -- `cycle_page` and `cycle_find` -- and both
-- are gone from the host surface: they are rebuilt here on `cycle_match`,
-- `cycle_read` and `cycle_click`, which is what
-- docs/plans/2026-07-31-script-owned-page-model.md 4 replaced them with.
--
-- THE SPLIT WITH `navigation` IS THAT LINE AND NOTHING ELSE. `walk_edge` is here
-- because it delivers a trigger and waits on frames; the stack it updates, the
-- depth cap it honours and the rule that a base page resets belief are there,
-- because none of them looks at a pixel.
--
-- THE PRIMITIVES ARRIVE AS A PARAMETER. Every verb below takes `ctx` first and
-- reaches the host only through it. Nothing here closes over the private
-- surface to do observation work, so a test can drive these verbs against any
-- ctx and a reader can see, from the signature alone, exactly which host calls
-- a verb can make. The one exception is at the very bottom of this comment.
--
-- NO POLICY IS IMPLIED ANYWHERE. Every timeout, poll interval, streak length
-- and text comparison is a parameter with no default, in the same spirit as
-- ctx.luau: a default the caller cannot see is a decision the caller did not
-- make. `observe.exact_text` exists so that "equality" is a named thing you
-- pass, not a thing that happens when you pass nothing.
--
-- A STOPPED SEARCH IS NEVER A MISS. `cycle_match` raises when a budget, a
-- deadline or a cancel stopped it, precisely because a search that stopped
-- looking has established nothing. No verb here wraps it in `pcall`, so that
-- raise travels out to the caller as the control error it is. Turning one into
-- "the element is not on screen" would make the model's answer a function of a
-- configuration number, which is the one thing the determinism rule forbids.
--
-- THE EXCEPTION. `wait_until` mints a Tier B timeout, and minting a host error
-- has no `ctx` forward -- ctx.luau's own wait loop reaches for `native.raise`
-- for exactly this. So this module takes the private surface as its chunk
-- argument and uses ONE thing from it. `native` is nil on a VM with no bound
-- session, as it is in ctx.luau, and a method then fails at the call rather
-- than at load.
local native = ...

local observe = {}

-- One thing this module located on one frame, and what a click on it would be
-- authorised by. `evidence` owns which frame it is about and who may mint one.
--
-- `positioned_by` says where the position came from, and it has three values.
-- "pixels" is a template match, whose rectangle C++ can still re-check at the
-- click edge. "page" is an element with no template, whose rectangle is the
-- model's own and whose whole claim to a position is that the page resolved.
-- "text" is a line `read_lines` found INSIDE an annotated region: the region is
-- authored and the line's place in it is not, so the frame is the only thing
-- that ever said where it is.
--
-- `text` and `confidence` are present exactly on a "text" hit, because that is
-- the only kind whose identity is what it SAYS. A caller that ignores the
-- confidence on one has thrown away the only evidence that would let it notice
-- a region the recogniser guessed at.
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
-- page it was asked about. `evidence.mint_receipt` is where it comes from and
-- why it is an object rather than a boolean.
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

-- What `observe.walk_edge` returns.
--
-- "arrived" carries WHICH page resolved, because an edge's `to` is a set: an
-- edge that can land on the result page or on an error page has told the caller
-- something real only when it says which one happened.
--
-- "interrupted" carries the interrupt page that resolved instead. Dismissing it
-- is the caller's policy and never this layer's -- some popups are closed, some
-- are waited out, some end the run -- so the framework's whole job is to name it
-- rather than to time out with nothing to say.
export type WalkOutcome = {
    outcome: string,
    page: any,
}

-- Whether a match clears an appearance's threshold.
--
-- The score is a sum of absolute differences and the maximum is the ceiling for
-- that template, both handed over by C++ without judgement -- scores stay in
-- layer one, judging moves to layer two
-- (docs/plans/2026-07-31-script-owned-page-model.md 7). The threshold is in
-- basis points of similarity, so the test is
-- `score <= maximum * (1 - threshold/10000)`, written multiplied out so no
-- division rounds it.
function observe.matched(match: any, threshold: number): boolean
    return match.score * 10000 <= match.maximum * (10000 - threshold)
end

-- Whether `candidate` beat `incumbent`, comparing two appearances of one
-- element.
--
-- Raw scores between two appearances are NOT comparable: each has its own
-- template size and therefore its own ceiling. The exact comparison is the
-- cross product -- `score_a / max_a < score_b / max_b` without the division --
-- and it is exact rather than approximate because both operands are whole
-- numbers well inside what a double represents exactly.
--
-- A TIE LOSES. That is the whole of what declaration order decides: it breaks
-- ties and nothing else. "First appearance past its threshold wins" is
-- deterministic too, so determinism is not the argument against it -- the
-- argument is that an over-wide early appearance would answer first and return
-- ITS rectangle, and the click would land on pixels that belong to another
-- appearance with nothing downstream noticing
-- (docs/plans/2026-07-31-annotation-model-capabilities.md 2.3, correction 1).
function observe.better(candidate: any, incumbent: any): boolean
    return candidate.score * incumbent.maximum
        < incumbent.score * candidate.maximum
end

-- Leading and trailing whitespace, removed. It belongs to the ENGINE rather than
-- to the screen: a single-line read of a padded rectangle comes back with the
-- padding, and a project file that had to spell " 出擊 " to match would be
-- recording an artefact of where the rectangle was drawn instead of a fact about
-- the UI. `%s` is the ASCII class, which is what the engine emits around a line;
-- a full-width space INSIDE the text is content and is left alone.
local function trimmed(text: string): string
    return (string.gsub(text, "^%s*(.-)%s*$", "%1"))
end

-- Whether a reading counts as the text the model expects.
--
-- IT IS EQUALITY AND NOT CONTAINMENT, and on this target the difference decides
-- which page a run thinks it is on. Every screen prints its own name in the same
-- top-left box, and "出擊" is a prefix of "出擊紀錄": a contains rule would
-- resolve the second screen as the first and then click the first one's buttons
-- on it. What it does forgive is whitespace at the ends, on both sides, for the
-- reason above.
--
-- It remains the comparison a caller PASSES rather than one this module applies
-- behind them -- `read_element` still takes it as an argument with no default --
-- because whether full width equals half width and whether traditional equals
-- simplified are rules this layer owns and a project may replace. What changed
-- on 2026-08-01 is only that identify-by-text calls this one itself, since a
-- page signature is not a place a project gets to relax the rule per call site.
function observe.exact_text(actual: string, expected: string): boolean
    return trimmed(actual) == trimmed(expected)
end

-- Whether a reading is sure enough to be evidence, judged against the floor its
-- element declared (`model.Element.read_floor`).
--
-- The split is `observe.matched`'s exactly: the number is DATA and lives in the
-- model where it can be argued with, the comparison is a judgement and lives
-- here. Reading is the one capability that fails open -- a rectangle aimed at
-- the wrong place returns plausible text rather than nothing -- so this is the
-- only thing standing between a guess and a page identity.
function observe.confident(reading: any, floor: number): boolean
    return reading.confidence >= floor
end

local function requireCtx(ctx: any, verb: string)
    if type(ctx) ~= "table" then
        error(verb .. " needs the ctx it should reach the host through", 2)
    end
end

local function searchRect(reference: any, element: any): any
    if reference ~= nil and reference.rect_override ~= nil then
        return reference.rect_override
    end
    return element.rect
end

-- What a read verb was pointed at: the page row when the caller passed one, and
-- the element either way.
--
-- Extracted because both read verbs take the same either-shape and refuse on the
-- same two grounds, and two copies of "which of these did I get, and may it be
-- read" is exactly where the two would come to disagree about what `read` means.
--
-- The refusals blame level 3 rather than 2: level 1 is this function, level 2 is
-- the verb that called it, and the caller a wrong argument belongs to is one
-- further out again.
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
-- middle, rounded down. It is deterministic and it is the only sensible answer
-- for an element with no template, whose whole claim to a position is "the page
-- was recognised, so the rectangle is where it was drawn"
-- (docs/plans/2026-07-31-annotation-model-capabilities.md 4-2.3).
local function centrePixel(rect: any): (number, number)
    return rect.x + (rect.width - 1) // 2, rect.y + (rect.height - 1) // 2
end

-- Searches one element's appearances inside `rect` and returns the best one, or
-- nil twice.
--
-- `only` restricts the search to a single appearance name, which is what a page
-- reference's pin does for a click. With `only` nil every appearance is
-- searched and the best normalised margin wins.
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
            -- Not wrapped: a raise here is a stopped search, and a stopped
            -- search is a control error rather than an absence.
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
-- IT COSTS A READ OUT OF THE CYCLE'S BUDGET, which is why it is only ever
-- reached for an element with no appearances: an element with a template
-- identifies by its pixels and never pays for this.
--
-- A READING BELOW THE FLOOR IS NOT A MATCH, AND IT IS NOT AN ERROR EITHER. The
-- engine returns plausible text for any rectangle it is given, so a low
-- confidence means "this is a guess" and a guess is not evidence -- but neither
-- is it a failure of the run: the commonest reason a title reads badly is that
-- the screen in front of the run is not the screen being asked about, which is
-- the answer "no, this is not that page" and not a fault. So it comes back as
-- "did not match", and a `forbidden` clause is satisfied by it, because that
-- clause asks for the absence of this evidence rather than for evidence of an
-- absence.
--
-- The read is NOT wrapped: `cycle_read` raises when the cycle's read budget is
-- spent, and a read that never happened has established nothing, exactly as a
-- stopped search has not. Turning it into "the page is not this page" would make
-- an identity a function of a configuration number.
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
-- here rather than reimplemented, because `evidence` is not a name any script
-- can reach and this is the question a layer-three caller legitimately asks
-- before it commits to a branch that ends in a click.
observe.is_receipt_for = evidence.is_receipt_for

-- observe.resolve_page(ctx, ticket, page) -> (Receipt?, string?)
--
-- Whether the frame behind `ticket` is this page, and when it is not, the one
-- sentence saying why. Only references exercising identify are consulted; the
-- rest of the page describes what a script may DO once it is here.
--
-- SUCCESS IS A RECEIPT AND NOT A BOOLEAN. See the Receipt type above: the
-- receipt is the evidence `observe.click` demands that this page resolved on
-- THIS ticket, and it is the whole of how the same-frame authorisation rule
-- survived the model's move out of C++.
--
-- The signature is strictly conjunctive: every required reference must match
-- and every forbidden one must not. Evaluation stops at the first clause that
-- fails, so the cost of rejecting a page is one search rather than all of them.
--
-- THERE ARE TWO KINDS OF EVIDENCE, AND THE ELEMENT DECIDES WHICH. An element
-- with appearances identifies by its pixels, which is every page signature this
-- model has ever had and is unchanged in every byte. An element with none
-- identifies by the TEXT its rectangle reads, checked against what this page
-- said it should read there (2026-08-01, closing open question 8 of
-- docs/plans/2026-08-01-three-layers-and-agent-operator.md). The second exists
-- because the target prints its own name in one box on every screen: one drawn
-- rectangle, one `read` capability, and a row per page saying which name belongs
-- to it -- against a template per page, each of which would have to be cut,
-- thresholded and falsified separately for a fact the screen states in words.
--
-- BOTH POLARITIES WORK ON BOTH KINDS. `required` demands the match, `forbidden`
-- demands its absence, and a reading the engine was not sure enough about is an
-- absence rather than a match (see `readsExpected`).
--
-- IDENTIFY FOLDS ACROSS EVERY APPEARANCE, INCLUDING A PINNED ONE. The pin on a
-- reference is honoured by `find` and ignored here, because an identify sweep
-- happens before the page is known and therefore cannot ask a page which
-- appearance to expect (docs/plans/2026-07-31-annotation-model-capabilities.md
-- 4-2.4 correction). Ignoring it silently would be a trap, so it is stated on
-- the field in `model` and again here.
--
-- This resolves ONE page. Walking a catalogue to ask "which page am I on" is a
-- layer-three loop over pages it cares about, and that is the intended shape:
-- automation knows where it just went and needs to confirm it, and searching
-- every marker of every page is the recovery path, not the entrance.
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
                -- The text clause. `Page.new` has already refused an identify
                -- row that would arrive here with nothing to check, so the
                -- expected text is present by construction and the cast says so.
                --
                -- It reads the ELEMENT's rectangle, like the fold below and for
                -- the same reason: an identify sweep runs before the page is
                -- known, so a page's refinement is not available to it -- which
                -- is why `Page.new` refuses a rect_override on an identify row
                -- rather than leaving one here to be quietly ignored.
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
-- FIND IS READ-ONLY AND DOES NOT REQUIRE INTERACT. A state element that only
-- ever identifies still has to be queryable -- reading `hit.appearance` to see
-- which speed the game is at is a find, not a click
-- (docs/plans/2026-08-01-three-layers-and-agent-operator.md 5-3). The interact
-- check happens in `observe.click`, and the authorisation travels on the hit so
-- the two cannot disagree.
--
-- An element with no appearances is positioned by the page: the hit is its
-- rectangle, `positioned_by` says "page", and `match` is nil. Such a hit has
-- had NO pixel-level re-verification -- there was no template to verify against
-- -- and clicking one is the one place this model delivers an action on the
-- strength of "the page resolved" alone. Reading the rectangle first and
-- checking the text is the recommended default there
-- (docs/plans/2026-07-31-annotation-model-capabilities.md 4-2.6).
--
-- FIND DOES NOT CHECK THAT THE PAGE ACTUALLY RESOLVED ON THIS TICKET, and that
-- is a deliberate difference from the C++ verb it replaces. `cycle_find` had to
-- resolve first because it had no other way to learn which row to read; here the
-- caller names the page, so a find is a question about pixels and answers it
-- without a resolution. Looking is free; ACTING is not, and `observe.click`
-- demands the receipt `resolve_page` mints on the same ticket. The distinction
-- matters most for a page-positioned element, whose whole claim to a position is
-- that the page was recognised -- so a hit on one is worth exactly the receipt
-- that will be produced beside it, and worth nothing without one.
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

-- observe.read_element(ctx, ticket, element_or_reference, compare) -> Reading?
--
-- Reads the element's rectangle and, when there is expected text, says whether
-- the reading counts as that text.
--
-- IT TAKES EITHER AN ELEMENT OR A PAGE'S ROW, AND THE ANSWER CAN DIFFER. Handed
-- an element it reads that element's own rectangle against that element's own
-- expected text -- the reading of the model on its own, which is what a caller
-- holding no page has. Handed a reference it reads what THIS page says about the
-- same element: the row's expected text if it carries one
-- (`model.Reference.expected_text`), and the row's `rect_override` if it carries
-- one, because a page that refined where this element sits refined it for every
-- use of the row and the row is what the caller passed in.
--
-- It takes no page-plus-element form, unlike `observe.find`. A caller holding a
-- page reaches the row through `model.Page.reference_for`, and one explicit line
-- there is better than a fifth argument on a verb whose fourth one is already a
-- policy.
--
-- THE EXPECTED TEXT IS DATA AND THE COMPARISON IS POLICY. "This cell says 出击"
-- is a fact about the UI, so it is written in the project file where it can be
-- falsified and replayed. Whether full width equals half width, whether
-- traditional equals simplified, whether contains counts as equals -- that is a
-- rule this layer owns, which is why `cycle_read` takes no expected text at all
-- (docs/plans/2026-08-01-three-layers-and-agent-operator.md 5-2).
--
-- `compare` is therefore required whenever there is expected text, and there is
-- no default: pass `observe.exact_text` to get equality. Reading an element
-- with no expected text needs no comparison and `matches` comes back nil.
--
-- Reading is the one capability that fails OPEN -- a rectangle pointed at the
-- wrong place returns plausible text rather than nothing -- so `confidence`
-- travels on every reading and a caller that ignores it has kept none of the
-- evidence that would let it notice.
function observe.read_element(
    ctx: any,
    ticket: any,
    elementOrReference: any,
    compare: any
): Reading?
    requireCtx(ctx, "observe.read_element")

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
        return nil
    end

    -- The element's name travels with the reading because reading is the one
    -- capability with no score to argue from: which region was read, what came
    -- back, and how sure the engine was are the whole of the evidence a wrong
    -- read can be found in afterwards
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

-- observe.read_lines(ctx, ticket, element_or_reference) -> { Hit }
--
-- Reads the element's whole region and returns one hit per line of text the
-- FRAME found inside it, top to bottom then left to right, each carrying its own
-- rectangle in target pixels. An empty list means the region held no text.
--
-- WHY THIS EXISTS AT ALL, WHICH IS THE ONLY INTERESTING QUESTION ABOUT IT. Every
-- other verb here starts from a rectangle the model drew: `find` searches one,
-- `read_element` reads one. That works while a thing stays where it was
-- annotated. It stops working the moment a region SCROLLS -- the target's
-- character grid has columns at fixed x and moves continuously in y, so dragging
-- it once moved every name by two hundred pixels and no row ever snapped back.
-- A rectangle per name is not a model of that; it is a model of one frame. So
-- the region is annotated, and what is in it is read.
--
-- A HIT AND NOT A READING, AND THIS IS THE DECISION THIS VERB IS ABOUT. What
-- comes back is the same `Hit` `find` mints, with `positioned_by` "text" and the
-- decoded string beside it -- so a line goes to `observe.click` through the same
-- door a template match does, and every check there applies to it unchanged:
-- minted here, the receipt is for THIS ticket, the receipt's page is the hit's
-- page, and the page exercises interact on the element. A separate "line" type
-- with its own click verb would have been a second door into the same delivery,
-- and the second door is always the one that is missing a lock.
--
-- WHY A LINE MAY BE CLICKED AT ALL. The rule this layer keeps is that an action
-- is authorised by the frame it is delivered against. A line satisfies it as
-- squarely as a template match does: the region it was found in is annotated,
-- and its position inside that region is evidence read off the very frame the
-- click will be authorised against -- the same frame, the same cycle, the same
-- lease. Nothing is being relaxed. What is new is only that the frame, rather
-- than the project file, is what says where the target is; and for a list whose
-- contents the author has never seen, the frame is the only thing that COULD
-- say. The one thing that has to be added is the rule that this evidence does
-- not travel: a line located on one frame is a claim about that frame, so
-- `observe.click` refuses it on any other (see `evidence`).
--
-- THE ELEMENT STILL DECIDES WHETHER A CLICK IS ALLOWED. `read` is what lets the
-- region be read; `interact` on the page's own row is what lets a line in it be
-- clicked, exactly as for every other hit. A region annotated as readable and
-- not as interactive is a list to look at, and the refusal for one comes from
-- `observe.click` unchanged. Reading is allowed from a bare element too, and
-- such a hit carries no page and no interact -- looking is free, acting needs
-- the row.
--
-- AN EMPTY-TEXT LINE IS KEPT. The host reports a region the detector found and
-- the recogniser could not read, and it is dropped nowhere on the way up:
-- that is a frame saying "there is something here I cannot spell", which is
-- worth seeing, and a text comparison simply never accepts it.
--
-- IT COSTS the cycle's read budget -- one for locating plus one per line found
-- -- and does NOT consume the cycle, so the same frame goes on to click. A
-- region holding more lines than the cycle can pay for RAISES rather than
-- returning the first few; the read is not wrapped, for the reason no read here
-- is: a region nobody finished looking at has established nothing about the
-- lines nobody looked at.
--
-- PICKING one of the lines out and AIMING beside it are `hits.matching` and
-- `hits.offset`. Neither costs a capture, which is why neither is here.
function observe.read_lines(ctx: any, ticket: any, elementOrReference: any): { Hit }
    requireCtx(ctx, "observe.read_lines")

    local reference, element = readTarget("observe.read_lines", elementOrReference)
    local rect  = searchRect(reference, element)
    local found =
        ctx:cycle_read_lines(ticket, rect.x, rect.y, rect.width, rect.height)

    -- `located` rather than `hits`, which is the name of a published framework
    -- module: shadowing it inside the one verb whose results that module
    -- operates on is exactly the confusion nobody would enjoy discovering.
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

-- observe.click(ctx, ticket, receipt, hit) -> ()
--
-- Delivers the click the hit describes and spends the observation cycle.
--
-- THIS IS WHERE AUTHORISATION IS ENFORCED. The four guarantees C++ still keeps
-- are that the match came from THIS ticket's own match on THIS frame, that the
-- observation lease is fresh, that the fingerprint is compatible, and that a
-- ticket delivers at most one input. "Only click an annotated element" is no
-- longer among them, because the annotation now lives in Luau -- so the trusted
-- framework enforces it, here, and the bare-point primitive it needs for a
-- page-positioned element is reachable only from this layer
-- (docs/plans/2026-08-01-three-layers-and-agent-operator.md 2).
--
-- THE RECEIPT IS THE FIFTH REQUISITE, AND IT IS THE ONE THAT WENT MISSING. The
-- other half of the old C++ rule was that the page authorising the element had
-- resolved on the SAME observation. `cycle_click` could enforce it because the
-- ledger held the resolution; once the page moved to Luau the ledger stopped
-- knowing what a page is, and `resolve_page` returning a boolean left nothing
-- tying an answer to the frame it was answered about. So a caller now hands
-- over the receipt, and three things are checked against it:
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
function observe.click(ctx: any, ticket: any, receipt: any, hit: any)
    requireCtx(ctx, "observe.click")
    if not evidence.is_receipt(receipt) then
        error(
            "observe.click needs the receipt observe.resolve_page returned for "
                .. "the page this element belongs to; a resolution that failed "
                .. "returns none, so there is nothing to pass and nothing to "
                .. "click on",
            2
        )
    end
    if not rawequal(receipt.ticket, ticket) then
        error(
            "observe.click was given a receipt for page '"
                .. receipt.page.name
                .. "' minted on another observation cycle; a click is "
                .. "authorised by the page THIS frame resolved, and evidence "
                .. "from an earlier frame describes a screen that has moved on",
            2
        )
    end
    if evidence.hit_ticket(hit) == nil then
        error(
            "observe.click takes a hit that observe.find returned on this "
                .. "cycle, and nothing else",
            2
        )
    end
    -- THE SIXTH REQUISITE, and the one a located line made necessary. A hit is
    -- evidence about the frame it was minted on: a template match is a rectangle
    -- that frame produced, and a line is a position that frame read. Neither is
    -- a claim about the next frame. C++ enforces this for a template hit through
    -- the match handle's cycle ordinal and cannot enforce it for a line, which
    -- reaches the host as a bare coordinate with no handle to check -- so it is
    -- enforced here, for every kind of hit alike rather than for the one kind
    -- that would otherwise escape.
    if not rawequal(evidence.hit_ticket(hit), ticket) then
        error(
            "observe.click was given a hit located on another observation "
                .. "cycle; where a thing was on an earlier frame is not where it "
                .. "is on this one, and a click authorised by this frame has to "
                .. "be aimed by it too",
            2
        )
    end
    if receipt.page.name ~= hit.page then
        error(
            "observe.click holds a receipt for page '"
                .. receipt.page.name
                .. "' and a hit page '"
                .. tostring(hit.page)
                .. "' authorised; one element referenced by two pages is "
                .. "clickable on the page that was recognised and on no other",
            2
        )
    end
    if not hit.interact then
        error(
            "this page does not exercise interact on element '"
                .. hit.element.name
                .. "'; finding it is allowed and clicking it is not",
            2
        )
    end

    if hit.match ~= nil then
        ctx:cycle_click(ticket, hit.match)
        return
    end

    -- The bare-point delivery for an element with no template: one verified by
    -- its expected text has a rectangle and no match handle, so the click lands
    -- where the page model says the element is
    -- (docs/plans/2026-08-01-three-layers-and-agent-operator.md 5, decision 4).
    --
    -- IT GOES THROUGH `native` AND NOT THROUGH `ctx`, and that is the whole
    -- confinement. `ctx` is published into every project environment, so a
    -- forward there would be a bare click a business task could call directly,
    -- skipping every check above. The private surface is a closure upvalue of
    -- this chunk and a key of no table either environment can name, so this is
    -- the one route to the primitive in run mode -- and it runs only after the
    -- receipt, the hit and the interact edge have all been checked.
    if native == nil then
        error(
            "observe.click needs the host's private surface to deliver a click "
                .. "at an element with no template; this VM has no bound session",
            2
        )
    end
    native.cycle_click_point(ticket, hit.click_x, hit.click_y)
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

-- One observation, opened and closed whatever `probe` did.
--
-- The observation's own failure outranks a failure from the close, so the caller
-- is told why the work failed rather than being handed the second-order
-- consequence of it. Both waits below go through here, so neither can leave a
-- cycle open on a raise.
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
-- CONSECUTIVE IS THE WHOLE POINT, AND IT REPLACES WAITING FOR A STILL FRAME.
-- The obvious primitive -- wait until the screen stops changing -- cannot work
-- on this target: a resting-point border animates forever, so a whole-frame
-- difference never reaches zero and the verb either deadlocks or lies. Asking
-- instead for K consecutive agreeing observations of a SEMANTIC fact says the
-- same thing about the part of the screen that matters, and says it about
-- something a trace can show
-- (docs/plans/2026-08-01-three-layers-and-agent-operator.md 7).
--
-- A streak is broken by one disagreeing observation, not decremented: two
-- agreements separated by a disagreement are not evidence of a settled screen.
-- Two agreements about DIFFERENT pages are not either, which is why the streak
-- is per-page rather than per-hit -- an edge whose `to` names three pages must
-- not settle by seeing each of them once.
--
-- Returning nil rather than raising is deliberate: `wait_until` turns an
-- exhausted budget into a Tier B timeout and `walk_edge` looks for an interrupt
-- page first, and neither policy belongs in the loop that counts the frames.
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
-- raises `uf.errors.timeout` when the budget runs out.
--
-- The timeout is a HOST-MINTED Tier B error rather than a Luau string, so a
-- project can catch it, a retry policy can name it, and the host classifies it
-- the same way it classifies every other automation failure. It is a control
-- error and never a "no": nothing that ran out of time learned anything about
-- the screen.
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
-- the run believes about the pages underneath.
--
-- IT DOES NOT PATHFIND AND IT DOES NOT CHOOSE. One edge is handed in and one
-- edge is walked. Which edge to walk is strategy and lives in layer three; where
-- an edge leads is fact and lives in the project file.
--
-- A CLICK EDGE RESOLVES THE PAGE IT LEAVES, ON THE FRAME THAT DELIVERS THE
-- TRIGGER. It has to: `observe.click` is authorised by a receipt from THIS
-- ticket, and this verb is the caller. It does not consult the page stack to
-- get that receipt -- the stack is belief and this is evidence -- it resolves
-- `edge.from` on the frame it just captured and walks nothing if that fails.
--
-- The cost of the rule is real and is the intended one: standing on a battle
-- screen with a card-detail overlay open, an edge leaving the battle page can
-- no longer be walked, because the battle page does not resolve while its
-- overlay is up (its forbidden clause fires). The end-turn button underneath is
-- still there and a click would still land, but nothing on that frame proves
-- the page authorising it is the page in front of the run. Reaching it means
-- popping the overlay first, or modelling the overlaid screen as its own page
-- with its own authorisations, which is what
-- docs/plans/2026-07-31-annotation-model-capabilities.md 4-2.6 already says to
-- do when a form changes what may be clicked.
--
-- THE CLICK GOES THROUGH `observe.click`, so the interact authorisation and the
-- receipt are enforced by the one place that enforces them. The key goes
-- through the ctx forward, which spends the cycle the same way a click does; it
-- needs no receipt because a keystroke names no position and no page declares
-- anything about keys. A spontaneous edge does nothing at all and the wait
-- below is the whole of the walk.
--
-- A TIMEOUT LOOKS FOR AN INTERRUPT PAGE BEFORE IT GIVES UP. Popups are declared
-- once, as a flag on the page, rather than as an inbound edge from everywhere
-- they can appear; when the destination never came and one of them is on screen
-- instead, the walk says so and hands the page over. What to do about it --
-- close it, wait it out, end the run -- is the caller's policy, and a framework
-- that dismissed it would be deciding that for every project at once.
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

    -- Both of these read the stack and neither costs a capture, so both live in
    -- `navigation` with the rest of the belief rules. The ORDER is what matters
    -- here: a push whose belief has nowhere to go is refused before its trigger
    -- is delivered, because a click that landed on a screen this run then
    -- refuses to reason about is worse than no click at all.
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
    -- reading of this function honest rather than leaving a path that falls off
    -- the end with no value.
    return table.freeze({ outcome = "interrupted", page = nil })
end

return observe
