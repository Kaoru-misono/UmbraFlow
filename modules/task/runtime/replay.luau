--!strict
-- replay: compare offline ObservedTransition records with RuntimeModel policy.
--
-- Replay never edits the model. A DeclaredTransition is policy, an
-- ObservedTransition is evidence, and the report is a third value. Resolved
-- endpoints are checked as exact RuntimeState values, including their ordered
-- Surface stack. Ambiguous and Unknown endpoints remain explicit findings.

local replay = {}

export type TransitionResult = {
    observed_id: string,
    status: string,
    declared_id: string?,
    from: any,
    to: any,
    trigger: any,
    detail: string?,
}

export type Finding = {
    kind: string,
    observed_id: string,
    detail: string,
    resolution_kind: string?,
    declared_ids: { string }?,
}

export type Report = {
    kind: string,
    accepted: boolean,
    observed_count: number,
    matched_count: number,
    results: { TransitionResult },
    findings: { Finding },
    declared_transitions: { any },
}

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

local function copy_strings(name: string, value: any): { string }
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
    return table.freeze(result)
end

local function records(value: any): { any }
    if type(value) == "table" and type(value.transitions) == "table" then
        return value.transitions
    end
    if type(value) ~= "table" then
        fail("replay.check", "needs an array of ObservedTransition records")
    end
    return value
end

local function normalize_observed(index: number, value: any): any
    if navigation.ObservedTransition.is(value) then
        return value
    end
    return navigation.ObservedTransition.new(value)
end

local function endpoint_state(value: any): any?
    return navigation.endpoint_state(value)
end

local function endpoint_kind(value: any): string
    return navigation.endpoint_kind(value)
end

local function declared_transitions(model: any): { any }
    if type(model) ~= "table" or type(model.transitions) ~= "table" then
        fail("replay.check", "needs a RuntimeModel with transitions")
    end
    local result: { any } = {}
    for index, transition in model.transitions do
        if not navigation.validate_declared_transition(transition) then
            fail("replay.check.transitions[" .. tostring(index) .. "]", "is not a DeclaredTransition")
        end
        result[index] = transition
    end
    return result
end

local function find_by_from_and_trigger(model: any, transitions: { any }, observed: any): { any }
    local from = endpoint_state(observed.from)
    if from == nil then
        return {}
    end
    local result: { any } = {}
    for _, declared in transitions do
        if navigation.state_equal(declared.from, from)
            and navigation.transition_trigger_matches(model, declared, observed.trigger)
        then
            table.insert(result, declared)
        end
    end
    return result
end

local function ids(values: { any }): { string }
    local result: { string } = {}
    for index, value in values do
        result[index] = value.id
    end
    return table.freeze(result)
end

local function result_for(observed: any, status: string, detail: string?, declared_id: string?): TransitionResult
    return {
        observed_id = observed.id,
        status = status,
        declared_id = declared_id,
        from = observed.from,
        to = observed.to,
        trigger = observed.trigger,
        detail = detail,
    }
end

local function finding_for(
    observed: any,
    kind: string,
    detail: string,
    resolution_kind: string?,
    declared_ids: { string }?
): Finding
    return {
        kind = kind,
        observed_id = observed.id,
        detail = detail,
        resolution_kind = resolution_kind,
        declared_ids = declared_ids,
    }
end

-- replay.check(model, observed_transitions) -> Report
--
-- `model` is the new RuntimeModel shape. `observed_transitions` can be the
-- array itself or `{ transitions = array }`; no frame asset is opened in either
-- form. A destination mismatch is reported separately from a trigger for which
-- no declared policy exists.
function replay.check(model: any, observed_transitions: any): Report
    local declared = declared_transitions(model)
    local input = records(observed_transitions)
    local results: { TransitionResult } = {}
    local findings: { Finding } = {}
    local matched = 0

    for index, value in input do
        local observed = normalize_observed(index, value)
        local from_kind = endpoint_kind(observed.from)
        local to_kind = endpoint_kind(observed.to)

        if from_kind ~= "resolved" then
            table.insert(results, result_for(observed, "unresolved", "observed source surface is not resolved", nil))
            table.insert(
                findings,
                finding_for(
                    observed,
                    if from_kind == "ambiguous" then "ambiguous_surface" else "unknown_surface",
                    "replay cannot verify a transition from a " .. from_kind .. " surface result",
                    from_kind,
                    nil
                )
            )
        elseif to_kind ~= "resolved" then
            table.insert(results, result_for(observed, "unresolved", "observed destination surface is not resolved", nil))
            table.insert(
                findings,
                finding_for(
                    observed,
                    if to_kind == "ambiguous" then "ambiguous_surface" else "unknown_surface",
                    "replay cannot verify a transition to a " .. to_kind .. " surface result",
                    to_kind,
                    nil
                )
            )
        else
            local from = endpoint_state(observed.from) :: any
            local to = endpoint_state(observed.to) :: any
            if not navigation.validate_surface_stack(model, from)
                or not navigation.validate_surface_stack(model, to)
            then
                table.insert(results, result_for(observed, "invalid_stack", "observed RuntimeState has an invalid Surface stack", nil))
                table.insert(
                    findings,
                    finding_for(
                        observed,
                        "invalid_surface_stack",
                        "replay requires valid bottom-to-top RuntimeState Surface stacks",
                        nil,
                        nil
                    )
                )
            else
                local candidates = find_by_from_and_trigger(model, declared, observed)
                local exact: any = nil
                for _, candidate in candidates do
                    if navigation.state_equal(candidate.to, to) then
                        exact = candidate
                        break
                    end
                end
                if exact ~= nil then
                    matched += 1
                    table.insert(results, result_for(observed, "matched", nil, exact.id))
                elseif #candidates > 0 then
                    table.insert(
                        results,
                        result_for(
                            observed,
                            "unexpected_destination",
                            "observed_destination_differs_from_declared_destination",
                            nil
                        )
                    )
                    table.insert(
                        findings,
                        finding_for(
                            observed,
                            "unexpected_destination",
                            "observed_destination_differs_from_declared_destination",
                            nil,
                            ids(candidates)
                        )
                    )
                else
                    table.insert(results, result_for(observed, "unexpected_transition", "no_declared_transition", nil))
                    table.insert(
                        findings,
                        finding_for(
                            observed,
                            "unexpected_transition",
                            "no_declared_transition_matches_source_and_trigger",
                            nil,
                            nil
                        )
                    )
                end
            end
        end
    end

    return {
        kind = "replay_report",
        accepted = #findings == 0,
        observed_count = #input,
        matched_count = matched,
        results = results,
        findings = findings,
        declared_transitions = declared,
    }
end

function replay.render(report: Report): string
    local lines: { string } = {}
    table.insert(
        lines,
        "replay " .. (if report.accepted then "accepted" else "rejected")
            .. " observed=" .. tostring(report.observed_count)
            .. " matched=" .. tostring(report.matched_count)
    )
    for _, finding in report.findings do
        table.insert(lines, finding.kind .. ": " .. finding.observed_id .. ": " .. finding.detail)
    end
    return table.concat(lines, "\n")
end

return replay
