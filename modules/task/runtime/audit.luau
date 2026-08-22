--!strict
-- Project-semantic audit records, automatically attributed to the run and the
-- position the seam assigned.
--
-- An audit record is a real Tool call. `framework.audit.record` has a
-- descriptor in the Framework Tool Catalog and is invoked read-only through the
-- same executor as every other scoped operation, so the record IS that call's
-- own durable outcome. There is deliberately no second path: an Operator-side
-- append that was nevertheless scoped would be a second native primitive with
-- its own delivery and durability semantics, and it would re-append on every
-- restart, because the durable Tool boundary is the only place this
-- architecture dedupes an effect.
--
-- Read-only is the right classification even though the call persists a row.
-- Read-only here means no external-world effect requiring plan authority and
-- approval grants -- `framework.screen.observe` is read-only and persists a
-- durable snapshot reference. Audit therefore needs no plan authority and
-- proposes no effect, but it DOES spend Tool-call budget like everything else.
-- If records ever prove too expensive the lever is the descriptor's budget
-- weight in the catalog, never an exemption from the path.
--
-- Attribution is not a member of anything here. The Tool's argument contract is
-- exactly `{"record": {...}}`, and the run root and call position come from the
-- seam; a caller that could state them could attribute its record to another
-- call's position. This module's whole job is to own that one-member envelope
-- so no script spells it, and to refuse a record the Tool would refuse after
-- the budget was already spent.
--
-- Every call here goes through `@umbraflow/tools`, which is the SDK's one
-- spelling of a Tool call; the private capability table this module is also
-- handed as its chunk argument is deliberately unused, for the reason
-- `@umbraflow/screen` states.

local tools = require("@umbraflow/tools")

local audit = {}

local k_recordTool = "framework.audit.record"
local k_recordMember = "record"

local function reject(message: string): never
    error("audit " .. message, 3)
end

-- A record is a JSON OBJECT. A Luau table with no string key crosses the value
-- boundary as an ARRAY, and an empty one is an empty array, so both would be
-- refused by the Tool after a Tool call had already been spent -- and the
-- diagnostic would be about a shape the script never intended to write.
local function checkedRecord(record: any): any
    if type(record) ~= "table" then
        reject("record must be an object")
    end
    local members = 0
    for key in next, record :: any do
        if type(key) ~= "string" then
            reject("record must be an object, so every key must be a string")
        end
        members += 1
    end
    if members == 0 then
        reject("record must carry at least one member")
    end
    return record
end

-- Record one project-semantic fact. The answer is an ordinary Tool answer, so
-- `@umbraflow/workflow` classifies it exactly as it classifies every other
-- call.
function audit.record(record: any): any
    return tools.call(k_recordTool, { [k_recordMember] = checkedRecord(record) })
end

-- Whether a record durably landed. It asserts provenance as well as the
-- classification, so a script cannot mistake another Tool's confirmed answer
-- for its own audit record.
function audit.recorded(answer: any): boolean
    if tools.tool_name(answer) ~= k_recordTool then
        reject("recorded needs the answer of " .. k_recordTool)
    end
    return tools.state(answer) == "confirmed"
end

return table.freeze(audit)
