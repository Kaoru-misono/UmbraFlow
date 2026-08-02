--!strict
-- scribe: measuring a screen and writing down what was measured.
--
-- This is the authoring loop of docs/plans/2026-08-01-agent-front-end-and-
-- exploration.md 1, in one place: crop a rectangle, measure its pixels, put the
-- crop in the project as a template asset, build the element that searches for
-- it, claim what it should do on a screen, save the file. Every step of that
-- chain used to live in the v4 authoring CLI, in C++, addressed over a command
-- line; here it is the layer that OWNS the model doing its own writing, which is
-- the whole point of the model having moved up.
--
-- NOT EVERY ELEMENT IS AUTHORED FROM PIXELS. `author_text_element` writes the
-- other kind -- a rectangle that verifies itself by the text it reads -- and it
-- takes no measurement, because there is nothing to cut and nothing to store.
-- The two verbs stand side by side rather than one taking a mode flag, so the
-- verification source an agent chose is visible in the line it wrote.
--
-- AND NOT EVERY ELEMENT AUTHORED FROM PIXELS KNOWS WHERE IT SITS.
-- `author_unplaced_element` writes the third kind: the same crop, the same
-- template asset, and no rectangle at all -- a minimap shape matched at
-- coordinates the script works out per frame, a confirm button drawn once and
-- reused on nine screens. It is a third verb for the reason the second one is a
-- second: which KIND of element a line authors has to be legible on that line,
-- and a `rect = nil` in a spec table is a hole a reader cannot tell from a
-- forgotten field. The three read as one set -- `author_element` is placed and
-- verified by pixels, `author_text_element` is placed and verified by a reading,
-- `author_unplaced_element` is verified by pixels and placed by whoever uses it.
--
-- THE CLAIM SIDE SPLITS ON THE SAME LINE, AND FOR THE SAME REASON. `claim` says
-- what an element's TEMPLATES should do on a screen and `claim_text` says what
-- its rectangle should READ there. An element authored by
-- `author_text_element` can only be claimed by the second, which is not this
-- file's ruling but `oracle.Expectation.new`'s -- so the pairing is one an agent
-- can see rather than one it discovers from a refusal.
--
-- NO THRESHOLD IS DECIDED HERE, AND NONE IS DEFAULTED. Both verbs that build an
-- element out of a measurement take the threshold as a required field, exactly
-- as the project file stores one.
-- The measurement this module hands back says how many pixels a colour key
-- takes and how many the rectangle holds; whether that is good enough is a
-- judgement, judgements belong to the agent and to the developer reading its
-- work, and a number this file picked would be a judgement nobody could argue
-- with because nobody would see it. The one thing it does derive is a FILE NAME,
-- which is not a judgement: a template asset is named by the hash of its own
-- bytes so that "which pixels" and "which file" cannot disagree.
--
-- A PROJECT CAN BE STARTED FROM NOTHING, WHICH IS WHAT `add_page` IS FOR. Every
-- page in every project used to come out of the file, so this channel could add
-- rows to pages somebody else had written and could not write the first one --
-- and a project with no page at all refuses every row, because a row is a row ON
-- a page. The verb takes the page's ROWS and not just its name, because there is
-- no legal empty page to create and fill in afterwards: `Page.new` refuses a page
-- with no required identify reference, so a page is born complete or not at all.
--
-- A PAGE IS FROZEN, SO WRITING ON ONE REBUILDS IT. `add_reference` does not
-- append a row to a page's reference list -- it cannot, and it should not want
-- to: `Page.new` is where a second row for one element, an exercised capability
-- the element does not declare, and a page left with no required identify are
-- all caught, and appending behind its back would skip every one of them. So the
-- page is built again from its own rows plus the new one.
--
-- AND THEN EVERY EDGE THAT NAMED IT IS BUILT AGAIN TOO. That half is the one
-- that is easy to skip and hard to notice, because skipping it breaks nothing
-- that looks broken: a stale edge still carries the page's NAME, so `Graph.new`
-- accepts it and the file it saves is correct in every byte. What is wrong is
-- only visible by identity -- `observe.walk_edge` resolves `edge.from` itself,
-- so it would go on resolving the superseded page, the one missing the reference
-- the agent just wrote, and looking for a row that is not there. The remap is
-- therefore not tidiness; it is the whole of what makes the new row reachable.
--
-- AN ELEMENT IS FROZEN TOO, AND `add_appearance` IS THE SAME REBUILD ONE LEVEL
-- DOWN. A crest met on day one and a second crest met on day two are two ways
-- ONE rectangle can look, so the second belongs on the element the file already
-- declares -- and there was no way to put it there, because `add_element`
-- refuses a name the project has and a frozen element cannot be written on.
-- Every project that hit it hand-edited the file instead, which is the shape
-- this repository has a name for.
--
-- THE RE-POINTING IS THE WHOLE OF IT, exactly as it is for a page. A reference
-- row holds the ELEMENT and not its name, and `observe.find` searches
-- `reference.element.appearances`, so a page left holding the superseded element
-- goes on searching yesterday's list -- while the file it saves is right in every
-- byte, because the file names the element and a NAME never goes stale. Nothing
-- downstream can notice; a match would quietly use the wrong appearance list. So
-- every page that referenced the element is built again, every edge that named
-- one of those pages or clicked the element is built again after them, and an
-- element an agent is still holding from before is refused rather than believed.
--
-- THE SAME HOLE, ONE SIZE SMALLER, SITS ON THE CLAIM SIDE. Every screen in
-- every project used to come out of the file by hand too, and `claim` and
-- `claim_text` refuse a screen name they cannot resolve for the same reason
-- `add_reference` refuses an element the file does not carry: addressing what
-- does not exist yet is not a claim, it is a typo the file would accept and
-- open onto nothing. `scribe.add_screen` closes it the way `add_page` closed
-- the page side -- except a screen is a name, a content hash and at most the
-- name of the page it is of, with no rows and no signature to be born complete
-- or incomplete about, so there is no analogue of "born complete" to rule on
-- here; `oracle.Screen.new` makes every ruling about the screen ITSELF, and the
-- two this file makes are both about the rest of the project. Whether it should
-- also read the PNG
-- off disk before believing in it is answered at `scribe.add_screen`'s own
-- definition rather than here, because the answer turns on how `Screen.new`
-- and `regress.check` already split that work between them.
--
-- LOAD ORDER. `scribe` sorts after `explore`, `model`, `navigation`, `oracle`
-- and `project`, which is the order it needs: it names all five. It also names
-- `mint`, for the bounded inheritance walk that tells the element the model
-- holds from one derived off it, and `mint` sorts before every module here. It
-- sorts before `task`, which it does not name.

local k_templateDirectory = "assets/templates/"
local k_templateSuffix    = ".png"

local scribe = {}

-- Where the template blob with content hash `hash` lives, relative to the
-- project root.
--
-- Derived rather than authored, for the reason oracle.screen_path is: one fact
-- about a template (which bytes it is) must not be able to disagree with a
-- second fact (where they are). It follows the store the runtime already reads.
function scribe.template_path(hash: string): string
    if type(hash) ~= "string" or #hash ~= 64 or string.match(hash, "^[0-9a-f]+$") == nil then
        error(
            "scribe.template_path needs the 64-character lowercase hex content "
                .. "hash explore.crop returned",
            2
        )
    end
    return k_templateDirectory .. hash .. k_templateSuffix
end

-- What one measurement of one rectangle produced.
export type Measurement = {
    -- The encoded crop, and the hash of exactly these bytes.
    png: string,
    hash: string,

    -- Where those bytes belong in the project, derived from the hash.
    source: string,

    -- The rectangle, in frame pixels, as the caller asked for it.
    rect: { x: number, y: number, width: number, height: number },

    -- What `probe` said about it. Carried whole rather than summarised: the
    -- fields are the host's vocabulary and reducing them here would be this file
    -- forming an opinion.
    stats: any,

    -- What the colour key took out of the crop, as the CROP verb reported it,
    -- or nil when there was no key. It is the host's own handle -- the same
    -- selected counts `stats` carries, measured on the same pixels by the same
    -- rule -- plus the `warning` sentence the host attaches when those counts
    -- look like a mask that cannot measure anything, and the key it actually
    -- applied.
    mask: any,

    -- That key as four plain numbers, ready to be written into an appearance.
    -- It is read off `mask` rather than off the caller's argument, because a
    -- caller that named no tolerance got the host's default and the project file
    -- has to record the key that really cut these pixels.
    key: { red: number, green: number, blue: number, tolerance: number }?,
}

-- scribe.measure(ctx, ticket, rect, key?, tolerance?) -> Measurement
--
-- One crop and one probe of the same rectangle of the same frame. They are one
-- verb because they are one question -- "what is here" -- asked of pixels that
-- must be the SAME pixels: cropping on one frame and probing a crop of another
-- would report counts about a screen that had already moved on.
--
-- THE KEY REACHES BOTH HALVES, WHICH IS THE WHOLE POINT OF IT BEING ONE VERB.
-- It used to reach only the probe, so an agent measured a masked selection and
-- then stored an UNMASKED template -- the counts described one thing and the
-- file held another. The same key now cuts the crop as well, so the pixels the
-- probe measured are the pixels the template compares. An agent with no key yet
-- passes none and gets exactly the bytes it always got; choosing a key is done
-- by cropping unkeyed first and probing that blob, which is what `explore.probe`
-- is for.
--
-- IT DECIDES NOTHING ABOUT THE KEY, on the same line the threshold is decided
-- on. Whether a key is worth using, and which colour it should be, are the
-- agent's; the host refuses only the one key that cannot work at all -- one that
-- selects no pixel, whose template would be fully transparent -- and warns about
-- counts that historically measured nothing. Both come back in `mask` for the
-- caller to read.
--
-- It spends one crop out of this cycle's budget and does not consume the cycle,
-- so a caller may go on to click on the same frame it measured.
function scribe.measure(
    ctx: any,
    ticket: any,
    rect: { x: number, y: number, width: number, height: number },
    key: { red: number, green: number, blue: number }?,
    tolerance: number?
): Measurement
    if type(ctx) ~= "table" then
        error("scribe.measure needs the ctx it should load templates through", 2)
    end
    if type(rect) ~= "table" then
        error(
            "scribe.measure needs rect = { x = , y = , width = , height = }",
            2
        )
    end

    local png, hash, mask = explore.crop(
        ticket,
        rect.x,
        rect.y,
        rect.width,
        rect.height,
        key,
        tolerance
    )
    local stats = explore.probe(
        png,
        0,
        0,
        rect.width,
        rect.height,
        key,
        tolerance
    )

    local applied: { red: number, green: number, blue: number, tolerance: number }? =
        nil
    if mask ~= nil then
        applied = {
            red       = mask.key_red,
            green     = mask.key_green,
            blue      = mask.key_blue,
            tolerance = mask.tolerance,
        }
    end

    return {
        png    = png,
        hash   = hash,
        source = scribe.template_path(hash),
        rect   = {
            x      = rect.x,
            y      = rect.y,
            width  = rect.width,
            height = rect.height,
        },
        stats  = stats,
        mask   = mask,
        key    = applied,
    }
end

-- scribe.save_template(ctx, measurement) -> source
--
-- Puts the measured crop in the project at the path its own hash names, and
-- hands that path back so an appearance can carry it.
--
-- Writing the same bytes twice is a no-op in effect rather than an error: the
-- name is the hash, so a second write replaces a file with itself. That is what
-- makes authoring two elements that happen to share a crop safe.
function scribe.save_template(ctx: any, measurement: Measurement): string
    if type(ctx) ~= "table" then
        error("scribe.save_template needs the ctx to write the file through", 2)
    end
    if type(measurement) ~= "table" or type(measurement.png) ~= "string" then
        error(
            "scribe.save_template needs the measurement scribe.measure returned",
            2
        )
    end

    ctx:project_write(measurement.source, measurement.png)
    return measurement.source
end

-- The one sentence every verb that takes a threshold out of a spec refuses with.
-- It comes BACK rather than being raised, which is `mint`'s convention and
-- `model`'s: each caller raises at its own level, so the agent reads the line it
-- wrote. There is one of it because a second spelling of "no default" is a
-- second place a default could grow.
local function thresholdMissing(verb: string): string
    return verb
        .. " needs threshold = the basis points this appearance must match "
        .. "within; it has no default, because a threshold nobody chose is a "
        .. "decision nobody can argue with"
end

-- The element both measurement-fed verbs build, differing only in the rectangle
-- they hand it.
--
-- IT IS ONE FUNCTION BECAUSE EVERYTHING EXCEPT THAT RECTANGLE IS ONE DECISION.
-- The threshold is the caller's and has no default, the template asset is
-- written before the element that names it can exist, and the appearance records
-- the key the HOST applied rather than one a spec could name. A second copy of
-- those three would be a second place a threshold could grow a default, a second
-- place a key could stop being written down, and a second place an appearance
-- could start pointing at a file nobody stored -- which is the drift the module
-- header calls out for rulings and is no different for construction.
--
-- `verb` is only the name a refusal reaches the agent under, and the refusal is
-- raised at level 3 so it names the line that called the verb rather than this
-- one -- the same reason `requireDeclaredElement` and `pageNamed` below use it.
-- `rect` is passed rather than read off `spec`, because what supplies it is
-- exactly what the two verbs disagree about: one takes the measured rectangle
-- unless the spec refines it, and the other has already refused a spec that
-- names one at all.
local function elementFromMeasurement(
    ctx: any,
    measurement: Measurement,
    spec: { [string]: any },
    verb: string,
    rect: any
): any
    if type(spec.threshold) ~= "number" then
        error(thresholdMissing(verb), 3)
    end

    local source = scribe.save_template(ctx, measurement)

    return model.Element.new({
        name          = spec.name,
        capabilities  = spec.capabilities,
        rect          = rect,
        appearances   = {
            {
                name      = spec.appearance,
                source    = source,
                template  = ctx:template_load(measurement.png),
                threshold = spec.threshold,
                key       = measurement.key,
            },
        },
        expected_text = spec.expected_text,
        read_floor    = spec.read_floor,
        extra         = spec.extra,
        residual      = spec.residual,
    })
end

-- scribe.author_element(ctx, measurement, spec) -> element
--
-- Builds the element that searches for the measured pixels: one appearance,
-- pointing at the template asset, with the threshold the CALLER decided.
--
-- `spec` carries name, capabilities, threshold, and optionally an appearance
-- name, a rect override and the model's two forward-compatibility fields. The
-- rect defaults to the rectangle that was measured, because that is what the
-- template was cut from and a different one would be a search region nobody
-- measured.
--
-- THAT DEFAULT IS WHY A SHAPE MATCHED AT A RECTANGLE THE SCRIPT WORKS OUT IS NOT
-- AUTHORED THROUGH THIS VERB. `Element.new` lets an element state no rectangle at
-- all, and there is no spelling of "none" here -- an absent `rect` already means
-- "the one I just measured", which is the common case and the honest one for a
-- verb whose whole subject is a measurement. Such an element is authored by
-- `scribe.author_unplaced_element`, which says in its own name that the pixels
-- and the position came from different places.
--
-- It writes the template asset as part of the same call. An element whose
-- appearance names a file the project does not hold is one that loads and then
-- fails at its first search, and there is no reason to be able to build one.
--
-- THE COLOUR KEY RIDES ACROSS FROM THE MEASUREMENT, AND IS NOT DECIDED HERE.
-- A crop cut under a key carries the mask in its own alpha channel, so an
-- element would work perfectly with the key written down nowhere -- and that is
-- exactly the state worth refusing: a mysteriously masked PNG whose author is
-- the only one who ever knew which colour it was cut on, and who cannot redraw
-- the rectangle a year later without guessing. The appearance therefore records
-- the key the HOST applied (`measurement.key`), so the file says what was keyed
-- as well as what came out. It is copied, never chosen: `spec` has no key field,
-- because a key that did not cut these pixels would be a caption on someone
-- else's picture.
function scribe.author_element(
    ctx: any,
    measurement: Measurement,
    spec: { [string]: any }
): any
    if type(spec) ~= "table" then
        error("scribe.author_element needs a table of fields", 2)
    end

    return elementFromMeasurement(
        ctx,
        measurement,
        spec,
        "scribe.author_element",
        spec.rect or measurement.rect
    )
end

-- scribe.author_unplaced_element(ctx, measurement, spec) -> element
--
-- Builds the element that searches for the measured pixels ANYWHERE it is put:
-- the same crop, the same template asset, the same caller-chosen threshold, and
-- no rectangle of its own.
--
-- WHY IT IS A THIRD VERB RATHER THAN A FLAG ON THE FIRST. It is the reason
-- `author_text_element` is a second one, one axis over. `author_element`'s rect
-- has exactly one honest reading -- "the rectangle I just measured" -- so the
-- only spelling of "none" a flag could offer is a field left out, and a field
-- left out is indistinguishable from a field forgotten. The kind of element a
-- line authors would then be a thing a reader worked out by NOT finding
-- something, on the one axis where getting it wrong is invisible: an element
-- carrying the rectangle it happened to be cropped from loads, searches, and
-- matches, while stating as a fact about the model something that is only a fact
-- about the crop. Three verbs put the kind in the name -- placed by pixels,
-- placed and read, placed by whoever uses it.
--
-- THE PIXELS ARE STILL A MEASUREMENT, WHICH IS WHY IT TAKES ONE. What is absent
-- is the POSITION, not the crop: this element identifies by a template like any
-- other, so the asset path and the colour key are settled exactly as
-- `author_element` settles them, by the same code, from the same measurement.
-- The rectangle in that measurement is where the pixels were CUT from, and it is
-- deliberately not carried across -- it is the fact the caller is declining to
-- state.
--
-- A `rect` IN THE SPEC IS REFUSED BY NAME. The caller who writes one wanted
-- `author_element`, which would have used it; ignoring it here would author an
-- element with no rectangle from a line that says it has one, and the file would
-- then be right and the call site wrong forever after.
--
-- IT DECIDES NOTHING THIS FILE DOES NOT ALREADY DECIDE. Threshold, appearance
-- name and colour key are `elementFromMeasurement`'s, in one place, under
-- `author_element`'s own rules -- see the "NO THRESHOLD IS DECIDED HERE" contract
-- at the top of this file. What it produces is an ordinary element that
-- `Element.new` accepts, and the rulings that keep "no rectangle" from becoming
-- "nobody said where to look" are then made where they have always been made:
-- `Page.new` refuses a row that states no `rect_override`, refuses such an
-- element in a page signature at all, and `Expectation.new` refuses a claim that
-- says no `rect`.
function scribe.author_unplaced_element(
    ctx: any,
    measurement: Measurement,
    spec: { [string]: any }
): any
    if type(spec) ~= "table" then
        error("scribe.author_unplaced_element needs a table of fields", 2)
    end
    if spec.rect ~= nil then
        error(
            "scribe.author_unplaced_element builds an element that states no "
                .. "rectangle of its own, so its spec carries no rect; the "
                .. "element that is searched at one rectangle everywhere it is "
                .. "used is authored with scribe.author_element, which takes the "
                .. "one the measurement was cut from",
            2
        )
    end

    return elementFromMeasurement(
        ctx,
        measurement,
        spec,
        "scribe.author_unplaced_element",
        nil
    )
end

-- scribe.author_text_element(spec) -> element
--
-- Builds the element that verifies itself by what its rectangle READS: no crop,
-- no template asset, no threshold, and therefore no measurement and no `ctx`.
--
-- WHY IT IS A SECOND VERB RATHER THAN A FLAG ON THE FIRST. `author_element`
-- takes a `Measurement` as its second argument and writes a template asset as
-- part of its work; an element verified by text has neither, so the flag would
-- turn half the parameters of one verb into "ignored in this mode", which is the
-- shape a reader cannot check a call site against. Two verbs make the choice of
-- verification source visible in the line the agent wrote
-- (docs/plans/2026-08-01-three-layers-and-agent-operator.md 5-4: an element
-- declares ONE source).
--
-- `spec` is handed to `Element.new` as written, so every ruling is made in the
-- constructor's own words: an element that identifies with no template of its
-- own has to declare `read`, and a `read_floor` on something nothing may read is
-- refused. A rectangle is optional there and it is optional here: a region drawn
-- once and reused on nine screens is placed by each page's own row, so the
-- element states none and `Page.new` refuses a row that states none either.
-- Nothing here defaults `expected_text`
-- either, and that absence is the interesting case rather than an oversight --
-- the shared title element states no text at all, because what it reads is
-- different on every page and belongs on each page's reference row
-- (`scribe.add_reference`).
function scribe.author_text_element(spec: { [string]: any }): any
    if type(spec) ~= "table" then
        error("scribe.author_text_element needs a table of fields", 2)
    end
    if spec.appearances ~= nil then
        error(
            "scribe.author_text_element builds an element with no template of "
                .. "its own; an element that searches for pixels is authored "
                .. "from a measurement with scribe.author_element",
            2
        )
    end

    return model.Element.new(spec)
end

-- scribe.add_element(built, element) -> ()
--
-- Puts `element` into the loaded model, so the next save writes it out and the
-- next matrix walk measures it.
--
-- The model's element list is the one part of a loaded project that is a plain
-- mutable table -- the elements IN it are frozen, and this adds one rather than
-- changing any. A name the project already declares is refused here rather than
-- at save time, because the caller still has the element in hand and can rename
-- it.
--
-- THE REFUSAL IS NOT WHERE A SECOND CREST GOES. Two crests one node can fly are
-- two ways ONE element looks, so the answer to "this name is taken" is rarely a
-- second element under a second name -- it is `scribe.add_appearance`.
function scribe.add_element(built: any, element: any)
    if type(built) ~= "table" or type(built.elements) ~= "table" then
        error(
            "scribe.add_element needs the model project.load_project returned",
            2
        )
    end
    if not model.is_element(element) then
        error(
            "scribe.add_element needs an element built by model.Element.new",
            2
        )
    end
    if built.element_by_name[element.name] ~= nil then
        error(
            "this project already declares an element named '"
                .. element.name
                .. "'; an element name is how a script and a claim address it, "
                .. "so two of them cannot share one",
            2
        )
    end

    table.insert(built.elements, element)
    built.element_by_name[element.name] = element
end

-- The three verbs below rewrite the page graph, so all three need the whole
-- loaded model rather than a table with a page list on it: they read the element
-- index to refuse a row nothing declares, and they hand the page list back to
-- `Graph.new`.
local function requireModel(built: any, verb: string)
    if
        type(built) ~= "table"
        or type(built.pages) ~= "table"
        or type(built.page_by_name) ~= "table"
        or type(built.element_by_name) ~= "table"
        or not navigation.is_graph(built.graph)
    then
        error(verb .. " needs the model project.load_project returned", 3)
    end
end

-- Refuses a row naming an element this project does not declare, and a row
-- naming an element the project has since replaced.
--
-- It is one sentence rather than one per verb because the two verbs that write
-- rows are refusing the same thing for the same reason, and two spellings of
-- that reason are two sentences that can come to disagree about which of them
-- meant it. The reason is that elements and rows are saved as SEPARATE sections:
-- a row naming an element no `[[element]]` declares is a file this build writes
-- happily and then refuses to parse, and the project it was written from is gone
-- by then.
--
-- `Page.new` cannot make this ruling. It is handed elements, not the project, so
-- the most it can ask is that the table was minted -- whether the file also
-- carries it is a question only the loaded model can answer.
--
-- THE SECOND HALF ASKS BY IDENTITY, AND IT HAS TO. `scribe.add_appearance`
-- REBUILDS an element, so one an agent put in a variable can already be one the
-- model no longer has -- and a row written from it is a page searching an
-- appearance list the file no longer carries, which saves and loads correctly
-- because the file names the element. Only identity can see it, so identity is
-- what is asked; an element derived from the current one with `setmetatable`
-- still passes, on the same bounded walk `model.is_element` uses.
local function requireDeclaredElement(built: any, element: any)
    local declared = built.element_by_name[element.name]
    if declared == nil then
        error(
            "this project declares no element named '"
                .. element.name
                .. "'; a row naming an element the file does not carry saves a "
                .. "project that will not open again, so add the element first",
            3
        )
    end
    if not mint.derives_from(element, { [declared] = true }) then
        error(
            "element '"
                .. element.name
                .. "' is not the one this project holds under that name; adding "
                .. "an appearance rebuilds an element, so an element an agent is "
                .. "holding can already be one the model no longer has, and a row "
                .. "written from it would search yesterday's appearances forever "
                .. "-- take the element from built.element_by_name, or use the one "
                .. "scribe.add_appearance handed back",
            3
        )
    end
end

-- A mutable copy of a caller's spec table.
--
-- The copy exists so a verb can write the object it resolved over the name it
-- was handed while every OTHER key rides through to the constructor untouched --
-- including one the constructor does not own, which is how it gets refused with
-- the model's own sentence instead of quietly dropped. That is the same
-- reservation `extra` rests on, and a spec this file filtered key by key would
-- be a second place it could be lost.
local function specCopy(spec: { [string]: any }): { [string]: any }
    local copy: { [string]: any } = {}
    for key, value in spec do
        copy[key] = value
    end
    return copy
end

-- The capability SET a constructor froze, back as the capability LIST a
-- constructor reads. Both spec rebuilds below need it, and the order comes from
-- `model.capability_order`, which is the one place that order is decided.
local function capabilityList(set: any): { string }
    local names: { string } = {}
    for _, capability in model.capability_order() do
        if set[capability] then
            table.insert(names, capability)
        end
    end
    return names
end

-- One of a page's frozen reference rows, turned back into the spec table
-- `Page.new` accepts. It is exactly what the constructor did, undone: the `page`
-- stamp comes off, because stamping it is the constructor's job and handing it
-- back would be an unknown key, and the capability SET becomes the capability
-- LIST the constructor reads. Everything else -- including `extra` and the raw
-- `residual` lines a newer schema wrote -- is passed straight through, because a
-- rebuild that dropped them would be the silent deletion the file format exists
-- to prevent.
--
-- `expected_text` is the field where that would hurt most, and it is worth
-- naming for it. A row identified by text carries the whole of its page's
-- signature in that one string, so a rebuild that let it fall out would leave a
-- page that either refuses to build (identify with nothing to check) or, if the
-- element happened to carry a fallback, resolves on the wrong screen. Writing
-- ONE new row would be what broke a page nobody touched.
local function referenceSpec(reference: any): { [string]: any }
    return {
        element           = reference.element,
        holding           = reference.holding,
        exercised         = capabilityList(reference.exercised),
        identify          = reference.identify,
        expected_text     = reference.expected_text,
        pinned_appearance = reference.pinned_appearance,
        rect_override     = reference.rect_override,
        extra             = reference.extra,
        residual          = reference.residual,
    }
end

-- One of an element's frozen appearances, turned back into the entry
-- `Element.new` accepts. It is `referenceSpec`'s twin one level down and exists
-- for the same reason: `key` is the colour the blob was cut under and `residual`
-- is the raw lines a newer schema wrote, so a rebuild that dropped either would
-- be a masked picture nobody can redraw and a silent deletion, in memory instead
-- of on disk. `template` rides across as the handle it already is.
local function appearanceSpec(appearance: any): { [string]: any }
    return {
        name      = appearance.name,
        source    = appearance.source,
        template  = appearance.template,
        threshold = appearance.threshold,
        key       = appearance.key,
        residual  = appearance.residual,
    }
end

-- `element`, as the spec table that builds it again, wearing `appearances`
-- instead of its own. The list is the only parameter because it is the only
-- thing a rebuild changes; everything else is handed straight back to the
-- constructor that froze it, so every ruling `Element.new` makes is made again.
local function elementSpec(
    element: any,
    appearances: { any }
): { [string]: any }
    return {
        name          = element.name,
        capabilities  = capabilityList(element.capabilities),
        rect          = element.rect,
        appearances   = appearances,
        expected_text = element.expected_text,
        read_floor    = element.read_floor,
        extra         = element.extra,
        residual      = element.residual,
    }
end

-- `edge`, rebuilt against everything it named that has just been replaced --
-- either end, if `pages` carries a new page under that name, and the element it
-- is taken by clicking, if that is `element` -- and the SAME edge back when it
-- named none of them.
--
-- Returning the identical table matters: `navigation.Graph.has_edge` and
-- `observe.walk_edge` ask whether this exact edge belongs to this exact graph,
-- so minting a new one for an edge nothing happened to would invalidate a handle
-- the caller is entitled to keep holding.
--
-- Rebuilding goes through `Edge.new` rather than through a field swap, which is
-- what re-resolves a click edge's `via_reference` against the page that now
-- holds the rows.
--
-- `element` IS THE HALF ONLY `add_appearance` USES, and it is not redundant with
-- the page remap it always accompanies. `Edge.new` refuses a click edge whose
-- from page does not reference the clicked element, so `pages` alone would
-- already mark such an edge changed -- but it would then be rebuilt still
-- holding the SUPERSEDED element in `via_element`, beside a `via_reference`
-- resolved off the new page. One edge carrying two generations of one element is
-- a disagreement nothing would report.
local function remappedEdge(
    edge: any,
    pages: { [string]: any },
    element: any?
): any
    local from    = pages[edge.from.name]
    local changed = from ~= nil

    local destinations: { any }? = nil
    if edge.to ~= nil then
        local list: { any } = {}
        for index, target in edge.to do
            local moved = pages[target.name]
            if moved ~= nil then
                changed = true
            end
            list[index] = moved or target
        end
        destinations = list
    end

    local clicked = edge.via_element
    if element ~= nil and clicked ~= nil and clicked.name == element.name then
        clicked = element
        changed = true
    end

    if not changed then
        return edge
    end

    return navigation.Edge.new({
        from        = from or edge.from,
        to          = destinations,
        via         = edge.via,
        via_element = clicked,
        via_key     = edge.via_key,
        kind        = edge.kind,
        extra       = edge.extra,
        residual    = edge.residual,
    })
end

-- `page`, built again from `references` and everything else it already carried.
-- Both verbs that rewrite a page's rows go through it -- one adding a row, one
-- swapping the element a row names -- because the part they share is the part
-- that is easy to get wrong: `overlay`, `interrupt`, `extra` and the raw
-- `residual` lines are the page's own facts, and a rebuild that dropped one
-- would be a page silently losing what nobody was looking at.
local function rebuiltPage(page: any, references: { any }): any
    return model.Page.new({
        name       = page.name,
        overlay    = page.overlay,
        interrupt  = page.interrupt,
        references = references,
        extra      = page.extra,
        residual   = page.residual,
    })
end

-- Moves the three copies of a project's page list together, around every page in
-- `replaced`.
--
-- A model keeps the same pages in three places, and each of the three is what
-- something different reads: `built.pages` is the list `save` writes and the
-- list a rebuilt graph is made from, `built.page_by_name` is the index
-- `add_edge` resolves a name through, and `built.graph` is what
-- `observe.walk_edge` resolves an edge against. Two of the three updated is a
-- model that saves a correct file and walks to a page nobody can see, so the
-- three moves are one function rather than a pattern each verb repeats.
--
-- IT TAKES A SET OF PAGES RATHER THAN ONE, because `add_appearance` replaces an
-- element any number of pages reference and rebuilds all of them in one act. A
-- verb touching a single page passes a set of one; a second helper for that case
-- would be a second place the three copies could come apart.
--
-- The graph is built FIRST and assigned LAST, so a page list `Graph.new` refuses
-- would leave the model exactly as it was. No caller reaches that refusal today
-- -- every verb rules out the duplicate name it would catch before it gets here
-- -- and the order is kept anyway, because a helper whose correctness depends on
-- which checks its callers happen to run first is one that breaks the day a
-- fourth caller arrives.
local function installPages(
    built: any,
    pages: { any },
    edges: { any },
    replaced: { [string]: any }
)
    local graph = navigation.Graph.new({ pages = pages, edges = edges })
    built.pages = pages
    for name, page in replaced do
        built.page_by_name[name] = page
    end
    built.graph = graph
end

-- Moves the model's two copies of its element list together with the three
-- copies of its page list, around one rebuilt element.
--
-- An element lives in `built.elements`, which `save` writes, and in
-- `built.element_by_name`, which every name is resolved through -- and beyond
-- those it lives inside every page that references it, which is why the pages
-- are installed here rather than separately. Two calls would leave a window in
-- which the index answers with the new element and a page still holds the old
-- one, which is exactly the state this file exists to prevent.
local function installElement(
    built: any,
    element: any,
    pages: { any },
    edges: { any },
    replaced: { [string]: any }
)
    local elements: { any } = {}
    for index, existing in built.elements do
        elements[index] =
            if existing.name == element.name then element else existing
    end

    installPages(built, pages, edges, replaced)
    built.elements = elements
    built.element_by_name[element.name] = element
end

-- scribe.add_page(built, spec) -> the new page
--
-- Declares a page the file does not have: its name, its flags, and the rows it
-- is BORN with.
--
-- IT IS BORN COMPLETE, WHICH IS WHY THE ROWS ARE A FIELD AND NOT A SEQUEL.
-- `Page.new` refuses a page with no required identify reference, so "make the
-- page, then write its rows onto it" is not a sequence this model can be walked
-- through -- the first step has nothing legal to build. The whole signature goes
-- in at once, and `add_reference` is what a LATER row uses.
--
-- A ROW NAMES ITS ELEMENT BY THE ELEMENT, which is where this spec differs from
-- `add_edge`'s and agrees with `add_reference`'s. An edge names its ends because
-- writing a row REBUILDS a page, so a page an agent put in a variable may
-- already be superseded and a NAME is what always resolves to the one the model
-- holds. An element can be superseded too -- `scribe.add_appearance` rebuilds
-- one and hands the replacement back -- but the two cases end differently, and
-- that is what keeps the object here: a stale element is CAUGHT rather than
-- believed, because `requireDeclaredElement` asks the model for the element of
-- that name and refuses anything that is not it. A name here would buy nothing
-- that refusal does not already give, and would cost spelling the same element
-- twice in three lines and letting the two spellings disagree. Everything else
-- in a row is what `add_reference`'s spec carries, under the same names, because
-- it IS that row.
--
-- TWO QUESTIONS ARE ASKED HERE AND EVERY OTHER ONE IS THE CONSTRUCTOR'S. This
-- file refuses a page name the project already declares, while the caller can
-- still choose another, and a row naming an element the file does not carry.
-- Holding, exercised, the identify polarity, a pin on something nothing clicks,
-- a second row for one element -- all of those are ruled on by `Page.new` in its
-- own words, including the required identify that shaped this verb, which is the
-- last rule this file would want to own a second copy of.
--
-- The spec goes to `Page.new` as written, so `overlay`, `interrupt`, `extra` and
-- the raw `residual` lines ride through untouched and a key the constructor does
-- not own is refused by it rather than dropped here.
--
-- Nothing on the model is touched until `Page.new` and `Graph.new` have both
-- agreed, so a page this refuses leaves the project exactly as it was.
function scribe.add_page(built: any, spec: { [string]: any }): any
    requireModel(built, "scribe.add_page")
    if type(spec) ~= "table" then
        error("scribe.add_page needs a table of fields", 2)
    end
    if type(spec.name) == "string" and built.page_by_name[spec.name] ~= nil then
        error(
            "this project already declares a page named '"
                .. spec.name
                .. "'; a page name is how an edge names its ends and how a saved "
                .. "row names the page it sits on, so two of them cannot share "
                .. "one",
            2
        )
    end

    -- Only the rows this file can walk are asked the one question it alone can
    -- answer. A `references` that is not a list, an entry that is not a table,
    -- and an entry naming something no constructor minted all fall through to
    -- `Page.new` untouched, because the sentence for each of those is already
    -- written there and a second copy of it here would be a second thing to
    -- keep in step.
    if type(spec.references) == "table" then
        for _, row in spec.references :: { any } do
            if type(row) == "table" and model.is_element(row.element) then
                requireDeclaredElement(built, row.element)
            end
        end
    end

    local page = model.Page.new(spec)

    local pages: { any } = table.clone(built.pages)
    table.insert(pages, page)
    -- Every existing edge rides through unchanged: this page is new, so no edge
    -- can already name it, and there is nothing to remap.
    installPages(built, pages, built.graph.edges, { [page.name] = page })
    return page
end

-- scribe.add_reference(built, page_name, element, spec) -> the rebuilt page
--
-- Writes one row on one page: what THIS page does with this element.
--
-- `spec` carries the row's own fields under the model's own names -- holding,
-- exercised, the identify polarity, the expected text this page's copy of the
-- region should read, an optional pinned appearance and rect override -- because
-- the spec IS the row. It is handed to `Page.new` as written, so every ruling
-- that constructor makes is made here in its own words: exercised is a subset of
-- what the element declares, identify carries a polarity, a pin belongs to a
-- click, an identify row does not refine the search rectangle, and an identify
-- row on an element with no template says what that region reads HERE. Nothing
-- is defaulted on the way, and nothing is filtered.
--
-- The last of those is what an agent writes when it annotates a page whose only
-- signature is its own printed name: the title element is authored once, and
-- each page adds `expected_text = "..."` to its own row.
--
-- THE PAGE COMES BACK BECAUSE THE OLD ONE IS GONE. The model's own copies are
-- all replaced -- the page list, the name index, and every edge that named it --
-- but a page an agent left in a variable is not something this can reach, so the
-- replacement is returned rather than left to be discovered later by an edge
-- that will not resolve.
--
-- Nothing on the model is touched until every constructor has agreed, so a row
-- this refuses leaves the project exactly as it was.
function scribe.add_reference(
    built: any,
    pageName: string,
    element: any,
    spec: { [string]: any }
): any
    requireModel(built, "scribe.add_reference")
    if type(spec) ~= "table" then
        error("scribe.add_reference needs a table of fields", 2)
    end
    if spec.element ~= nil then
        error(
            "scribe.add_reference takes the element as its own argument, so the "
                .. "spec does not name a second one",
            2
        )
    end
    if not model.is_element(element) then
        error(
            "scribe.add_reference needs an element built by model.Element.new",
            2
        )
    end

    local page = built.page_by_name[pageName]
    if page == nil then
        error(
            "this project holds no page named '"
                .. tostring(pageName)
                .. "'; a reference is a row ON a page, so there is nowhere to "
                .. "write one",
            2
        )
    end
    requireDeclaredElement(built, element)
    if model.Page.reference_for(page, element) ~= nil then
        error(
            "page '"
                .. page.name
                .. "' already references element '"
                .. element.name
                .. "'; one page uses one element in one way, and a second row "
                .. "would be two answers to what these pixels are for here",
            2
        )
    end

    local references: { any } = {}
    for index, existing in page.references do
        references[index] = referenceSpec(existing)
    end
    local row   = specCopy(spec)
    row.element = element
    table.insert(references, row)

    local rebuilt = rebuiltPage(page, references)

    local pages: { any } = {}
    for index, existing in built.pages do
        pages[index] = if existing.name == page.name then rebuilt else existing
    end
    local replaced: { [string]: any } = { [page.name] = rebuilt }
    local edges: { any } = {}
    for index, edge in built.graph.edges do
        edges[index] = remappedEdge(edge, replaced, nil)
    end

    installPages(built, pages, edges, replaced)
    return rebuilt
end

-- scribe.add_appearance(ctx, built, element_name, measurement, spec) -> the
-- rebuilt element
--
-- Writes one more way an element the project already declares can look: the
-- measured crop, stored as a template asset, appended to that element's
-- appearance list, with the threshold the CALLER decided.
--
-- IT IS ONE VERB AND NOT AN `author_` / `add_` PAIR, for the reason `model`
-- gives for there being no `Reference.new`: an appearance exists only as part of
-- the element that declares it. Nothing mints one, so an `author_appearance`
-- would hand back a plain table an agent could equally have written itself --
-- `source` naming a file nobody stored -- and the `add_` half could not tell the
-- two apart. `author_element` splits because an ELEMENT is minted and
-- `add_element` can say so. This is shaped like `add_reference` instead.
--
-- THE ELEMENT IS NAMED RATHER THAN HELD, which is where it differs from
-- `add_reference` and agrees with `add_edge`: this verb is what makes an element
-- go stale, so taking one by value would take the very object a second call
-- supersedes. A name always resolves to the element the model holds now.
--
-- `spec` IS THE APPEARANCE, under the model's own field names, and rides to
-- `Element.new` as written, so a key that constructor does not own is refused in
-- its words rather than dropped here. It may not carry the three the measurement
-- settles -- `source`, `template` and `key` -- because those are facts about the
-- pixels that were cut. The key is copied across exactly as
-- `scribe.author_element` copies it: a masked template with no key written down
-- is a picture nobody can redraw.
--
-- IT REFUSES AN ELEMENT THAT DECLARES NO APPEARANCES, which is the one ruling
-- here about meaning rather than shape. Such an element is verified by what its
-- rectangle READS and positioned by the page that referenced it -- `observe.find`
-- reports `positioned_by = "page"` and `resolve_page` takes the text branch.
-- Pixels move every page that identifies by it onto the template branch and every
-- click onto a searched rectangle, which is a different element rather than this
-- one wearing a second face.
--
-- A NAME IS REQUIRED THOUGH `Element.new` LEAVES IT OPTIONAL, and the two do not
-- disagree: a lone appearance needs no name because nothing has to tell it from
-- anything, and the moment there are two, everything that addresses one -- a
-- page's `pinned_appearance`, a claim's `appearance`, the name a hit reports,
-- the appearance the matrix says owns a screen -- addresses it by that string.
-- `Element.new` is handed a finished list and cannot know which entry is new.
--
-- THE CLAIM SIDE NEEDS NO RE-POINTING, which is worth saying rather than leaving
-- a reader to check: `oracle.Expectation` stores the element by NAME, so every
-- claim naming it goes on naming it, and a claim about the appearance just added
-- is written afterwards with `scribe.claim` in the ordinary way.
--
-- Nothing on the model is touched until every constructor has agreed, so an
-- appearance this refuses leaves the project exactly as it was.
function scribe.add_appearance(
    ctx: any,
    built: any,
    elementName: string,
    measurement: Measurement,
    spec: { [string]: any }
): any
    requireModel(built, "scribe.add_appearance")
    if type(spec) ~= "table" then
        error("scribe.add_appearance needs a table of fields", 2)
    end

    local element = built.element_by_name[elementName]
    if element == nil then
        error(
            "this project declares no element named '"
                .. tostring(elementName)
                .. "'; an appearance is a second way something the file already "
                .. "carries can look, so there is nothing here to add one to -- "
                .. "author the element with scribe.author_element first",
            2
        )
    end

    if #element.appearances == 0 then
        error(
            "element '"
                .. element.name
                .. "' declares no appearances, so it is verified by what its "
                .. "rectangle reads and positioned by the page that referenced "
                .. "it; pixels would change both for every page that uses it, "
                .. "which is a new element rather than a second face on this one",
            2
        )
    end

    if type(spec.name) ~= "string" or spec.name == "" then
        error(
            "scribe.add_appearance needs name = what to call this way of "
                .. "looking; a pin, a claim and a hit all address an appearance "
                .. "by that name, so a second one without it is unaddressable",
            2
        )
    end
    if model.Element.appearance_named(element, spec.name) ~= nil then
        error(
            "element '"
                .. element.name
                .. "' already has an appearance named '"
                .. spec.name
                .. "'; a script tells appearances apart by name, so two of them "
                .. "cannot share one",
            2
        )
    end
    if type(spec.threshold) ~= "number" then
        error(thresholdMissing("scribe.add_appearance"), 2)
    end

    if spec.source ~= nil or spec.template ~= nil or spec.key ~= nil then
        error(
            "scribe.add_appearance takes source, template and key from the "
                .. "measurement, so its spec names none of them; a spec that "
                .. "named one would be a caption on someone else's picture",
            2
        )
    end

    local appearances: { any } = {}
    for index, existing in element.appearances do
        appearances[index] = appearanceSpec(existing)
    end
    local entry    = specCopy(spec)
    entry.source   = scribe.save_template(ctx, measurement)
    entry.template = ctx:template_load(measurement.png)
    entry.key      = measurement.key
    table.insert(appearances, entry)

    local rebuilt = model.Element.new(elementSpec(element, appearances))

    -- Only the pages that named it are built again. A page nothing changed for
    -- rides through as the same table, which is what lets an agent go on holding
    -- one and what keeps an edge between two untouched pages the exact edge
    -- `Graph.has_edge` was asked about.
    local replaced: { [string]: any } = {}
    local pages: { any } = {}
    for index, page in built.pages do
        if model.Page.reference_for(page, element) == nil then
            pages[index] = page
        else
            local references: { any } = {}
            for slot, existing in page.references do
                local row = referenceSpec(existing)
                if existing.element.name == element.name then
                    row.element = rebuilt
                end
                references[slot] = row
            end
            local fresh         = rebuiltPage(page, references)
            pages[index]        = fresh
            replaced[page.name] = fresh
        end
    end

    local edges: { any } = {}
    for index, edge in built.graph.edges do
        edges[index] = remappedEdge(edge, replaced, rebuilt)
    end

    installElement(built, rebuilt, pages, edges, replaced)
    return rebuilt
end

-- The page this project declares under `name`, for a field of an edge spec.
local function pageNamed(built: any, name: any, field: string): any
    if type(name) ~= "string" then
        error(
            "scribe.add_edge needs "
                .. field
                .. " = the NAME of a page this project declares; writing a "
                .. "reference REBUILDS a page, so a page an agent is holding can "
                .. "already be one the model no longer has, and a name is what "
                .. "always resolves to the one it does",
            3
        )
    end
    local page = built.page_by_name[name]
    if page == nil then
        error(
            "this project holds no page named '"
                .. name
                .. "', so no edge can name it as its "
                .. field,
            3
        )
    end
    return page
end

-- scribe.add_edge(built, spec) -> the new edge
--
-- Draws one edge: the page it leaves, the trigger that takes it, the pages it
-- can land on, and what the run should believe afterwards.
--
-- EVERY END IS NAMED RATHER THAN HELD, which is the one place this spec differs
-- from `Edge.new`'s. `from`, `to` and `via_element` are the names the project
-- file spells, and the reason is `add_reference` directly above: writing a row
-- rebuilds a page, so a page an agent put in a variable three lines ago may
-- already be superseded, and an edge drawn from that one would leave a page the
-- model no longer holds. A name cannot go stale.
--
-- Everything else is `Edge.new`'s and is not repeated here: which triggers
-- exist, which kinds exist, that a click names an element the from page already
-- references AND exercises interact on, that a push only leads to an overlay,
-- that a pop declares no destination. `Graph.new` then checks the edge against
-- the model it is joining, which is where two edges leaving one page by one
-- trigger are caught.
function scribe.add_edge(built: any, spec: { [string]: any }): any
    requireModel(built, "scribe.add_edge")
    if type(spec) ~= "table" then
        error("scribe.add_edge needs a table of fields", 2)
    end

    local entry = specCopy(spec)
    entry.from  = pageNamed(built, spec.from, "from")

    if spec.to ~= nil then
        if type(spec.to) ~= "table" then
            error(
                'scribe.add_edge needs to = { "page", ... }, the names of the '
                    .. "pages this edge can land on",
                2
            )
        end
        local destinations: { any } = {}
        for index, name in spec.to :: { any } do
            destinations[index] = pageNamed(built, name, "to")
        end
        entry.to = destinations
    end

    if spec.via_element ~= nil then
        if type(spec.via_element) ~= "string" then
            error(
                "scribe.add_edge needs via_element = the NAME of the element "
                    .. "this edge is taken by clicking",
                2
            )
        end
        local element = built.element_by_name[spec.via_element]
        if element == nil then
            error(
                "this project declares no element named '"
                    .. spec.via_element
                    .. "', so no edge can be taken by clicking it",
                2
            )
        end
        entry.via_element = element
    end

    local edge = navigation.Edge.new(entry)

    local edges: { any } = {}
    for index, existing in built.graph.edges do
        edges[index] = existing
    end
    table.insert(edges, edge)

    built.graph = navigation.Graph.new({ pages = built.pages, edges = edges })
    return edge
end

-- Refuses when `built` is not (or is no longer) the model project.load_project
-- returned, in the one sentence `add_screen`, `claim` and `claim_text` all
-- need: each is about to read or grow `built.claims`, so there has to be a
-- `Claims` object there to read or grow. It is `requireModel`'s twin for the
-- other half of a loaded model -- the graph and the claims never share one
-- check, because a caller working on pages has no reason to be told about
-- screens, and the reverse.
--
-- Every caller reaches this directly from its own body, one frame from the
-- error, the same shape `requireModel` relies on -- which is why `claim` and
-- `claim_text` call it themselves rather than leaving it to `recordClaim`
-- below: routing it through one more frame would make level 3 point at the
-- wrong line for two of this helper's three callers.
local function requireClaims(built: any, verb: string)
    if type(built) ~= "table" or not oracle.is_claims(built.claims) then
        error(verb .. " needs the model project.load_project returned", 3)
    end
end

-- Rebuilds `built.claims` around `screens` and `expectations`, and installs
-- the result.
--
-- `Claims.new` is where a duplicate screen name or hash, a screen an
-- expectation names but the collection does not hold, and a second claim on
-- one cell are all caught, so `add_screen` and `recordClaim` below both grow
-- one of these two lists and hand BOTH back here rather than writing
-- `built.claims` themselves -- the same reason `installPage` above exists for
-- a page's three copies. There is only the one field to move here, unlike a
-- page's three, because `screen_by_name`, `state_by_cell` and `text_by_cell`
-- all live inside the one table `Claims.new` returns; a caller that grows
-- `expectations` still has to pass `screens` through unchanged; and the
-- reverse.
local function installClaims(built: any, screens: { any }, expectations: { any })
    built.claims = oracle.Claims.new({
        screens      = screens,
        expectations = expectations,
    })
end

-- scribe.add_screen(built, spec) -> the new screen
--
-- Declares a screen the project does not have yet: its name, and the content
-- hash of the PNG a capture already wrote to `assets/screens/<hash>.png`. It
-- is what closes the hole `claim` and `claim_text` were always going to hit --
-- an agent that has just captured a screen through the exploration channel
-- had pixels on disk and no way to tell the model they belonged to anything.
--
-- `spec` goes to `oracle.Screen.new` exactly as handed in, so the hash-shape
-- check, the non-empty name, and the frozen `extra`/`residual` rulings are all
-- made once, in the constructor's own words, and are not restated here. The
-- two things this file rules on are the two things `Screen.new` cannot see, and
-- both are questions about the rest of the project rather than about the screen:
-- whether the NAME is already taken, in the voice `add_page` and `add_element`
-- use for the same refusal and for the same reason -- a screen name is how a
-- claim addresses it and how a verdict row reports it, so two of them cannot
-- share one -- and whether the PAGE it says it is of exists.
--
-- THE PAGE CHECK IS HERE AND IN `project.build`, AND TWO PLACES IS THE POINT.
-- They are not two copies of one check but the two doors into a model: a
-- hand-edited file and an agent's session, and a screen entering through either
-- one reaches the same matrix. Guarding one would leave a hole in the exact shape
-- of the other, and what gets through it is the one state this field must never
-- reach -- a declaration `recognition` has no page to resolve, so the screen's
-- own claim cannot be measured at all. The sentences differ because what the
-- caller can do about it differs: that one names the line the author wrote, and
-- this one tells an agent to write the page first, which it is holding the
-- session to do.
--
-- WHY THIS DOES NOT ALSO READ `assets/screens/<hash>.png`. It would be one
-- more line: `ctx:project_read` already exists on this surface, and
-- `regress.check` already calls exactly that, on every screen it walks, as
-- what its own comment there calls "the existence proof". But `Screen.new`
-- has already drawn this line on purpose, for this very hash, one call below:
-- it checks the hash's SHAPE and says outright that a malformed one otherwise
-- "surfaces as a capture that ran out of frames" rather than as a refusal at
-- construction -- and `project.build` extends that same choice to every
-- screen a hand-edited file declares, minting an `oracle.Screen` with no read
-- of its own. Reading the file here would make a screen's measurability
-- depend on HOW it was declared: checked once, immediately, through this
-- verb, but merely trusted through a hand-edited file. `regress.check` still
-- has to keep re-checking either way -- a file present today can be deleted
-- before the next walk -- so a second check here would be an extra place that
-- answer could drift from the one place already responsible for it, not a
-- stronger guarantee of it. One required check, in the one place that owns
-- it, beats two that can disagree.
function scribe.add_screen(built: any, spec: { [string]: any }): any
    requireClaims(built, "scribe.add_screen")
    if type(spec) ~= "table" then
        error("scribe.add_screen needs a table of fields", 2)
    end
    if
        type(spec.name) == "string"
        and built.claims.screen_by_name[spec.name] ~= nil
    then
        error(
            "this project already declares a screen named '"
                .. spec.name
                .. "'; a screen name is how a claim addresses it and how a "
                .. "verdict row reports it, so two of them cannot share one",
            2
        )
    end
    -- Only the string case is asked here. Every other shape falls through to
    -- `Screen.new`, which already says what a page field must be, and a second
    -- copy of that sentence would be a second thing to keep in step.
    if type(spec.page) == "string" then
        local pages: any = built.page_by_name
        if type(pages) ~= "table" or pages[spec.page] == nil then
            error(
                "this project declares no page named '"
                    .. spec.page
                    .. "'; a screen that says which page it is of is measured by "
                    .. "resolving that page on it, so a name nothing declares is "
                    .. "a claim with nothing to measure -- add the page first",
                2
            )
        end
    end

    local screen = oracle.Screen.new(spec)

    local screens: { any } = table.clone(built.claims.screens)
    table.insert(screens, screen)
    installClaims(built, screens, built.claims.expectations)
    return screen
end

-- The half of a claim that does not depend on what the cell is measured by:
-- resolve the screen it names, and hand the whole expectation list back to
-- `Claims.new` with one more on it.
--
-- IT DOES NOT CHECK `built.claims` ITSELF. Both callers below -- `claim` and
-- `claim_text` -- call `requireClaims` on their own first line, before they
-- ever reach here, in the same voice `scribe.add_screen` does; routing that
-- check through this extra frame would only change which line an agent's
-- error names, not whether it is checked.
--
-- The collection is rebuilt rather than mutated, because `Claims.new` is where a
-- duplicate cell and a screen this project does not hold are caught, and
-- appending behind its back would skip both checks.
--
-- `spec` rides through to `Expectation.new` with the screen OBJECT written over
-- the name it was addressed by, and nothing else is added or removed on the way.
-- That is what keeps the rulings a cell is subject to in one place: which state
-- words exist, that a cell names one verification source and not two, that an
-- element with no templates is claimed by what it reads, and that nothing may
-- read a rectangle the element never declared `read` on.
--
-- The table is written on rather than copied, because both callers mint theirs
-- at the call site out of their own arguments -- there is no caller table for
-- the write to be visible in, and a copy would only be a second place a field
-- could be lost.
local function recordClaim(
    built: any,
    screenName: string,
    spec: { [string]: any }
)
    local screen = built.claims.screen_by_name[screenName]
    if screen == nil then
        error(
            "this project holds no screen named '"
                .. tostring(screenName)
                .. "'; a claim about a screen nobody captured is a claim nothing "
                .. "can measure",
            3
        )
    end

    spec.screen = screen

    local expectations: { any } = table.clone(built.claims.expectations)
    table.insert(expectations, oracle.Expectation.new(spec))

    installClaims(built, built.claims.screens, expectations)
end

-- scribe.claim(built, screen_name, element, state, appearance?, rect?) -> ()
--
-- Records what the model says this element's TEMPLATES should do on one of the
-- screens the project already holds: the folded element when no appearance is
-- named, or one appearance searched alone when one is.
--
-- WHY A CLAIM IS NOT OPTIONAL WORK. A template always matches the image it was
-- cut from, at a distance of zero, so authoring one and looking at it on its own
-- screen demonstrates nothing. The claim is what turns the matrix into evidence:
-- it is the sentence the walk can contradict.
--
-- `rect` IS WHERE, and it belongs to the element that draws no rectangle of its
-- own: a minimap shape is searched at coordinates the script works out per frame,
-- so the only falsifiable form of the fact is "on THIS stored screen it matches
-- HERE". It is the last argument rather than a field of its own verb because it
-- is orthogonal to the two above it -- legal beside an appearance and legal
-- beside none -- unlike `text`, which is the OTHER kind of measurement and got a
-- verb of its own for exactly that reason. Whether it is required, and whether it
-- is allowed at all, is `oracle.Expectation.new`'s and is not restated here.
function scribe.claim(
    built: any,
    screenName: string,
    element: any,
    state: string,
    appearance: string?,
    rect: any
)
    requireClaims(built, "scribe.claim")
    recordClaim(built, screenName, {
        element    = element,
        appearance = appearance,
        rect       = rect,
        state      = state,
    })
end

-- scribe.claim_text(built, screen_name, element, state, text, rect?) -> ()
--
-- Records what the model says this element's rectangle should READ on one of the
-- screens the project already holds. `rect` says WHICH rectangle, for an element
-- that draws none of its own; see `scribe.claim` for why it rides last.
--
-- WHY IT IS A SECOND VERB RATHER THAN A SIXTH PARAMETER ON `claim`. It is the
-- split `author_element` and `author_text_element` already stand on, arriving at
-- the other end of the same session: a cell is measured by ONE verification
-- source (docs/plans/2026-08-01-three-layers-and-agent-operator.md 5-4), and
-- the source is the thing a reader most needs to see in the line that was
-- written. A `text` added beside `appearance` would make the two mutually
-- exclusive tail arguments of one call, so the common spelling of a text claim
-- would be `scribe.claim(built, screen, element, "match", nil, "Sortie")` -- a
-- nil whose only meaning is "not the other kind", in a position no reader can
-- check without counting. Two verbs also put the pairing where the agent picks
-- it: an element authored with `author_text_element` is claimed with
-- `claim_text`, and neither name has a mode to get wrong.
--
-- `text` is required rather than optional, because a text claim with no text is
-- a `claim` -- and `Expectation.new` says so, in its own words, on the cell it
-- refuses.
--
-- WHAT IT DOES NOT RULE ON. Everything: that this element has no templates of
-- its own, that it declares `read`, that the state is one of the three, that the
-- text is a non-empty string. All of those are `Expectation.new`'s and are made
-- there, because a second copy here would be a second sentence to keep in step
-- with the model.
function scribe.claim_text(
    built: any,
    screenName: string,
    element: any,
    state: string,
    text: string,
    rect: any
)
    requireClaims(built, "scribe.claim_text")
    recordClaim(built, screenName, {
        element = element,
        text    = text,
        rect    = rect,
        state   = state,
    })
end

-- scribe.save(ctx, built) -> ()
--
-- Writes the model back over the project file. A thin forward, and it is here so
-- an agent's whole authoring session names one module: the alternative is a
-- chunk that reaches for `project` for its last line only, which reads as if the
-- save were a different kind of act from the eight lines above it.
function scribe.save(ctx: any, built: any)
    project.save_project(ctx, built)
end

return scribe
