--!strict
-- Private same-cycle evidence ledger.  Record-shaped tables have no authority.

local evidence = {}

local nextCycleId = 0
local nextRecordId = 0
local cycles: { [any]: any } = setmetatable({}, { __mode = "k" }) :: any
local records: { [any]: any } = setmetatable({}, { __mode = "k" }) :: any

local unknownReasons = {
    not_measured = true,
    low_confidence = true,
    ocr_unreadable = true,
    locator_failed = true,
    stale_cycle = true,
    host_unavailable = true,
    internal_error = true,
}

local function copyFreeze(value: any): any
    if type(value) ~= "table" then return value end
    local result = {}
    for key, child in value do result[key] = copyFreeze(child) end
    return table.freeze(result)
end

local function requireCycle(cycle: any): any
    local entry = cycles[cycle]
    if entry == nil then error("evidence needs a live framework cycle", 3) end
    return entry
end

local function confidence(value: any): number
    if type(value) ~= "number" or value ~= value or value < 0 or value > 1 then
        error("evidence confidence must be between 0 and 1", 3)
    end
    return value
end

local function source(value: any): any
    if type(value) ~= "table" then error("evidence source must be a table", 3) end
    return copyFreeze(value)
end

local function issue(cycle: any, result: any, hostProof: any): any
    requireCycle(cycle)
    nextRecordId += 1
    local record = table.freeze({ id = "evidence-" .. tostring(nextRecordId) })
    records[record] = {
        cycle = cycle,
        result = table.freeze(result),
        host_proof = hostProof,
    }
    return record
end

function evidence.begin_cycle(): any
    nextCycleId += 1
    local cycle = table.freeze({ id = "evidence-cycle-" .. tostring(nextCycleId) })
    cycles[cycle] = true
    return cycle
end

function evidence.end_cycle(cycle: any)
    cycles[cycle] = nil
end

function evidence.is_cycle_live(cycle: any): boolean
    return cycles[cycle] ~= nil
end

function evidence.present(cycle: any, proofSource: any, value: any, score: any, hostProof: any?): any
    return issue(cycle, {
        kind = "present",
        confidence = confidence(score),
        source = source(proofSource),
        value = copyFreeze(value),
    }, hostProof)
end

function evidence.absent(cycle: any, proofSource: any): any
    return issue(cycle, { kind = "absent", source = source(proofSource) }, nil)
end

function evidence.unknown(cycle: any, reason: any, proofSource: any, score: any?): any
    if type(reason) ~= "string" or not unknownReasons[reason] then
        error("unsupported Unknown evidence reason", 2)
    end
    return issue(cycle, {
        kind = "unknown",
        reason = reason,
        confidence = if score == nil then nil else confidence(score),
        source = source(proofSource),
    }, nil)
end

function evidence.result_of(record: any, cycle: any): any?
    local entry = records[record]
    if entry == nil or entry.cycle ~= cycle or cycles[cycle] == nil then return nil end
    return entry.result
end

function evidence.host_proof_of(record: any, cycle: any): any?
    local entry = records[record]
    if entry == nil or entry.cycle ~= cycle or cycles[cycle] == nil then return nil end
    return entry.host_proof
end

function evidence.summary(record: any, cycle: any): any?
    local entry = records[record]
    if entry == nil or entry.cycle ~= cycle or cycles[cycle] == nil then return nil end
    return table.freeze({
        id = record.id,
        kind = entry.result.kind,
        reason = entry.result.reason,
        confidence = entry.result.confidence,
        source = entry.result.source,
    })
end

return table.freeze(evidence)
