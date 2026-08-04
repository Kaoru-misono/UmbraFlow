--!strict
-- oracle: the screens a model is measured against, and what each one is
-- supposed to show.
--
-- It is DATA rather than a test because a template always matches the image it
-- was cut from at a distance of zero, so the only evidence that a mark
-- identifies ONE screen is what it does on the others
-- (docs/pitfalls/colour-key-annotation.md) -- and that measurement is worthless
-- unless the file says which answer was expected
-- (docs/plans/2026-08-01-three-layers-and-agent-operator.md 5-4). `regress`
-- walks this vocabulary.
--
-- THREE STATES AND NOT TWO. A page's signature is a conjunction over the marks
-- that identify it, never an inventory of its screen, so "the model says nothing
-- about this element here" is a different sentence from "these pixels are not
-- there". A card-detail overlay is the battle screen with a card selected, draw
-- pile and end-turn button still on it and merely dimmed; reading each of those
-- as a defect trains an author to break a correct model
-- (docs/plans/2026-07-31-annotation-model-capabilities.md 4-2.1).
--
-- A cell nobody claimed is `unclaimed`, deliberately permissively: the
-- falsification pressure comes from the rules `regress` applies whatever the file
-- says, most of which hold on unclaimed cells too (the same document, 2.3
-- correction (b)).
--
-- A screen may also say which page it is of. That is not a licence to be measured
-- less: one of those rules stops firing between two captures of ONE page, and in
-- exchange the declaration is itself measured. Both halves are `recognition`'s.
--
-- Framework modules load in the sorted order of their file names. `oracle` sorts
-- after `mint`, `model`, `navigation` and `observe`, and before `project` and
-- `regress`. It names `mint` and `model`.

-- Where a screen's pixels live, relative to the project root. The file name is
-- the content hash, so the path is DERIVED and never authored: which bytes a
-- screen is cannot then disagree with where they are. It follows the store the
-- runtime templates use, `assets/templates/<hex>.png`.
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
    page     = true,
    extra    = true,
    residual = true,
}

local k_expectationKeys: { [string]: boolean } = {
    screen     = true,
    element    = true,
    rect       = true,
    appearance = true,
    text       = true,
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
--
-- `page` is the only field a rule reads about a PAIR of screens, and it is what
-- tells two captures of one page from two pages. Both rules that read it are in
-- `recognition`.
--
-- It is OPTIONAL because a screen captured while exploring is usually of a page
-- nobody has annotated yet -- the pixels come first and the page model is written
-- afterwards -- so requiring it would refuse exactly the screens a session starts
-- with and push an author to invent a page.
--
-- It is a NAME and not a page, the same ruling `scribe.add_edge` makes for the
-- ends of an edge: writing one reference REBUILDS a page, so a frozen screen
-- holding the page object would go on naming the superseded copy.
export type Screen = {
    name: string,
    hash: string,
    page: string?,
    path: string,
    extra: { [string]: any },
    residual: { string },
}

-- A rectangle in target pixels: the same four numbers `model.Rect` is, restated
-- because a chunk loaded under the framework environment cannot name another
-- chunk's exported types -- there is no module resolver in this VM, only a global
-- binding. The CHECK is not restated: every rectangle passes through
-- `model.check_rect`.
export type Rect = {
    x: number,
    y: number,
    width: number,
    height: number,
}

-- What the model states about one cell of the matrix.
--
-- `rect` is WHERE THE CELL WAS MEASURED, and it exists for the element that draws
-- no rectangle of its own: a minimap cell is searched where the script works out
-- per frame because the map pans, so the only place that position can be written
-- down and falsified is a claim about ONE stored screen. An element that draws
-- its own rectangle states none here.
--
-- It is also what makes a second claim about one element on one screen legal --
-- "it matches HERE and stays away from THERE" is two claims, one element, one
-- screen, two rectangles -- so a cell is keyed by its rectangle as well as by its
-- appearance.
--
-- `appearance` and `text` name the two VERIFICATION SOURCES and at most one of
-- the pair is ever set:
--
--   * NEITHER: the subject is the folded element measured against its templates,
--     which is what a page signature and a click read.
--   * `appearance`: one template searched alone. It is the only way to ask
--     whether a mark discriminates on a screen another appearance owns, where a
--     hit is invisible once folded with a correct one
--     (docs/plans/2026-07-31-annotation-model-capabilities.md 2.3 correction
--     (c)).
--   * `text`: what the element's rectangle READS on this screen, which is the
--     cell an element with no templates has
--     (docs/plans/2026-08-01-three-layers-and-agent-operator.md 5-4). It is
--     stated per screen because one shared title box reads a different name on
--     every screen.
export type Expectation = {
    screen: string,
    element: string,
    rect: Rect?,
    appearance: string?,
    text: string?,
    state: string,
    extra: { [string]: any },
    residual: { string },
}

-- Every screen and every expectation of one project, checked against each other
-- and indexed for the walk.
--
-- `text_by_cell` is a second index keyed like the first, so a walk can ask for
-- the text of the cell it is standing on without first finding the expectation.
-- Only a claim about what a region reads has a text, while every claim has a
-- state, which is why the two are not one index.
--
-- `rects_by_subject` is keyed by screen and element ALONE, because a walk cannot
-- reach a cell without first knowing the rectangle it is filed under; the list of
-- rectangles is a property of the pairing rather than of any one cell.
export type Claims = {
    screens: { Screen },
    screen_by_name: { [string]: Screen },
    expectations: { Expectation },
    state_by_cell: { [string]: string },
    text_by_cell: { [string]: string },
    rects_by_subject: { [string]: { Rect } },
}

-- Weak keys, for the reason spelled out over `model`'s registries.
local mintedScreens: { [any]: boolean } = setmetatable({}, { __mode = "k" }) :: any
local mintedExpectations: { [any]: boolean } = setmetatable({}, { __mode = "k" }) :: any
local mintedClaims: { [any]: boolean } = setmetatable({}, { __mode = "k" }) :: any

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

-- One rectangle as the text a key is built from, and the empty string when there
-- is none. Published because `reading` and `regress` file things by the same
-- rectangle, and a second spelling of "which rectangle" is a way for a lookup to
-- miss its row -- which reads as a cell nobody claimed rather than as a defect.
function oracle.rect_key(rect: any): string
    if rect == nil then
        return ""
    end
    return tostring(rect.x)
        .. ","
        .. tostring(rect.y)
        .. ","
        .. tostring(rect.width)
        .. ","
        .. tostring(rect.height)
end

-- The key one cell is filed under. NUL separates the parts because none of the
-- four can contain it, so two different cells cannot collide by concatenation.
-- The rectangle is part of the key, which is what lets one element be claimed
-- twice on one screen; two claims at ONE rectangle are still a contradiction and
-- `Claims.new` still refuses them.
local function cellKey(
    screen: string,
    element: string,
    appearance: string?,
    rect: any
): string
    return screen
        .. "\0"
        .. element
        .. "\0"
        .. (appearance or "")
        .. "\0"
        .. oracle.rect_key(rect)
end

-- The key the rectangles of one (screen, element) pairing are collected under.
local function subjectKey(screen: string, element: string): string
    return screen .. "\0" .. element
end

local Screen = {}
oracle.Screen = Screen

-- Screen.new{ name = , hash = , page = } -> a frozen screen.
--
-- The hash is checked for shape rather than trusted, because the path is built
-- from it: a stray character names a file that does not exist, and the failure
-- would otherwise surface as a capture that ran out of frames.
--
-- Whether the project declares that PAGE is not asked here, because this
-- constructor is handed one screen's own facts and can see no pages. Both callers
-- that can see them refuse it -- `project.build` with the line number the author
-- wrote it on, `scribe.add_screen` in the voice it refuses a taken screen name --
-- because a name checked at neither would be a declaration `recognition` cannot
-- measure.
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

    if
        spec.page ~= nil
        and (type(spec.page) ~= "string" or spec.page == "")
    then
        error(
            "screen '"
                .. name
                .. "' page must be the NAME of a page this project declares; a "
                .. "screen that says which page it is of is a claim the matrix "
                .. "measures, and a screen that says nothing is a screen nobody "
                .. "has annotated yet rather than a malformed one",
            2
        )
    end

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
        page     = spec.page,
        path     = oracle.screen_path(hash),
        extra    = mint.frozen_extra(spec.extra),
        residual = mint.frozen_residual(spec.residual),
    })
    mintedScreens[screen] = true
    return screen
end

local Expectation = {}
oracle.Expectation = Expectation

-- Expectation.new{ screen = , element = , rect = , appearance = , text = , state = }
--
-- `screen` is a screen this project declares and `element` is an element built
-- by `Element.new`; both are the OBJECTS rather than names, so an expectation
-- about a screen or an element nothing declares cannot be constructed at all.
--
-- Five rulings, all failing closed at construction:
--
--   * A cell is about ONE verification source: `appearance` names a template and
--     `text` names a reading, so stating both leaves nothing saying which was
--     measured.
--   * An element with no appearances is claimed by what it READS and only that.
--     It carries no template, so there is nothing to search for and no score to
--     read an answer against; it is positioned BY the page that was recognised
--     (docs/plans/2026-07-31-annotation-model-capabilities.md 4-2.2). A cell with
--     no text is a permanently green cell, so it is refused.
--   * An element nothing may read cannot be claimed by text: it has to declare
--     the `read` capability first, as `observe.read_element` demands.
--   * A named appearance must be one the element has, or the claim is filed under
--     a cell the walk never visits -- the same silence as writing nothing.
--   * EXACTLY ONE thing says where the cell was measured. An element that draws
--     its own rectangle may not be given a second answer here; an element that
--     draws none has nowhere to be searched until this claim says so, and a claim
--     that does not is another permanently green cell.
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

    if spec.text ~= nil then
        if type(spec.text) ~= "string" or spec.text == "" then
            error(
                what
                    .. " text must be the text this element's region reads on "
                    .. "this screen",
                2
            )
        end
        if spec.appearance ~= nil then
            error(
                what
                    .. " names both an appearance and a text; those are the two "
                    .. "ways an element can be verified and a cell is measured "
                    .. "by exactly one of them, so a cell claiming both says "
                    .. "nothing about which measurement was taken",
                2
            )
        end
    end

    if #element.appearances == 0 then
        if spec.text == nil then
            error(
                what
                    .. " claims an outcome for an element that declares no "
                    .. "appearances and says no text; it has no template to "
                    .. "search for, so it is located by the page rather than by "
                    .. "its own pixels and there is no measurement this claim "
                    .. 'could ever be read against -- say what the region reads '
                    .. 'on this screen with text = "..."',
                2
            )
        end
        if not element.capabilities.read then
            error(
                what
                    .. " claims a text for element '"
                    .. element.name
                    .. "', which does not declare the read capability; nothing "
                    .. "may ask that rectangle for a reading, so the claim names "
                    .. "a measurement no run is allowed to take",
                2
            )
        end
    else
        if spec.text ~= nil then
            error(
                what
                    .. " claims a text for element '"
                    .. element.name
                    .. "', which verifies itself by its template pixels; the "
                    .. "matrix measures that element by what its appearances "
                    .. "score, and a second answer with no rule between them is "
                    .. "not a stronger check",
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
    end

    -- Where this cell is measured, asked after the verification source because
    -- the two are independent: a shape with no rectangle of its own may be
    -- claimed by a template or by a reading, and "where" is the same question
    -- either way.
    if spec.rect ~= nil then
        if element.rect ~= nil then
            error(
                what
                    .. " states a rectangle for element '"
                    .. element.name
                    .. "', which draws its own; a cell is measured at ONE region "
                    .. "and the element already said which, so a second answer "
                    .. "here would let a claim quietly move the measurement it is "
                    .. "judged by",
                2
            )
        end
        local rectProblem = model.check_rect(what .. " rect", spec.rect)
        if rectProblem ~= nil then
            error(rectProblem, 2)
        end
    elseif element.rect == nil then
        error(
            what
                .. " claims an outcome for element '"
                .. element.name
                .. "', which draws no rectangle of its own and is given none "
                .. "here; there is nowhere to look, so nothing could ever "
                .. "contradict this claim -- say where the shape is on this "
                .. "screen with rect = { x = , y = , width = , height = }",
            2
        )
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
        rect       = if spec.rect ~= nil
            then model.frozen_rect(spec.rect)
            else nil,
        appearance = spec.appearance,
        text       = spec.text,
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
-- formed and start being consistent with each other: no two screens share a name,
-- no two share a hash, and no cell is claimed twice. Each failure silently costs
-- a measurement -- two screens with one hash are one screen measured twice, and
-- two claims about one cell make the model whichever line was read last.
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

    local expectations: { Expectation }     = {}
    local stateByCell: { [string]: string } = {}
    local textByCell: { [string]: string }  = {}

    local rectsBySubject: { [string]: { Rect } } = {}
    local rectSeen: { [string]: boolean }        = {}

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
            expectation.appearance,
            expectation.rect
        )
        if stateByCell[key] ~= nil then
            error(
                "element '"
                    .. expectation.element
                    .. "' on screen '"
                    .. expectation.screen
                    .. "' is claimed twice"
                    .. (
                        if expectation.rect ~= nil
                            then " at ["
                                .. oracle.rect_key(expectation.rect)
                                .. "]"
                            else ""
                    )
                    .. "; one cell has one expectation, and two of them means the "
                    .. "model is whichever line was read last",
                2
            )
        end
        stateByCell[key] = expectation.state
        if expectation.text ~= nil then
            textByCell[key] = expectation.text
        end
        -- Collected in the order the file names them, each one once, so a reader
        -- following one element down the verdict meets its cells in the order
        -- they were written.
        if expectation.rect ~= nil then
            local subject = subjectKey(
                expectation.screen,
                expectation.element
            )
            local rects = rectsBySubject[subject]
            if rects == nil then
                rects                   = {}
                rectsBySubject[subject] = rects
            end
            local seen = subject .. "\0" .. oracle.rect_key(expectation.rect)
            if not rectSeen[seen] then
                rectSeen[seen] = true
                table.insert(rects, expectation.rect)
            end
        end
        expectations[index] = expectation
    end

    -- Frozen in place rather than replaced, because a walk holds the list this
    -- returns and nothing outside may grow it afterwards.
    for _, rects in rectsBySubject do
        table.freeze(rects)
    end

    local claims = table.freeze({
        screens          = table.freeze(screens),
        screen_by_name   = table.freeze(screenByName),
        expectations     = table.freeze(expectations),
        state_by_cell    = table.freeze(stateByCell),
        text_by_cell     = table.freeze(textByCell),
        rects_by_subject = table.freeze(rectsBySubject),
    })
    mintedClaims[claims] = true
    return claims
end

-- What the model states about one cell, or "unclaimed" when it states nothing.
-- `rect` is the rectangle the cell was measured at, nil for every element that
-- draws its own.
function Claims.state_for(
    claims: Claims,
    screen: string,
    element: string,
    appearance: string?,
    rect: any
): string
    local state = claims.state_by_cell[cellKey(screen, element, appearance, rect)]
    if state == nil then
        return oracle.state_unclaimed
    end
    return state
end

-- The text one cell claims its element's region reads there, or nil when the
-- cell claims nothing about a reading.
--
-- It takes no appearance, because a cell is measured by ONE verification source
-- so the cell a text belongs to is always the folded one -- the text of an
-- appearance cell is a question `Expectation.new` refuses to let anyone answer.
-- It does take the rectangle, because one region drawn once and placed nine times
-- reads nine different things.
function Claims.text_for(
    claims: Claims,
    screen: string,
    element: string,
    rect: any
): string?
    return claims.text_by_cell[cellKey(screen, element, nil, rect)]
end

-- Every rectangle this file wants `element` measured at on `screen`, in the
-- order the file names them, and an empty list when it names none.
--
-- It is the walk's only entry into a shape with no rectangle of its own.
--
-- A fresh list each call, so nothing outside can reorder what a walk reads.
function Claims.rects_for(
    claims: Claims,
    screen: string,
    element: string
): { Rect }
    local rects = claims.rects_by_subject[subjectKey(screen, element)]
    if rects == nil then
        return {}
    end
    return table.clone(rects)
end

-- Whether anything this project claims can only be measured by reading a region.
--
-- It exists for the composition root rather than for the walk: reading costs an
-- optional OCR engine binding, so whoever assembles the run asks this BEFORE the
-- walk starts and refuses in its own words, naming the missing flag. The walk
-- never asks -- by then the engine is there or the read fails as a control error.
function Claims.reads_text(claims: Claims): boolean
    return next(claims.text_by_cell) ~= nil
end

-- The screens in the order the host serves their pixels: by content hash.
--
-- This order is a CONTRACT with the frame source. A screen's file is
-- `assets/screens/<hash>.png`, the host's file-backed frame source serves that
-- directory one file per capture in file-name order, and the walk opens one
-- observation per screen, so sorting by hash pairs the Nth capture with the Nth
-- screen. The two sides walk the same set because `regress.check` reads every
-- declared screen's file -- a screen with no file fails by name -- and the host
-- refuses the run unless it served as many frames as it holds files.
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
