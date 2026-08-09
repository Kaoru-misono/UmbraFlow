--!strict
-- Privileged authoring-only exploration surface.  It is never a business
-- global and does not participate in RuntimeModel resolution or Receipt flow.

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

function explore.terminal(): boolean
    return requireNative("explore_terminal")()
end

return table.freeze(explore)
