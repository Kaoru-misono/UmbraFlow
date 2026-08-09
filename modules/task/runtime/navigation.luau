--!strict
-- navigation: the runtime transition graph and its offline observations.
--
-- This module deliberately has three different record types:
--
--   DeclaredTransition  runtime policy compiled into RuntimeModel;
--   ObservedTransition  an offline fact recovered from a trace;
--   StatePattern        the exact RuntimeState shape a policy names.
--
-- None of these records is a mutable runtime belief. A transition comparison
-- belongs to replay; this module only validates records and supplies
-- deterministic state/trigger operations.

local navigation = {}

export type RuntimeState = {
    context: string,
    surfaces: { string },
}

export type StatePattern = RuntimeState

export type DeclaredTransition = {
    id: string,
    from: StatePattern,
    trigger: { [string]: any },
    to: StatePattern,
    verification: {
        kind: string,
        timeout_ms: number,
        attempts: number,
    },
}

export type ObservedTransition = {
    id: string,
    from: any,
    trigger: { [string]: any },
    to: any,
    frames: { string },
}

local declaredRecords: { [any]: boolean } = setmetatable({}, { __mode = "k" }) :: any
local observedRecords: { [any]: boolean } = setmetatable({}, { __mode = "k" }) :: any
local stateRecords: { [any]: boolean } = setmetatable({}, { __mode = "k" }) :: any

local function fail(name: string, message: string)
    error(name .. " " .. message, 3)
end

local function require_table(name: string, value: any): { [string]: any }
    if type(value) ~= "table" then
        fail(name, "needs a table")
    end
    return value
end

local function require_string(name: string, value: any): string
    if type(value) ~= "string" or value == "" then
        fail(name, "needs a non-empty string")
    end
    return value
end

local function require_integer(name: string, value: any, minimum: number): number
    if type(value) ~= "number" or value % 1 ~= 0 or value < minimum then
        fail(name, "needs an integer >= " .. tostring(minimum))
    end
    return value
end

local function reject_unknown(
    name: string,
    value: { [string]: any },
    known: { [string]: boolean }
)
    for key in value do
        if not known[key] then
            fail(name, "does not accept field '" .. tostring(key) .. "'")
        end
    end
end

local function copy_strings(name: string, value: any, non_empty: boolean): { string }
    if type(value) ~= "table" then
        fail(name, "needs an array of strings")
    end
    local result: { string } = {}
    local seen: { [string]: boolean } = {}
    for index = 1, #value do
        local item = require_string(name .. "[" .. tostring(index) .. "]", value[index])
        if seen[item] then
            fail(name, "contains duplicate '" .. item .. "'")
        end
        seen[item] = true
        result[index] = item
    end
    if non_empty and #result == 0 then
        fail(name, "must not be empty")
    end
    return table.freeze(result)
end

local function copy_values(name: string, value: any): { any }
    if type(value) ~= "table" then
        fail(name, "needs an array")
    end
    local result: { any } = {}
    for index = 1, #value do
        result[index] = value[index]
    end
    return table.freeze(result)
end

local function state_value(name: string, value: any): RuntimeState
    local spec = require_table(name, value)
    reject_unknown(name, spec, { context = true, surfaces = true })
    local context = require_string(name .. ".context", spec.context)
    local surfaces = copy_strings(name .. ".surfaces", spec.surfaces, true)
    local state = table.freeze({ context = context, surfaces = surfaces })
    stateRecords[state] = true
    return state
end

local function state_like(name: string, value: any): RuntimeState
    if stateRecords[value] then
        return value :: RuntimeState
    end
    return state_value(name, value)
end

local function copy_trigger(name: string, value: any): { [string]: any }
    local spec = require_table(name, value)
    local kind = require_string(name .. ".kind", spec.kind)
    if kind == "action" then
        reject_unknown(name, spec, { kind = true, binding = true, action = true })
        return table.freeze({
            kind = kind,
            binding = require_string(name .. ".binding", spec.binding),
            action = require_string(name .. ".action", spec.action),
        })
    elseif kind == "timeout" then
        reject_unknown(name, spec, { kind = true, milliseconds = true })
        return table.freeze({
            kind = kind,
            milliseconds = require_integer(name .. ".milliseconds", spec.milliseconds, 0),
        })
    elseif kind == "external" then
        reject_unknown(name, spec, { kind = true, name = true })
        return table.freeze({
            kind = kind,
            name = require_string(name .. ".name", spec.name),
        })
    end
    fail(name, "has unknown trigger kind '" .. kind .. "'")
end

local function observed_trigger(name: string, value: any): { [string]: any }
    local spec = require_table(name, value)
    local kind = require_string(name .. ".kind", spec.kind)
    if kind == "action" then
        return copy_trigger(name, spec)
    elseif kind == "timeout" then
        return copy_trigger(name, spec)
    elseif kind == "external" then
        return copy_trigger(name, spec)
    elseif kind == "click" or kind == "key" then
        -- Offline adapters may not know the binding ID. They retain the
        -- observed target/key without pretending it is a runtime action.
        local known = if kind == "click"
            then { kind = true, target = true }
            else { kind = true, key = true }
        reject_unknown(name, spec, known)
        local result: { [string]: any } = { kind = kind }
        if kind == "click" then
            result.target = require_string(name .. ".target", spec.target)
        else
            result.key = require_string(name .. ".key", spec.key)
        end
        return table.freeze(result)
    end
    fail(name, "has unknown observed trigger kind '" .. kind .. "'")
end

local function endpoint(name: string, value: any): any
    local spec = require_table(name, value)
    if spec.kind == "resolved" then
        reject_unknown(name, spec, { kind = true, state = true, evidence = true })
        return table.freeze({
            kind = "resolved",
            state = state_like(name .. ".state", spec.state),
            evidence = copy_values(name .. ".evidence", spec.evidence or {}),
        })
    elseif spec.kind == "ambiguous" then
        reject_unknown(name, spec, { kind = true, candidates = true, conflicts = true, evidence = true })
        return table.freeze({
            kind = "ambiguous",
            candidates = copy_values(name .. ".candidates", spec.candidates),
            conflicts = copy_values(name .. ".conflicts", spec.conflicts or {}),
            evidence = copy_values(name .. ".evidence", spec.evidence or {}),
        })
    elseif spec.kind == "unknown" then
        reject_unknown(name, spec, { kind = true, reason = true, evidence = true, diagnostic = true })
        return table.freeze({
            kind = "unknown",
            reason = require_string(name .. ".reason", spec.reason),
            evidence = copy_values(name .. ".evidence", spec.evidence or {}),
            diagnostic = spec.diagnostic,
        })
    end
    return state_like(name, value)
end

local function frame_ids(name: string, value: any): { string }
    return copy_strings(name, value, true)
end

local function trigger_key(trigger: { [string]: any }): string
    if trigger.kind == "action" then
        return "action\0" .. trigger.binding .. "\0" .. trigger.action
    elseif trigger.kind == "timeout" then
        return "timeout\0" .. tostring(trigger.milliseconds)
    elseif trigger.kind == "external" then
        return "external\0" .. trigger.name
    elseif trigger.kind == "click" then
        return "click\0" .. trigger.target
    elseif trigger.kind == "key" then
        return "key\0" .. trigger.key
    end
    return "unknown\0" .. tostring(trigger.kind)
end

local function state_key(state: RuntimeState): string
    return state.context .. "\0" .. table.concat(state.surfaces, "\0")
end

local function equal_lists(left: { string }, right: { string }): boolean
    if #left ~= #right then
        return false
    end
    for index = 1, #left do
        if left[index] ~= right[index] then
            return false
        end
    end
    return true
end

function navigation.runtime_state(value: any): RuntimeState
    return state_like("RuntimeState", value)
end

navigation.state_pattern = navigation.runtime_state

function navigation.is_runtime_state(value: any): boolean
    if stateRecords[value] then
        return true
    end
    if type(value) ~= "table" then
        return false
    end
    local ok = pcall(function()
        state_value("RuntimeState", value)
    end)
    return ok
end

function navigation.state_equal(left: any, right: any): boolean
    if not navigation.is_runtime_state(left) or not navigation.is_runtime_state(right) then
        return false
    end
    local a = state_like("left", left)
    local b = state_like("right", right)
    return a.context == b.context and equal_lists(a.surfaces, b.surfaces)
end

function navigation.state_key(value: any): string
    return state_key(state_like("RuntimeState", value))
end

function navigation.trigger_key(value: any): string
    return trigger_key(observed_trigger("Trigger", value))
end

local DeclaredTransition = {}
navigation.DeclaredTransition = DeclaredTransition

function DeclaredTransition.new(spec: { [string]: any }): DeclaredTransition
    local value = require_table("DeclaredTransition.new", spec)
    reject_unknown("DeclaredTransition.new", value, {
        id = true,
        from = true,
        trigger = true,
        to = true,
        verification = true,
    })
    local verification = require_table("DeclaredTransition.new.verification", value.verification)
    reject_unknown(
        "DeclaredTransition.new.verification",
        verification,
        { kind = true, timeout_ms = true, attempts = true }
    )
    if verification.kind ~= "resolve" then
        fail("DeclaredTransition.new.verification.kind", "must be 'resolve'")
    end
    local record = table.freeze({
        id = require_string("DeclaredTransition.new.id", value.id),
        from = state_value("DeclaredTransition.new.from", value.from),
        trigger = copy_trigger("DeclaredTransition.new.trigger", value.trigger),
        to = state_value("DeclaredTransition.new.to", value.to),
        verification = table.freeze({
            kind = "resolve",
            timeout_ms = require_integer(
                "DeclaredTransition.new.verification.timeout_ms",
                verification.timeout_ms,
                0
            ),
            attempts = require_integer(
                "DeclaredTransition.new.verification.attempts",
                verification.attempts,
                1
            ),
        }),
    })
    declaredRecords[record] = true
    return record
end

function DeclaredTransition.is(value: any): boolean
    return declaredRecords[value] == true
end

function navigation.validate_declared_transition(value: any): boolean
    if DeclaredTransition.is(value) then
        return true
    end
    local ok = pcall(function()
        DeclaredTransition.new(value)
    end)
    return ok
end

local ObservedTransition = {}
navigation.ObservedTransition = ObservedTransition

function ObservedTransition.new(spec: { [string]: any }): ObservedTransition
    local value = require_table("ObservedTransition.new", spec)
    reject_unknown("ObservedTransition.new", value, {
        id = true,
        from = true,
        trigger = true,
        to = true,
        frames = true,
    })
    local record = table.freeze({
        id = require_string("ObservedTransition.new.id", value.id),
        from = endpoint("ObservedTransition.new.from", value.from),
        trigger = observed_trigger("ObservedTransition.new.trigger", value.trigger),
        to = endpoint("ObservedTransition.new.to", value.to),
        frames = frame_ids("ObservedTransition.new.frames", value.frames),
    })
    observedRecords[record] = true
    return record
end

function ObservedTransition.is(value: any): boolean
    return observedRecords[value] == true
end

function navigation.is_endpoint_resolved(value: any): boolean
    return type(value) == "table"
        and value.kind == "resolved"
        and navigation.is_runtime_state(value.state)
end

function navigation.endpoint_state(value: any): RuntimeState?
    if navigation.is_runtime_state(value) then
        return state_like("endpoint", value)
    end
    if navigation.is_endpoint_resolved(value) then
        return state_like("endpoint.state", value.state)
    end
    return nil
end

function navigation.endpoint_kind(value: any): string
    if navigation.is_runtime_state(value) then
        return "resolved"
    end
    if type(value) == "table" and type(value.kind) == "string" then
        return value.kind
    end
    return "unknown"
end

function navigation.transition_trigger_matches(
    model: any,
    declared: any,
    observed: any
): boolean
    if type(declared) ~= "table" or type(observed) ~= "table" then
        return false
    end
    local expected = declared.trigger
    if expected.kind == observed.kind then
        if expected.kind == "action" then
            if observed.binding ~= nil and observed.action ~= nil then
                return expected.binding == observed.binding and expected.action == observed.action
            end
            return false
        end
        if expected.kind == "timeout" then
            return expected.milliseconds == observed.milliseconds
        end
        return expected.name == observed.name
    end
    if expected.kind == "action" and observed.kind == "click" then
        if type(model) ~= "table" or type(model.bindings) ~= "table" then
            return false
        end
        for _, binding in model.bindings do
            if binding.id == expected.binding and binding.target == observed.target then
                for _, action in binding.actions do
                    if action.id == expected.action and action.kind == "click" then
                        return true
                    end
                end
            end
        end
    end
    return false
end

function navigation.validate_surface_stack(model: any, state: any): boolean
    local value = state_like("RuntimeState", state)
    if type(model) ~= "table" or type(model.surfaces) ~= "table" then
        return false
    end
    local by_id: { [string]: any } = {}
    for _, surface in model.surfaces do
        if type(surface) == "table" and type(surface.id) == "string" then
            by_id[surface.id] = surface
        end
    end
    local previous: any = nil
    local seen: { [string]: boolean } = {}
    for index, id in value.surfaces do
        if seen[id] or by_id[id] == nil then
            return false
        end
        seen[id] = true
        local surface = by_id[id]
        if index == 1 then
            if surface.kind ~= "scene" then
                return false
            end
            if type(surface.contexts) ~= "table" then
                return false
            end
            local context_ok = false
            for _, context in surface.contexts do
                if context == value.context then
                    context_ok = true
                end
            end
            if not context_ok then
                return false
            end
        elseif surface.kind == "overlay" then
            if previous == nil or type(surface.covers) ~= "table" then
                return false
            end
            local context_ok = false
            for _, context in surface.contexts or {} do
                if context == value.context then
                    context_ok = true
                end
            end
            if not context_ok then
                return false
            end
            local covers_previous = false
            for _, covered in surface.covers do
                if covered == previous.id then
                    covers_previous = true
                end
            end
            if not covers_previous then
                return false
            end
        elseif surface.kind == "interrupt" then
            if index ~= #value.surfaces then
                return false
            end
            if type(surface.contexts) == "table" and #surface.contexts > 0 then
                local context_ok = false
                for _, context in surface.contexts do
                    if context == value.context then
                        context_ok = true
                    end
                end
                if not context_ok then
                    return false
                end
            end
        else
            return false
        end
        previous = surface
    end
    return true
end

function navigation.find_declared(
    model: any,
    from_state: any,
    trigger: any,
    to_state: any?
): { any }
    local from = state_like("find_declared.from", from_state)
    local observed_trigger = observed_trigger("find_declared.trigger", trigger)
    local matches: { any } = {}
    if type(model) ~= "table" or type(model.transitions) ~= "table" then
        return matches
    end
    for _, value in model.transitions do
        if type(value) == "table"
            and navigation.state_equal(value.from, from)
            and navigation.transition_trigger_matches(model, value, observed_trigger)
        then
            if to_state == nil or navigation.state_equal(value.to, to_state) then
                table.insert(matches, value)
            end
        end
    end
    return matches
end

return navigation
