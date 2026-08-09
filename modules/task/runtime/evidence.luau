--!strict
-- evidence: the trusted identity ledger for annotation-v2 observations.
--
-- A result table is descriptive data.  It is not, by itself, proof that the
-- current frame was measured, and a receipt-shaped table is not permission to
-- perform an input action.  This module is deliberately not published to the
-- project environment.  The framework uses the identity of the frozen values
-- below to keep that distinction true even when a project copies every field it
-- can see.

local evidence = {}

type Proof = { [string]: any }
type Result = { [string]: any }
type CycleToken = any

local nextEvidenceId = 0
local nextReceiptId = 0

local function nonEmptyString(what: string, value: any): string
    if type(value) ~= "string" or value == "" then
        error(what .. " must be a non-empty string", 3)
    end
    return value
end

local function sha256(what: string, value: any): string
    local result = nonEmptyString(what, value)
    if #result ~= 64 or string.match(result, "^[0-9a-f]+$") == nil then
        error(what .. " must be a lowercase SHA-256", 3)
    end
    return result
end

local evidenceLedger: { [any]: { cycle: CycleToken, result: Result } } =
    setmetatable({}, { __mode = "k" }) :: any
local receiptLedger: {
    [any]: {
        cycle: CycleToken,
        authorization: { [string]: any },
        consumed: boolean,
    },
} = setmetatable({}, { __mode = "k" }) :: any

local function deepFreeze(value: any, seen: { [any]: boolean }?): any
    if type(value) ~= "table" then
        return value
    end

    local visited = seen
    if visited == nil then
        visited = setmetatable({}, { __mode = "k" }) :: any
    end
    if visited[value] then
        return value
    end
    visited[value] = true
    for _, child in value do
        deepFreeze(child, visited)
    end
    return table.freeze(value)
end

local function cloneProof(proof: any): { Proof }
    local result: { Proof } = {}
    if type(proof) ~= "table" then
        return result
    end
    for index = 1, #proof do
        if type(proof[index]) == "table" then
            table.insert(result, table.clone(proof[index]))
        end
    end
    return result
end

local function requireConfidence(value: any): number
    if type(value) ~= "number" or value < 0 or value > 1 then
        error("evidence confidence must be a number between 0 and 1", 3)
    end
    return value
end

local function requireProof(proof: any, required: boolean): { Proof }
    local result = cloneProof(proof)
    if required and #result == 0 then
        error("present and absent evidence require at least one proof", 3)
    end
    return result
end

function evidence.present(
    value: any,
    confidence: number,
    proof: { Proof }
): Result
    return deepFreeze({
        kind = "present",
        value = value,
        confidence = requireConfidence(confidence),
        proof = requireProof(proof, true),
    })
end

function evidence.absent(proof: { Proof }): Result
    return deepFreeze({
        kind = "absent",
        proof = requireProof(proof, true),
    })
end

function evidence.unknown(
    reason: string,
    confidence: number?,
    proof: { Proof }?
): Result
    local result: { [string]: any } = {
        kind = "unknown",
        reason = reason,
        proof = requireProof(proof, false),
    }
    if confidence ~= nil then
        result.confidence = requireConfidence(confidence)
    end
    return deepFreeze(result)
end

function evidence.mint_evidence(
    subject: string,
    result: Result,
    cycle: CycleToken
): any
    if type(subject) ~= "string" or subject == "" then
        error("evidence subject must be a non-empty identifier", 2)
    end
    if type(result) ~= "table" or type(result.kind) ~= "string" then
        error("evidence result must be Present, Absent, or Unknown", 2)
    end
    if cycle == nil then
        error("evidence must belong to an observation cycle", 2)
    end

    nextEvidenceId += 1
    local record = deepFreeze({
        id = "evidence-" .. tostring(nextEvidenceId),
        subject = subject,
        result = result,
    })
    evidenceLedger[record] = {
        cycle = cycle,
        result = result,
    }
    return record
end

function evidence.result_of(record: any, cycle: CycleToken): Result?
    local entry = evidenceLedger[record]
    if entry == nil or not rawequal(entry.cycle, cycle) then
        return nil
    end
    return entry.result
end

function evidence.is_evidence_for(record: any, cycle: CycleToken): boolean
    return evidenceLedger[record] ~= nil
        and rawequal(evidenceLedger[record].cycle, cycle)
end

-- The public shape is exactly the Receipt schema.  The action proof, match
-- handle, click point, and cycle token stay in this private ledger instead of
-- being smuggled through fields a project can construct.
function evidence.mint_receipt(
    fields: { [string]: any },
    cycle: CycleToken,
    authorization: { [string]: any }
): any
    if cycle == nil then
        error("a receipt must belong to an observation cycle", 2)
    end
    if type(fields) ~= "table" or type(authorization) ~= "table" then
        error("evidence.mint_receipt needs receipt fields and authorization", 2)
    end

    local ticketId = nonEmptyString("receipt ticket_id", fields.ticket_id)
    local cycleId = nonEmptyString("receipt cycle_id", fields.cycle_id)
    local frameId = nonEmptyString("receipt frame_id", fields.frame_id)
    local modelHash = sha256("receipt model_hash", fields.model_hash)
    nonEmptyString("receipt surface", fields.surface)
    nonEmptyString("receipt binding", fields.binding)
    nonEmptyString("receipt action", fields.action)
    if type(fields.state) ~= "table"
        or type(fields.state.context) ~= "string"
        or fields.state.context == ""
        or type(fields.state.surfaces) ~= "table"
        or #fields.state.surfaces == 0
    then
        error("receipt state must be a non-empty RuntimeState", 2)
    end
    if type(fields.evidence_ids) ~= "table" or #fields.evidence_ids == 0 then
        error("receipt evidence_ids must be a non-empty array", 2)
    end
    local authorizationTarget = nonEmptyString(
        "receipt authorization target",
        authorization.target
    )
    local authorizationAction = nonEmptyString(
        "receipt authorization action",
        authorization.action
    )
    if authorization.ticket_id ~= ticketId
        or authorization.cycle_id ~= cycleId
        or authorizationAction ~= fields.action
    then
        error("receipt authorization does not match receipt identity", 2)
    end

    nextReceiptId += 1
    local receipt = deepFreeze({
        kind = "receipt",
        id = "receipt-" .. tostring(nextReceiptId),
        ticket_id = ticketId,
        cycle_id = cycleId,
        frame_id = frameId,
        model_hash = modelHash,
        state = fields.state,
        surface = fields.surface,
        binding = fields.binding,
        action = fields.action,
        evidence_ids = table.clone(fields.evidence_ids),
        authorization = {
            ticket_id = nonEmptyString(
                "receipt authorization ticket_id",
                authorization.ticket_id
            ),
            cycle_id = nonEmptyString(
                "receipt authorization cycle_id",
                authorization.cycle_id
            ),
            target = authorizationTarget,
            action = authorizationAction,
        },
    })

    receiptLedger[receipt] = {
        cycle = cycle,
        authorization = authorization,
        consumed = false,
    }
    return receipt
end

function evidence.receipt_authorization(
    receipt: any,
    cycle: CycleToken
): { [string]: any }?
    local entry = receiptLedger[receipt]
    if entry == nil or not rawequal(entry.cycle, cycle) then
        return nil
    end
    if entry.consumed then
        return nil
    end
    return entry.authorization
end

function evidence.is_receipt_for(receipt: any, cycle: CycleToken): boolean
    return evidence.receipt_authorization(receipt, cycle) ~= nil
end

function evidence.consume_receipt(receipt: any, cycle: CycleToken): boolean
    local entry = receiptLedger[receipt]
    if entry == nil or not rawequal(entry.cycle, cycle) or entry.consumed then
        return false
    end
    entry.consumed = true
    return true
end

-- These two names are intentionally framework-only.  They let the still-being
-- replaced collection reader reuse the same frame ledger without making a hit
-- itself an authorization object.
function evidence.mint_hit(hit: any, cycle: CycleToken): any
    if cycle == nil then
        error("a hit must belong to an observation cycle", 2)
    end
    local frozen = deepFreeze(hit)
    evidenceLedger[frozen] = {
        cycle = cycle,
        result = {
            kind = "present",
            value = frozen,
            confidence = if type(hit.confidence) == "number" then hit.confidence else 1,
            proof = {},
        },
    }
    return frozen
end

function evidence.hit_ticket(hit: any): any
    local entry = evidenceLedger[hit]
    return if entry == nil then nil else entry.cycle
end

return evidence
