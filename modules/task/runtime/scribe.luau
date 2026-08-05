--!strict
-- scribe: measuring a screen and writing down what was measured -- the authoring
-- loop of docs/plans/2026-08-01-agent-front-end-and-exploration.md 1 in one
-- place: crop a rectangle, measure its pixels, put the crop in the project as a
-- template asset, build the element that searches for it, claim what it should
-- do on a screen, save the file.
--
-- There is one author verb per KIND of element, because which kind a line
-- authors has to be legible on that line.
--
-- No threshold is decided here and none is defaulted; whether a measurement is
-- good enough is the agent's judgement. The one thing derived is a FILE NAME: a
-- template asset is named by the hash of its own bytes, so "which pixels" and
-- "which file" cannot disagree.
--
-- Pages and elements are frozen, so writing on one REBUILDS it through the
-- constructor that makes every ruling, and everything holding the old object is
-- re-pointed at the new one. The re-pointing is not tidiness: a stale edge or
-- reference row still carries the right NAME, so the file it saves is correct in
-- every byte while `observe.walk_edge` resolves the superseded page and
-- `observe.find` searches yesterday's appearance list. Only identity can see it,
-- which is why an element an agent is still holding from before is refused
-- rather than believed (`requireDeclaredElement`).
--
-- Load order. `scribe` sorts after `explore`, `model`, `navigation`, `oracle`
-- and `project`, which is the order it needs: it names all five. It also names
-- `mint`, which sorts before every module here. It sorts before `task`, which it
-- does not name.

local k_templateDirectory = "assets/templates/"
local k_templateSuffix    = ".png"

local scribe = {}

-- Where the template blob with content hash `hash` lives, relative to the
-- project root. Derived rather than authored, for the reason
-- `oracle.screen_path` is: which bytes a template is and where they are must not
-- be able to disagree. It follows the store the runtime already reads.
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

    -- What `probe` said about it, carried whole rather than summarised: the
    -- fields are the host's vocabulary.
    stats: any,

    -- What the colour key took out of the crop, as the CROP verb reported it, or
    -- nil when there was no key: the host's own handle, plus the `warning`
    -- sentence it attaches when the counts look like a mask that cannot measure
    -- anything, and the key it actually applied.
    mask: any,

    -- That key as four plain numbers, ready to be written into an appearance.
    -- Read off `mask` rather than off the caller's argument, because a caller
    -- that named no tolerance got the host's default and the project file has to
    -- record the key that really cut these pixels.
    key: {
        red: number,
        green: number,
        blue: number,
        tolerance: number,
        removes: boolean,
    }?,
}

-- scribe.measure(ctx, ticket, rect, key?, tolerance?) -> Measurement
--
-- One crop and one probe of the same rectangle of the same frame. They are one
-- verb because they must be the SAME pixels: cropping on one frame and probing a
-- crop of another reports counts about a screen that has already moved on.
--
-- The key cuts the crop as well as the probe, so the pixels the probe measured
-- are the pixels the template compares. An agent with no key yet passes none;
-- choosing a key is done by cropping unkeyed first and probing that blob, which
-- is what `explore.probe` is for.
--
-- It decides nothing about the key. The host refuses only the one key that
-- cannot work at all -- one that selects no pixel, whose template would be fully
-- transparent -- and warns about counts that historically measured nothing; both
-- come back in `mask` for the caller to read.
--
-- It spends one crop out of this cycle's budget and does not consume the cycle,
-- so a caller may go on to click on the same frame it measured.
function scribe.measure(
    ctx: any,
    ticket: any,
    rect: { x: number, y: number, width: number, height: number },
    key: { red: number, green: number, blue: number, removes: boolean? }?,
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

    local applied: {
        red: number,
        green: number,
        blue: number,
        tolerance: number,
        removes: boolean,
    }? = nil
    if mask ~= nil then
        applied = {
            red       = mask.key_red,
            green     = mask.key_green,
            blue      = mask.key_blue,
            tolerance = mask.tolerance,
            removes   = mask.key_removes,
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
-- hands that path back so an appearance can carry it. Writing the same bytes
-- twice replaces a file with itself rather than failing, which is what makes
-- authoring two elements that happen to share a crop safe.
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
-- wrote.
local function thresholdMissing(verb: string): string
    return verb
        .. " needs threshold = the basis points this appearance must match "
        .. "within; it has no default, because a threshold nobody chose is a "
        .. "decision nobody can argue with"
end

-- The element both measurement-fed verbs build, differing only in the rectangle
-- they hand it. `verb` is the name a refusal reaches the agent under, raised at
-- level 3 so it names the line that called the verb rather than this one.
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
-- pointing at the template asset, with the threshold the CALLER decided. It
-- writes the template asset as part of the same call, because an element whose
-- appearance names a file the project does not hold loads and then fails at its
-- first search.
--
-- `spec` carries name, capabilities, threshold, and optionally an appearance
-- name, a rect override and the model's two forward-compatibility fields. The
-- rect defaults to the rectangle that was measured, which is what the template
-- was cut from; there is no spelling of "no rectangle at all", and such an
-- element is authored by `scribe.author_unplaced_element`.
--
-- The colour key rides across from the measurement (`measurement.key`) and is
-- not decided here: a crop cut under a key carries the mask in its own alpha
-- channel, so an element would work perfectly with the key written down nowhere
-- -- a masked PNG nobody can redraw the rectangle for a year later. `spec` has
-- no key field.
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
-- no rectangle of its own -- a minimap shape matched at coordinates the script
-- works out per frame, a confirm button drawn once and reused on nine screens.
--
-- The pixels are still a measurement, which is why it takes one; what is absent
-- is the POSITION. The rectangle in that measurement is where the pixels were
-- cut from, and it is deliberately not carried across.
--
-- Threshold, appearance name and colour key are `elementFromMeasurement`'s. The
-- rulings that keep "no rectangle" from becoming "nobody said where to look" are
-- made where they always were: `Page.new` refuses a row that states no
-- `rect_override` and refuses such an element in a page signature at all, and
-- `Expectation.new` refuses a claim that says no `rect`.
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
-- It is a second verb rather than a flag, so the verification source an agent
-- chose is visible in the line it wrote (an element declares ONE source:
-- docs/plans/2026-08-01-three-layers-and-agent-operator.md 5-4).
--
-- `spec` is handed to `Element.new` as written, so every ruling is made in the
-- constructor's own words.
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
-- next matrix walk measures it. The model's element list is the one part of a
-- loaded project that is a plain mutable table; the elements IN it are frozen.
--
-- A name the project already declares is refused here rather than at save time,
-- while the caller still has the element in hand.
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
-- naming an element the project has since replaced. Elements and rows are saved
-- as SEPARATE sections, so a row naming an element no `[[element]]` declares is
-- a file this build writes happily and then refuses to parse, by which time the
-- project it was written from is gone.
--
-- `Page.new` cannot make this ruling: it is handed elements, not the project, so
-- the most it can ask is that the table was minted.
--
-- The second half asks by IDENTITY, and it has to: a row written from a
-- superseded element is a page searching an appearance list the file no longer
-- carries, which saves and loads correctly because the file names the element.
-- An element derived from the current one with `setmetatable` still passes, on
-- the same bounded walk `model.is_element` uses.
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

-- A mutable copy of a caller's spec table. It exists so a verb can write the
-- object it resolved over the name it was handed while every OTHER key rides
-- through to the constructor untouched -- including one the constructor does not
-- own, which is how it gets refused with the model's own sentence instead of
-- quietly dropped.
local function specCopy(spec: { [string]: any }): { [string]: any }
    local copy: { [string]: any } = {}
    for key, value in spec do
        copy[key] = value
    end
    return copy
end

-- One of a page's frozen reference rows, turned back into the spec table
-- `Page.new` accepts: the `page` stamp comes off, because stamping it is the
-- constructor's job and handing it back would be an unknown key, and the
-- capability SET becomes the capability LIST the constructor reads. Everything
-- else -- including `extra` and the raw `residual` lines a newer schema wrote --
-- passes straight through, because a rebuild that dropped them would be the
-- silent deletion the file format exists to prevent. `expected_text` is where
-- that would hurt most: a row identified by text carries the whole of its page's
-- signature in that one string, so writing ONE new row would break a page nobody
-- touched.
local function referenceSpec(reference: any): { [string]: any }
    return {
        element           = reference.element,
        holding           = reference.holding,
        exercised         = model.capability_list(reference.exercised),
        identify          = reference.identify,
        expected_text     = reference.expected_text,
        pinned_appearance = reference.pinned_appearance,
        rect_override     = reference.rect_override,
        extra             = reference.extra,
        residual          = reference.residual,
    }
end

-- One of an element's frozen appearances, turned back into the entry
-- `Element.new` accepts. `referenceSpec`'s twin one level down: `key` is the
-- colour the blob was cut under and `residual` is what a newer schema wrote, so
-- dropping either is a masked picture nobody can redraw and a silent deletion.
-- `template` rides across as the handle it already is.
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
-- thing a rebuild changes; everything else goes straight back to the constructor
-- that froze it, so every ruling `Element.new` makes is made again.
local function elementSpec(
    element: any,
    appearances: { any }
): { [string]: any }
    return {
        name          = element.name,
        capabilities  = model.capability_list(element.capabilities),
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
-- `element` is the half only `add_appearance` uses, and it is not redundant with
-- the page remap it always accompanies: `pages` alone would already mark a click
-- edge changed, but it would then be rebuilt still holding the SUPERSEDED
-- element in `via_element` beside a `via_reference` resolved off the new page.
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
-- Both verbs that rewrite a page's rows go through it: `overlay`, `interrupt`,
-- `extra` and the raw `residual` lines are the page's own facts, and a rebuild
-- that dropped one would be a page silently losing what nobody was looking at.
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
-- `replaced`. `built.pages` is the list `save` writes and a rebuilt graph is
-- made from, `built.page_by_name` is the index `add_edge` resolves a name
-- through, and `built.graph` is what `observe.walk_edge` resolves an edge
-- against; two of the three updated is a model that saves a correct file and
-- walks to a page nobody can see.
--
-- It takes a SET of pages, because `add_appearance` replaces an element any
-- number of pages reference and rebuilds all of them in one act.
--
-- The graph is built FIRST and assigned LAST, so a page list `Graph.new` refuses
-- leaves the model exactly as it was.
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
-- copies of its page list, around one rebuilt element. An element lives in
-- `built.elements`, in `built.element_by_name`, and inside every page that
-- references it, which is why the pages are installed here rather than
-- separately: two calls would leave a window in which the index answers with the
-- new element and a page still holds the old one.
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
-- is BORN with. `Page.new` refuses a page with no required identify reference,
-- so "make the page, then write its rows onto it" is not a sequence this model
-- can be walked through; `add_reference` is what a LATER row uses.
--
-- A row names its element by the ELEMENT, where `add_edge` names its ends by
-- name: a stale element is CAUGHT rather than believed, because
-- `requireDeclaredElement` refuses anything that is not the element the model
-- holds under that name. Everything else in a row is what `add_reference`'s spec
-- carries, under the same names, because it IS that row.
--
-- Two questions are asked here and every other one is the constructor's: a page
-- name the project already declares, and a row naming an element the file does
-- not carry.
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
    -- answer. Every other shape falls through to `Page.new` untouched, because
    -- the sentence for it is already written there.
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
    -- can already name it.
    installPages(built, pages, built.graph.edges, { [page.name] = page })
    return page
end

-- scribe.add_reference(built, page_name, element, spec) -> the rebuilt page
--
-- Writes one row on one page: what THIS page does with this element.
--
-- `spec` carries the row's own fields under the model's own names and is handed
-- to `Page.new` as written, so every ruling that constructor makes is made here
-- in its own words.
--
-- The rebuilt page comes back because the old one is gone from every copy the
-- model keeps, and a page an agent left in a variable is not something this can
-- reach. Nothing on the model is touched until every constructor has agreed, so
-- a row this refuses leaves the project exactly as it was.
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
-- An appearance exists only as part of the element that declares it, so nothing
-- mints one.
--
-- The element is NAMED rather than held, where `add_reference` takes the object:
-- this verb is what makes an element go stale, so taking one by value would take
-- the very object a second call supersedes.
--
-- `spec` IS the appearance, under the model's own field names, and rides to
-- `Element.new` as written. It may not carry `source`, `template` or `key`,
-- which are facts about the pixels that were cut; the key is copied across
-- exactly as `scribe.author_element` copies it.
--
-- A name is required though `Element.new` leaves it optional, and the two do not
-- disagree: a lone appearance needs no name, and the moment there are two, a
-- pin, a claim, a hit and the matrix all address one by that string.
--
-- The claim side needs no re-pointing, because `oracle.Expectation` stores the
-- element by NAME. Nothing on the model is touched until every constructor has
-- agreed, so an appearance this refuses leaves the project exactly as it was.
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
    -- one and keeps an edge between two untouched pages the exact edge
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
-- Every end is NAMED rather than held, which is the one place this spec differs
-- from `Edge.new`'s.
--
-- Everything else is `Edge.new`'s and is not repeated here. `Graph.new` then
-- checks the edge against the model it is joining, which is where two edges
-- leaving one page by one trigger are caught.
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

    local edges: { any } = table.clone(built.graph.edges)
    table.insert(edges, edge)

    built.graph = navigation.Graph.new({ pages = built.pages, edges = edges })
    return edge
end

-- Refuses when `built` is not the model `project.load_project` returned, in the
-- one sentence `add_screen`, `claim` and `claim_text` all need: each is about to
-- read or grow `built.claims`. It is `requireModel`'s twin for the other half of
-- a loaded model, kept separate because a caller working on pages has no reason
-- to be told about screens.
--
-- Every caller reaches it directly from its own body, one frame from the error,
-- which is why `claim` and `claim_text` call it themselves rather than leaving
-- it to `recordClaim` below: one more frame would make level 3 point at the
-- wrong line.
local function requireClaims(built: any, verb: string)
    if type(built) ~= "table" or not oracle.is_claims(built.claims) then
        error(verb .. " needs the model project.load_project returned", 3)
    end
end

-- Rebuilds `built.claims` around `screens` and `expectations`, and installs the
-- result. `Claims.new` is where a duplicate screen name or hash, a screen an
-- expectation names but the collection does not hold, and a second claim on one
-- cell are all caught, so both callers grow one of the two lists and hand BOTH
-- back here rather than writing `built.claims` themselves. There is only the one
-- field to move, because `screen_by_name`, `state_by_cell` and `text_by_cell`
-- all live inside the one table `Claims.new` returns.
local function installClaims(built: any, screens: { any }, expectations: { any })
    built.claims = oracle.Claims.new({
        screens      = screens,
        expectations = expectations,
    })
end

-- scribe.add_screen(built, spec) -> the new screen
--
-- Declares a screen the project does not have yet: its name, and the content
-- hash of the PNG a capture already wrote to `assets/screens/<hash>.png`. It is
-- what lets an agent that has just captured a screen tell the model those pixels
-- belong to something.
--
-- `spec` goes to `oracle.Screen.new` exactly as handed in, so the hash-shape
-- check, the non-empty name and the frozen `extra`/`residual` rulings are made
-- once in the constructor's own words. The two things this file rules on are the
-- two `Screen.new` cannot see, and both are about the rest of the project:
-- whether the NAME is already taken -- a screen name is how a claim addresses it
-- and how a verdict row reports it -- and whether the PAGE it says it is of
-- exists.
--
-- That page check is here AND in `project.build`, which is the point: they are
-- the two doors into a model, a hand-edited file and an agent's session, and
-- what gets through an unguarded one is a declaration `recognition` has no page
-- to resolve, so the screen's own claim cannot be measured at all.
--
-- It does not also read `assets/screens/<hash>.png`. `Screen.new` checks the
-- hash's SHAPE and says outright that a malformed one surfaces as a capture that
-- ran out of frames, and `project.build` extends the same choice to every screen
-- a hand-edited file declares. Reading here would make a screen's measurability
-- depend on HOW it was declared, and `regress.check` has to re-check either way.
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
    -- Only the string case is asked here; every other shape falls through to
    -- `Screen.new`, which already says what a page field must be.
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
-- `Claims.new` with one more on it. It does not check `built.claims` itself --
-- both callers do that on their own first line, one frame from the error.
--
-- `spec` rides through to `Expectation.new` with the screen OBJECT written over
-- the name it was addressed by, and nothing else is added or removed on the way,
-- which is what keeps the rulings a cell is subject to in one place. The table
-- is written on rather than copied, because both callers mint theirs at the call
-- site out of their own arguments.
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
-- A claim is not optional work: a template always matches the image it was cut
-- from, so the claim is the sentence the walk can contradict.
--
-- `rect` is WHERE, and it belongs to the element that draws no rectangle of its
-- own: a minimap shape is searched at coordinates the script works out per frame,
-- so the only falsifiable form of the fact is "on THIS stored screen it matches
-- HERE". It rides last because it is legal beside an appearance and legal beside
-- none. Whether it is required, and whether it is allowed at all, is
-- `oracle.Expectation.new`'s.
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
-- `text` is required rather than optional, because a text claim with no text is
-- a `claim` -- and `Expectation.new` says so on the cell it refuses.
--
-- It rules on nothing else; every other ruling is `Expectation.new`'s.
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
-- Writes the model back over the project file. A thin forward, so an agent's
-- whole authoring session names one module.
function scribe.save(ctx: any, built: any)
    project.save_project(ctx, built)
end

return scribe
