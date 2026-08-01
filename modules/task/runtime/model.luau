--!strict
-- model: the trusted framework's element and page vocabulary.
--
-- The page model used to be C++ types under modules/annotation. It is here
-- instead because of one observation repeated three times in a single day of
-- annotating a real game (docs/plans/2026-07-31-script-owned-page-model.md 1):
-- C++ held a fixed opinion about what a page IS, and the game kept not growing
-- that way. Every collision cost a C++ change, and the next game would collide
-- again. So C++ keeps the primitives that carry a GUARANTEE -- pixels for a
-- rectangle, text for a rectangle, a click delivered against a live ticket --
-- and the vocabulary that carries an OPINION lives here, where changing it is
-- a Luau edit.
--
-- WHAT THIS FILE IS NOT. It is not a place any guarantee rests on. The ticket
-- ledger, the observation lease, the fingerprint and the single-consumption
-- rule are all in C++ and are all checked against the TICKET, never against a
-- table this file built (the same document, 5). A layer-three script that
-- corrupts an element table breaks its own run and cannot produce a click at a
-- wrong pixel. Nothing here may ever cache a fingerprint, a lease or a ticket
-- identity to make that faster.
--
-- INHERITANCE. Layer three extends what is built here the way Luau does it:
-- `setmetatable({ ... }, { __index = base })` (the same document, 3). Both
-- constructors below accept an element derived that way, so an extension does
-- not have to re-mint its base; what they refuse is a table nobody minted.
--
-- THE PAGE GRAPH IS NEXT DOOR, IN `navigation`. Edges, the collection that
-- checks them against each other, and the runtime page stack are a second
-- vocabulary built on this one, and they are a file of their own rather than
-- more of this one. See `Page.new` for why the two page FLAGS are here while
-- the edges are not.
--
-- LOAD ORDER. Framework modules load in bundle order, which is the sorted
-- order of their file names, and each module's frozen exports are bound in the
-- framework environment under its own name. `model` sorts after `ctx` and
-- `mint` and before `navigation`, `observe` and `project`, which is what lets
-- those three name it. The only module it names itself is `mint`.

-- Every capability an element can declare, in the one canonical order the
-- project file writes them in. Serialising a set needs an order, and taking it
-- from a constant rather than from a table walk is what keeps two runs over the
-- same model producing the same bytes.
local k_capabilityOrder = { "identify", "interact", "read" }

local k_capabilityKnown: { [string]: boolean } = {
    identify = true,
    interact = true,
    read     = true,
}

local k_holdingKnown: { [string]: boolean } = {
    owned      = true,
    referenced = true,
}

local k_polarityKnown: { [string]: boolean } = {
    required  = true,
    forbidden = true,
}

-- The keys each constructor owns. Anything else in a spec table is refused and
-- the author is pointed at `extra`, which is the reserved subtable a project
-- may put its own fields under (docs/plans/2026-07-31-script-owned-page-model.md
-- 8). The refusal is the whole reason `extra` works: this layer may add a key
-- next year, and a project that had quietly put its own key of that name at the
-- top level would silently lose it.
local k_elementKeys: { [string]: boolean } = {
    name          = true,
    capabilities  = true,
    rect          = true,
    appearances   = true,
    expected_text = true,
    extra         = true,
    residual      = true,
}

local k_appearanceKeys: { [string]: boolean } = {
    name      = true,
    source    = true,
    template  = true,
    threshold = true,
    residual  = true,
}

local k_pageKeys: { [string]: boolean } = {
    name       = true,
    overlay    = true,
    interrupt  = true,
    references = true,
    extra      = true,
    residual   = true,
}

local k_referenceKeys: { [string]: boolean } = {
    element           = true,
    holding           = true,
    exercised         = true,
    identify          = true,
    pinned_appearance = true,
    rect_override     = true,
    extra             = true,
    residual          = true,
}

-- A rectangle in target pixels. Four numbers rather than a handle, because a
-- rectangle is authored data and never a host-minted object.
export type Rect = {
    x: number,
    y: number,
    width: number,
    height: number,
}

-- The three capabilities, as a set rather than a list. A set is what every
-- reader of it actually asks -- "does this element identify?" -- and it is what
-- makes `exercised` a subset check rather than a nested loop. The FILE form is
-- a list, and `k_capabilityOrder` is the bridge between the two.
export type CapabilitySet = {
    identify: boolean,
    interact: boolean,
    read: boolean,
}

-- One way a single element can look. An appearance changes what a rectangle
-- LOOKS LIKE; it never changes where the rectangle is or what it means
-- (docs/plans/2026-07-31-annotation-model-capabilities.md 4-2.4). A geometry
-- change is not an appearance and does not belong in this model at all.
--
-- `template` is the opaque handle `ctx:template_load` minted; `source` is the
-- project file the blob came from. Both are required, and that is deliberate:
-- an appearance holding only a handle cannot be written back to the project
-- file, so a model loaded and saved would silently lose its own pixels.
--
-- `threshold` is in basis points of similarity, so 10000 demands an exact
-- match. Judging a score against it is `observe.matched`, in the module that
-- owns the verbs; a threshold on its own decides nothing.
export type Appearance = {
    name: string?,
    source: string,
    template: any,
    threshold: number,
    residual: { string },
}

export type Element = {
    name: string,
    capabilities: CapabilitySet,
    rect: Rect,
    appearances: { Appearance },
    expected_text: string?,
    extra: { [string]: any },
    residual: { string },
}

-- What one page does with one element. The holding relation and the exercised
-- capabilities live HERE and not on the element, because "which page owns these
-- pixels" and "what does THIS page use them for" are properties of the pairing
-- (docs/plans/2026-07-31-annotation-model-capabilities.md 2.2).
--
-- `identify` is the polarity, present exactly when identify is exercised:
-- required means the page needs this element on screen, forbidden means the
-- page is not this page if the element is there.
--
-- `pinned_appearance` names the appearance this page's pixels actually wear.
-- It applies to INTERACT ONLY. Identify folds across every appearance no matter
-- what is pinned, because the identify sweep runs before the page is known and
-- therefore cannot consult a page's pin (the same document, 4-2.4 correction).
-- The constructor accepts a pin beside an exercised identify and documents that
-- identify ignores it, rather than refusing, so an element that is both a page
-- anchor and a click target can still pin the appearance its click needs.
--
-- `page` is the NAME of the page that declared this row. It is not a field an
-- author writes -- `Page.new` stamps it, and the file already spells it as the
-- `[[reference]]` section's own `page` key -- and it exists so a row can answer
-- "which page authorised me" on its own. `observe.click` is what needs that
-- answer: it holds a receipt proving one page resolved on this frame, and it
-- has to check that the hit in front of it belongs to THAT page rather than to
-- some other page that happens to reference the same element.
export type Reference = {
    element: Element,
    page: string,
    holding: string,
    exercised: CapabilitySet,
    identify: string?,
    pinned_appearance: string?,
    rect_override: Rect?,
    extra: { [string]: any },
    residual: { string },
}

-- `overlay` says this page COVERS the screen it appeared over instead of
-- replacing it, which is the fact the runtime stack is built on: closing an
-- overlay returns you to where you were, and that is a stack rather than an edge
-- (docs/plans/2026-07-31-script-owned-page-model.md 10). It is a property of the
-- page and not of the edge that reaches it, because the same overlay opened from
-- three screens covers all three.
--
-- `interrupt` says this page can appear over ANYTHING -- a network error box, a
-- daily-reward popup -- so it is declared once here rather than as an inbound
-- edge from every page it can interrupt. `observe.walk_edge` consults the flag
-- when a walk runs out of budget; what to do about the page it found is the
-- caller's policy and never this layer's.
--
-- Both default to false and both are refinements: an ordinary page states
-- neither. That is not a hidden policy default -- it is the reading of an absent
-- fact, the same way an element with no `expected_text` is an element with no
-- expected text.
export type Page = {
    name: string,
    overlay: boolean,
    interrupt: boolean,
    references: { Reference },
    extra: { [string]: any },
    residual: { string },
}

-- The tables this module minted, held by identity, exactly as the task
-- declaration surface holds its interrupt descriptors. It is what makes
-- `Page.new` refuse a hand-written table that merely looks like an element:
-- not because a forged one would confer anything -- authorisation is checked
-- against the reference row at click time, and the click itself is checked
-- against the ticket in C++ -- but because the failure mode of accepting one is
-- SILENCE. A misspelled `capabilities` yields an element that exercises
-- nothing, and a page that never authorises a click looks exactly like a game
-- that never reached that page.
local mintedElements: { [any]: boolean } = {}
local mintedPages: { [any]: boolean } = {}
local mintedReferences: { [any]: boolean } = {}

local model = {}

-- The bounded inheritance walk is `mint`'s, so `navigation` asks the same
-- question of its own registries through the same code rather than a second copy
-- of it. The registries above stay private and are reachable only as the
-- PREDICATES below, because a script that could write to one could mint
-- anything.
local mintedOrDerived = mint.derives_from

function model.is_element(value: any): boolean
    return mintedOrDerived(value, mintedElements)
end

function model.is_page(value: any): boolean
    return mintedOrDerived(value, mintedPages)
end

-- References are minted too, so a verb handed one directly can tell a real page
-- row from a table shaped like one. There is no Reference.new: a reference only
-- exists as part of the page that declared it, which is the whole point of
-- putting holding and exercised on the pairing.
function model.is_reference(value: any): boolean
    return mintedOrDerived(value, mintedReferences)
end

-- The capability names in canonical order, for a caller that has to serialise a
-- set. A fresh copy each call, so nothing outside can reorder the constant.
function model.capability_order(): { string }
    return table.clone(k_capabilityOrder)
end

-- Every validator below returns a SENTENCE or nil rather than raising, so the
-- constructor can raise once, at level 2, and the author reads their own call
-- site instead of a line of this file. The sentences say what is wrong in plain
-- language and name the field, because a model is authored by an agent reading
-- the message and trying again. The four every constructor in this framework
-- repeats -- unknown keys, extra, residual, and the mint walk -- are in `mint`,
-- because `navigation` asks them of its own spec tables too.
local wholeNumber      = mint.whole_number
local checkExtra       = mint.check_extra
local frozenExtra      = mint.frozen_extra
local checkResidual    = mint.check_residual
local frozenResidual   = mint.frozen_residual
local checkUnknownKeys = mint.unknown_key

local function checkRect(what: string, value: any): string?
    if type(value) ~= "table" then
        return what .. " must be a table { x = , y = , width = , height = }"
    end
    for _, key in { "x", "y", "width", "height" } do
        if not wholeNumber((value :: any)[key]) then
            return what .. "." .. key .. " must be a whole number of pixels"
        end
    end
    local rect = value :: Rect
    if rect.x < 0 or rect.y < 0 then
        return what .. " cannot start at a negative pixel"
    end
    if rect.width < 1 or rect.height < 1 then
        return what .. " must be at least one pixel wide and one pixel tall"
    end
    return nil
end

local function frozenRect(value: Rect): Rect
    return table.freeze({
        x      = value.x,
        y      = value.y,
        width  = value.width,
        height = value.height,
    })
end

-- Turns the file's list form into the set form everything else reads. Returns
-- the set, or nil and a sentence.
local function capabilitySet(
    what: string,
    value: any
): (CapabilitySet?, string?)
    if type(value) ~= "table" then
        return nil, what .. ' must be a list of capability names, such as { "identify" }'
    end
    local list = value :: { any }
    if #list == 0 then
        return nil,
            what
                .. " must name at least one capability; an element nothing can "
                .. "identify, click or read is not a temporarily unused element, "
                .. "it is an entry nobody can explain"
    end

    local set   = { identify = false, interact = false, read = false }
    local flags = set :: any
    for index = 1, #list do
        local name = list[index]
        if type(name) ~= "string" or not k_capabilityKnown[name] then
            return nil,
                what
                    .. " entry "
                    .. tostring(index)
                    .. ' is not a capability; the three are "identify", '
                    .. '"interact" and "read"'
        end
        if flags[name] then
            return nil, what .. ' names "' .. name .. '" twice'
        end
        flags[name] = true
    end
    return table.freeze(set), nil
end

local Element = {}
model.Element = Element

-- Element.new{ ... } -> a frozen element.
--
-- `capabilities` is a LIST of names on the way in and a SET on the way out, and
-- that asymmetry is on purpose: the list is the file's form and the set is what
-- every reader asks. `element.capabilities.identify` is the reading.
--
-- Two invariants here are not shape checks but rulings, and both fail closed:
--
--   * An element with no appearances is positioned BY THE PAGE, not by its own
--     pixels, so it cannot be identity evidence and identify is refused
--     (docs/plans/2026-07-31-annotation-model-capabilities.md 4-2.2). This is
--     the one rule that covers both a read-only region and a click target whose
--     content is decided at runtime.
--   * An element declares ONE verification source: template appearances, or
--     expected text (docs/plans/2026-08-01-three-layers-and-agent-operator.md
--     5-4). Both at once is not a stronger element, it is two answers to "did I
--     find it" with nothing saying which one wins.
function Element.new(spec: { [string]: any }): Element
    if type(spec) ~= "table" then
        error("Element.new needs a table of fields", 2)
    end

    local unknown = checkUnknownKeys("Element.new", spec, k_elementKeys, true)
    if unknown ~= nil then
        error(unknown, 2)
    end

    if type(spec.name) ~= "string" or spec.name == "" then
        error("an element needs a non-empty name", 2)
    end
    local name = spec.name :: string

    local capabilities, capabilityProblem =
        capabilitySet("element '" .. name .. "' capabilities", spec.capabilities)
    if capabilities == nil then
        error(capabilityProblem :: string, 2)
    end

    local rectProblem = checkRect("element '" .. name .. "' rect", spec.rect)
    if rectProblem ~= nil then
        error(rectProblem, 2)
    end

    local appearances: { Appearance } = {}
    local seenAppearance: { [string]: boolean } = {}
    if spec.appearances ~= nil then
        if type(spec.appearances) ~= "table" then
            error("element '" .. name .. "' appearances must be a list", 2)
        end
        local declared = spec.appearances :: { any }
        for index = 1, #declared do
            local entry = declared[index]
            local what  = "element '"
                .. name
                .. "' appearance "
                .. tostring(index)
            if type(entry) ~= "table" then
                error(what .. " must be a table", 2)
            end

            local entryUnknown =
                checkUnknownKeys(what, entry, k_appearanceKeys, false)
            if entryUnknown ~= nil then
                error(entryUnknown, 2)
            end
            if entry.name ~= nil then
                if type(entry.name) ~= "string" or entry.name == "" then
                    error(what .. " has a name that is not a non-empty string", 2)
                end
                if seenAppearance[entry.name] then
                    error(
                        what
                            .. ' repeats the appearance name "'
                            .. entry.name
                            .. "\"; a script tells appearances apart by name, so "
                            .. "two of them cannot share one",
                        2
                    )
                end
                seenAppearance[entry.name] = true
            end
            if type(entry.source) ~= "string" or entry.source == "" then
                error(
                    what
                        .. " needs source = the project file its template blob "
                        .. "came from; an appearance that cannot be written back "
                        .. "would vanish the next time the model is saved",
                    2
                )
            end
            -- The handle, not the blob. Both are values an author has in hand at
            -- this point and only one of them can be matched against, and the
            -- wrong one would otherwise be found out at the first search rather
            -- than at the line that chose it.
            if typeof(entry.template) ~= "userdata" then
                error(
                    what
                        .. " needs template = the handle ctx:template_load "
                        .. "returned for its blob, not the blob itself",
                    2
                )
            end
            if
                not wholeNumber(entry.threshold)
                or entry.threshold < 0
                or entry.threshold > 10000
            then
                error(
                    what
                        .. " needs threshold = a whole number of basis points "
                        .. "between 0 and 10000, where 10000 demands an exact "
                        .. "match",
                    2
                )
            end
            local residualProblem = checkResidual(what, entry.residual)
            if residualProblem ~= nil then
                error(residualProblem, 2)
            end

            appearances[index] = table.freeze({
                name      = entry.name,
                source    = entry.source,
                template  = entry.template,
                threshold = entry.threshold,
                residual  = frozenResidual(entry.residual),
            })
        end
    end

    if spec.expected_text ~= nil then
        if type(spec.expected_text) ~= "string" or spec.expected_text == "" then
            error(
                "element '"
                    .. name
                    .. "' expected_text must be the text the region should read",
                2
            )
        end
        if #appearances > 0 then
            error(
                "element '"
                    .. name
                    .. "' declares both appearances and expected_text; an "
                    .. "element verifies itself EITHER by its template pixels OR "
                    .. "by the text it reads, and two answers with no rule "
                    .. "between them is not a stronger check",
                2
            )
        end
        if not (capabilities :: CapabilitySet).read then
            error(
                "element '"
                    .. name
                    .. "' has expected_text but does not declare the read "
                    .. 'capability; add "read" or drop the expected text',
                2
            )
        end
    end

    if #appearances == 0 and (capabilities :: CapabilitySet).identify then
        error(
            "element '"
                .. name
                .. "' declares identify with no appearances; an element with no "
                .. "template of its own is positioned BY the page that was "
                .. "recognised, so it has no pixels to offer as evidence that "
                .. "the page IS that page",
            2
        )
    end

    local extraProblem = checkExtra("element '" .. name .. "'", spec.extra)
    if extraProblem ~= nil then
        error(extraProblem, 2)
    end
    local residualProblem =
        checkResidual("element '" .. name .. "'", spec.residual)
    if residualProblem ~= nil then
        error(residualProblem, 2)
    end

    local element = table.freeze({
        name          = name,
        capabilities  = capabilities :: CapabilitySet,
        rect          = frozenRect(spec.rect :: Rect),
        appearances   = table.freeze(appearances),
        expected_text = spec.expected_text,
        extra         = frozenExtra(spec.extra),
        residual      = frozenResidual(spec.residual),
    })
    mintedElements[element] = true
    return element
end

-- The appearance on `element` carrying `name`, or nil. Declaration order is
-- preserved, so the index this walks is the order the model file wrote.
function Element.appearance_named(element: Element, name: string): Appearance?
    for index = 1, #element.appearances do
        local appearance = element.appearances[index]
        if appearance.name == name then
            return appearance
        end
    end
    return nil
end

local Page = {}
model.Page = Page

-- THE TRANSITION GRAPH LANDED BESIDE THIS TABLE RATHER THAN INSIDE IT.
--
-- A page is still a set of references plus two flags: what has to be on screen
-- for this to be that page, what this page lets a script do, and whether it
-- covers the screen or can appear over anything. It carries no outbound edge and
-- no target page.
--
-- That is the shape the 2026-08-01 ruling settled on
-- (docs/plans/2026-07-31-script-owned-page-model.md 10). Edges are their own
-- objects collected by `Graph.new`, for a reason worth stating where a reader
-- looking for `page.edges` will find it: an edge is a fact about a PAIR of pages
-- and a trigger, so hanging it off one end makes the other end's model depend on
-- the order the two were built in, and a `to` set spanning three pages would
-- have no single owner at all.
--
-- The two flags ARE on the page, and by the same test: "this screen covers the
-- one underneath" and "this screen can appear over anything" are properties of
-- the page itself, true of every way of reaching it.
--
-- Page.new{ ... } -> a frozen page.
function Page.new(spec: { [string]: any }): Page
    if type(spec) ~= "table" then
        error("Page.new needs a table of fields", 2)
    end

    local unknown = checkUnknownKeys("Page.new", spec, k_pageKeys, true)
    if unknown ~= nil then
        error(unknown, 2)
    end

    if type(spec.name) ~= "string" or spec.name == "" then
        error("a page needs a non-empty name", 2)
    end
    local name = spec.name :: string

    if spec.overlay ~= nil and type(spec.overlay) ~= "boolean" then
        error(
            "page '"
                .. name
                .. "' overlay must be true or false; it says whether this page "
                .. "COVERS the screen it appeared over instead of replacing it, "
                .. "and a run's page stack is built entirely on that answer",
            2
        )
    end
    if spec.interrupt ~= nil and type(spec.interrupt) ~= "boolean" then
        error(
            "page '"
                .. name
                .. "' interrupt must be true or false; it says whether this page "
                .. "can appear over anything, which is declared once here instead "
                .. "of as an inbound edge from every page it can interrupt",
            2
        )
    end

    if type(spec.references) ~= "table" then
        error("page '" .. name .. "' needs references = { ... }", 2)
    end
    local declared = spec.references :: { any }
    if #declared == 0 then
        error(
            "page '"
                .. name
                .. "' has no references; a page is defined by what it shows and "
                .. "what it lets a script touch, so an empty one names nothing",
            2
        )
    end

    local references: { Reference } = {}
    local seenElement: { [string]: boolean } = {}
    local requiredCount = 0

    for index = 1, #declared do
        local entry = declared[index]
        local what  = "page '" .. name .. "' reference " .. tostring(index)
        if type(entry) ~= "table" then
            error(what .. " must be a table", 2)
        end

        local entryUnknown =
            checkUnknownKeys(what, entry, k_referenceKeys, true)
        if entryUnknown ~= nil then
            error(entryUnknown, 2)
        end

        if not mintedOrDerived(entry.element, mintedElements) then
            error(
                what
                    .. " needs element = an element built by Element.new (or one "
                    .. "derived from it with setmetatable); a look-alike table "
                    .. "would fail by matching nothing rather than by failing",
                2
            )
        end
        local element = entry.element :: Element
        if seenElement[element.name] then
            error(
                what
                    .. " is the second reference to element '"
                    .. element.name
                    .. "' on this page; one page uses one element in one way",
                2
            )
        end
        seenElement[element.name] = true

        if type(entry.holding) ~= "string" or not k_holdingKnown[entry.holding] then
            error(
                what
                    .. ' needs holding = "owned" or "referenced"; owned means '
                    .. "this page is where the element lives, referenced means it "
                    .. "lives on another page",
                2
            )
        end

        local exercised, exercisedProblem =
            capabilitySet(what .. " exercised", entry.exercised)
        if exercised == nil then
            error(exercisedProblem :: string, 2)
        end
        for _, capability in k_capabilityOrder do
            if
                (exercised :: any)[capability]
                and not (element.capabilities :: any)[capability]
            then
                error(
                    what
                        .. ' exercises "'
                        .. capability
                        .. "\" but element '"
                        .. element.name
                        .. "' does not declare it; a page uses a subset of what "
                        .. "an element can do, never more",
                    2
                )
            end
        end

        if (exercised :: CapabilitySet).identify then
            if
                type(entry.identify) ~= "string"
                or not k_polarityKnown[entry.identify]
            then
                error(
                    what
                        .. ' exercises identify, so it needs identify = "required"'
                        .. ' or "forbidden"; a signature clause with no polarity '
                        .. "says nothing",
                    2
                )
            end
            if entry.identify == "required" then
                requiredCount += 1
            end
            if entry.rect_override ~= nil then
                error(
                    what
                        .. " exercises identify and also refines the search "
                        .. "rectangle; an identify sweep runs before the page is "
                        .. "known, so it uses the element's own rectangle, and a "
                        .. "per-page refinement here would only make the same "
                        .. "pixels get searched twice",
                    2
                )
            end
        elseif entry.identify ~= nil then
            error(
                what
                    .. " sets a polarity without exercising identify; required "
                    .. "and forbidden describe a page signature, and this "
                    .. "reference is not part of one",
                2
            )
        end

        if entry.pinned_appearance ~= nil then
            if type(entry.pinned_appearance) ~= "string" then
                error(what .. " pinned_appearance must be an appearance name", 2)
            end
            if not (exercised :: CapabilitySet).interact then
                error(
                    what
                        .. " pins an appearance without exercising interact; "
                        .. "pinning is what spares a CLICK from searching every "
                        .. "appearance, and identify folds across all of them "
                        .. "whatever is pinned",
                    2
                )
            end
            if Element.appearance_named(element, entry.pinned_appearance) == nil then
                error(
                    what
                        .. ' pins appearance "'
                        .. entry.pinned_appearance
                        .. "\" which element '"
                        .. element.name
                        .. "' does not have",
                    2
                )
            end
        end

        if entry.rect_override ~= nil then
            local overrideProblem =
                checkRect(what .. " rect_override", entry.rect_override)
            if overrideProblem ~= nil then
                error(overrideProblem, 2)
            end
        end

        local extraProblem = checkExtra(what, entry.extra)
        if extraProblem ~= nil then
            error(extraProblem, 2)
        end
        local residualProblem = checkResidual(what, entry.residual)
        if residualProblem ~= nil then
            error(residualProblem, 2)
        end

        local reference = table.freeze({
            element           = element,
            page              = name,
            holding           = entry.holding,
            exercised         = exercised :: CapabilitySet,
            identify          = entry.identify,
            pinned_appearance = entry.pinned_appearance,
            rect_override     = if entry.rect_override ~= nil
                then frozenRect(entry.rect_override :: Rect)
                else nil,
            extra             = frozenExtra(entry.extra),
            residual          = frozenResidual(entry.residual),
        })
        mintedReferences[reference] = true
        references[index] = reference
    end

    if requiredCount == 0 then
        error(
            "page '"
                .. name
                .. "' has no required identify reference; without one, every "
                .. "screen resolves to this page, including the ones that are "
                .. "not it",
            2
        )
    end

    local extraProblem = checkExtra("page '" .. name .. "'", spec.extra)
    if extraProblem ~= nil then
        error(extraProblem, 2)
    end
    local residualProblem = checkResidual("page '" .. name .. "'", spec.residual)
    if residualProblem ~= nil then
        error(residualProblem, 2)
    end

    local page = table.freeze({
        name       = name,
        overlay    = spec.overlay == true,
        interrupt  = spec.interrupt == true,
        references = table.freeze(references),
        extra      = frozenExtra(spec.extra),
        residual   = frozenResidual(spec.residual),
    })
    mintedPages[page] = true
    return page
end

-- This page's reference to `element`, or nil when the page does not reference
-- it at all. Comparison is by element NAME rather than by table identity, so a
-- layer-three script that derived its own element from the one the project file
-- built still finds its row.
function Page.reference_for(page: Page, element: Element): Reference?
    for index = 1, #page.references do
        local reference = page.references[index]
        if reference.element.name == element.name then
            return reference
        end
    end
    return nil
end

return model
