--!strict
-- One structural vocabulary of frozen success/error envelopes for Project code.

local result = {}

local function reject(message: string): never
    error("result " .. message, 3)
end

local function checked(value: any): any
    if type(value) ~= "table" then reject("value must be a result") end
    local kind = rawget(value, "kind")
    if kind == "ok" then
        return value
    end
    local failure = rawget(value, "error")
    if kind ~= "error" or type(failure) ~= "table" then
        reject("value must be a result")
    end
    local code = rawget(failure, "code")
    if type(code) ~= "string" or code == "" then
        reject("error code must be a non-empty string")
    end
    if type(rawget(failure, "message")) ~= "string" then
        reject("error message must be a string")
    end
    return value
end

function result.ok(value: any): any
    return table.freeze({
        kind = "ok",
        value = value,
    })
end

function result.err(code: any, message: any, details: any?): any
    if type(code) ~= "string" or code == "" then
        reject("error code must be a non-empty string")
    end
    if type(message) ~= "string" then
        reject("error message must be a string")
    end
    return table.freeze({
        kind = "error",
        error = table.freeze({
            code = code,
            message = message,
            details = details,
        }),
    })
end

function result.is_ok(value: any): boolean
    return rawget(checked(value), "kind") == "ok"
end

function result.is_error(value: any): boolean
    return rawget(checked(value), "kind") == "error"
end

function result.map(value: any, mapper: any): any
    local current = checked(value)
    if type(mapper) ~= "function" then reject("map needs a function") end
    if rawget(current, "kind") == "error" then return current end
    return result.ok(mapper(rawget(current, "value")))
end

function result.map_error(value: any, mapper: any): any
    local current = checked(value)
    if type(mapper) ~= "function" then reject("map_error needs a function") end
    if rawget(current, "kind") == "ok" then return current end
    local code, message, details = mapper(rawget(current, "error"))
    return result.err(code, message, details)
end

function result.and_then(value: any, continuation: any): any
    local current = checked(value)
    if type(continuation) ~= "function" then reject("and_then needs a function") end
    if rawget(current, "kind") == "error" then return current end
    return checked(continuation(rawget(current, "value")))
end

function result.unwrap_or(value: any, fallback: any): any
    local current = checked(value)
    return if rawget(current, "kind") == "ok"
        then rawget(current, "value")
        else fallback
end

function result.match(value: any, onOk: any, onError: any): any
    local current = checked(value)
    if type(onOk) ~= "function" or type(onError) ~= "function" then
        reject("match needs ok and error functions")
    end
    if rawget(current, "kind") == "ok" then
        return onOk(rawget(current, "value"))
    end
    return onError(rawget(current, "error"))
end

return table.freeze(result)
