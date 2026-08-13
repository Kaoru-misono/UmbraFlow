--!strict
-- Luau may construct one same-cycle request.  The opaque Host Receipt stays
-- entirely outside this module contract.

local function detector(locator: string): any
    return { all = { { kind = "locator_present", locator = locator } }, any = {}, none = {} }
end

local runtime = model.compile({
    schema_version = 2,
    base_resolution = { 800, 600 },
    base_dpi = { 96, 96 },
    ui_targets = {
        { id = "title", kind = "region" },
        { id = "confirm", kind = "control" },
        { id = "commit", kind = "control" },
    },
    locators = {
        { id = "title.locator", kind = "template", asset_path = "assets/title.png", threshold = 0.9 },
        { id = "confirm.locator", kind = "template", asset_path = "assets/confirm.png", threshold = 0.9 },
    },
    readers = {},
    bindings = {
        {
            id = "screen.identity",
            surface = "screen",
            ui_target = "title",
            variant = "default",
            placement = { kind = "fixed", rect = { 10, 10, 200, 50 } },
            detector = detector("title.locator"),
            actions = {},
        },
        {
            id = "confirm.enabled",
            surface = "screen",
            ui_target = "confirm",
            variant = "enabled",
            placement = { kind = "fixed", rect = { 550, 480, 180, 70 }, action_point = { 640, 515 } },
            detector = detector("confirm.locator"),
            actions = { { id = "activate", kind = "click", proof_locator = "confirm.locator" } },
        },
        -- The same Surface and the same proof, granting a keystroke instead:
        -- what separates the two requests below is the kind and nothing else.
        {
            id = "commit.enabled",
            surface = "screen",
            ui_target = "commit",
            variant = "enabled",
            placement = { kind = "fixed", rect = { 300, 480, 180, 70 } },
            detector = detector("confirm.locator"),
            actions = {
                { id = "commit", kind = "key", key = "ENTER", proof_locator = "confirm.locator" },
            },
        },
    },
    collections = {},
    surfaces = {
        { id = "screen", kind = "scene", covers = {}, identity = { all = { "screen.identity" }, any = {}, none = {} } },
    },
    transitions = {},
})

local function present(cycle: any, binding: string, locator: string, hostProof: any?): any
    return evidence.present(
        cycle,
        { kind = "template", binding = binding, locator = locator },
        { locator = locator },
        0.99,
        hostProof
    )
end

local function key(binding: string, locator: string): string
    return model.measurement_key(binding, { kind = "locator_present", locator = locator })
end

local cycle = evidence.begin_cycle()
local hostProof = table.freeze({ host_owned = true })
local title = present(cycle, "screen.identity", "title.locator")
local confirm = present(cycle, "confirm.enabled", "confirm.locator", hostProof)
local commit = present(cycle, "commit.enabled", "confirm.locator", hostProof)
local records = {
    [key("screen.identity", "title.locator")] = title,
    [key("confirm.enabled", "confirm.locator")] = confirm,
    [key("commit.enabled", "confirm.locator")] = commit,
}
local state = resolution.resolve_state(runtime, cycle, records)
local binding = resolution.resolve_binding(runtime, cycle, state, "confirm", nil, records)
local request, reason = resolution.authorize(runtime, cycle, binding, "activate")
assert(request ~= nil, reason)
assert(request.kind == "receipt_request")
assert(request.binding == "confirm.enabled")
assert(request.variant == "enabled")
assert(request.action == "activate")
assert(request.receipt == nil)

-- A click names a coordinate and carries no key.
assert(request.action_kind == "click")
assert(request.key == nil)

local payload = resolution.take_receipt_request(request, cycle)
assert(payload ~= nil)
assert(payload.host_proof == hostProof)
assert(payload.placement.action_point[1] == 640)
assert(resolution.take_receipt_request(request, cycle) == nil)
assert(resolution.take_receipt_request({ id = request.id }, cycle) == nil)

-- A keystroke is the mirror: the key travels so the Host is told what to
-- deliver without reading the model, and the placement it authorizes against
-- carries no point at all.
local keyBinding = resolution.resolve_binding(runtime, cycle, state, "commit", nil, records)
local keyRequest, keyReason = resolution.authorize(runtime, cycle, keyBinding, "commit")
assert(keyRequest ~= nil, keyReason)
assert(keyRequest.action_kind == "key")
assert(keyRequest.key == "ENTER")
assert(keyRequest.action == "commit")

local keyPayload = resolution.take_receipt_request(keyRequest, cycle)
assert(keyPayload ~= nil)
assert(keyPayload.placement.action_point == nil)
assert(keyPayload.proof_locator == "confirm.locator")

-- Copying a resolved value loses the private ledger identity.
local copied = {}
for field, value in binding do copied[field] = value end
local denied, deniedReason = resolution.authorize(runtime, cycle, copied, "activate")
assert(denied == nil)
assert(deniedReason == "binding_not_actionable")

-- A state or Binding cannot cross cycle boundaries.
local otherCycle = evidence.begin_cycle()
local stale, staleReason = resolution.authorize(runtime, otherCycle, binding, "activate")
assert(stale == nil)
assert(staleReason == "binding_not_actionable")
evidence.end_cycle(otherCycle)

-- Even a valid request becomes unusable when its evidence cycle closes.
local closingRequest = resolution.authorize(runtime, cycle, binding, "activate")
assert(closingRequest ~= nil)
evidence.end_cycle(cycle)
assert(resolution.take_receipt_request(closingRequest, cycle) == nil)

-- There is no Luau Host-delivery or direct production input surface.
assert(observe.deliver == nil)
assert(observe.click == nil)
assert(observe.drag == nil)
assert(observe.key == nil)

return 1
