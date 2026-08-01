--!strict
-- recognition: the page half of the falsification matrix.
--
-- WHAT IT IS FOR. A screen may say which page it is OF (`oracle.Screen`), and
-- that one word is the only thing in the model that can say two pictures are two
-- views of ONE page. Two rules rest on it, and neither can be stated without it:
--
--   * A REPEATED TEXT IS ONLY A CONFUSION BETWEEN TWO PAGES. `reading.confusions`
--     finds every pair of screens one element's claims cannot tell apart, which
--     is exactly right as a FACT and wrong as a defect: a project that captured
--     one scrolling grid at two scroll positions -- with the page's anchors
--     deliberately outside the scrolling area -- reads the same title in the same
--     box on both, and that is the property being verified rather than a hole in
--     it. The finding's own sentence says why the pair matters, "a page signature
--     resting on it resolves on whichever one is in front of the run", and when
--     the two screens are one page either answer is the right one.
--   * A DECLARATION IS ITSELF A CLAIM, AND IT IS THE ONLY ONE THAT MEASURES A
--     WHOLE SIGNATURE. Every other cell of the matrix measures one element on one
--     screen. A page is a CONJUNCTION over the rows that identify it, and until a
--     screen could name one, nothing walked such a conjunction end to end -- so a
--     model could pass every cell and still hold a page that resolves on nothing.
--     The declaration therefore buys its own silence: `verify` resolves that page
--     on that screen, on the same frame the cells are measured off.
--
-- WHY THE ANSWER IS A SENTENCE AND NOT A BOOLEAN. `observe.resolve_page` already
-- builds the one sentence saying which clause failed and what the region read
-- instead. A verb that answered `false` here would either throw that away or make
-- `regress` write a second, worse copy of it -- and a second copy of a sentence
-- about a model is a second thing that can come to disagree with the model.
--
-- IT ASKS ONE DIRECTION AND NOT THE OTHER, and that is a ruling rather than an
-- omission. "A screen declaring page P must resolve NO OTHER declared page" looks
-- like the same evidence for free, and it is false on this model: a page
-- signature names the marks that IDENTIFY a page and is never an inventory of its
-- screen (`oracle`, "three states and not two"), so the card-detail overlay IS
-- the battle screen with a card selected, every battle anchor still on it and
-- merely dimmed. A screen of the overlay page resolves the base page too, and
-- correctly. Nothing in the model says which page an overlay sits on -- there are
-- `push` and `pop` edges and no base relation -- so the exception could not even
-- be written down, and a rule that fires on every correct overlay is one a
-- project switches off, taking the rules that do work with it. The cost is the
-- second argument and not the first: resolving every declared page on every
-- screen multiplies a check's reads by the page count, and that budget is a
-- number the host fixes from the file before the VM exists
-- (entry/cli/check.cpp). What the other direction was reaching for is already
-- measured, per element, where it CAN be stated -- an appearance that hits a
-- screen it does not own is a misfire cell, two appearances hitting one screen is
-- `ambiguous_appearances`, and one region reading one text on two different pages
-- is the first rule above.
--
-- LOAD ORDER. `recognition` sorts after `model`, `observe`, `oracle`, `project`
-- and `reading`, and before `regress` -- which is what lets it name `observe` and
-- lets `regress` name it. It holds no state: every verb takes the model it judges
-- and, where pixels are involved, the ctx and ticket it judges them through.

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
-- UNDECLARED IS NEVER "THE SAME PAGE", AND THAT IS THE RULING THE THIRD CASE
-- NEEDED. Silence costs a finding, so silence has to be BOUGHT with a fact: two
-- screens are one page when both say so, and a screen that says nothing has said
-- nothing. The direction is picked from what each mistake costs, not from which
-- is tidier.
--
-- A false finding is one sentence in a report, and the way to clear it is to
-- write down something true -- which the matrix then MEASURES, since a declared
-- page has to resolve on its screen. So the escape from this rule is not a way to
-- silence it; it is a second claim, and a wrong one is louder than what it
-- silenced. A missed finding is the opposite in every respect: a page signature
-- that resolves on whichever screen the run happens to be looking at, discovered
-- at run time, in a project whose report was green.
--
-- The usual objection to failing loud -- a rule that cries wolf gets switched off
-- -- is about rules with no answer to give. This one names the screens, and the
-- answer is one field on each. A rule that goes quiet whenever somebody forgot to
-- label a screen would be worth strictly less than the labels it waits for.
function recognition.same_page(
    claims: any,
    screen: string,
    rival: string
): boolean
    local declared = declaredPage(claims, screen)
    return declared ~= nil and declared == declaredPage(claims, rival)
end

-- The page `screen` says it is, resolved against the model that holds it.
--
-- A PAGE THE MODEL DOES NOT HOLD IS A RAISE AND NEVER A FINDING. A finding says
-- the model is wrong about the target; this says the model is not a model --
-- there is no signature to measure and no reading of the screen that could be
-- right or wrong. Both doors into a model refuse it already (`project.build`,
-- `scribe.add_screen`), so reaching the walk with one means a model was assembled
-- past both, and reporting it as a measured defect would file a construction
-- error under evidence about the target.
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
-- A SCREEN THAT DECLARES NO PAGE COSTS NOTHING AT ALL -- no search, no read and
-- no row. That is the same asymmetry `reading` draws on an unclaimed text cell:
-- there is no question in it, rather than a question this declined to answer.
--
-- IT RESOLVES ON THE CALLER'S TICKET, which is what makes this evidence about the
-- screen being measured rather than about a frame of its own. The walk opens one
-- observation per screen and the host serves one file per capture, so a
-- resolution on a second ticket would be the NEXT screen's pixels.
--
-- The receipt is deliberately dropped. It authorises a click, and a matrix
-- delivers no input; what is wanted here is only whether one exists.
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
-- IT EXISTS FOR THE COMPOSITION ROOT, beside `oracle.Claims.reads_text` and for
-- the same reason: reading costs an OCR engine, the engine is an optional
-- binding, and whoever assembles a run has to refuse in its own words -- naming
-- the flag -- rather than let a read fail on screen three. `reads_text` answers
-- for the CLAIMED cells and cannot answer for these: a page identified by what
-- its title box reads is read on every screen that declares it, whether or not
-- any cell of that screen claims a text at all.
--
-- IT REPEATS ONE FACT `observe.resolve_page` OWNS -- that an identify row on an
-- element with no appearances is measured by reading it -- and that is the one
-- thing here worth watching. It is a second reader of that rule and not a second
-- copy of the rule: it decides only whether an engine will be WANTED, so the
-- worst a drift can do is ask for a flag the run turns out not to need. The
-- opposite mistake, deciding what a reading means, stays where it is.
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
