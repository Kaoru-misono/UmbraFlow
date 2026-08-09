--!strict
-- regress: offline validation of RuntimeModel coverage and evidence quality.
--
-- This module consumes decoded offline records only. It does not capture frames,
-- load screenshot bytes, or mutate a RuntimeModel. It reports three independent
-- dimensions:
--
--   ambiguity       a frame/resolution or candidate conflict has competing Surfaces;
--   coverage        what each Frame has actually had reviewed;
--   missing_evidence what is still needed before a Frame/Surface can support a model.
--
-- Absence of an Assertion is `unreviewed`, never `absent`.

local regress = {}

export type Coverage = {
    frame_id: string,
    status: string,
    assertion_ids: { string },
    positive_surface_ids: { string },
    explicit_absence_ids: { string },
    unknown_assertion_ids: { string },
}

export type Finding = {
    kind: string,
    severity: string,
    frame_id: string?,
    surface_ids: { string }?,
    assertion_ids: { string }?,
    detail: string,
}

export type Report = {
    kind: string,
    accepted: boolean,
    ambiguity: { any },
    coverage: { Coverage },
    missing_evidence: { any },
    findings: { Finding },
}

local function fail(name: string, message: string)
    error(name .. " " .. message, 3)
end

local function require_string(name: string, value: any): string
    if type(value) ~= "string" or value == "" then
        fail(name, "needs a non-empty string")
    end
    return value
end

local function copy_strings(value: { string }): { string }
    local result: { string } = {}
    for index, item in value do
        result[index] = item
    end
    return table.freeze(result)
end

local function list(value: any): { any }
    if type(value) ~= "table" then
        return {}
    end
    return value
end

local function resolution_of(value: any): any?
    if type(value) ~= "table" then
        return nil
    end
    if type(value.resolution) == "table" then
        return value.resolution
    end
    if value.kind == "resolved" or value.kind == "ambiguous" or value.kind == "unknown" then
        return value
    end
    return nil
end

local function frame_id_of(value: any): string?
    if type(value) ~= "table" then
        return nil
    end
    if type(value.frame_id) == "string" then
        return value.frame_id
    end
    if type(value.frame) == "string" then
        return value.frame
    end
    return nil
end

local function surface_ids_from_resolution(resolution: any): { string }
    if type(resolution) ~= "table" then
        return {}
    end
    if resolution.kind == "resolved" and type(resolution.state) == "table" then
        if type(resolution.state.surfaces) == "table" then
            return copy_strings(resolution.state.surfaces)
        end
    elseif resolution.kind == "ambiguous" and type(resolution.candidates) == "table" then
        local result: { string } = {}
        for _, candidate in resolution.candidates do
            if type(candidate) == "table" and type(candidate.state) == "table" then
                local surfaces = candidate.state.surfaces
                if type(surfaces) == "table" and type(surfaces[#surfaces]) == "string" then
                    table.insert(result, surfaces[#surfaces])
                end
            elseif type(candidate) == "string" then
                table.insert(result, candidate)
            end
        end
        return result
    end
    return {}
end

local function unique_append(result: { string }, seen: { [string]: boolean }, value: any)
    if type(value) == "string" and not seen[value] then
        seen[value] = true
        table.insert(result, value)
    end
end

local function candidate_conflicts(workspace: any): { any }
    local result: { any } = {}
    for _, candidate_model in workspace.candidates do
        for _, conflict in candidate_model.conflicts do
            if conflict.status == "open"
                and (conflict.kind == "multiple_surface_match"
                    or conflict.kind == "shared_target_ambiguity")
            then
                table.insert(result, {
                    kind = conflict.kind,
                    severity = conflict.severity,
                    subject_ids = conflict.subject_ids,
                    detail = conflict.message,
                })
            end
        end
    end
    return result
end

local function model_surface_ids(model: any): { string }
    local result: { string } = {}
    if type(model) ~= "table" or type(model.surfaces) ~= "table" then
        return result
    end
    for _, surface in model.surfaces do
        if type(surface) == "table" and type(surface.id) == "string" then
            table.insert(result, surface.id)
        end
    end
    table.sort(result)
    return result
end

local function active_assertions(workspace: any, frame_id: string): { any }
    local result: { any } = {}
    for _, assertion in workspace.assertions do
        if assertion.frame_id == frame_id and oracle.assertion_is_active(assertion) then
            table.insert(result, assertion)
        end
    end
    return result
end

local function assertion_ids(values: { any }): { string }
    local result: { string } = {}
    for index, value in values do
        result[index] = value.id
    end
    return table.freeze(result)
end

-- regress.check(model, workspace, resolutions?) -> Report
--
-- `resolutions` is an optional offline list of `{ frame_id, resolution }`
-- records produced by the resolver adapter. The RuntimeModel is only consulted
-- for Surface IDs and is never modified. The workspace may be a decoded table;
-- oracle.read_workspace performs structural reading without touching assets.
function regress.check(model: any, workspace_input: any, resolutions: any?): Report
    local workspace = oracle.read_workspace(workspace_input)
    local ambiguity: { any } = {}
    local coverage: { Coverage } = {}
    local missing: { any } = {}
    local findings: { Finding } = {}

    for _, resolution_record in list(resolutions) do
        local resolution = resolution_of(resolution_record)
        local frame_id = frame_id_of(resolution_record)
        if resolution ~= nil and resolution.kind == "ambiguous" then
            local surfaces = surface_ids_from_resolution(resolution)
            table.insert(ambiguity, {
                frame_id = frame_id,
                candidates = surfaces,
                conflicts = resolution.conflicts,
            })
            table.insert(findings, {
                kind = "ambiguity",
                severity = "error",
                frame_id = frame_id,
                surface_ids = surfaces,
                detail = "multiple_surface_candidates_remain",
            })
        elseif resolution ~= nil and resolution.kind == "unknown" then
            table.insert(missing, {
                kind = "unresolved_frame",
                frame_id = frame_id,
                status = "unknown",
                detail = resolution.reason,
            })
            table.insert(findings, {
                kind = "missing_evidence",
                severity = "warning",
                frame_id = frame_id,
                detail = "resolution_is_unknown: " .. tostring(resolution.reason),
            })
        end
    end

    for _, conflict in candidate_conflicts(workspace) do
        table.insert(ambiguity, conflict)
        table.insert(findings, {
            kind = "ambiguity",
            severity = conflict.severity,
            surface_ids = conflict.subject_ids,
            detail = conflict.detail,
        })
    end

    for _, frame in workspace.frames do
        local assertions = active_assertions(workspace, frame.id)
        local assertion_id_list = assertion_ids(assertions)
        local positive: { string } = {}
        local absent: { string } = {}
        local unknown: { string } = {}
        local positive_seen: { [string]: boolean } = {}
        for _, assertion in assertions do
            if assertion.outcome == oracle.present then
                if oracle.assertion_claims(assertion, "surface_identity") then
                    unique_append(positive, positive_seen, assertion.claim.surface_id)
                elseif oracle.assertion_claims(assertion, "surface_stack") then
                    for _, surface_id in assertion.claim.surfaces do
                        unique_append(positive, positive_seen, surface_id)
                    end
                end
            elseif assertion.outcome == oracle.absent then
                table.insert(absent, assertion.id)
            elseif assertion.outcome == oracle.unknown then
                table.insert(unknown, assertion.id)
            end
        end

        local status: string
        if #positive > 0 then
            status = "covered"
        elseif #unknown > 0 then
            status = "unknown"
        elseif #absent > 0 then
            status = "uncovered"
        else
            status = oracle.unreviewed
        end
        table.insert(coverage, {
            frame_id = frame.id,
            status = status,
            assertion_ids = assertion_id_list,
            positive_surface_ids = copy_strings(positive),
            explicit_absence_ids = copy_strings(absent),
            unknown_assertion_ids = copy_strings(unknown),
        })

        if status == oracle.unreviewed then
            table.insert(missing, {
                kind = "frame_missing_identity_assertion",
                frame_id = frame.id,
                status = oracle.unreviewed,
                detail = "no accepted surface identity or surface stack Assertion exists",
            })
            table.insert(findings, {
                kind = "missing_evidence",
                severity = "warning",
                frame_id = frame.id,
                detail = "frame_is_unreviewed; absence_of_Assertion_is_not_absent",
            })
        elseif status == "unknown" then
            table.insert(missing, {
                kind = "frame_unknown_identity_evidence",
                frame_id = frame.id,
                status = oracle.unknown,
                assertion_ids = copy_strings(unknown),
                detail = "accepted Assertion records reviewed uncertainty",
            })
            table.insert(findings, {
                kind = "missing_evidence",
                severity = "warning",
                frame_id = frame.id,
                assertion_ids = copy_strings(unknown),
                detail = "frame_identity_is_unknown",
            })
        elseif status == "uncovered" then
            table.insert(missing, {
                kind = "frame_has_no_positive_identity",
                frame_id = frame.id,
                status = "absent",
                assertion_ids = copy_strings(absent),
                detail = "explicit negative evidence exists but no Surface identity is present",
            })
            table.insert(findings, {
                kind = "uncovered_frame",
                severity = "error",
                frame_id = frame.id,
                assertion_ids = copy_strings(absent),
                detail = "frame_has_no_positive_surface_identity",
            })
        end
    end

    local evidenced_surfaces: { [string]: boolean } = {}
    for _, entry in coverage do
        for _, surface_id in entry.positive_surface_ids do
            evidenced_surfaces[surface_id] = true
        end
    end
    for _, surface_id in model_surface_ids(model) do
        if not evidenced_surfaces[surface_id] then
            table.insert(missing, {
                kind = "surface_missing_identity_evidence",
                surface_id = surface_id,
                detail = "no accepted positive Assertion identifies this Surface",
            })
            table.insert(findings, {
                kind = "missing_evidence",
                severity = "warning",
                surface_ids = { surface_id },
                detail = "surface_has_no_accepted_identity_evidence",
            })
        end
    end

    -- A report with unresolved evidence is not a valid promotion gate, even
    -- when the individual finding is only a warning. `unreviewed`, `unknown`,
    -- and explicit absence remain different statuses in the report, but all
    -- three require more offline work before the corpus can be accepted.
    local accepted = #ambiguity == 0 and #missing == 0
    return {
        kind = "offline_regression_report",
        accepted = accepted,
        ambiguity = ambiguity,
        coverage = coverage,
        missing_evidence = missing,
        findings = findings,
    }
end

function regress.render(report: Report): string
    local lines: { string } = {}
    table.insert(lines, "offline regression " .. (if report.accepted then "accepted" else "rejected"))
    table.insert(lines, "ambiguity=" .. tostring(#report.ambiguity))
    table.insert(lines, "coverage=" .. tostring(#report.coverage))
    table.insert(lines, "missing_evidence=" .. tostring(#report.missing_evidence))
    for _, finding in report.findings do
        table.insert(lines, finding.kind .. ": " .. finding.detail)
    end
    return table.concat(lines, "\n")
end

return regress
