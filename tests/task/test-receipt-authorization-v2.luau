--!strict
-- Receipt safety tests. These run in the trusted framework environment where
-- `resolution` and the private `evidence` module are available.

local modelHash = string.rep("a", 64)

local function result(value: any): any
    return {
        kind = "present",
        value = value,
        confidence = 0.99,
        proof = { { kind = "locator", target = "test_target" } },
    }
end

local runtimeModel = {
    schema_version = 1,
    surfaces = {
        { id = "shop", kind = "scene", contexts = { "menu" } },
    },
    bindings = {
        {
            id = "shop_leave",
            surface = "shop",
            target = "leave_button",
            placement = { kind = "target_geometry" },
            identity = {
                all = { { kind = "locator_present", locator = { locator = "shop.title" } } },
                any = {},
                none = {},
            },
            readers = {},
            actions = {
                {
                    id = "leave",
                    kind = "click",
                    locator = { locator = "leave_button.locator" },
                    preconditions = {},
                },
            },
        },
    },
}

local function cycle(ticketId: string, cycleId: string, frameId: string): any
    local value = {
        ticket_id = ticketId,
        cycle_id = cycleId,
        frame_id = frameId,
        model_hash = modelHash,
        cycle_token = {},
    }
    function value:probe(subject: string, predicate: any): any
        local id = if predicate.kind == "locator_present"
            then predicate.locator.locator
            else predicate.reader
        if id == "shop.title" then
            return result(true)
        end
        if id == "leave_button.locator" then
            return result({
                match = "private-match-handle",
                click_point = { x = 200, y = 320 },
            })
        end
        return { kind = "unknown", reason = "not_measured" }
    end
    return value
end

local before = cycle("ticket-17", "cycle-1", "frame-before")
local resolved = resolution.resolve(runtimeModel, before, { context = "menu" })
assert(resolved.kind == "resolved")

local receipt, reason = resolution.authorize(
    runtimeModel,
    before,
    resolved,
    { target = "leave_button", kind = "click" }
)
assert(receipt ~= nil, reason)
assert(receipt.kind == "receipt")
assert(receipt.ticket_id == "ticket-17")
assert(receipt.cycle_id == "cycle-1")
assert(receipt.frame_id == "frame-before")
assert(receipt.model_hash == modelHash)
assert(receipt.state.context == "menu")
assert(receipt.state.surfaces[1] == "shop")
assert(receipt.surface == "shop")
assert(receipt.binding == "shop_leave")
assert(receipt.action == "leave")
assert(#receipt.evidence_ids >= 2)
assert(evidence.is_receipt_for(receipt, before.cycle_token))

local after = cycle("ticket-17", "cycle-2", "frame-after")
assert(not evidence.is_receipt_for(receipt, after.cycle_token))
assert(not evidence.is_receipt_for(receipt, {}))

local forged = {
    kind = "receipt",
    id = receipt.id,
    ticket_id = receipt.ticket_id,
    cycle_id = receipt.cycle_id,
    frame_id = receipt.frame_id,
    model_hash = receipt.model_hash,
    state = receipt.state,
    surface = receipt.surface,
    binding = receipt.binding,
    action = receipt.action,
    evidence_ids = receipt.evidence_ids,
    authorization = receipt.authorization,
}
assert(not evidence.is_receipt_for(forged, before.cycle_token))

local ambiguousModel = {
    schema_version = 1,
    surfaces = {
        { id = "shop", kind = "scene", contexts = { "menu" } },
        { id = "equip", kind = "scene", contexts = { "menu" } },
    },
    bindings = {
        {
            id = "shop_leave",
            surface = "shop",
            target = "leave_button",
            placement = { kind = "target_geometry" },
            identity = {
                all = { { kind = "locator_present", locator = { locator = "shared.header" } } },
                any = {},
                none = {},
            },
            readers = {},
            actions = {},
        },
        {
            id = "equip_leave",
            surface = "equip",
            target = "leave_button",
            placement = { kind = "target_geometry" },
            identity = {
                all = { { kind = "locator_present", locator = { locator = "shared.header" } } },
                any = {},
                none = {},
            },
            readers = {},
            actions = {},
        },
    },
}
local ambiguousCycle = cycle("ticket-18", "cycle-3", "frame-ambiguous")
function ambiguousCycle:probe(subject: string, predicate: any): any
    if predicate.locator.locator == "shared.header" then
        return result(true)
    end
    return { kind = "unknown", reason = "not_measured" }
end
local ambiguous = resolution.resolve(ambiguousModel, ambiguousCycle, { context = "menu" })
assert(ambiguous.kind == "ambiguous")
local noReceipt, noReceiptReason = resolution.authorize(
    ambiguousModel,
    ambiguousCycle,
    ambiguous,
    { target = "leave_button", kind = "click" }
)
assert(noReceipt == nil)
assert(noReceiptReason == "resolution_not_actionable")
