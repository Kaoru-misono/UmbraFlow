--!strict
-- oracle: offline annotation records only.
--
-- Frame, Observation, Assertion, and CandidateModel are evidence/workspace
-- records. They are never read by the ordinary runtime resolver and this module
-- never opens an asset path. A Frame asset is metadata identifying an offline
-- corpus file; it is not a runtime locator asset.

local oracle = {}

oracle.present = "present"
oracle.absent = "absent"
oracle.unknown = "unknown"
oracle.unreviewed = "unreviewed"

oracle.review_open = "open"
oracle.review_accepted = "accepted"
oracle.review_rejected = "rejected"
oracle.review_superseded = "superseded"

export type Frame = {
    id: string,
    asset: { path: string, sha256: string, width: number, height: number },
    captured_at: string,
    source: string,
    tags: { string }?,
}

export type Observation = {
    id: string,
    frame_id: string,
    subject: { [string]: any },
    measurement: { [string]: any },
    classification: string,
    confidence: number,
    provenance: { [string]: any },
}

export type Assertion = {
    id: string,
    frame_id: string,
    claim: { [string]: any },
    outcome: string,
    supporting_observations: { string },
    review: { [string]: any },
    provenance: { [string]: any },
}

export type CandidateModel = {
    id: string,
    project_id: string,
    revision: number,
    status: string,
    source_frame_ids: { string },
    targets: { any },
    entities: { any },
    patches: { any },
    conflicts: { any },
}

export type Workspace = {
    schema_version: number,
    project_id: string,
    frames: { Frame },
    observations: { Observation },
    assertions: { Assertion },
    candidates: { CandidateModel },
}

local frameRecords: { [any]: boolean } = setmetatable({}, { __mode = "k" }) :: any
local observationRecords: { [any]: boolean } = setmetatable({}, { __mode = "k" }) :: any
local assertionRecords: { [any]: boolean } = setmetatable({}, { __mode = "k" }) :: any
local candidateRecords: { [any]: boolean } = setmetatable({}, { __mode = "k" }) :: any
local workspaceRecords: { [any]: boolean } = setmetatable({}, { __mode = "k" }) :: any

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

local function require_confidence(name: string, value: any): number
    if type(value) ~= "number" or value < 0 or value > 1 then
        fail(name, "needs a number between 0 and 1")
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

local function hash(value: any, name: string): string
    local text = require_string(name, value)
    if #text ~= 64 or string.match(text, "[^0123456789abcdef]") ~= nil then
        fail(name, "must be a lowercase SHA-256 digest")
    end
    return text
end

local function asset(name: string, value: any): { [string]: any }
    local spec = require_table(name, value)
    reject_unknown(name, spec, { path = true, sha256 = true, width = true, height = true })
    return table.freeze({
        path = require_string(name .. ".path", spec.path),
        sha256 = hash(spec.sha256, name .. ".sha256"),
        width = require_integer(name .. ".width", spec.width, 1),
        height = require_integer(name .. ".height", spec.height, 1),
    })
end

local function provenance(name: string, value: any): { [string]: any }
    local spec = require_table(name, value)
    reject_unknown(name, spec, { run_id = true, agent = true, agent_version = true, created_at = true })
    local result: { [string]: any } = {
        run_id = require_string(name .. ".run_id", spec.run_id),
        agent = require_string(name .. ".agent", spec.agent),
        created_at = require_string(name .. ".created_at", spec.created_at),
    }
    if spec.agent_version ~= nil then
        result.agent_version = require_string(name .. ".agent_version", spec.agent_version)
    end
    return table.freeze(result)
end

local function rect(name: string, value: any): { number }
    if type(value) ~= "table" or #value ~= 4 then
        fail(name, "needs [x, y, width, height]")
    end
    local result: { number } = {}
    for index = 1, 4 do
        result[index] = require_integer(name .. "[" .. tostring(index) .. "]", value[index], 0)
    end
    if result[3] == 0 or result[4] == 0 then
        fail(name, "needs positive width and height")
    end
    return table.freeze(result)
end

local function subject(name: string, value: any): { [string]: any }
    local spec = require_table(name, value)
    local kind = require_string(name .. ".kind", spec.kind)
    if kind == "target" then
        reject_unknown(name, spec, { kind = true, target_id = true, rect = true })
        local result: { [string]: any } = {
            kind = kind,
            target_id = require_string(name .. ".target_id", spec.target_id),
        }
        if spec.rect ~= nil then
            result.rect = rect(name .. ".rect", spec.rect)
        end
        return table.freeze(result)
    elseif kind == "surface" then
        reject_unknown(name, spec, { kind = true, surface_id = true })
        return table.freeze({
            kind = kind,
            surface_id = require_string(name .. ".surface_id", spec.surface_id),
        })
    elseif kind == "region" then
        reject_unknown(name, spec, { kind = true, rect = true })
        return table.freeze({ kind = kind, rect = rect(name .. ".rect", spec.rect) })
    end
    fail(name, "has unknown subject kind '" .. kind .. "'")
end

local function measurement(name: string, value: any): { [string]: any }
    local spec = require_table(name, value)
    local kind = require_string(name .. ".kind", spec.kind)
    if kind == "template_score" then
        reject_unknown(name, spec, { kind = true, locator = true, variant = true, score = true, rect = true })
        local result: { [string]: any } = {
            kind = kind,
            locator = require_string(name .. ".locator", spec.locator),
            score = if type(spec.score) == "number" then spec.score else fail(name .. ".score", "needs a number"),
            rect = rect(name .. ".rect", spec.rect),
        }
        if spec.variant ~= nil then
            result.variant = require_string(name .. ".variant", spec.variant)
        end
        return table.freeze(result)
    elseif kind == "ocr" then
        reject_unknown(name, spec, { kind = true, reader = true, text = true, confidence = true, lines = true, rect = true })
        return table.freeze({
            kind = kind,
            reader = require_string(name .. ".reader", spec.reader),
            text = if type(spec.text) == "string" then spec.text else fail(name .. ".text", "needs a string"),
            confidence = require_confidence(name .. ".confidence", spec.confidence),
            lines = spec.lines,
            rect = rect(name .. ".rect", spec.rect),
        })
    elseif kind == "geometry" then
        reject_unknown(name, spec, { kind = true, rect = true })
        return table.freeze({ kind = kind, rect = rect(name .. ".rect", spec.rect) })
    elseif kind == "item_count" then
        reject_unknown(name, spec, { kind = true, count = true })
        return table.freeze({ kind = kind, count = require_integer(name .. ".count", spec.count, 0) })
    end
    fail(name, "has unknown measurement kind '" .. kind .. "'")
end

local Frame = {}
oracle.Frame = Frame

function Frame.new(spec: { [string]: any }): Frame
    local value = require_table("Frame.new", spec)
    reject_unknown("Frame.new", value, { id = true, asset = true, captured_at = true, source = true, tags = true })
    local source = require_string("Frame.new.source", value.source)
    if source ~= "exploration_trace" and source ~= "manual_capture" and source ~= "import" then
        fail("Frame.new.source", "must be exploration_trace, manual_capture, or import")
    end
    local result: { [string]: any } = {
        id = require_string("Frame.new.id", value.id),
        asset = asset("Frame.new.asset", value.asset),
        captured_at = require_string("Frame.new.captured_at", value.captured_at),
        source = source,
    }
    if value.tags ~= nil then
        result.tags = copy_strings("Frame.new.tags", value.tags, false)
    end
    local frame = table.freeze(result) :: any
    frameRecords[frame] = true
    return frame
end

function Frame.is(value: any): boolean
    return frameRecords[value] == true
end

local Observation = {}
oracle.Observation = Observation

function Observation.new(spec: { [string]: any }): Observation
    local value = require_table("Observation.new", spec)
    reject_unknown("Observation.new", value, {
        id = true,
        frame_id = true,
        subject = true,
        measurement = true,
        classification = true,
        confidence = true,
        provenance = true,
    })
    local classification = require_string("Observation.new.classification", value.classification)
    if classification ~= oracle.present
        and classification ~= oracle.absent
        and classification ~= oracle.unknown
    then
        fail("Observation.new.classification", "must be present, absent, or unknown")
    end
    local observation = table.freeze({
        id = require_string("Observation.new.id", value.id),
        frame_id = require_string("Observation.new.frame_id", value.frame_id),
        subject = subject("Observation.new.subject", value.subject),
        measurement = measurement("Observation.new.measurement", value.measurement),
        classification = classification,
        confidence = require_confidence("Observation.new.confidence", value.confidence),
        provenance = provenance("Observation.new.provenance", value.provenance),
    })
    observationRecords[observation] = true
    return observation
end

function Observation.is(value: any): boolean
    return observationRecords[value] == true
end

local function claim(name: string, value: any): { [string]: any }
    local spec = require_table(name, value)
    local kind = require_string(name .. ".kind", spec.kind)
    if kind == "surface_identity" then
        reject_unknown(name, spec, { kind = true, surface_id = true })
        return table.freeze({ kind = kind, surface_id = require_string(name .. ".surface_id", spec.surface_id) })
    elseif kind == "target_locator" then
        reject_unknown(name, spec, { kind = true, target_id = true, locator = true, variant = true })
        local result: { [string]: any } = {
            kind = kind,
            target_id = require_string(name .. ".target_id", spec.target_id),
            locator = require_string(name .. ".locator", spec.locator),
        }
        if spec.variant ~= nil then
            result.variant = require_string(name .. ".variant", spec.variant)
        end
        return table.freeze(result)
    elseif kind == "text" then
        reject_unknown(name, spec, { kind = true, reader = true, operator = true, value = true })
        local operator = require_string(name .. ".operator", spec.operator)
        if operator ~= "equals" and operator ~= "contains_all" and operator ~= "present" then
            fail(name .. ".operator", "must be equals, contains_all, or present")
        end
        local value_field = spec.value
        if operator == "equals" and type(value_field) ~= "string" then
            fail(name .. ".value", "equals needs a string")
        elseif operator == "contains_all" then
            value_field = copy_strings(name .. ".value", value_field, true)
        elseif operator == "present" and value_field ~= nil then
            fail(name, "present text claims do not carry value")
        end
        return table.freeze({
            kind = kind,
            reader = require_string(name .. ".reader", spec.reader),
            operator = operator,
            value = value_field,
        })
    elseif kind == "action_permission" then
        reject_unknown(name, spec, { kind = true, binding_id = true, action_id = true, allowed = true })
        if type(spec.allowed) ~= "boolean" then
            fail(name .. ".allowed", "needs a boolean")
        end
        return table.freeze({
            kind = kind,
            binding_id = require_string(name .. ".binding_id", spec.binding_id),
            action_id = require_string(name .. ".action_id", spec.action_id),
            allowed = spec.allowed,
        })
    elseif kind == "surface_stack" then
        reject_unknown(name, spec, { kind = true, context = true, surfaces = true })
        return table.freeze({
            kind = kind,
            context = require_string(name .. ".context", spec.context),
            surfaces = copy_strings(name .. ".surfaces", spec.surfaces, true),
        })
    end
    fail(name, "has unknown claim kind '" .. kind .. "'")
end

local Assertion = {}
oracle.Assertion = Assertion

function Assertion.new(spec: { [string]: any }): Assertion
    local value = require_table("Assertion.new", spec)
    reject_unknown("Assertion.new", value, {
        id = true,
        frame_id = true,
        claim = true,
        outcome = true,
        supporting_observations = true,
        review = true,
        provenance = true,
    })
    local outcome = require_string("Assertion.new.outcome", value.outcome)
    if outcome ~= oracle.present and outcome ~= oracle.absent and outcome ~= oracle.unknown then
        fail("Assertion.new.outcome", "must be present, absent, or unknown")
    end
    local review = require_table("Assertion.new.review", value.review)
    reject_unknown("Assertion.new.review", review, { status = true, actor = true, reviewed_at = true, comment = true })
    local status = require_string("Assertion.new.review.status", review.status)
    if status ~= oracle.review_open
        and status ~= oracle.review_accepted
        and status ~= oracle.review_rejected
        and status ~= oracle.review_superseded
    then
        fail("Assertion.new.review.status", "has an unknown review status")
    end
    local support = copy_strings("Assertion.new.supporting_observations", value.supporting_observations, true)
    local review_copy: { [string]: any } = { status = status }
    if review.actor ~= nil then
        review_copy.actor = require_string("Assertion.new.review.actor", review.actor)
    end
    if review.reviewed_at ~= nil then
        review_copy.reviewed_at = require_string("Assertion.new.review.reviewed_at", review.reviewed_at)
    end
    if review.comment ~= nil then
        if type(review.comment) ~= "string" then
            fail("Assertion.new.review.comment", "needs a string")
        end
        review_copy.comment = review.comment
    end
    local assertion = table.freeze({
        id = require_string("Assertion.new.id", value.id),
        frame_id = require_string("Assertion.new.frame_id", value.frame_id),
        claim = claim("Assertion.new.claim", value.claim),
        outcome = outcome,
        supporting_observations = support,
        review = table.freeze(review_copy),
        provenance = provenance("Assertion.new.provenance", value.provenance),
    })
    assertionRecords[assertion] = true
    return assertion
end

function Assertion.is(value: any): boolean
    return assertionRecords[value] == true
end

local function candidate_record(name: string, value: any): { [string]: any }
    local spec = require_table(name, value)
    local candidate_id = require_string(name .. ".candidate_id", spec.candidate_id)
    local state = require_string(name .. ".state", spec.state)
    if state ~= "proposed" and state ~= "accepted" and state ~= "rejected" and state ~= "superseded" then
        fail(name .. ".state", "has an unknown candidate state")
    end
    local result: { [string]: any } = {
        candidate_id = candidate_id,
        state = state,
        value = spec.value,
        confidence = require_confidence(name .. ".confidence", spec.confidence),
        evidence_ids = copy_strings(name .. ".evidence_ids", spec.evidence_ids, true),
        provenance = provenance(name .. ".provenance", spec.provenance),
    }
    if spec.conflict_ids ~= nil then
        result.conflict_ids = copy_strings(name .. ".conflict_ids", spec.conflict_ids, false)
    end
    return table.freeze(result)
end

local function generic_candidate(name: string, value: any): { [string]: any }
    local spec = require_table(name, value)
    reject_unknown(name, spec, {
        candidate_id = true,
        entity_kind = true,
        state = true,
        value = true,
        confidence = true,
        evidence_ids = true,
        conflict_ids = true,
        provenance = true,
    })
    local kind = require_string(name .. ".entity_kind", spec.entity_kind)
    if kind ~= "context"
        and kind ~= "locator_variant"
        and kind ~= "reader"
        and kind ~= "surface"
        and kind ~= "binding"
        and kind ~= "transition"
    then
        fail(name .. ".entity_kind", "has an unknown candidate entity kind")
    end
    local state = require_string(name .. ".state", spec.state)
    if state ~= "proposed" and state ~= "accepted" and state ~= "rejected" and state ~= "superseded" then
        fail(name .. ".state", "has an unknown candidate state")
    end
    local result: { [string]: any } = {
        candidate_id = require_string(name .. ".candidate_id", spec.candidate_id),
        entity_kind = kind,
        state = state,
        value = spec.value,
        confidence = require_confidence(name .. ".confidence", spec.confidence),
        evidence_ids = copy_strings(name .. ".evidence_ids", spec.evidence_ids, true),
        provenance = provenance(name .. ".provenance", spec.provenance),
    }
    if spec.conflict_ids ~= nil then
        result.conflict_ids = copy_strings(name .. ".conflict_ids", spec.conflict_ids, false)
    end
    return table.freeze(result)
end

local function patch_record(name: string, value: any): { [string]: any }
    local spec = require_table(name, value)
    reject_unknown(name, spec, {
        id = true,
        change = true,
        summary = true,
        confidence = true,
        risk = true,
        evidence_ids = true,
        conflict_ids = true,
        provenance = true,
        status = true,
    })
    local change = require_string(name .. ".change", spec.change)
    if change ~= "create_entity"
        and change ~= "set_field"
        and change ~= "grant_action"
        and change ~= "revoke_action"
        and change ~= "merge_entities"
        and change ~= "split_entity"
    then
        fail(name .. ".change", "has an unknown patch change")
    end
    local risk = require_string(name .. ".risk", spec.risk)
    if risk ~= "low" and risk ~= "medium" and risk ~= "high" and risk ~= "critical" then
        fail(name .. ".risk", "has an unknown patch risk")
    end
    local status = require_string(name .. ".status", spec.status)
    if status ~= "proposed"
        and status ~= "accepted"
        and status ~= "rejected"
        and status ~= "applied"
        and status ~= "superseded"
    then
        fail(name .. ".status", "has an unknown patch status")
    end
    local result: { [string]: any } = {
        id = require_string(name .. ".id", spec.id),
        change = change,
        summary = require_string(name .. ".summary", spec.summary),
        confidence = require_confidence(name .. ".confidence", spec.confidence),
        risk = risk,
        evidence_ids = copy_strings(name .. ".evidence_ids", spec.evidence_ids, true),
        provenance = provenance(name .. ".provenance", spec.provenance),
        status = status,
    }
    if spec.conflict_ids ~= nil then
        result.conflict_ids = copy_strings(name .. ".conflict_ids", spec.conflict_ids, false)
    end
    return table.freeze(result)
end

local function conflict_record(name: string, value: any): { [string]: any }
    local spec = require_table(name, value)
    reject_unknown(name, spec, {
        id = true,
        kind = true,
        severity = true,
        subject_ids = true,
        message = true,
        evidence_ids = true,
        suggested_patch_ids = true,
        status = true,
    })
    local kind = require_string(name .. ".kind", spec.kind)
    local allowed_kinds: { [string]: boolean } = {
        multiple_surface_match = true,
        missing_identity = true,
        unknown_evidence = true,
        shared_target_ambiguity = true,
        invalid_action = true,
        uncovered_frame = true,
        invalid_transition = true,
    }
    if not allowed_kinds[kind] then
        fail(name .. ".kind", "has an unknown conflict kind")
    end
    local severity = require_string(name .. ".severity", spec.severity)
    if severity ~= "info" and severity ~= "warning" and severity ~= "error" and severity ~= "critical" then
        fail(name .. ".severity", "has an unknown conflict severity")
    end
    local status = require_string(name .. ".status", spec.status)
    if status ~= "open" and status ~= "resolved" and status ~= "ignored" then
        fail(name .. ".status", "has an unknown conflict status")
    end
    local result: { [string]: any } = {
        id = require_string(name .. ".id", spec.id),
        kind = kind,
        severity = severity,
        subject_ids = copy_strings(name .. ".subject_ids", spec.subject_ids, true),
        message = require_string(name .. ".message", spec.message),
        status = status,
    }
    if spec.evidence_ids ~= nil then
        result.evidence_ids = copy_strings(name .. ".evidence_ids", spec.evidence_ids, false)
    end
    if spec.suggested_patch_ids ~= nil then
        result.suggested_patch_ids = copy_strings(name .. ".suggested_patch_ids", spec.suggested_patch_ids, false)
    end
    return table.freeze(result)
end

local CandidateModel = {}
oracle.CandidateModel = CandidateModel

function CandidateModel.new(spec: { [string]: any }): CandidateModel
    local value = require_table("CandidateModel.new", spec)
    reject_unknown("CandidateModel.new", value, {
        id = true,
        project_id = true,
        revision = true,
        status = true,
        source_frame_ids = true,
        targets = true,
        entities = true,
        patches = true,
        conflicts = true,
    })
    local status = require_string("CandidateModel.new.status", value.status)
    if status ~= "candidate" and status ~= "accepted" and status ~= "rejected" and status ~= "compiled" then
        fail("CandidateModel.new.status", "has an unknown candidate model status")
    end
    local function records(name: string, input: any, builder: (string, any) -> { [string]: any }): { any }
        if type(input) ~= "table" then
            fail(name, "needs an array")
        end
        local output: { any } = {}
        for index = 1, #input do
            output[index] = builder(name .. "[" .. tostring(index) .. "]", input[index])
        end
        return table.freeze(output)
    end
    local candidate = table.freeze({
        id = require_string("CandidateModel.new.id", value.id),
        project_id = require_string("CandidateModel.new.project_id", value.project_id),
        revision = require_integer("CandidateModel.new.revision", value.revision, 1),
        status = status,
        source_frame_ids = copy_strings("CandidateModel.new.source_frame_ids", value.source_frame_ids, true),
        targets = records("CandidateModel.new.targets", value.targets, candidate_record),
        entities = records("CandidateModel.new.entities", value.entities, generic_candidate),
        patches = records("CandidateModel.new.patches", value.patches, patch_record),
        conflicts = records("CandidateModel.new.conflicts", value.conflicts, conflict_record),
    }) :: any
    candidateRecords[candidate] = true
    return candidate
end

function CandidateModel.is(value: any): boolean
    return candidateRecords[value] == true
end

local function unique_records(name: string, values: { any }, predicate: (any) -> string)
    local seen: { [string]: boolean } = {}
    for index, value in values do
        local id = predicate(value)
        if seen[id] then
            fail(name, "contains duplicate id '" .. id .. "'")
        end
        seen[id] = true
        if value == nil then
            fail(name .. "[" .. tostring(index) .. "]", "is missing")
        end
    end
end

local Workspace = {}
oracle.Workspace = Workspace

function Workspace.new(spec: { [string]: any }): Workspace
    local value = require_table("Workspace.new", spec)
    reject_unknown("Workspace.new", value, {
        schema_version = true,
        project_id = true,
        frames = true,
        observations = true,
        assertions = true,
        candidates = true,
    })
    if value.schema_version ~= 1 then
        fail("Workspace.new.schema_version", "must be 1")
    end
    local function list(name: string, input: any): { any }
        if type(input) ~= "table" then
            fail(name, "needs an array")
        end
        return input
    end
    local frames: { Frame } = {}
    for index, value_frame in list("Workspace.new.frames", value.frames) do
        frames[index] = if Frame.is(value_frame)
            then value_frame
            else Frame.new(value_frame)
    end
    local observations: { Observation } = {}
    for index, value_observation in list("Workspace.new.observations", value.observations) do
        observations[index] = if Observation.is(value_observation)
            then value_observation
            else Observation.new(value_observation)
    end
    local assertions: { Assertion } = {}
    for index, value_assertion in list("Workspace.new.assertions", value.assertions) do
        assertions[index] = if Assertion.is(value_assertion)
            then value_assertion
            else Assertion.new(value_assertion)
    end
    local candidates: { CandidateModel } = {}
    for index, value_candidate in list("Workspace.new.candidates", value.candidates) do
        candidates[index] = if CandidateModel.is(value_candidate)
            then value_candidate
            else CandidateModel.new(value_candidate)
    end
    unique_records("Workspace.new.frames", frames, function(record): string return record.id end)
    unique_records("Workspace.new.observations", observations, function(record): string return record.id end)
    unique_records("Workspace.new.assertions", assertions, function(record): string return record.id end)
    unique_records("Workspace.new.candidates", candidates, function(record): string return record.id end)

    local frame_ids: { [string]: boolean } = {}
    for _, frame in frames do
        frame_ids[frame.id] = true
    end
    local observation_ids: { [string]: boolean } = {}
    for _, observation in observations do
        if not frame_ids[observation.frame_id] then
            fail("Workspace.new.observations", "references unknown frame '" .. observation.frame_id .. "'")
        end
        observation_ids[observation.id] = true
    end
    for _, assertion in assertions do
        if not frame_ids[assertion.frame_id] then
            fail("Workspace.new.assertions", "references unknown frame '" .. assertion.frame_id .. "'")
        end
        for _, observation_id in assertion.supporting_observations do
            if not observation_ids[observation_id] then
                fail("Workspace.new.assertions", "references unknown observation '" .. observation_id .. "'")
            end
        end
    end
    local workspace = table.freeze({
        schema_version = 1,
        project_id = require_string("Workspace.new.project_id", value.project_id),
        frames = table.freeze(frames),
        observations = table.freeze(observations),
        assertions = table.freeze(assertions),
        candidates = table.freeze(candidates),
    }) :: any
    workspaceRecords[workspace] = true
    return workspace
end

function Workspace.is(value: any): boolean
    return workspaceRecords[value] == true
end

function oracle.read_workspace(value: any): Workspace
    if Workspace.is(value) then
        return value
    end
    return Workspace.new(value)
end

function oracle.assertion_status(value: any): string
    if type(value) ~= "table" or type(value.review) ~= "table" then
        return oracle.unreviewed
    end
    if value.review.status ~= oracle.review_accepted then
        return oracle.unreviewed
    end
    return value.outcome
end

function oracle.assertion_is_active(value: any): boolean
    return type(value) == "table"
        and type(value.review) == "table"
        and value.review.status == oracle.review_accepted
end

function oracle.assertion_claims(value: any, kind: string): boolean
    return type(value) == "table"
        and type(value.claim) == "table"
        and value.claim.kind == kind
end

function oracle.frames_by_id(workspace: Workspace): { [string]: Frame }
    local result: { [string]: Frame } = {}
    for _, frame in workspace.frames do
        result[frame.id] = frame
    end
    return result
end

function oracle.observations_by_id(workspace: Workspace): { [string]: Observation }
    local result: { [string]: Observation } = {}
    for _, observation in workspace.observations do
        result[observation.id] = observation
    end
    return result
end

function oracle.assertions_for_frame(workspace: Workspace, frame_id: string): { Assertion }
    local result: { Assertion } = {}
    for _, assertion in workspace.assertions do
        if assertion.frame_id == frame_id then
            table.insert(result, assertion)
        end
    end
    return result
end

return oracle
