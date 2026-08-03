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
-- `recognition` sorts after `model`, `observe`, `oracle`, `project` and
-- `reading`, and before `regress`. It holds no state: every verb takes the model
-- it judges and, where pixels are involved, the ctx and ticket.

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

-- recognition.needs_engine(built) -> whether verifying this project's screen
-- declarations will read a region.
--
-- It exists for the composition root, beside `oracle.Claims.reads_text`: reading
-- costs an optional OCR engine binding, and whoever assembles a run must refuse
-- in its own words, naming the flag, rather than let a read fail on screen three.
-- `reads_text` answers for the CLAIMED cells and cannot answer for these, since a
-- page identified by what its title box reads is read on every screen that
-- declares it whether or not any cell claims a text.
--
-- It re-reads a rule `observe.resolve_page` owns -- an identify row on an element
-- with no appearances is measured by reading it -- rather than re-stating it: it
-- decides only whether an engine will be WANTED, so a drift can at worst ask for
-- a flag the run does not need.
function recognition.needs_engine(built: any): boolean
    for _, screen in built.claims.screens do
        if screen.page ~= nil then
            local page = pageOf(built, screen)
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
