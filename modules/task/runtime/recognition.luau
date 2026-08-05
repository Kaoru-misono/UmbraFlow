--!strict
-- recognition: the page half of the falsification matrix. A screen may say which
-- page it is OF (`oracle.Screen`), and that word is the only thing in the model
-- that can say two pictures are two views of ONE page. Two rules rest on it:
--
--   * A repeated text is only a confusion between two PAGES. One scrolling grid
--     captured at two scroll positions, with the page's anchors outside the
--     scrolling area, reads the same title in the same box on both -- the
--     property being verified rather than a hole in it.
--   * A declaration is the only claim that measures a WHOLE signature. Every
--     other cell measures one element on one screen, while a page is a
--     conjunction over the rows that identify it, so without this a model could
--     pass every cell and still hold a page that resolves on nothing. `verify`
--     resolves that page on that screen, on the frame the cells came off.
--
-- The answer is a sentence rather than a boolean because
-- `observe.resolve_page` already builds the one saying which clause failed and
-- what the region read instead; a second copy is a second thing that can come to
-- disagree with the model.
--
-- TWO DIRECTIONS, ONE OF WHICH IS NEVER JUDGED. `verify` asks whether the page a
-- screen DECLARES resolves there, and its answer is a finding. `sweep` asks
-- which of every declared page resolves there, and its answer is a measurement
-- and nothing else -- because "a screen declaring page P resolves no other
-- declared page" is false on this model and stays false: an overlay screen
-- resolves the page underneath it, correctly, and the model holds no relation
-- saying which page an overlay sits over
-- (docs/plans/2026-08-01-three-layers-and-agent-operator.md, "Recognition asks
-- one direction only"). Until a page can declare what it covers, a co-resolution
-- is a fact about the corpus and a defect about nothing.
--
-- `recognition` sorts after `model`, `observe`, `oracle`, `project` and
-- `reading`, and before `regress`. It holds no state: every verb takes the model
-- it judges and, where pixels are involved, the ctx and ticket.

-- One page's answer on one frame: whether its signature resolved, and when it
-- did not, `observe.resolve_page`'s own sentence naming the clause that failed.
export type Resolution = {
    page: string,
    resolved: boolean,
    why: string?,
}

-- Two pages whose identify clauses stand in a containment relation: every clause
-- `general` names, `specific` names too.
--
-- What follows is about ORDER and holds whatever the extra clauses are: a frame
-- that resolves `specific` resolves `general` as well, so a loop that offers
-- `general` first can never reach `specific`.
--
-- `extra_forbidden` COUNTS AND EXPLAINS NOTHING. It is how many of the extra
-- clauses are refusals, and that number arbitrates no pair: `specific` never
-- resolves outside its own set whatever the polarities, so an extra required
-- clause separates the two exactly as much as an extra forbidden one. The
-- configuration that WOULD arbitrate a pair -- `general` refusing a mark
-- `specific` requires -- produces no row here at all, because polarity is part
-- of the clause key and containment fails on it. Read this field as a fact about
-- the clauses, never as a reason the order is safe.
--
-- `extra` of zero is the degenerate case worth reading twice: two pages with the
-- same clauses cannot be told apart on any frame, by this loop or any other.
export type AnchorSubset = {
    general: string,
    specific: string,
    extra: number,
    extra_forbidden: number,
}

local recognition = {}

-- The page one screen says it is of, or nil when it says nothing -- including
-- when the claims hold no screen of that name at all, which is the same silence
-- for the one caller below and is refused by `Claims.new` for every other.
local function declaredPage(claims: any, screenName: string): string?
    local screen = claims.screen_by_name[screenName]
    if screen == nil then
        return nil
    end
    return screen.page
end

-- recognition.same_page(claims, screen, rival) -> whether these two screens are
-- KNOWN to be two views of one page.
--
-- UNDECLARED IS NEVER "THE SAME PAGE": two screens are one page when both say
-- so, and a screen that says nothing has said nothing.
function recognition.same_page(
    claims: any,
    screen: string,
    rival: string
): boolean
    local declared = declaredPage(claims, screen)
    return declared ~= nil and declared == declaredPage(claims, rival)
end

-- The page `screen` says it is, resolved against the model that holds it. A page
-- the model does not hold is a RAISE and never a finding: a finding says the
-- model is wrong about the target, while this says there is no signature to
-- measure at all. Both doors into a model refuse it already (`project.build`,
-- `scribe.add_screen`), so reaching the walk with one means a construction error.
local function pageOf(built: any, screen: any): any
    local pages: any = built.page_by_name
    local page       = if type(pages) == "table" then pages[screen.page] else nil
    if page == nil then
        error(
            "screen '"
                .. screen.name
                .. "' says it is page '"
                .. tostring(screen.page)
                .. "', which this model does not declare; there is no signature "
                .. "to resolve, so the screen's own claim cannot be measured at "
                .. "all",
            0
        )
    end
    return page
end

-- recognition.verify(ctx, ticket, built, screen) -> string?
--
-- Whether the frame behind `ticket` really is the page this screen says it is:
-- nil when it resolved, and `observe.resolve_page`'s own sentence when it did
-- not.
--
-- A screen that declares no page costs nothing -- no search, no read, no row.
--
-- It asks this direction only, and never which OTHER declared pages resolve
-- here (docs/plans/2026-08-01-three-layers-and-agent-operator.md, "Recognition
-- asks one direction only").
--
-- It resolves on the CALLER'S ticket. The walk opens one observation per screen
-- and the host serves one file per capture, so a resolution on a second ticket
-- would be the next screen's pixels.
--
-- The receipt is dropped: it authorises a click, and a matrix delivers no input.
function recognition.verify(
    ctx: any,
    ticket: any,
    built: any,
    screen: any
): string?
    if screen.page == nil then
        return nil
    end

    local receipt, why = observe.resolve_page(ctx, ticket, pageOf(built, screen))
    if receipt ~= nil then
        return nil
    end
    return why
end

-- recognition.sweep(ctx, ticket, built) -> one row per declared page, saying
-- which of them resolve on the frame behind `ticket`.
--
-- The whole model, in `built.pages` order, on ONE ticket: a walk opens one
-- observation per screen and the host serves one file per capture, so a second
-- ticket would be the next screen's pixels. Every page is asked, including the
-- one a screen declares -- a sweep that skipped it would answer a different
-- question on the screens that declare something than on the ones that do not.
--
-- The ceiling is one read per identify row measured by reading, over every page,
-- and the real cost is far under it: `observe.resolve_page` stops at the first
-- clause that fails and most pages fail on their first. Pages that share an
-- element share its reading too -- the host answers one rectangle once per frame
-- (`task::CycleAnswers`) -- so a sweep costs distinct rectangles, not rows.
--
-- The receipts are dropped. They authorise a click, and a matrix delivers no
-- input.
function recognition.sweep(ctx: any, ticket: any, built: any): { Resolution }
    local rows: { Resolution } = {}
    for index = 1, #built.pages do
        local page            = built.pages[index]
        local receipt, why    = observe.resolve_page(ctx, ticket, page)
        rows[index]           = {
            page     = page.name,
            resolved = receipt ~= nil,
            why      = why,
        }
    end
    return rows
end

-- recognition.declared_refusal(built, screen, resolutions) -> the sentence saying
-- why the page this screen DECLARES did not resolve, or nil when it did or when
-- the screen declares nothing.
--
-- It reads a sweep rather than resolving again: the two would ask the same
-- question of the same frame, and the second answer would be a second thing that
-- can come to disagree. `verify` remains the verb for a caller that wants this
-- one page and is not sweeping.
function recognition.declared_refusal(
    built: any,
    screen: any,
    resolutions: { Resolution }
): string?
    if screen.page == nil then
        return nil
    end

    local page = pageOf(built, screen)
    for _, row in resolutions do
        if row.page == page.name then
            return if row.resolved then nil else row.why
        end
    end

    error(
        "the sweep this screen's verdict was read from holds no row for page '"
            .. page.name
            .. "', which this model declares; a sweep that does not cover every "
            .. "declared page cannot answer for any of them",
        0
    )
end

-- The identify clauses of one page, as the keys a containment test compares.
--
-- A clause is the element, the polarity, and WHAT is checked: an element with
-- appearances is folded across its templates, and one with none is measured
-- against the text this reference expects there. The text belongs in the key
-- because five pages of the reference project share one `page_title` element and
-- are told apart by nothing else -- a key on the element name alone would call
-- them one signature.
-- One text expectation as a key: the kind, then what it asks for. Two rows
-- asking different things of one region are different clauses, so a page pinning
-- a title to one string and a page asking only that it read something are not
-- reported as one signature.
--
-- WHAT THIS DOES NOT SEE, and it is the containment the new kinds introduce:
-- these keys are compared for EQUALITY, so an `expected_presence` clause and an
-- `expected_text` clause on one element read as unrelated -- while in truth the
-- first is implied by the second, and a page resting on presence alone swallows
-- every page that pins the same region to a string. Seeing that needs clause
-- IMPLICATION rather than set containment, which is a different algorithm from
-- the one below. Until it exists the matrix reports no row for that pair, which
-- is silence and not a clean bill.
local function expectationKey(expectation: any): string
    if expectation == nil then
        return "nil"
    end
    if expectation.kind == "equals" then
        return "equals\0" .. expectation.text
    end
    if expectation.kind == "fragments" then
        return "fragments\0" .. table.concat(expectation.fragments, "\0")
    end
    return expectation.kind
end

local function identifyClauses(page: any): { [string]: string }
    local clauses: { [string]: string } = {}
    for _, reference in page.references do
        if reference.exercised.identify then
            local element = reference.element
            local checked = if #element.appearances == 0
                then expectationKey(model.Reference.text_expectation(reference))
                else "*"
            clauses[reference.identify .. "\0" .. element.name .. "\0" .. checked] =
                reference.identify
        end
    end
    return clauses
end

-- recognition.anchor_subsets(built) -> every ordered pair of pages whose identify
-- clauses stand in a containment relation.
--
-- It costs no capture and no read: containment between two conjunctions of
-- clauses is decided by the file. What it CANNOT decide is the case the pixels
-- own -- two pages with disjoint clause names whose marks happen to sit on the
-- same frames -- so this is one half of a co-resolution report and never the
-- whole of it.
--
-- Pages with identical clauses appear once, under the name that sorts first, so
-- that a pair which is a subset both ways is one row rather than two.
function recognition.anchor_subsets(built: any): { AnchorSubset }
    local clausesByPage: { [string]: { [string]: string } } = {}
    local counts: { [string]: number }                      = {}
    for _, page in built.pages do
        local clauses          = identifyClauses(page)
        clausesByPage[page.name] = clauses
        local size             = 0
        for _ in clauses do
            size += 1
        end
        counts[page.name] = size
    end

    local subsets: { AnchorSubset } = {}
    for _, general in built.pages do
        for _, specific in built.pages do
            if general.name ~= specific.name then
                local extra = counts[specific.name] - counts[general.name]
                local mutual = extra == 0
                if extra >= 0 and not (mutual and specific.name < general.name) then
                    local contained     = true
                    for clause in clausesByPage[general.name] do
                        if clausesByPage[specific.name][clause] == nil then
                            contained = false
                            break
                        end
                    end
                    if contained then
                        local forbidden = 0
                        for clause, polarity in clausesByPage[specific.name] do
                            if
                                polarity == "forbidden"
                                and clausesByPage[general.name][clause] == nil
                            then
                                forbidden += 1
                            end
                        end
                        table.insert(subsets, {
                            general         = general.name,
                            specific        = specific.name,
                            extra           = extra,
                            extra_forbidden = forbidden,
                        })
                    end
                end
            end
        end
    end
    return subsets
end

-- recognition.needs_engine(built, swept) -> whether resolving this project's
-- pages will read a region.
--
-- It exists for the composition root, beside `oracle.Claims.reads_text`: reading
-- costs an optional OCR engine binding, and whoever assembles a run must refuse
-- in its own words, naming the flag, rather than let a read fail on screen three.
-- `reads_text` answers for the CLAIMED cells and cannot answer for these, since a
-- page identified by what its title box reads is read on every screen the walk
-- opens.
--
-- WHICH PAGES ARE ASKED ABOUT DEPENDS ON `swept`, and it is a parameter rather
-- than an assumption because the two answers differ on real projects. A sweeping
-- walk offers every declared page to every screen, so a page NO SCREEN NAMES
-- still costs a read; a walk that does not sweep resolves only the page each
-- screen declares, and demanding an engine for the others would name
-- `--ocr-models` on a run where that flag changes nothing.
--
-- It re-reads a rule `observe.resolve_page` owns -- an identify row on an element
-- with no appearances is measured by reading it -- rather than re-stating it: it
-- decides only whether an engine will be WANTED, so a drift can at worst ask for
-- a flag the run does not need.
function recognition.needs_engine(built: any, swept: boolean): boolean
    if type(swept) ~= "boolean" then
        error(
            "recognition.needs_engine has to be told whether the walk sweeps; "
                .. "the pages a run reads are every declared page when it does "
                .. "and only the pages screens name when it does not, and a "
                .. "missing answer would refuse a run over a flag it never needs",
            2
        )
    end

    local declared: { [string]: boolean } = {}
    if not swept then
        for _, screen in built.claims.screens do
            if screen.page ~= nil then
                declared[screen.page] = true
            end
        end
    end

    for _, page in built.pages do
        if swept or declared[page.name] then
            for _, reference in page.references do
                if
                    reference.exercised.identify
                    and #reference.element.appearances == 0
                then
                    return true
                end
            end
        end
    end
    return false
end

return recognition
