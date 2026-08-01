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
-- WHAT IT DOES NOT DO, DELIBERATELY. It does not add a page reference and it
-- does not draw an edge. Both are real parts of a finished annotation and both
-- would mean rebuilding a frozen page and remapping every edge that names it --
-- a second body of work with its own failure modes, and one the plan's authoring
-- chain does not list. An agent that has authored an element with this module
-- still has to have its page reference written for it. See the work order's
-- report for that gap.
--
-- LOAD ORDER. `scribe` sorts after `explore`, `model`, `observe`, `oracle` and
-- `project`, which is the order it needs: it names all five. It sorts before
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
