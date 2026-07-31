--!strict
-- observe: the verbs that turn a model plus one observation into a fact or an
-- action.
--
-- `model` owns the nouns; this file owns everything that costs a capture. Both
-- halves used to be C++ verbs -- `cycle_page` and `cycle_find` -- and both are
-- gone from the host surface: they are rebuilt here on `cycle_match`,
-- `cycle_read` and `cycle_click`, which is what
-- docs/plans/2026-07-31-script-owned-page-model.md 4 replaced them with.
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

-- The hits this module minted. `observe.click` refuses anything else, which is
-- what makes the authorisation carried on a hit unforgeable from layer three:
-- a script cannot hand `click` a table claiming interact on an element the page
-- never authorised. C++ still checks the ticket, the lease and the fingerprint
-- underneath; what lives here is the rule those four cannot express any more
-- now that pages are Luau
-- (docs/plans/2026-08-01-three-layers-and-agent-operator.md 2).
local mintedHits: { [any]: boolean } = {}

local observe = {}

export type Hit = {
    element: any,
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

function observe.exact_text(actual: string, expected: string): boolean
    return actual == expected
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

-- observe.resolve_page(ctx, ticket, page) -> (boolean, string?)
--
-- Whether the frame behind `ticket` is this page, and when it is not, the one
-- sentence saying why. Only references exercising identify are consulted; the
-- rest of the page describes what a script may DO once it is here.
--
-- The signature is strictly conjunctive: every required reference must match
-- and every forbidden one must not. Evaluation stops at the first clause that
-- fails, so the cost of rejecting a page is one search rather than all of them.
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
function observe.resolve_page(ctx: any, ticket: any, page: any): (boolean, string?)
    requireCtx(ctx, "observe.resolve_page")
    if not model.is_page(page) then
        error("observe.resolve_page needs a page built by Page.new", 2)
    end

    for index = 1, #page.references do
        local reference = page.references[index]
        if reference.exercised.identify then
            local element = reference.element
            local appearance, found = foldAppearances(
                ctx,
                ticket,
                element,
                element.rect,
                nil
            )
            if reference.identify == "required" and found == nil then
                return false,
                    "page '"
                        .. page.name
                        .. "' requires element '"
                        .. element.name
                        .. "' and none of its appearances matched"
            end
            if reference.identify == "forbidden" and found ~= nil then
                return false,
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

    return true, nil
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
-- caller names the page, so the ordering requirement dissolves into an ordinary
-- script convention -- call `resolve_page` first, and believe its answer. The
-- convention still matters most for a page-positioned element, whose whole claim
-- to a position is that the page was recognised.
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
        local hit = table.freeze({
            element       = target,
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
        })
        mintedHits[hit] = true
        return hit
    end

    -- The pin is consulted only for a reference that exercises interact, which
    -- Page.new already guarantees is the only place a pin may appear.
    local appearance, found =
        foldAppearances(ctx, ticket, target, rect, reference.pinned_appearance)
    if found == nil then
        return nil
    end

    local hit = table.freeze({
        element       = target,
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
    })
    mintedHits[hit] = true
    return hit
end

-- observe.read_element(ctx, ticket, element, compare) -> Reading?
--
-- Reads the element's rectangle and, when the element declared expected text,
-- says whether the reading counts as that text.
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
    element: any,
    compare: any
): Reading?
    requireCtx(ctx, "observe.read_element")
    if not model.is_element(element) then
        error("observe.read_element needs an element built by Element.new", 2)
    end
    if not element.capabilities.read then
        error(
            "element '"
                .. element.name
                .. "' does not declare the read capability, so nothing may ask "
                .. "it for text",
            2
        )
    end
    if element.expected_text ~= nil and type(compare) ~= "function" then
        error(
            "reading element '"
                .. element.name
                .. "' needs a comparison function, because it declares expected "
                .. "text; pass observe.exact_text for equality",
            2
        )
    end

    local rect = element.rect
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
        expected   = element.expected_text,
        matches    = if element.expected_text ~= nil
            then compare(reading.text, element.expected_text) == true
            else nil,
    })
end

-- observe.click(ctx, ticket, hit) -> ()
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
function observe.click(ctx: any, ticket: any, hit: any)
    requireCtx(ctx, "observe.click")
    if not mintedHits[hit] then
        error(
            "observe.click takes a hit that observe.find returned on this "
                .. "cycle, and nothing else",
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
    ctx:cycle_click_point(ticket, hit.click_x, hit.click_y)
end

local function requireWholeAtLeast(
    options: any,
    key: string,
    floor: number,
    what: string
): number
    local value = options[key]
    if type(value) ~= "number" or value % 1 ~= 0 or value < floor then
        error(
            "observe.wait_until needs "
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
    local resolved = observe.resolve_page(ctx, ticket, page)
    if not resolved then
        return false
    end
    if element == nil then
        return true
    end
    return observe.find(ctx, ticket, page, element) ~= nil
end

-- observe.wait_until(ctx, options) -> true
--
-- Observes until the target holds on `consecutive` observations in a row, or
-- raises `uf.errors.timeout` when the budget runs out.
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

    local consecutive =
        requireWholeAtLeast(options, "consecutive", 1, "a whole number of agreeing observations, at least one")
    local timeoutMillis =
        requireWholeAtLeast(options, "timeout_ms", 0, "a whole number of milliseconds")
    local intervalMillis =
        requireWholeAtLeast(options, "interval_ms", 0, "a whole number of milliseconds")

    local deadline = ctx:deadline(timeoutMillis)
    local streak   = 0

    while true do
        local ticket = ctx:cycle_open()
        local ok, held           = pcall(holdsNow, ctx, ticket, options.page, options.element)
        local closed, closeError = pcall(ctx.cycle_close, ctx, ticket)

        -- The observation's own failure outranks a failure from the close, so
        -- the caller is told why the work failed rather than being handed the
        -- second-order consequence of it.
        if not ok then
            error(held, 0)
        end
        if not closed then
            error(closeError, 0)
        end

        if held then
            streak += 1
            if streak >= consecutive then
                return true
            end
        else
            streak = 0
        end

        if not ctx:wait(deadline, intervalMillis) then
            native.raise(
                "timeout",
                "waiting for page '"
                    .. options.page.name
                    .. "' to hold "
                    .. tostring(consecutive)
                    .. " observations in a row ran out of budget"
            )
        end
    end
end

return observe
