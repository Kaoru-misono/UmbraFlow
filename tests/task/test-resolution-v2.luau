--!strict
-- Contract tests for the pure v1 resolver. The task test harness supplies the
-- private `resolution` export; no screenshots, OCR engine, or host cycle is
-- needed for these synthetic measurements.

local function present(value: any, confidence: number): any
    return {
        kind = "present",
        value = value,
        confidence = confidence,
        proof = { { kind = "locator", target = "test_target" } },
    }
end

local function absent(): any
    return {
        kind = "absent",
        proof = { { kind = "locator", target = "test_target" } },
    }
end

local function unknown(reason: string): any
    return {
        kind = "unknown",
        reason = reason,
        confidence = 0.31,
    }
end

local function locator(id: string): any
    return { kind = "locator_present", locator = { locator = id } }
end

local function binding(
    id: string,
    surface: string,
    target: string,
    identity: any,
    actionId: string?
): any
    local actions = {}
    if actionId ~= nil then
        actions[1] = {
            id = actionId,
            kind = "click",
            locator = { locator = target .. ".locator" },
            preconditions = {},
        }
    end
    return {
        id = id,
        surface = surface,
        target = target,
        placement = { kind = "target_geometry" },
        identity = identity,
        readers = {},
        actions = actions,
    }
end

local function model(surfaces: any, bindings: any): any
    return {
        schema_version = 1,
        surfaces = surfaces,
        bindings = bindings,
    }
end

local function measure(values: any): any
    return function(predicate: any): any
        local id = if predicate.kind == "locator_present"
            then predicate.locator.locator
            else predicate.reader
        return values[id] or unknown("not_measured")
    end
end

do
    local runtimeModel = model(
        {
            { id = "battle", kind = "scene", contexts = { "chaos" } },
            {
                id = "enemy_inspect",
                kind = "overlay",
                contexts = { "chaos" },
                covers = { "battle" },
            },
        },
        {
            binding(
                "battle_identity",
                "battle",
                "battle_title",
                { all = { locator("battle.identity") }, any = {}, none = {} },
                nil
            ),
            binding(
                "inspect_identity",
                "enemy_inspect",
                "close_overlay",
                { all = { locator("enemy_inspect.identity") }, any = {}, none = {} },
                "close"
            ),
        }
    )
    local result = resolution.resolve(
        runtimeModel,
        measure({
            ["battle.identity"] = present(true, 0.96),
            ["enemy_inspect.identity"] = present(true, 0.99),
            ["close_overlay.locator"] = present(true, 0.98),
        }),
        { context = "chaos" }
    )
    assert(result.kind == "resolved", "scene plus overlay must resolve")
    assert(result.state.surfaces[1] == "battle")
    assert(result.state.surfaces[2] == "enemy_inspect")

    local authorization = resolution.authorize(
        runtimeModel,
        result,
        { target = "close_overlay", kind = "click" },
        measure({ ["close_overlay.locator"] = present(true, 0.98) })
    )
    assert(authorization.authorized, "top overlay action must be authorized")
end

do
    local runtimeModel = model(
        {
            { id = "shop", kind = "scene", contexts = { "menu" } },
            { id = "equip", kind = "scene", contexts = { "menu" } },
        },
        {
            binding(
                "shop_identity",
                "shop",
                "leave_button",
                { all = { locator("common.header") }, any = {}, none = {} },
                "leave"
            ),
            binding(
                "equip_identity",
                "equip",
                "leave_button",
                { all = { locator("common.header") }, any = {}, none = {} },
                "leave"
            ),
        }
    )
    local result = resolution.resolve(
        runtimeModel,
        measure({ ["common.header"] = present(true, 0.97) }),
        { context = "menu" }
    )
    assert(result.kind == "ambiguous", "equal candidates must not first-match")
    assert(#result.candidates == 2)
    assert(result.candidates[1].state.surfaces[1] ~= nil)

    local noReceipt = resolution.authorize(
        runtimeModel,
        result,
        { target = "leave_button", kind = "click" },
        measure({ ["leave_button.locator"] = present(true, 0.98) })
    )
    assert(not noReceipt.authorized, "ambiguous resolution must deny action")
end

do
    local runtimeModel = model(
        {
            { id = "shop", kind = "scene", contexts = { "menu" } },
        },
        {
            binding(
                "shop_identity",
                "shop",
                "leave_button",
                { all = { locator("shop.title") }, any = {}, none = {} },
                "leave"
            ),
        }
    )
    local result = resolution.resolve(
        runtimeModel,
        measure({ ["shop.title"] = unknown("ocr_unreadable") }),
        { context = "menu" }
    )
    assert(result.kind == "unknown", "unknown OCR must remain unknown")
    assert(result.reason == "unknown_evidence")
end

do
    local runtimeModel = model(
        {
            { id = "camp", kind = "scene", contexts = { "camp" } },
            { id = "battle", kind = "scene", contexts = { "battle" } },
            {
                id = "network_retry",
                kind = "interrupt",
                contexts = {},
            },
        },
        {
            binding(
                "camp_identity",
                "camp",
                "camp_title",
                { all = { locator("camp.identity") }, any = {}, none = {} },
                nil
            ),
            binding(
                "retry_identity",
                "network_retry",
                "retry",
                { all = { locator("network_retry.identity") }, any = {}, none = {} },
                "retry"
            ),
        }
    )
    local result = resolution.resolve(
        runtimeModel,
        measure({
            ["camp.identity"] = present(true, 0.95),
            ["network_retry.identity"] = present(true, 0.99),
        }),
        { context = "camp" }
    )
    assert(result.kind == "resolved")
    assert(result.state.surfaces[1] == "camp")
    assert(result.state.surfaces[2] == "network_retry")
end

do
    local runtimeModel = model(
        {
            { id = "known_scene", kind = "scene", contexts = { "menu" } },
        },
        {
            binding(
                "known_identity",
                "known_scene",
                "dismiss",
                { all = { locator("known.identity") }, any = {}, none = {} },
                "dismiss"
            ),
        }
    )
    local result = resolution.resolve(
        runtimeModel,
        measure({ ["known.identity"] = absent() }),
        { context = "menu" }
    )
    assert(result.kind == "unknown")
    assert(result.reason == "no_surface_candidate")
end
