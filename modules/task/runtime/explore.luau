--!strict
-- Privileged authoring-only exploration surface.  It is never a business
-- global and does not participate in RuntimeModel resolution or Receipt flow.
--
-- The acting verbs live on the CYCLE view rather than on this module, because
-- every one of them spends the cycle it was called on: a ticket that is never
-- handed out cannot be carried past the act that consumed it, and a chunk that
-- wants to measure what it did has to open another cycle to do it.  The host
-- refuses a second act on a spent ticket, so this layer keeps no copy of that
-- rule -- one place answers "is this cycle still alive", and it is the ledger.

local native = ...
local explore = {}

local function requireNative(name: string): any
    if type(native) ~= "table" or type(native[name]) ~= "function" then
        error("exploration Host does not provide '" .. name .. "'", 3)
    end
    return native[name]
end

local function keyScalars(key: any, tolerance: any)
    if key == nil then return end
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
    return key.red, key.green, key.blue, tolerance, key.removes
end

function explore.cycle(callback: any): any
    if type(callback) ~= "function" then error("explore.cycle needs a function", 2) end
    local state = { live = true, ticket = requireNative("explore_cycle_open")() }
    if state.ticket == nil then error("Host refused exploration cycle", 2) end
    local view = {}

    local function live()
        if not state.live then error("exploration cycle is closed", 3) end
    end

    function view:crop(
        x: number,
        y: number,
        width: number,
        height: number,
        key: any?,
        tolerance: number?
    ): (string, string, any)
        live()
        return requireNative("explore_crop")(
            state.ticket,
            x,
            y,
            width,
            height,
            keyScalars(key, tolerance)
        )
    end

    -- Clicks a bare coordinate and spends the cycle.  A business script clicks
    -- what a Binding placed; an agent has no model yet and clicks where it
    -- decided to look, which is how the first screen ever gets modelled.
    function view:click_point(x: number, y: number)
        live()
        requireNative("explore_click_point")(state.ticket, x, y)
    end

    -- click_point with a duration.  A long press is how a target volunteers
    -- what it will not show otherwise, so it is a thing to be measured.  The
    -- hold has NO DEFAULT: how long a target wants a press held is a fact about
    -- that target, and a duration the caller cannot see is a decision the
    -- caller did not make.
    function view:long_press(x: number, y: number, holdMillis: number)
        live()
        if type(holdMillis) ~= "number" then
            error("view:long_press needs hold_ms in milliseconds", 2)
        end
        requireNative("explore_long_press")(state.ticket, x, y, holdMillis)
    end

    -- Presses one bare point, travels to another with the button held, and
    -- releases there.  Both ends are bare coordinates: an agent measuring the
    -- geometry annotation will later rest on cannot be made to name an element
    -- that does not exist yet.  The travel has no default, for the hold's
    -- reason.
    function view:drag(
        x: number,
        y: number,
        endX: number,
        endY: number,
        travelMillis: number
    )
        live()
        if type(travelMillis) ~= "number" then
            error("view:drag needs travel_ms in milliseconds", 2)
        end
        requireNative("explore_drag")(state.ticket, x, y, endX, endY, travelMillis)
    end

    -- Moves the pointer, pressing nothing.  It is the input a wheel needs in
    -- front of it, because a scroll names no position and lands wherever the
    -- pointer already was.
    function view:move_pointer(x: number, y: number)
        live()
        requireNative("explore_move_pointer")(state.ticket, x, y)
    end

    -- One wheel scroll of `notches` detents, positive away from the operator
    -- and negative toward them.
    function view:scroll(notches: number)
        live()
        requireNative("explore_scroll")(state.ticket, notches)
    end

    -- One press and release of the key `name` prints, spending the cycle
    -- exactly as a click does.  `name` is the target's own printed spelling and
    -- the host holds the only list of which spellings exist; this layer does
    -- not repeat it, so the two can never come to disagree about it.
    function view:key(name: string)
        live()
        requireNative("explore_key")(state.ticket, name)
    end

    local ok, result = pcall(callback, table.freeze(view))
    state.live = false
    local closed, closeError = pcall(requireNative("explore_cycle_close"), state.ticket)
    if ok and not closed then error(closeError, 0) end
    if not ok then error(result, 0) end
    return result
end

function explore.probe(
    blob: string,
    x: number,
    y: number,
    width: number,
    height: number,
    key: any?,
    tolerance: number?
): any
    return requireNative("explore_probe")(
        blob,
        x,
        y,
        width,
        height,
        keyScalars(key, tolerance)
    )
end

function explore.read(path: any): string
    if type(path) ~= "string" then error("explore.read needs a project path", 2) end
    return requireNative("explore_project_read")(path)
end

function explore.write(path: any, content: any)
    if type(path) ~= "string" or type(content) ~= "string" then
        error("explore.write needs a project path and string content", 2)
    end
    requireNative("explore_project_write")(path, content)
end

-- Pauses for the whole duration, and refuses while a cycle is open.
--
-- It is not on the cycle view for the reason it exists: a frame held across a
-- wait is older than the wait by exactly the interval that made the wait worth
-- taking.  Acting, settling and then measuring is therefore written as three
-- statements -- act inside one cycle, settle outside every cycle, measure
-- inside the next -- and the host refuses every other order.
function explore.settle(millis: number)
    if type(millis) ~= "number" then
        error("explore.settle needs a wait in milliseconds", 2)
    end
    requireNative("explore_settle")(millis)
end

function explore.terminal(): boolean
    return requireNative("explore_terminal")()
end

return table.freeze(explore)
