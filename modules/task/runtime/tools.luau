--!strict
-- Tool discovery, description, and direct calling for scoped Project code.
--
-- This module owns the ONE spelling of "make a Tool call" in the Framework SDK.
-- `@umbraflow/screen`, `@umbraflow/workflow` and `@umbraflow/audit` are handed
-- the same private capability table and deliberately ignore it; they reach the
-- Tool Runtime through `tools.call`, so argument shaping, catalog checking and
-- answer validation exist once rather than four times.
--
-- The chunk argument is the private capability table an interactive scoped Tool
-- session builds for one run and then drops. Project Tool handlers are leaves
-- and therefore never receive this capability. It carries
-- exactly one primitive,
-- `invoke(toolName, argumentsValue)`, which runs the Tool to completion and
-- answers one frozen decoded JSON value. Two facts about it shape everything
-- below. It is synchronous, so nothing here yields, polls in the background, or
-- takes a completion callback. And a REFUSAL from it is terminal: the run is
-- recorded as failed and the VM is destroyed without resuming this script, so
-- no `pcall` in this module or above it can observe one. A Tool that ran and
-- failed is not a refusal -- it returns `ok = false`, its terminal delivery
-- value, and the provider's error directly.
--
-- The call's POSITION is assigned by the seam and is never a parameter here.
-- The issuing context is the interactive request itself, and its call is a
-- parentless position under that request. A script that could name its ordinal could name
-- another call's position and inherit that call's recorded outcome without
-- authorisation, so `call` takes a tool name and one value and nothing else.
--
-- Discovery and description are frozen DATA, not a call: the run's pinned Tool
-- catalog arrives as the read-only JSON resource named below, is validated once
-- while this module loads, and is answered from there forever after. Reading it
-- spends no Tool-call budget and creates no ToolInvocation. It is ADVISORY: the
-- Operator validates the tool name, the arguments and the authority again on
-- every call, so a catalog that disagreed with the Operator can only make this
-- module refuse early, never make the Operator accept.
--
-- One answer has exactly one of these shapes:
--
--     { ok = true, call_identity = "<hash>", delivery = "confirmed", result = <value> }
--     { ok = false, call_identity = "<hash>", delivery = "<terminal>", error = <error> }

local native = ...

local tools = {}

-- The read-only resource the host bakes the run's pinned Tool catalog into. It
-- is a resource rather than a native call because discovery must cost nothing
-- and must be identical on replay: the bytes are pinned at program
-- construction, so a description cannot change under a script mid-run.
local k_catalogResource = "umbraflow.tool-catalog"

local k_identityHexLength = 64

-- Only terminal delivery values cross a synchronous call boundary. Proposed,
-- admitted and dispatching remain ledger states and are not answers.
local k_deliveries = table.freeze({
    confirmed = true,
    proven_absent = true,
    possible = true,
    terminal_failure = true,
    terminally_unresolved = true,
})

local function reject(message: string): never
    error("tools " .. message, 3)
end

-- A load-time refusal, reported without a caller frame because there is no
-- script frame to blame: it fires when this module is loaded by a program type
-- that publishes no Tool Runtime seam at all.
local function refuseLoad(message: string): never
    error("tools " .. message, 0)
end

if type(native) ~= "table" then
    refuseLoad("requires the scoped Tool Runtime capability table")
end
local invoke = rawget(native :: any, "invoke")
if type(invoke) ~= "function" then
    refuseLoad("requires the scoped Tool Runtime invoke primitive")
end

local function contentHash(value: any, where: string): string
    if type(value) ~= "string" or #value ~= k_identityHexLength then
        reject(where .. " must be a 64 character content hash")
    end
    if string.find(value :: string, "^[0-9a-f]+$") == nil then
        reject(where .. " must be lowercase hexadecimal")
    end
    return value :: string
end

local function checkedAnswer(answer: any): any
    if type(answer) ~= "table" then
        reject("answer must be the object the Tool Runtime returned")
    end
    local ok = rawget(answer, "ok")
    if type(ok) ~= "boolean" then
        reject("answer must carry an ok boolean")
    end
    local delivery = rawget(answer, "delivery")
    if type(delivery) ~= "string" or k_deliveries[delivery] ~= true then
        reject("answer must carry a terminal delivery value")
    end
    local _ = contentHash(rawget(answer, "call_identity"), "answer call_identity")
    if ok then
        if delivery ~= "confirmed" or rawget(answer, "result") == nil then
            reject("an ok answer must be confirmed and carry result")
        end
        if rawget(answer, "error") ~= nil then
            reject("an ok answer must not carry error")
        end
    else
        if delivery == "confirmed" or rawget(answer, "result") ~= nil then
            reject("a failed answer must be unconfirmed and carry no result")
        end
        local recorded = rawget(answer, "error")
        if type(recorded) ~= "table"
            or type(rawget(recorded, "code")) ~= "string"
            or type(rawget(recorded, "message")) ~= "string"
            or type(rawget(recorded, "retryable")) ~= "boolean"
        then
            reject("a failed answer must carry a code, message, retryable error")
        end
    end
    return answer
end

-- The catalog, validated once. Every refusal here fails the whole scoped
-- generation rather than one call, which is correct: a catalog a script cannot
-- read is a program that cannot be reviewed.
local function loadedCatalog(): ({ [string]: any }, { string }, string)
    local document = resource.readJson(k_catalogResource)
    if type(document) ~= "table" then
        refuseLoad("catalog resource must be a JSON object")
    end
    local digest = rawget(document, "catalog_hash")
    if
        type(digest) ~= "string"
        or #digest ~= k_identityHexLength
        or string.find(digest :: string, "^[0-9a-f]+$") == nil
    then
        refuseLoad("catalog resource must carry a lowercase hex catalog_hash")
    end
    local entries = rawget(document, "tools")
    if type(entries) ~= "table" then
        refuseLoad("catalog resource must carry a tools list")
    end

    local byName: { [string]: any } = {}
    local names: { string } = {}
    local count = rawlen(entries :: any)
    for index = 1, count do
        local entry = rawget(entries :: any, index)
        if type(entry) ~= "table" then
            refuseLoad("catalog tools must all be descriptor objects")
        end
        local name = rawget(entry, "name")
        if type(name) ~= "string" or name == "" then
            refuseLoad("catalog descriptor must name its tool")
        end
        -- Strictly increasing, which refuses an unsorted catalog and a
        -- duplicated tool in one comparison. The catalog owner renders the
        -- array in UTF-8 order of the names, so a catalog that arrives out of
        -- order is a catalog that was rewritten by something else.
        if index > 1 and not (names[index - 1] < (name :: string)) then
            refuseLoad("catalog tools must be sorted by name and unique")
        end
        names[index] = name :: string
        byName[name :: string] = entry
    end
    if count == 0 then
        refuseLoad("catalog resource declares no tool at all")
    end
    return byName, table.freeze(names), digest :: string
end

local descriptorByName, catalogNames, catalogHash = loadedCatalog()

-- The identity of the exact catalog bytes this run was pinned to. A script that
-- records it has recorded which descriptions it acted on.
tools.catalog_hash = catalogHash

function tools.names(): any
    return catalogNames
end

-- The closure this run may call, rendered for a refusal. A refusal that named
-- only the Tool leaves the caller guessing which set it was measured against,
-- and "the pinned catalog" is a phrase rather than a set.
local function pinnedClosure(): string
    if #catalogNames == 0 then
        return "no Tool at all"
    end
    return table.concat(catalogNames, ", ")
end

function tools.knows(name: any): boolean
    if type(name) ~= "string" then
        reject("knows needs a tool name string")
    end
    return descriptorByName[name] ~= nil
end

function tools.describe(name: any): any
    if type(name) ~= "string" then
        reject("describe needs a tool name string")
    end
    local descriptor = descriptorByName[name]
    if descriptor == nil then
        reject(
            "describe names a tool outside the closure this session pinned: "
                .. name
                .. "; the closure holds "
                .. pinnedClosure()
        )
    end
    return descriptor
end

-- Issue one Tool call at this run's next position. `arguments` is the value the
-- Tool's own input schema states; it is a table because every Tool schema in
-- the catalog takes an object, and an empty object is spelled
-- `canon.emptyObject` rather than `{}`, which is an empty ARRAY at this
-- boundary.
--
-- The catalog check refuses a name no descriptor covers before a Tool-call
-- budget is spent on it. It is not an authority check: the Operator judges the
-- name, the arguments, the policy and the grants again, and only its verdict
-- decides anything.
function tools.call(name: any, arguments: any): any
    if type(name) ~= "string" then
        reject("call needs a tool name string")
    end
    local descriptor = descriptorByName[name]
    if descriptor == nil then
        reject(
            "call names a tool outside the closure this session pinned: "
                .. name
                .. "; the closure holds "
                .. pinnedClosure()
        )
    end
    if type(arguments) ~= "table" then
        reject("call needs an object value for arguments")
    end
    return checkedAnswer(invoke(name, arguments))
end

return table.freeze(tools)
