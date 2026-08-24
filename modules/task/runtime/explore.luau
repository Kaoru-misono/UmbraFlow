--!strict
-- The exploration front end's vocabulary, and every verb in it is a Tool call.
--
-- There are no private verbs here any more.  The sixteen `explore_*` natives
-- this module used to stand on reached the engine and the project directly, so
-- an input an annotator delivered carried no Receipt and left no ledger row at
-- all; each of them is now an ordinary Tool of the same framework catalog a
-- production session holds, recorded against this session's own identity and
-- admitted or refused by the Operator's policy artifact
-- (docs/decisions/2026-08-24-there-is-no-annotation-phase.md, step 4 of the
-- ordering in
-- docs/decisions/2026-08-24-policy-is-the-axis-and-observation-holds-a-frame.md).
--
-- What that changes for a chunk, stated plainly rather than smoothed over:
--
--   * Measuring is written INSIDE an observation's body.  `explore.observe`
--     takes a function, hands it the frame view, and the crop, read and census
--     verbs on that view measure THE ONE FRAME the observation is holding --
--     which is what those verbs always meant.  Calling one outside a body is
--     refused by name.
--   * Acting is written OUTSIDE a body, at the top of the chunk.  An input is a
--     mutating call and an observation is read-only, so an input issued inside
--     an observation's body is outside the admitted root effect envelope and is
--     refused; an input at the top of the chunk opens and closes its own frame,
--     which is word for word what an observation with no body does.
--   * `explore.settle` is gone.  Waiting is `framework.workflow.wait`, which is
--     the one spelling of it, and it is not among the Tools an observation may
--     delegate -- so "wait, THEN observe" stays the only expressible order for
--     the reason it always was.
--   * `explore.terminal` is gone.  Whether the generation was spent is the
--     session's outcome, reported on the result line the front end writes, and
--     never a verb a chunk asks about mid-run.

local native = ...
local explore = {}

local k_observeTool = "framework.screen.observe"
local k_deliverTool = "framework.input.deliver"
local k_waitTool = "framework.workflow.wait"
local k_readLinesTool = "framework.screen.read_lines"
local k_probeTool = "framework.screen.probe"
local k_censusGridTool = "framework.screen.census_grid"
local k_projectReadTool = "framework.project.read"
local k_projectWriteTool = "framework.project.write"

-- Resolved per call rather than at load, and deliberately: every VM that boots
-- the framework bundle loads this module, and only an exploration VM is handed
-- the Tool Runtime seam.  A module that refused while loading would take the
-- trusted runtime VM down with it, and what is actually true is narrower --
-- this module has no seam to call, which is a fact about the call and not about
-- the build.
local function requireInvoke(): any
    if type(native) ~= "table" or type(rawget(native :: any, "invoke")) ~= "function" then
        error("exploration Host does not provide the Tool Runtime seam", 3)
    end
    return rawget(native :: any, "invoke")
end

-- One Tool call, answered as the Tool Runtime recorded it:
-- { tool, call_identity, state, result?, evidence? }.  A chunk that wants to
-- read a refusal rather than be stopped by one calls this directly.
function explore.call(toolName: any, arguments: any, body: any?): any
    if type(toolName) ~= "string" then
        error("explore.call needs a tool name string", 2)
    end
    if type(arguments) ~= "table" then
        error("explore.call needs a table of arguments", 2)
    end
    return requireInvoke()(toolName, arguments, body)
end

-- The same call, with anything but a confirmed outcome raised.  It is the shape
-- every verb below is written in: an annotator wants the refusal in front of it,
-- and the Operator's own sentence is what it wants to read.
local function confirmed(toolName: string, arguments: any, body: any?): any
    local answer = requireInvoke()(toolName, arguments, body)
    local state = rawget(answer, "state")
    if state ~= "confirmed" then
        -- The recorded outcome's own sentence, wherever it put it: a refused
        -- delivery records `reason` and a provider failure records `message`.
        local result = rawget(answer, "result")
        local reason = nil
        if type(result) == "table" then
            reason = rawget(result, "reason") or rawget(result, "message")
        end
        error(
            toolName
                .. " was recorded "
                .. tostring(state)
                .. (if type(reason) == "string" then ": " .. reason else ""),
            3
        )
    end
    return rawget(answer, "result")
end

-- The frame view an observation's body measures through.  It carries no frame
-- handle and never could: the measuring Tools bind by call position to the
-- innermost open frame, so a view that escaped its body measures nothing and
-- earns the same refusal any other call outside a body earns
-- (docs/decisions/2026-08-24-an-observation-frame-is-the-scope-of-its-call.md
-- W3).
local frameView = {}
frameView.__index = frameView

local function requireColour(key: any, tolerance: any): (number, number, number, number, boolean)
    if type(key) ~= "table"
        or type(key.red) ~= "number"
        or type(key.green) ~= "number"
        or type(key.blue) ~= "number"
        or type(key.removes) ~= "boolean"
    then
        error("key must contain numeric red/green/blue and boolean removes", 3)
    end
    if tolerance ~= nil and type(tolerance) ~= "number" then
        error("key tolerance must be numeric", 3)
    end
    return key.red, key.green, key.blue, tolerance or 12, key.removes
end

-- Every line of text the held frame carries inside the rectangle, each at the
-- rectangle THE FRAME held it in rather than the one asked about.
function frameView:read_lines(
    x: number,
    y: number,
    width: number,
    height: number
): any
    return confirmed(k_readLinesTool, {
        x = x,
        y = y,
        width = width,
        height = height,
    })
end

-- What one rectangle of the held frame is made of: its pixel count, how many
-- distinct colours it holds, its dominant colour, and how much of it the key
-- takes.  The pixels themselves never cross -- keeping a piece of the screen is
-- `explore.capture`, which is an authoring write and is judged as one.
function frameView:probe(
    x: number,
    y: number,
    width: number,
    height: number,
    key: any,
    tolerance: number?
): any
    local red, green, blue, keyTolerance, removes = requireColour(key, tolerance)
    return confirmed(k_probeTool, {
        x = x,
        y = y,
        width = width,
        height = height,
        colour_red = red,
        colour_green = green,
        colour_blue = blue,
        tolerance = keyTolerance,
        removes = removes,
    })
end

-- The colour census of one rectangle of the held frame, downsampled into cells.
-- The answer carries `columns`, `rows`, `cell_width`, `cell_height` and the flat
-- row-major `selected_pixels`, so the cell at (column, row) is at index
-- row * columns + column + 1.  Where the rectangle does not divide evenly the
-- right and bottom edges keep a PARTIAL cell and count only its real pixels.
--
-- The key is required: a cell reports how much of itself is one colour, and
-- there is no such thing as a grid of no colour.
function frameView:census_grid(
    x: number,
    y: number,
    width: number,
    height: number,
    cellWidth: number,
    cellHeight: number,
    key: any,
    tolerance: number?
): any
    local red, green, blue, keyTolerance, removes = requireColour(key, tolerance)
    return confirmed(k_censusGridTool, {
        x = x,
        y = y,
        width = width,
        height = height,
        cell_width = cellWidth,
        cell_height = cellHeight,
        colour_red = red,
        colour_green = green,
        colour_blue = blue,
        tolerance = keyTolerance,
        removes = removes,
    })
end

-- One observation.  With no body it opens the frame, resolves the state and
-- closes, which is what a single-shot observation has always been; with a body
-- it opens, resolves, RUNS THE BODY on that one frame, and closes on whichever
-- way the body ended.  The close is the observation Tool call's own scope
-- exiting, so it can be neither forgotten nor written twice.
--
-- An observation inside an observation's body is refused by name, naming the
-- holding call and the limit it exceeded.
function explore.observe(body: any?): any
    if body ~= nil and type(body) ~= "function" then
        error("explore.observe takes the function that measures its frame", 2)
    end
    if body == nil then
        return confirmed(k_observeTool, {})
    end
    local view = setmetatable({}, frameView)
    return confirmed(k_observeTool, {}, function()
        body(view)
    end)
end

-- The six input verbs, each one arm of the one input Tool's closed contract.
--
-- A pointer hold is the body-taking arm: the press is engaged before the body,
-- every observation the body issues sees it held, and scope exit lifts it even
-- when the body raises.  There is no duration beside the body; its extent is
-- the hold's extent.
function explore.hold(x: number, y: number, body: any): any
    if type(body) ~= "function" then
        error("explore.hold takes the function that observes while held", 2)
    end
    return confirmed(k_deliverTool, { action = "hold", x = x, y = y }, body)
end

function explore.click(x: number, y: number): any
    return confirmed(k_deliverTool, { action = "click", x = x, y = y })
end

function explore.move(x: number, y: number): any
    return confirmed(k_deliverTool, { action = "move", x = x, y = y })
end

function explore.scroll(notches: number): any
    return confirmed(k_deliverTool, { action = "scroll", notches = notches })
end

function explore.key(name: string): any
    return confirmed(k_deliverTool, { action = "key", key = name })
end

function explore.drag(
    x: number,
    y: number,
    endX: number,
    endY: number,
    travelMillis: number
): any
    if type(travelMillis) ~= "number" then
        error("explore.drag needs travel_ms in milliseconds", 2)
    end
    return confirmed(k_deliverTool, {
        action = "drag",
        x = x,
        y = y,
        to_x = endX,
        to_y = endY,
        travel_ms = travelMillis,
    })
end

-- The project's own authoring store.  Reading it is read-only and a session
-- under deny-all may do it; writing it is the authoring-write effect line, and
-- a session whose Operator granted none is refused by name.
function explore.read(path: any): any
    if type(path) ~= "string" then
        error("explore.read needs a project path", 2)
    end
    return confirmed(k_projectReadTool, { path = path })
end

function explore.write(path: any, content: any): any
    if type(path) ~= "string" or type(content) ~= "string" then
        error("explore.write needs a project path and string content", 2)
    end
    return confirmed(k_projectWriteTool, {
        action = "text",
        path = path,
        content = content,
    })
end

-- Keep one rectangle of the screen as a PNG inside the project.  It is the same
-- authoring write `explore.write` is and not a screen verb: what leaves the
-- frame is the file's content hash, never its pixels.
function explore.capture(
    path: any,
    x: number,
    y: number,
    width: number,
    height: number
): any
    if type(path) ~= "string" then
        error("explore.capture needs a project path", 2)
    end
    return confirmed(k_projectWriteTool, {
        action = "capture",
        path = path,
        x = x,
        y = y,
        width = width,
        height = height,
    })
end

-- Let time pass.  It is not callable inside an observation's body, and that is
-- the point rather than a precaution: a frame held across a wait is older than
-- the wait by exactly the interval that made the wait worth taking, so "wait,
-- THEN observe" is the only expressible order.
function explore.wait(millis: any): any
    if type(millis) ~= "number" then
        error("explore.wait needs a wait in milliseconds", 2)
    end
    return confirmed(k_waitTool, { duration_ms = millis })
end

return table.freeze(explore)
