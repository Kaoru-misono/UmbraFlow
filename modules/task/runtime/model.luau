--!strict
-- model: the trusted framework's element and page vocabulary.
--
-- C++ keeps the primitives that carry a GUARANTEE -- pixels for a rectangle,
-- text for a rectangle, a click delivered against a live ticket -- and the
-- vocabulary that carries an OPINION lives here, where changing it for the next
-- game is a Luau edit (docs/plans/2026-07-31-script-owned-page-model.md 1).
--
-- No guarantee rests on this file: the ledger, the lease, the fingerprint and
-- the single-consumption rule are C++'s and are checked against the TICKET
-- (docs/plans/2026-07-31-script-owned-page-model.md 5), so nothing here may
-- cache one.
--
-- Layer three extends what is built here with
-- `setmetatable({ ... }, { __index = base })` (the same document, 3), and both
-- constructors accept an element derived that way; what they refuse is a table
-- nobody minted.
--
-- The page graph is next door in `navigation`: an edge is a fact about a PAIR of
-- pages, so it has no single owner here. See `Page.new` for why a page's own
-- flags are here while the edges are not.
--
-- Framework modules load in the sorted order of their file names, and each
-- module's frozen exports are bound in the framework environment under its own
-- name. `model` sorts after `ctx` and `mint` and before `navigation`, `observe`
-- and `project`. The only module it names is `mint`.

-- The private capability surface, for the one number this file cannot decide;
-- see `k_maxPageNameBytes`. No primitive is called here: a page is authored
-- data, so nothing in this file observes or acts.
local native = ...

-- How long a page name may be, in bytes, taken from the host rather than
-- spelled here so it cannot drift from the ceiling that forces it
-- (`k_maxScopeLabelBytes`, modules/trace/source/trace/stream-validator.hpp).
-- Nil on a VM booted with no bound task session, which has no trace stream and
-- so no label to bound.
local k_maxPageNameBytes: number? = if native ~= nil
    then native.max_label_bytes
    else nil

-- Every capability an element can declare, in the one canonical order the
-- project file writes them in. Serialising a set needs an order, and a constant
-- rather than a table walk is what makes two runs produce the same bytes.
local k_capabilityOrder = { "identify", "interact", "read" }

local k_capabilityKnown: { [string]: boolean } = {
    identify = true,
    interact = true,
    read     = true,
}

-- The three shapes an element can be. It is stated and never inferred: an
-- element with no rectangle could be one a project meant to place from the row,
-- or one whose rectangle somebody forgot, and nothing downstream can tell those
-- apart from the absence alone.
local k_formKnown: { [string]: boolean } = {
    -- A rectangle and evidence: everything the model held before the forms
    -- existed, and what almost every element still is.
    fixed = true,

    -- NO rectangle of its own: where to look comes from the row that uses it or
    -- from the claim that measures it. What a minimap cell is -- the map pans,
    -- so where the cell is depends on the frame -- and equally what a confirm
    -- button drawn once and reused on nine screens is. Under this form writing a
    -- rectangle is refused rather than merely pointless, which is the whole
    -- reason to name the form: an absent rectangle could be this or a mistake,
    -- and only the form can say which.
    shape = true,

    -- A container rectangle and the spacing that divides what is inside it into
    -- items. See `Element.item_spacing`.
    strip = true,
}

local k_holdingKnown: { [string]: boolean } = {
    owned      = true,
    referenced = true,
}

local k_polarityKnown: { [string]: boolean } = {
    required  = true,
    forbidden = true,
}

-- How sure the engine has to be, in basis points, before a reading of a region
-- counts as evidence about it -- for an element that does not say.
--
-- There is a floor because reading is the one capability that fails OPEN: a
-- template search on the wrong screen returns nothing, while a text read on the
-- wrong screen returns plausible text. So identity by reading asks two questions,
-- and "was the engine reading rather than guessing" needs a number.
--
-- 8000 because the engine this project ships reads the target's own page titles
-- at 9600 basis points and above. An element may raise it where the glyphs are
-- small or lower it where the font is hostile; an absent `read_floor` reads as
-- this number rather than as zero, so the question is never left unasked.
local k_defaultReadFloor = 8000

-- The widest colour-key tolerance that means anything: a key's distance is the
-- sum of the three channel distances, so 765 already admits every colour and the
-- host refuses anything above it. A mirror of `k_maximumColourKeyTolerance` in
-- modules/vision/source/vision/frame-analysis.hpp rather than a second opinion,
-- since 255 * 3 is arithmetic and not policy.
local k_maximumColourTolerance = 765

-- The keys each constructor owns. Anything else in a spec table is refused and
-- the author is pointed at `extra`, the reserved subtable a project puts its own
-- fields under (docs/plans/2026-07-31-script-owned-page-model.md 8); see
-- `mint.unknown_key` for why the refusal is what makes `extra` work.
local k_elementKeys: { [string]: boolean } = {
    name          = true,
    form          = true,
    capabilities  = true,
    rect          = true,
    appearances   = true,
    expected_text = true,
    expected_fragments = true,
    expected_presence  = true,
    read_floor    = true,
    item_spacing  = true,
    extra         = true,
    residual      = true,
}

local k_appearanceKeys: { [string]: boolean } = {
    name      = true,
    source    = true,
    template  = true,
    threshold = true,
    key       = true,
    residual  = true,
}

local k_pageKeys: { [string]: boolean } = {
    name       = true,
    overlay    = true,
    interrupt  = true,
    state      = true,
    over       = true,
    catch_all  = true,
    references = true,
    extra      = true,
    residual   = true,
}

local k_referenceKeys: { [string]: boolean } = {
    element           = true,
    holding           = true,
    exercised         = true,
    identify          = true,
    expected_text     = true,
    expected_fragments = true,
    expected_presence  = true,
    pinned_appearance = true,
    interact_requires = true,
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

-- The three capabilities as a set, which is what every reader asks and what
-- makes `exercised` a subset check rather than a nested loop. The FILE form is a
-- list, and `k_capabilityOrder` is the bridge between the two.
export type CapabilitySet = {
    identify: boolean,
    interact: boolean,
    read: boolean,
}

-- One way a single element can look. An appearance changes what a rectangle
-- LOOKS LIKE; it never changes where the rectangle is or what it means
-- (docs/plans/2026-07-31-annotation-model-capabilities.md 4-2.4), so a geometry
-- change is not an appearance.
--
-- `template` is the opaque handle `ctx:template_load` minted and `source` is the
-- project file the blob came from. Both are required: an appearance holding only
-- a handle cannot be written back, so a model loaded and saved would silently
-- lose its own pixels.
--
-- `threshold` is in basis points of similarity, so 10000 demands an exact match.
-- Judging a score against it is `observe.matched`.
--
-- `key` is the colour key the template blob was CUT under: a record rather than
-- an instruction, since the mask already lives in the PNG's alpha channel and
-- nothing reads this field to match. It is written down because a masked
-- template whose key is unrecorded cannot be re-cut on a newer capture without
-- guessing. Absent on every unkeyed appearance.
export type Appearance = {
    name: string?,
    source: string,
    template: any,
    threshold: number,
    key: ColourKey?,
    residual: { string },
}

-- A colour key: the RGB triple a mask was cut around, and how far a pixel may
-- sit from it and still be taken. The tolerance is carried rather than defaulted,
-- because a file that omitted it is a file whose mask cannot be reproduced.
--
-- `removes` inverts what the triple names: false, the mask KEPT the pixels near
-- it, and true, the mask kept everything else. A glyph is one colour and is
-- selected directly; an emblem of a dozen hues over a flat backdrop is selected
-- only by naming the backdrop, and the same one-colour rule does both.
--
-- It is required, here and in the file. The two directions cut complementary
-- templates out of one colour, so a key that leaves its direction to a default
-- is a key whose mask cannot be reproduced from what was written down.
export type ColourKey = {
    red: number,
    green: number,
    blue: number,
    tolerance: number,
    removes: boolean,
}

-- `expected_text` is what this region should read wherever the element is used,
-- and `read_floor` is how sure the engine has to be before a reading of it is
-- worth anything. Both are absent on most elements: the first because the element
-- verifies itself by pixels or because the text differs from page to page (see
-- `Reference`), the second because the project's floor holds. Ask the second
-- through `Element.read_floor`, so nothing outside has to know the default.
--
-- `rect` means WHERE TO LOOK and nothing else: `observe.find` searches it and
-- reports the hit's own position, so a title box at [125, 14, 300, 56] and a flag
-- anywhere in [1170, 20, 300, 850] are one field at two extremes.
--
-- It can be absent, because some elements have no answer -- a minimap cell is
-- matched where the SCRIPT computes for each frame since the map pans, and a
-- confirm button drawn once and reused on nine screens sits somewhere different
-- on each. A rectangle is then supplied by the page reference
-- (`rect_override`) or by the falsification claim (`oracle.Expectation.rect`),
-- and for any one use exactly one of the three supplies it: `Page.new` refuses a
-- row that neither inherits one nor states one and `Expectation.new` refuses a
-- cell that does the same, so absent here means "the caller says where".
-- `item_spacing` makes the element a STRIP: a container rectangle holding a row
-- of things whose NUMBER is not a fact about the model at all. A hand of cards is
-- three cards or seven depending on the turn, and a model that wrote down a
-- rectangle per slot would be writing down a number the screen decides -- which
-- is how one project came to carry `fate_card_1`, `_2` and `_3` and a
-- `rest_card_2` and `_3` that are the same element five times.
--
-- The spacing is the widest gap in target pixels that still belongs to ONE item.
-- Reading a strip clusters what the frame located inside the container by that
-- gap and counts the clusters, so the count comes off the frame and the spacing
-- is the one thing the model asserts -- and therefore the one thing a screen can
-- contradict (`observe.read_strip`).
--
-- A strip carries the container rectangle it clusters inside and no appearances:
-- what it holds is read, and an element that verified itself by template pixels
-- would be asserting where its items are, which is the thing being counted.
export type Element = {
    name: string,
    form: string,
    capabilities: CapabilitySet,
    rect: Rect?,
    item_spacing: number?,
    appearances: { Appearance },
    expected_text: string?,
    expected_fragments: { string }?,
    expected_presence: boolean?,
    read_floor: number?,
    extra: { [string]: any },
    residual: { string },
}

-- What one page does with one element. The holding relation and the exercised
-- capabilities live HERE and not on the element, because "which page owns these
-- pixels" and "what does THIS page use them for" are properties of the pairing
-- (docs/plans/2026-07-31-annotation-model-capabilities.md 2.2). `expected_text`
-- and `rect_override` are here for the same reason.
--
-- `identify` is the polarity, present exactly when identify is exercised:
-- required means the page needs this element on screen, forbidden means the page
-- is not this page if the element is there.
--
-- `pinned_appearance` names the appearance this page's pixels wear, and it
-- applies to INTERACT ONLY: identify folds across every appearance whatever is
-- pinned, because the identify sweep runs before the page is known (the same
-- document, 4-2.4 correction). A pin beside an exercised identify is accepted
-- rather than refused, so an element that is both a page anchor and a click
-- target can pin the appearance its click needs.
--
-- `expected_text` is what THIS page's copy of the region should read, which is
-- what lets one drawn rectangle serve every page: the element carries the
-- geometry and the read capability, and the fact that the box says "出擊" here
-- and "戰鬥員配置" next door belongs to the pairing. The row wins over the
-- element, and `Reference.expected_text` is where that precedence is written.
--
-- `rect_override` is where THIS page's copy of the region sits: a refinement on
-- an element that draws its own rectangle, REQUIRED on an element that draws
-- none. It is refused beside an exercised identify, because the identify sweep
-- runs before the page is known, so an element with no rectangle of its own can
-- never be part of a page signature.
--
-- `page` is the NAME of the page that declared this row; `Page.new` stamps it and
-- an author never writes it. `observe.click` needs it: it holds a receipt proving
-- one page resolved on this frame, and has to check the hit belongs to THAT page
-- rather than to another page referencing the same element.
export type Reference = {
    element: Element,
    page: string,
    holding: string,
    exercised: CapabilitySet,
    identify: string?,
    expected_text: string?,
    expected_fragments: { string }?,
    expected_presence: boolean?,
    pinned_appearance: string?,
    interact_requires: string?,
    rect_override: Rect?,
    extra: { [string]: any },
    residual: { string },
}

-- `overlay` says this page COVERS the screen it appeared over instead of
-- replacing it, which is what the runtime page stack is built on
-- (docs/plans/2026-07-31-script-owned-page-model.md 10). It belongs to the page
-- rather than to the edge, because the same overlay opened from three screens
-- covers all three.
--
-- `interrupt` says this page can appear over ANYTHING -- a network error box, a
-- daily-reward popup -- so it is declared once here rather than as an inbound
-- edge from every page it can interrupt. `observe.walk_edge` consults it when a
-- walk runs out of budget; what to do about the page found is the caller's.
--
-- `catch_all` is `interrupt`'s opposite ORDER and never its synonym. It says the
-- signature describes a CLASS of screens rather than one screen -- "there is an
-- overlay, and it says a tap closes it" -- so it must be asked LAST, after every
-- specific page has had its turn. Asked first it swallows every page whose
-- clauses contain its own, which is the failure the co-resolution matrix
-- measured (docs/plans/2026-08-04-state-layer-and-policy-slots.md 3.4). A page
-- may declare one of the two and not both.
--
-- `state` is the mode a scene page belongs to. The name is the project's own
-- string and this framework carries no enumeration of modes: which modes a
-- target has is a fact about that target, and a list here would be a fact about
-- one game frozen into the framework. A run narrows what it asks to the current
-- mode's pages; it never REFUSES to ask, because a crash to the home screen is
-- a real thing that happens (`navigation`, "state is belief, the frame is
-- truth").
--
-- `over` is the same question for an overlay: which modes it can be drawn on top
-- of. An overlay carries no `state`, because it covers whatever the run was
-- already in rather than being somewhere itself -- the reward strip appears
-- after a battle and after a campfire, which is what makes it a family and not
-- a mode.
--
-- The three flags default to false, nil and nil: an ordinary page states none.
export type Page = {
    name: string,
    overlay: boolean,
    interrupt: boolean,
    catch_all: boolean,
    state: string?,
    over: { string }?,
    references: { Reference },
    extra: { [string]: any },
    residual: { string },
}

-- The tables this module minted, held by identity: `Page.new` refuses a
-- look-alike because accepting one fails SILENTLY -- a misspelled `capabilities`
-- yields an element that exercises nothing.
--
-- The keys are WEAK. A strong key here outlives every use of the object it
-- names: nothing removes an entry, so one `project.load_project` left its whole
-- model alive forever and a session that loaded ~150 of them hit the VM's
-- memory ceiling. Weak keys cost the registry nothing it was doing -- an entry
-- can only vanish once the object is unreachable, and an object nobody can
-- reach is one nobody can present to a predicate.
local mintedElements: { [any]: boolean } = setmetatable({}, { __mode = "k" }) :: any
local mintedPages: { [any]: boolean } = setmetatable({}, { __mode = "k" }) :: any
local mintedReferences: { [any]: boolean } = setmetatable({}, { __mode = "k" }) :: any

local model = {}

-- The bounded inheritance walk is `mint`'s, so `navigation` asks the same
-- question of its own registries through the same code. The registries above
-- stay private and are reachable only as the predicates below, because a script
-- that could write to one could mint anything.
local mintedOrDerived = mint.derives_from

function model.is_element(value: any): boolean
    return mintedOrDerived(value, mintedElements)
end

function model.is_page(value: any): boolean
    return mintedOrDerived(value, mintedPages)
end

-- References are minted too, so a verb handed one can tell a real page row from a
-- table shaped like one. There is no Reference.new: a reference exists only as
-- part of the page that declared it.
function model.is_reference(value: any): boolean
    return mintedOrDerived(value, mintedReferences)
end

-- The capability names in canonical order, for a caller that has to serialise a
-- set. A fresh copy each call, so nothing outside can reorder the constant.
function model.capability_order(): { string }
    return table.clone(k_capabilityOrder)
end

-- The capabilities `set` holds, in the file's canonical order.
function model.capability_list(set: any): { string }
    local names: { string } = {}
    for _, capability in k_capabilityOrder do
        if set[capability] then
            table.insert(names, capability)
        end
    end
    return names
end

-- Every validator below returns a SENTENCE or nil rather than raising, so the
-- constructor raises once at level 2 and the author reads their own call site;
-- the four checks every constructor repeats are in `mint`.
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

-- Published because a rectangle is authored in three places and only one of them
-- is here; `oracle` builds the third, the rectangle a falsification claim
-- searches at. The checker returns a SENTENCE or nil as it does for the callers
-- in this file, so each constructor raises at its own level.
model.check_rect  = checkRect
model.frozen_rect = frozenRect

-- Checks the shape of a recorded colour key and nothing about its worth. How many
-- pixels a key takes is a fact about the PIXELS, which this layer does not hold:
-- the crop verb measures it as it cuts, refuses the key that can never work, and
-- warns about counts that measured nothing
-- (docs/pitfalls/colour-key-annotation.md).
-- Whether `spec` states anything at all about what its region should read.
-- One question with three spellings, because a region can be pinned three ways:
-- the whole reading equals a string, ordered fragments appear in it with gaps
-- allowed, or there is any confident reading at all.
local function statesTextExpectation(spec: any): boolean
    return spec.expected_text ~= nil
        or spec.expected_fragments ~= nil
        or spec.expected_presence ~= nil
end

-- The shape of those three, and that at most ONE is stated. They are three
-- answers to one question -- what would make this reading count -- and a row
-- carrying two states no order between them, so the weaker would silently decide
-- or the stronger would silently be ignored depending on which was asked first
-- (docs/plans/2026-08-04-state-layer-and-policy-slots.md 3.3).
--
-- The framework owns all three and a project may only choose among them. A
-- custom predicate would put a page's identity inside a project function, and a
-- page signature is not a place a caller may relax per call site
-- (`observe.resolve_page`).
local function checkTextExpectation(what: string, spec: any): string?
    local stated = 0
    if spec.expected_text ~= nil then
        stated += 1
        if type(spec.expected_text) ~= "string" or spec.expected_text == "" then
            return what .. " expected_text must be the text the region reads"
        end
    end
    if spec.expected_fragments ~= nil then
        stated += 1
        local list = spec.expected_fragments
        if type(list) ~= "table" or #(list :: { any }) == 0 then
            return what
                .. " expected_fragments must be a non-empty list of the pieces "
                .. "that appear IN ORDER, with anything allowed between them"
        end
        for index, fragment in list :: { any } do
            if type(fragment) ~= "string" or fragment == "" then
                return what
                    .. " expected_fragments entry "
                    .. tostring(index)
                    .. " must be a non-empty piece of text"
            end
        end
    end
    if spec.expected_presence ~= nil then
        stated += 1
        if spec.expected_presence ~= true then
            return what
                .. " expected_presence is true or absent; false would say a "
                .. "region must read nothing, which is what a forbidden identify "
                .. "clause already says and says better"
        end
    end
    if stated > 1 then
        return what
            .. " states more than one of expected_text, expected_fragments and "
            .. "expected_presence; they are three answers to one question and "
            .. "nothing orders them"
    end
    return nil
end

local function checkColourKey(what: string, value: any): string?
    if type(value) ~= "table" then
        return what
            .. " must be a table { red = , green = , blue = ,"
            .. " tolerance = , removes = }"
    end
    for _, channel in { "red", "green", "blue" } do
        local component = (value :: any)[channel]
        if not wholeNumber(component) or component < 0 or component > 255 then
            return what .. "." .. channel .. " must be a channel value 0-255"
        end
    end
    local key = value :: ColourKey
    if
        not wholeNumber(key.tolerance)
        or key.tolerance < 0
        or key.tolerance > k_maximumColourTolerance
    then
        return what
            .. ".tolerance must be a whole number between 0 and "
            .. tostring(k_maximumColourTolerance)
            .. ", the widest the summed per-channel distance can express"
    end
    if type(key.removes) ~= "boolean" then
        return what
            .. ".removes must be true or false; it says whether the colour names "
            .. "the pixels the mask KEPT or the pixels it dropped, and a mask "
            .. "read the wrong way round selects the complement of its subject"
    end
    return nil
end

local function frozenColourKey(value: ColourKey): ColourKey
    return table.freeze({
        red       = value.red,
        green     = value.green,
        blue      = value.blue,
        tolerance = value.tolerance,
        removes   = value.removes,
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
-- `capabilities` is a LIST of names on the way in and a SET on the way out: the
-- list is the file's form, the set is what every reader asks.
--
-- Two rulings here fail closed:
--
--   * An element with no appearances is positioned BY THE PAGE, so before it may
--     identify it has to offer other evidence
--     (docs/plans/2026-07-31-annotation-model-capabilities.md 4-2.2). What is
--     refused is an element with neither -- no appearances and no `read`
--     capability -- which can produce no evidence at all. WHAT the region should
--     read is not asked here: one title rectangle shared by every page reads a
--     different name on each, and only the page knows which, so `Page.new` is
--     where an identify row is made to say it. An element that reads the same
--     text everywhere may state it here and every row inherits it.
--   * An element declares ONE verification source: template appearances, or
--     expected text (docs/plans/2026-08-01-three-layers-and-agent-operator.md
--     5-4). Both at once is two answers to "did I find it" with no rule between
--     them.
--
-- `rect` is the one field whose absence is ruled on elsewhere, because "where do
-- I look" has an answer per USE: the identify sweep looks in the element's own
-- rectangle, a click in the row's, a matrix cell in the claim's. See the type.
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

    if spec.rect ~= nil then
        local rectProblem = checkRect("element '" .. name .. "' rect", spec.rect)
        if rectProblem ~= nil then
            error(rectProblem, 2)
        end
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
            -- The handle, not the blob: an author holds both here and only one
            -- can be matched against, so the wrong one would otherwise be found
            -- out at the first search rather than at the line that chose it.
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
            if entry.key ~= nil then
                local keyProblem = checkColourKey(what .. " key", entry.key)
                if keyProblem ~= nil then
                    error(keyProblem, 2)
                end
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
                key       = if entry.key ~= nil
                    then frozenColourKey(entry.key :: ColourKey)
                    else nil,
                residual  = frozenResidual(entry.residual),
            })
        end
    end

    local expectationProblem = checkTextExpectation("element '" .. name .. "'", spec)
    if expectationProblem ~= nil then
        error(expectationProblem, 2)
    end

    if statesTextExpectation(spec) then
        if #appearances > 0 then
            error(
                "element '"
                    .. name
                    .. "' declares both appearances and a text expectation; an "
                    .. "element verifies itself EITHER by its template pixels OR "
                    .. "by what it reads, and two answers with no rule between "
                    .. "them is not a stronger check",
                2
            )
        end
        if not (capabilities :: CapabilitySet).read then
            error(
                "element '"
                    .. name
                    .. "' expects something of what it reads but does not "
                    .. 'declare the read capability; add "read" or drop the '
                    .. "expectation",
                2
            )
        end
    end

    if type(spec.form) ~= "string" or not k_formKnown[spec.form] then
        error(
            "element '"
                .. name
                .. '\' needs form = "fixed", "shape" or "strip": whether it '
                .. "draws its own rectangle, is found by its pixels wherever "
                .. "they are, or is a container whose items are counted off the "
                .. "frame. It is stated because an element with no rectangle "
                .. "could be either of the last two or a mistake, and the "
                .. "absence cannot say which",
            2
        )
    end
    local form = spec.form :: string

    if form == "shape" then
        if spec.rect ~= nil then
            error(
                "element '"
                    .. name
                    .. "' is a shape and states a rect; a shape is found by its "
                    .. "pixels wherever they are, so a rectangle here is a claim "
                    .. "about a position nothing keeps -- delete it or make the "
                    .. 'element form = "fixed"',
                2
            )
        end
    elseif spec.rect == nil then
        error(
            "element '"
                .. name
                .. "' is a "
                .. form
                .. " and states no rect; only a shape goes without one, and it "
                .. "says so by naming that form",
            2
        )
    end

    if form == "strip" then
        if
            not wholeNumber(spec.item_spacing)
            or (spec.item_spacing :: number) < 1
        then
            error(
                "element '"
                    .. name
                    .. "' is a strip and needs item_spacing = a whole number of "
                    .. "pixels, at least one: it is the widest gap that still "
                    .. "belongs to one item, and a gap of zero divides nothing",
                2
            )
        end
        if #appearances > 0 then
            error(
                "element '"
                    .. name
                    .. "' is a strip and declares appearances; a strip's items "
                    .. "are located by the frame and counted, so a template "
                    .. "would be asserting where they are -- which is the thing "
                    .. "being counted",
                2
            )
        end
        if not (capabilities :: CapabilitySet).read then
            error(
                "element '"
                    .. name
                    .. "' is a strip but does not declare the read capability; "
                    .. 'its items are located by reading, so add "read"',
                2
            )
        end
    elseif spec.item_spacing ~= nil then
        error(
            "element '"
                .. name
                .. "' states item_spacing and is a "
                .. form
                .. "; a spacing divides a container into items, which is what a "
                .. 'strip is -- say form = "strip" or drop the spacing',
            2
        )
    end

    if spec.read_floor ~= nil then
        if
            not wholeNumber(spec.read_floor)
            or spec.read_floor < 0
            or spec.read_floor > 10000
        then
            error(
                "element '"
                    .. name
                    .. "' read_floor must be a whole number of basis points "
                    .. "between 0 and 10000; it is how sure the engine has to be "
                    .. "before a reading of this region counts as evidence about "
                    .. "it",
                2
            )
        end
        if not (capabilities :: CapabilitySet).read then
            error(
                "element '"
                    .. name
                    .. "' has a read_floor but does not declare the read "
                    .. "capability; the floor judges a reading, and nothing may "
                    .. "ask this element for one",
                2
            )
        end
    end

    if
        #appearances == 0
        and (capabilities :: CapabilitySet).identify
        and not (capabilities :: CapabilitySet).read
    then
        error(
            "element '"
                .. name
                .. "' declares identify with no appearances and no read "
                .. "capability; an element with no template of its own is "
                .. "positioned BY the page that was recognised, so it has no "
                .. 'pixels to offer as evidence that the page IS that page -- add '
                .. '"read" and the text this region should show, and the reading '
                .. "becomes the evidence instead",
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
        rect          = if spec.rect ~= nil
            then frozenRect(spec.rect :: Rect)
            else nil,
        appearances   = table.freeze(appearances),
        form          = form,
        item_spacing  = spec.item_spacing,
        expected_text = spec.expected_text,
        expected_fragments = if spec.expected_fragments ~= nil
            then table.freeze(table.clone(spec.expected_fragments))
            else nil,
        expected_presence = spec.expected_presence,
        read_floor    = spec.read_floor,
        extra         = frozenExtra(spec.extra),
        residual      = frozenResidual(spec.residual),
    })
    mintedElements[element] = true
    return element
end

-- How sure a reading of this element has to be before it counts as evidence, in
-- basis points. Asked OF an element rather than read off one, because the field
-- is absent on almost every element and the answer never is: reading
-- `element.read_floor` directly is how a caller compares a confidence to nil.
function Element.read_floor(element: Element): number
    if element.read_floor ~= nil then
        return element.read_floor :: number
    end
    return k_defaultReadFloor
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

-- Page.new{ ... } -> a frozen page.
--
-- A page is a set of references plus two flags and carries no outbound edge: an
-- edge is a fact about a PAIR of pages, so `Graph.new` collects them
-- (docs/plans/2026-07-31-script-owned-page-model.md 10).
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

    -- The name is the label of the `framework.page_resolved` line
    -- `observe.resolve_page` writes the instant a signature holds, and the host
    -- refuses a label over its ceiling. Refused HERE so the cost is an authoring
    -- error: refused there, the first frame a page is actually on ends the run,
    -- inside a wait loop or a check walk where nothing catches it.
    if k_maxPageNameBytes ~= nil and #name > k_maxPageNameBytes then
        error(
            "page name '"
                .. name
                .. "' is "
                .. tostring(#name)
                .. " bytes, over the host's "
                .. tostring(k_maxPageNameBytes)
                .. "-byte ceiling for a name written to the evidence stream; "
                .. "this name is written there every time the page resolves, so "
                .. "the run would end on the first frame that is this page",
            2
        )
    end

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

    if spec.catch_all ~= nil and type(spec.catch_all) ~= "boolean" then
        error(
            "page '"
                .. name
                .. "' catch_all must be true or false; it says this signature "
                .. "describes a CLASS of screens rather than one screen, so it "
                .. "is asked last, after every specific page has been tried",
            2
        )
    end

    -- The two flags are opposite ORDERS and a page cannot want both. `interrupt`
    -- says ask this first because it can be covering anything; `catch_all` says
    -- ask this last because anything specific would answer better. A page
    -- claiming both would be asked first under a rule that exists to have it
    -- asked last, and the specific pages it describes would never resolve
    -- (docs/plans/2026-08-04-state-layer-and-policy-slots.md 3.4).
    if spec.interrupt == true and spec.catch_all == true then
        error(
            "page '"
                .. name
                .. "' declares both interrupt and catch_all, which are opposite "
                .. "positions in the order pages are asked in: interrupt is "
                .. "asked first because it may be covering anything, catch_all "
                .. "last because anything more specific answers better",
            2
        )
    end

    if spec.state ~= nil and (type(spec.state) ~= "string" or spec.state == "") then
        error(
            "page '"
                .. name
                .. "' state must be a non-empty mode name; the framework carries "
                .. "no enumeration of modes, because which modes a target has is "
                .. "a fact about that target",
            2
        )
    end

    -- A mode is where a run IS, and an overlay is something drawn over wherever
    -- it is. A page cannot be both, so the two fields are exclusive rather than
    -- an overlay carrying the mode it happens to sit on.
    if spec.state ~= nil and spec.overlay == true then
        error(
            "page '"
                .. name
                .. "' is an overlay and states a mode; an overlay covers "
                .. "whatever the run was already in, so what it may cover is "
                .. "`over` and never `state`",
            2
        )
    end

    if spec.over ~= nil then
        if spec.overlay ~= true then
            error(
                "page '"
                    .. name
                    .. "' states `over` and is not an overlay; `over` names the "
                    .. "modes this page can be drawn ON TOP OF, which only a "
                    .. "page that covers another has",
                2
            )
        end
        if type(spec.over) ~= "table" or #(spec.over :: { any }) == 0 then
            error(
                "page '"
                    .. name
                    .. "' needs over = { \"mode\", ... }, the modes it can cover; "
                    .. "an empty list says it covers none, which is a page no "
                    .. "run can ever be shown",
                2
            )
        end
        local seen: { [string]: boolean } = {}
        for index, mode in spec.over :: { any } do
            if type(mode) ~= "string" or mode == "" then
                error(
                    "page '"
                        .. name
                        .. "' over entry "
                        .. tostring(index)
                        .. " must be a non-empty mode name",
                    2
                )
            end
            if seen[mode] then
                error(
                    "page '"
                        .. name
                        .. "' names mode '"
                        .. mode
                        .. "' twice in `over`; it is a set, and naming one mode "
                        .. "again says nothing a second time",
                    2
                )
            end
            seen[mode] = true
        end
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

        -- The per-page refinement of what a shared region says, checked before
        -- the identify clause below because that clause consults it.
        local rowProblem = checkTextExpectation(what, entry)
        if rowProblem ~= nil then
            error(rowProblem, 2)
        end
        if statesTextExpectation(entry) then
            if not (element.capabilities :: any).read then
                error(
                    what
                        .. " expects something of what it reads but element '"
                        .. element.name
                        .. "' does not declare the read capability; a page cannot "
                        .. "refine a reading of something nothing may read",
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
            -- The half of the identify ruling `Element.new` cannot make: on a
            -- shared element the text differs per page, so the row is the only
            -- thing that can state it. This keeps "identify with neither pixels
            -- nor text" impossible while the shared element stays buildable.
            if
                #element.appearances == 0
                and not statesTextExpectation(entry)
                and not statesTextExpectation(element)
            then
                error(
                    what
                        .. " identifies by element '"
                        .. element.name
                        .. "', which has no appearances and no expected text "
                        .. "here; an element with no template of its own is "
                        .. "positioned BY the page that was recognised, so it has "
                        .. "no pixels to offer as evidence -- say what would "
                        .. "make a reading of it count on this page, with "
                        .. "expected_text, expected_fragments or "
                        .. "expected_presence, or give the element a template of "
                        .. "its own",
                    2
                )
            end
            -- Must come before the rect_override refusal below, or the author is
            -- refused twice: once for omitting the override, again for supplying
            -- it. An identify sweep runs BEFORE the page is known, so an element
            -- that draws no rectangle can never be part of a signature.
            if element.rect == nil then
                error(
                    what
                        .. " identifies by element '"
                        .. element.name
                        .. "', which draws no rectangle of its own; an identify "
                        .. "sweep runs before the page is known, so it can only "
                        .. "search the element's OWN rectangle and a row that "
                        .. "supplies one cannot be part of a page signature -- "
                        .. "give the element a rect, or identify this page by "
                        .. "something that has one",
                    2
                )
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

        -- The appearance GATE, and the reason it is not `pinned_appearance`
        -- wearing a second meaning: a pin says which appearance a click should
        -- search for, and this says a click is authorised ONLY while that
        -- appearance is the one on screen. A confirm button drawn lit and drawn
        -- grey is one element with two appearances, and clicking it grey is a
        -- click the target ignores while the run believes it landed
        -- (docs/plans/2026-08-04-state-layer-and-policy-slots.md 3.2).
        if entry.interact_requires ~= nil then
            if type(entry.interact_requires) ~= "string" then
                error(
                    what .. " interact_requires must be an appearance name",
                    2
                )
            end
            if not (exercised :: CapabilitySet).interact then
                error(
                    what
                        .. " gates an appearance without exercising interact; "
                        .. "the gate decides whether a CLICK is authorised, and "
                        .. "a row that may not click has nothing to gate",
                    2
                )
            end
            if Element.appearance_named(element, entry.interact_requires) == nil then
                error(
                    what
                        .. ' requires appearance "'
                        .. entry.interact_requires
                        .. "\" for interact, which element '"
                        .. element.name
                        .. "' does not have",
                    2
                )
            end
            -- A pin decides what a click SEARCHES for and a gate decides what
            -- authorises one, so naming two different appearances is a door
            -- whose key is on the other side: the search can only ever answer
            -- with the pinned look, and the gate refuses everything but the
            -- other. Refused for the reason a gate naming an absent appearance
            -- is -- both are configurations that open on no frame.
            if
                entry.pinned_appearance ~= nil
                and entry.pinned_appearance ~= entry.interact_requires
            then
                error(
                    what
                        .. ' pins appearance "'
                        .. entry.pinned_appearance
                        .. '" and requires "'
                        .. entry.interact_requires
                        .. '" for interact; the pin is what a click searches '
                        .. "for, so the gate could never see anything else and "
                        .. "would refuse every frame",
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
        elseif element.rect == nil then
            -- What keeps "a rect is optional" from meaning "nobody said where to
            -- look": a row that inherits no rectangle and states none leaves
            -- `observe.find` with no region, found out as a nil field deep inside
            -- a verb rather than at the line that wrote the row.
            error(
                what
                    .. " references element '"
                    .. element.name
                    .. "', which draws no rectangle of its own, and says no "
                    .. "rect_override; such an element is placed by whoever uses "
                    .. "it, so this row is the only thing that can say where it "
                    .. 'sits on this page -- add rect_override = { x = , y = , '
                    .. "width = , height = }",
                2
            )
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
            expected_text     = entry.expected_text,
            expected_fragments = if entry.expected_fragments ~= nil
                then table.freeze(table.clone(entry.expected_fragments))
                else nil,
            expected_presence = entry.expected_presence,
            pinned_appearance = entry.pinned_appearance,
            interact_requires = entry.interact_requires,
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
        state      = spec.state,
        over       = if spec.over ~= nil
            then table.freeze(table.clone(spec.over))
            else nil,
        catch_all  = spec.catch_all == true,
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

local Reference = {}
model.Reference = Reference

-- The exact text this row expects, or nil -- INCLUDING when it expects something
-- else. A row asking for ordered fragments or for any reading at all has an
-- expectation and no expected text, and every caller here wants the second
-- reading rather than the first.
--
-- Precedence is not decided here any more; `Reference.text_expectation` below is
-- the one place it is written down.
function Reference.expected_text(reference: Reference): string?
    local expectation = Reference.text_expectation(reference)
    if expectation ~= nil and expectation.kind == "equals" then
        return expectation.text
    end
    return nil
end

-- What would make a reading of this row's region count, or nil when neither the
-- row nor the element asks anything of it.
--
-- THE ROW WINS AS A WHOLE, which is the part a per-field precedence would get
-- wrong: a row asking only for presence must not inherit the element's exact
-- text and end up demanding both. So a row that states any of the three answers
-- for all three.
--
-- Three kinds, spelled here and nowhere else:
--
--   * "equals"    -- the whole reading is this text, whitespace at the ends
--                    forgiven. The strictest, and the only one that can tell
--                    two screens apart by their titles.
--   * "fragments" -- these pieces appear in this order, with anything between
--                    them. What a sentence with a number in the middle needs.
--   * "presence"  -- there is a confident reading at all. What a region whose
--                    text is never twice the same needs, and the weakest thing
--                    that is still evidence.
function Reference.text_expectation(reference: Reference): any
    local source: any = reference
    if not statesTextExpectation(reference) then
        source = reference.element
    end
    if source.expected_text ~= nil then
        return table.freeze({ kind = "equals", text = source.expected_text })
    end
    if source.expected_fragments ~= nil then
        return table.freeze({
            kind      = "fragments",
            fragments = source.expected_fragments,
        })
    end
    if source.expected_presence ~= nil then
        return table.freeze({ kind = "presence" })
    end
    return nil
end

return model
