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
-- NO THRESHOLD IS DECIDED HERE, AND NONE IS DEFAULTED. `author_element` takes
-- the threshold as a required field, exactly as the project file stores one.
-- The measurement this module hands back says how many pixels a colour key
-- takes and how many the rectangle holds; whether that is good enough is a
-- judgement, judgements belong to the agent and to the developer reading its
-- work, and a number this file picked would be a judgement nobody could argue
-- with because nobody would see it. The one thing it does derive is a FILE NAME,
-- which is not a judgement: a template asset is named by the hash of its own
-- bytes so that "which pixels" and "which file" cannot disagree.
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
-- LOAD ORDER. `scribe` sorts after `explore`, `model`, `navigation`, `oracle`
-- and `project`, which is the order it needs: it names all five. It sorts before
-- `task`, which it does not name.

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
}

-- scribe.measure(ctx, ticket, rect, key?, tolerance?) -> Measurement
--
-- One crop and one probe of the same rectangle of the same frame. They are one
-- verb because they are one question -- "what is here" -- asked of pixels that
-- must be the SAME pixels: cropping on one frame and probing a crop of another
-- would report counts about a screen that had already moved on.
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

    local png, hash = explore.crop(ticket, rect.x, rect.y, rect.width, rect.height)
    local stats     = explore.probe(
        png,
        0,
        0,
        rect.width,
        rect.height,
        key,
        tolerance
    )

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
-- It writes the template asset as part of the same call. An element whose
-- appearance names a file the project does not hold is one that loads and then
-- fails at its first search, and there is no reason to be able to build one.
function scribe.author_element(
    ctx: any,
    measurement: Measurement,
    spec: { [string]: any }
): any
    if type(spec) ~= "table" then
        error("scribe.author_element needs a table of fields", 2)
    end
    if type(spec.threshold) ~= "number" then
        error(
            "scribe.author_element needs threshold = the basis points this "
                .. "appearance must match within; it has no default, because a "
                .. "threshold nobody chose is a decision nobody can argue with",
            2
        )
    end

    local source = scribe.save_template(ctx, measurement)

    return model.Element.new({
        name          = spec.name,
        capabilities  = spec.capabilities,
        rect          = spec.rect or measurement.rect,
        appearances   = {
            {
                name      = spec.appearance,
                source    = source,
                template  = ctx:template_load(measurement.png),
                threshold = spec.threshold,
            },
        },
        expected_text = spec.expected_text,
        extra         = spec.extra,
        residual      = spec.residual,
    })
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

-- The two verbs below rewrite the page graph, so both need the whole loaded
-- model rather than a table with a page list on it: they read the element index
-- to refuse a row nothing declares, and they hand the page list back to
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

-- One of a page's frozen reference rows, turned back into the spec table
-- `Page.new` accepts. It is exactly what the constructor did, undone: the `page`
-- stamp comes off, because stamping it is the constructor's job and handing it
-- back would be an unknown key, and the capability SET becomes the capability
-- LIST the constructor reads. Everything else -- including `extra` and the raw
-- `residual` lines a newer schema wrote -- is passed straight through, because a
-- rebuild that dropped them would be the silent deletion the file format exists
-- to prevent.
local function referenceSpec(reference: any): { [string]: any }
    local exercised: { string } = {}
    for _, capability in model.capability_order() do
        if (reference.exercised :: any)[capability] then
            table.insert(exercised, capability)
        end
    end
    return {
        element           = reference.element,
        holding           = reference.holding,
        exercised         = exercised,
        identify          = reference.identify,
        pinned_appearance = reference.pinned_appearance,
        rect_override     = reference.rect_override,
        extra             = reference.extra,
        residual          = reference.residual,
    }
end

-- `edge`, rebuilt against `replacement` wherever it named the page called
-- `pageName`, and the SAME edge back when it named neither end.
--
-- Returning the identical table matters: `navigation.Graph.has_edge` and
-- `observe.walk_edge` ask whether this exact edge belongs to this exact graph,
-- so minting a new one for an edge nothing happened to would invalidate a handle
-- the caller is entitled to keep holding.
--
-- Rebuilding goes through `Edge.new` rather than through a field swap, which is
-- what re-resolves a click edge's `via_reference` against the page that now
-- holds the rows.
local function remappedEdge(edge: any, pageName: string, replacement: any): any
    local leaves  = edge.from.name == pageName
    local arrives = false

    local destinations: { any }? = nil
    if edge.to ~= nil then
        local list: { any } = {}
        for index, target in edge.to do
            if target.name == pageName then
                arrives     = true
                list[index] = replacement
            else
                list[index] = target
            end
        end
        destinations = list
    end

    if not leaves and not arrives then
        return edge
    end

    return navigation.Edge.new({
        from        = if leaves then replacement else edge.from,
        to          = destinations,
        via         = edge.via,
        via_element = edge.via_element,
        via_key     = edge.via_key,
        kind        = edge.kind,
        extra       = edge.extra,
        residual    = edge.residual,
    })
end

-- scribe.add_reference(built, page_name, element, spec) -> the rebuilt page
--
-- Writes one row on one page: what THIS page does with this element.
--
-- `spec` carries the row's own fields under the model's own names -- holding,
-- exercised, the identify polarity, an optional pinned appearance and rect
-- override -- because the spec IS the row. It is handed to `Page.new` as
-- written, so every ruling that constructor makes is made here in its own words:
-- exercised is a subset of what the element declares, identify carries a
-- polarity, a pin belongs to a click, and an identify row does not refine the
-- search rectangle. Nothing is defaulted on the way, and nothing is filtered.
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
    if built.element_by_name[element.name] == nil then
        error(
            "this project declares no element named '"
                .. element.name
                .. "'; a row naming an element the file does not carry saves a "
                .. "project that will not open again, so add the element first",
            2
        )
    end
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

    local rebuilt = model.Page.new({
        name       = page.name,
        overlay    = page.overlay,
        interrupt  = page.interrupt,
        references = references,
        extra      = page.extra,
        residual   = page.residual,
    })

    local pages: { any } = {}
    for index, existing in built.pages do
        pages[index] = if existing.name == page.name then rebuilt else existing
    end
    local edges: { any } = {}
    for index, edge in built.graph.edges do
        edges[index] = remappedEdge(edge, page.name, rebuilt)
    end
    local graph = navigation.Graph.new({ pages = pages, edges = edges })

    built.pages = pages
    built.page_by_name[page.name] = rebuilt
    built.graph = graph
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

-- scribe.claim(built, screen_name, element, state, appearance?) -> ()
--
-- Records what the model says this element should do on one of the screens the
-- project already holds.
--
-- WHY A CLAIM IS NOT OPTIONAL WORK. A template always matches the image it was
-- cut from, at a distance of zero, so authoring one and looking at it on its own
-- screen demonstrates nothing. The claim is what turns the matrix into evidence:
-- it is the sentence the walk can contradict.
--
-- The claims object is rebuilt rather than mutated, because `Claims.new` is
-- where a duplicate cell and a screen this project does not hold are caught, and
-- appending behind its back would skip both checks.
function scribe.claim(
    built: any,
    screenName: string,
    element: any,
    state: string,
    appearance: string?
)
    if type(built) ~= "table" or not oracle.is_claims(built.claims) then
        error("scribe.claim needs the model project.load_project returned", 2)
    end
    local screen = built.claims.screen_by_name[screenName]
    if screen == nil then
        error(
            "this project holds no screen named '"
                .. tostring(screenName)
                .. "'; a claim about a screen nobody captured is a claim nothing "
                .. "can measure",
            2
        )
    end

    local expectations: { any } = {}
    for index, existing in built.claims.expectations do
        expectations[index] = existing
    end
    table.insert(
        expectations,
        oracle.Expectation.new({
            screen     = screen,
            element    = element,
            appearance = appearance,
            state      = state,
        })
    )

    built.claims = oracle.Claims.new({
        screens      = built.claims.screens,
        expectations = expectations,
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
