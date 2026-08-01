--!strict
-- oracle: the screens a model is measured against, and what each one is
-- supposed to show.
--
-- WHY THIS IS DATA AND NOT A TEST. A template always matches the image it was
-- cut from, at a distance of zero, so a mark that hits the screen it was
-- authored on has demonstrated nothing at all. The only evidence that it
-- identifies ONE screen is what it does on the others
-- (docs/pitfalls/colour-key-annotation.md). That measurement is worthless
-- unless something says which answer was expected, and an expectation that
-- lives in a reviewer's head is not falsifiable and not replayable -- so it is
-- written into the project file, exactly as an element's expected text is
-- (docs/plans/2026-08-01-three-layers-and-agent-operator.md 5-4). This module
-- is that vocabulary. `regress` is what walks it.
--
-- THREE STATES AND NOT TWO. A page's signature is a conjunction over the marks
-- that identify it, never an inventory of everything on its screen, so "the
-- model says nothing about this element here" is a different sentence from
-- "these pixels are not there". The card-detail overlay is the proof: it is the
-- battle screen with a card selected, and the draw pile, the discard pile and
-- the end-turn button are all still on it, merely dimmed. Reading each of those
-- as a defect trains an author to break a model that is correct
-- (docs/plans/2026-07-31-annotation-model-capabilities.md 4-2.1, and the
-- ModelCellExpectation this replaces).
--
-- A CELL NOBODY CLAIMED IS `unclaimed`, AND THAT IS DELIBERATELY PERMISSIVE.
-- The falsification pressure does not come from forcing an author to fill in
-- every cell -- they would fill it in with whatever they measured -- it comes
-- from the rules `regress` applies whatever the file says: two appearances of
-- one element may never hit one screen, and the winner must beat every rival by
-- a factor. Those hold on unclaimed cells too, which is exactly where an
-- author's natural habit leaves the holes (the same document, 2.3 correction
-- (b)).
--
-- LOAD ORDER. Framework modules load in the sorted order of their file names.
-- `oracle` sorts after `mint`, `model`, `navigation` and `observe`, and before
-- `project` and `regress` -- which is what lets `project` build these objects
-- while reading a file and `regress` read them while walking one. It names
-- `mint` and `model`.

-- Where a screen's pixels live, relative to the project root. The file name is
-- the content hash, so the path is DERIVED and never authored: one fact about a
-- screen (which bytes it is) cannot then disagree with a second fact (where
-- they are). It follows the store the runtime templates already use,
-- `assets/templates/<hex>.png`.
local k_screenDirectory = "assets/screens/"
local k_screenSuffix    = ".png"

-- How long a content hash is, in lowercase hex characters. SHA-256, which is
-- the repository's one digest.
local k_hashLength = 64

local k_stateKnown: { [string]: boolean } = {
    match     = true,
    absent    = true,
    unclaimed = true,
}

local k_screenKeys: { [string]: boolean } = {
    name     = true,
    hash     = true,
    extra    = true,
    residual = true,
}

local k_expectationKeys: { [string]: boolean } = {
    screen     = true,
    element    = true,
    appearance = true,
    state      = true,
    extra      = true,
    residual   = true,
}

local k_claimsKeys: { [string]: boolean } = {
    screens      = true,
    expectations = true,
}

-- One screen the model is measured against.
--
-- `path` is derived from `hash` and is here so nothing has to re-derive it;
-- `name` is what a verdict row and a finding are read by, because a hex digest
-- tells a reader nothing about which screen went wrong.
export type Screen = {
    name: string,
    hash: string,
    path: string,
    extra: { [string]: any },
    residual: { string },
}

-- What the model states about one cell of the matrix.
--
-- The subject is an ELEMENT when `appearance` is absent -- the folded answer,
-- which is what a page signature and a click actually read -- and one
-- appearance searched alone when it is present. Both are needed and they answer
-- different questions: the fold is what the runtime does, and the single
-- appearance is the only way to ask whether a mark discriminates on a screen
-- another appearance owns, where a hit is invisible once folded with a correct
-- one (docs/plans/2026-07-31-annotation-model-capabilities.md 2.3 correction
-- (c)).
export type Expectation = {
    screen: string,
    element: string,
    appearance: string?,
    state: string,
    extra: { [string]: any },
    residual: { string },
}

-- Every screen and every expectation of one project, checked against each other
-- and indexed for the walk.
export type Claims = {
    screens: { Screen },
    screen_by_name: { [string]: Screen },
    expectations: { Expectation },
    state_by_cell: { [string]: string },
}

local mintedScreens: { [any]: boolean } = {}
local mintedExpectations: { [any]: boolean } = {}
local mintedClaims: { [any]: boolean } = {}

local oracle = {}

oracle.screen_directory = k_screenDirectory

function oracle.is_screen(value: any): boolean
    return mint.derives_from(value, mintedScreens)
end

function oracle.is_expectation(value: any): boolean
    return mint.derives_from(value, mintedExpectations)
end

function oracle.is_claims(value: any): boolean
    return mint.derives_from(value, mintedClaims)
end

-- The three states, so a caller comparing against one spells it from here
-- rather than from a literal of its own.
oracle.state_match     = "match"
oracle.state_absent    = "absent"
oracle.state_unclaimed = "unclaimed"

-- The project-relative path holding the pixels of the screen with this hash.
function oracle.screen_path(hash: string): string
    return k_screenDirectory .. hash .. k_screenSuffix
end

-- The key one cell is filed under. NUL separates the parts because none of the
-- three can contain it, so two different cells cannot collide by concatenation.
local function cellKey(
    screen: string,
    element: string,
    appearance: string?
): string
    return screen .. "\0" .. element .. "\0" .. (appearance or "")
end

local Screen = {}
oracle.Screen = Screen

-- Screen.new{ name = , hash = } -> a frozen screen.
--
-- The hash is checked for shape here rather than trusted, because it is what
-- the path is built from: a hash with a stray character names a file that does
-- not exist, and the failure would otherwise surface as a capture that ran out
-- of frames rather than as the line the author wrote.
function Screen.new(spec: { [string]: any }): Screen
    if type(spec) ~= "table" then
        error("Screen.new needs a table of fields", 2)
    end

    local unknown = mint.unknown_key("Screen.new", spec, k_screenKeys, true)
    if unknown ~= nil then
        error(unknown, 2)
    end

    if type(spec.name) ~= "string" or spec.name == "" then
        error(
            "a screen needs a non-empty name; the hash says which bytes it is "
                .. "and the name is what a verdict row is read by",
            2
        )
    end
    local name = spec.name :: string

    if
        type(spec.hash) ~= "string"
        or #(spec.hash :: string) ~= k_hashLength
        or string.match(spec.hash :: string, "^[0-9a-f]+$") == nil
    then
        error(
            "screen '"
                .. name
                .. "' needs hash = the "
                .. tostring(k_hashLength)
                .. "-character lowercase hex content hash of its PNG; the file "
                .. "it names is derived from it, so a malformed one is a screen "
                .. "with no pixels",
            2
        )
    end
    local hash = spec.hash :: string

    local extraProblem = mint.check_extra("screen '" .. name .. "'", spec.extra)
    if extraProblem ~= nil then
        error(extraProblem, 2)
    end
    local residualProblem =
        mint.check_residual("screen '" .. name .. "'", spec.residual)
    if residualProblem ~= nil then
        error(residualProblem, 2)
    end

    local screen = table.freeze({
        name     = name,
        hash     = hash,
        path     = oracle.screen_path(hash),
        extra    = mint.frozen_extra(spec.extra),
        residual = mint.frozen_residual(spec.residual),
    })
    mintedScreens[screen] = true
    return screen
end

local Expectation = {}
oracle.Expectation = Expectation

-- Expectation.new{ screen = , element = , appearance = , state = } -> frozen.
--
-- `screen` is a screen this project declares and `element` is an element built
-- by `Element.new`; both are the OBJECTS rather than names, so an expectation
-- about a screen or an element nothing declares cannot be constructed at all.
--
-- Two rulings, both failing closed at construction:
--
--   * AN ELEMENT WITH NO APPEARANCES CANNOT BE CLAIMED. It carries no template,
--     so there is nothing to search for and no measurement any answer could be
--     read against; it is positioned BY the page that was recognised
--     (docs/plans/2026-07-31-annotation-model-capabilities.md 4-2.2). A file
--     stating `match` about one is stating something no run can check, and
--     accepting it would put a permanently green cell in the matrix.
--   * A NAMED APPEARANCE MUST BE ONE THE ELEMENT HAS. A misspelled appearance
--     would otherwise file its claim under a cell the walk never visits, which
--     is the same silence as having written nothing.
function Expectation.new(spec: { [string]: any }): Expectation
    if type(spec) ~= "table" then
        error("Expectation.new needs a table of fields", 2)
    end

    local unknown =
        mint.unknown_key("Expectation.new", spec, k_expectationKeys, true)
    if unknown ~= nil then
        error(unknown, 2)
    end

    if not oracle.is_screen(spec.screen) then
        error(
            "an expectation needs screen = a screen built by Screen.new; a "
                .. "claim about a screen this project does not hold is a claim "
                .. "nothing can measure",
            2
        )
    end
    local screen = spec.screen :: Screen

    if not model.is_element(spec.element) then
        error(
            "an expectation needs element = an element built by Element.new",
            2
        )
    end
    local element = spec.element
    local what    = "the expectation for element '"
        .. element.name
        .. "' on screen '"
        .. screen.name
        .. "'"

    if type(spec.state) ~= "string" or not k_stateKnown[spec.state] then
        error(
            what
                .. ' needs state = "match", "absent" or "unclaimed"; a page '
                .. "signature names the marks that identify it rather than "
                .. "everything on its screen, so \"the model says nothing here\" "
                .. "is a third answer and not a missing one",
            2
        )
    end

    if #element.appearances == 0 then
        error(
            what
                .. " claims an outcome for an element that declares no "
                .. "appearances; it has no template to search for, so it is "
                .. "located by the page rather than by its own pixels and there "
                .. "is no measurement this claim could ever be read against",
            2
        )
    end

    if spec.appearance ~= nil then
        if type(spec.appearance) ~= "string" then
            error(what .. " appearance must be an appearance name", 2)
        end
        if
            model.Element.appearance_named(element, spec.appearance) == nil
        then
            error(
                what
                    .. ' names appearance "'
                    .. spec.appearance
                    .. "\" which element '"
                    .. element.name
                    .. "' does not declare",
                2
            )
        end
    end

    local extraProblem = mint.check_extra(what, spec.extra)
    if extraProblem ~= nil then
        error(extraProblem, 2)
    end
    local residualProblem = mint.check_residual(what, spec.residual)
    if residualProblem ~= nil then
        error(residualProblem, 2)
    end

    local expectation = table.freeze({
        screen     = screen.name,
        element    = element.name,
        appearance = spec.appearance,
        state      = spec.state,
        extra      = mint.frozen_extra(spec.extra),
        residual   = mint.frozen_residual(spec.residual),
    })
    mintedExpectations[expectation] = true
    return expectation
end

local Claims = {}
oracle.Claims = Claims

-- Claims.new{ screens = { ... }, expectations = { ... } } -> frozen claims.
--
-- The collection is where a screen and an expectation stop being locally well
-- formed and start being consistent with each other: no two screens share a
-- name, no two share a hash, and no cell is claimed twice. All three are things
-- one constructor cannot see, and each one silently costs a measurement --
-- two screens with one hash are one screen measured twice under two names, and
-- two claims about one cell mean whichever the walk reads last is the model.
function Claims.new(spec: { [string]: any }): Claims
    if type(spec) ~= "table" then
        error("Claims.new needs a table of fields", 2)
    end

    local unknown = mint.unknown_key("Claims.new", spec, k_claimsKeys, false)
    if unknown ~= nil then
        error(unknown, 2)
    end
    if type(spec.screens) ~= "table" then
        error("Claims.new needs screens = { ... }", 2)
    end
    if type(spec.expectations) ~= "table" then
        error(
            "Claims.new needs expectations = { ... }; a project that has "
                .. "recorded none passes an empty list, which says so",
            2
        )
    end

    local screens: { Screen }                = {}
    local screenByName: { [string]: Screen } = {}
    local screenByHash: { [string]: string } = {}
    local declared = spec.screens :: { any }
    for index = 1, #declared do
        local screen = declared[index]
        if not oracle.is_screen(screen) then
            error(
                "Claims.new screen "
                    .. tostring(index)
                    .. " is not a screen built by Screen.new",
                2
            )
        end
        if screenByName[screen.name] ~= nil then
            error(
                "Claims.new was given screen '"
                    .. screen.name
                    .. "' twice; a project names each screen once",
                2
            )
        end
        local sameBytes = screenByHash[screen.hash]
        if sameBytes ~= nil then
            error(
                "screens '"
                    .. sameBytes
                    .. "' and '"
                    .. screen.name
                    .. "' carry one content hash, so they are one picture under "
                    .. "two names; every cell would be measured twice and every "
                    .. "answer counted twice",
                2
            )
        end
        screenByName[screen.name] = screen
        screenByHash[screen.hash] = screen.name
        screens[index]            = screen
    end

    local expectations: { Expectation }    = {}
    local stateByCell: { [string]: string } = {}
    local claimed = spec.expectations :: { any }
    for index = 1, #claimed do
        local expectation = claimed[index]
        if not oracle.is_expectation(expectation) then
            error(
                "Claims.new expectation "
                    .. tostring(index)
                    .. " is not one built by Expectation.new",
                2
            )
        end
        if screenByName[expectation.screen] == nil then
            error(
                "the expectation for element '"
                    .. expectation.element
                    .. "' names screen '"
                    .. expectation.screen
                    .. "', which this collection does not hold",
                2
            )
        end
        local key = cellKey(
            expectation.screen,
            expectation.element,
            expectation.appearance
        )
        if stateByCell[key] ~= nil then
            error(
                "element '"
                    .. expectation.element
                    .. "' on screen '"
                    .. expectation.screen
                    .. "' is claimed twice; one cell has one expectation, and "
                    .. "two of them means the model is whichever line was read "
                    .. "last",
                2
            )
        end
        stateByCell[key]    = expectation.state
        expectations[index] = expectation
    end

    local claims = table.freeze({
        screens        = table.freeze(screens),
        screen_by_name = table.freeze(screenByName),
        expectations   = table.freeze(expectations),
        state_by_cell  = table.freeze(stateByCell),
    })
    mintedClaims[claims] = true
    return claims
end

-- What the model states about one cell, or "unclaimed" when it states nothing.
function Claims.state_for(
    claims: Claims,
    screen: string,
    element: string,
    appearance: string?
): string
    local state = claims.state_by_cell[cellKey(screen, element, appearance)]
    if state == nil then
        return oracle.state_unclaimed
    end
    return state
end

-- The screens in the order the host serves their pixels: by content hash.
--
-- THIS ORDER IS A CONTRACT WITH THE FRAME SOURCE and not a presentation choice.
-- A screen's file is `assets/screens/<hash>.png`, the host's file-backed frame
-- source serves that directory one file per capture in file-name order, and the
-- walk opens one observation per screen -- so sorting by hash here is what
-- pairs the Nth capture with the Nth screen. The set the two sides walk is kept
-- equal by two facts checked elsewhere: `regress.check` reads every declared
-- screen's file, so a screen with no file fails by name, and the host refuses
-- the run unless it served exactly as many frames as it holds files.
--
-- A fresh list each call, so nothing outside can reorder what a walk reads.
function Claims.screens_in_frame_order(claims: Claims): { Screen }
    local ordered = table.clone(claims.screens)
    table.sort(ordered, function(left: Screen, right: Screen): boolean
        return left.hash < right.hash
    end)
    return ordered
end

return oracle
