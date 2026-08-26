--!strict
-- Two-stage, same-cycle RuntimeModel v4 resolver.

local model = require("@umbraflow/internal/model")
local evidence = require("@umbraflow/internal/evidence")

local resolution = {}

local nextStateId = 0
local nextBindingId = 0
local nextRequestId = 0
local stateLedger: { [any]: any } = setmetatable({}, { __mode = "k" }) :: any
local bindingLedger: { [any]: any } = setmetatable({}, { __mode = "k" }) :: any
local requestLedger: { [any]: any } = setmetatable({}, { __mode = "k" }) :: any

local function freezeList(values: { any }): { any }
    return table.freeze(values)
end

local function copyList(values: { any }): { any }
    local result = {}
    for index, value in values do result[index] = value end
    return freezeList(result)
end

local function sameList(left: { any }, right: { any }): boolean
    if #left ~= #right then return false end
    for index, value in left do
        if right[index] ~= value then return false end
    end
    return true
end

local function summaries(records: { any }, cycle: any): { any }
    local result = {}
    local seen: { [string]: boolean } = {}
    for _, record in records do
        local summary = evidence.summary(record, cycle)
        if summary ~= nil and not seen[summary.id] then
            seen[summary.id] = true
            table.insert(result, summary)
        end
    end
    return freezeList(result)
end

local function allEvidence(evaluations: { any }): { any }
    local result = {}
    local seen: { [string]: boolean } = {}
    for _, evaluation in evaluations do
        for _, summary in evaluation.evidence do
            if not seen[summary.id] then
                seen[summary.id] = true
                table.insert(result, summary)
            end
        end
    end
    return freezeList(result)
end

local function combine(
    allGroup: { any },
    anyGroup: { any },
    noneGroup: { any },
    outcome: (any) -> any
): any
    local absent = false
    local unknown = false
    local weakest = 1

    for _, item in allGroup do
        local value = outcome(item)
        if value.kind == "absent" then absent = true end
        if value.kind == "unknown" then unknown = true end
        if value.kind == "present" and value.confidence < weakest then weakest = value.confidence end
    end

    if #anyGroup > 0 then
        local anyPresent = false
        local anyUnknown = false
        for _, item in anyGroup do
            local value = outcome(item)
            if value.kind == "present" then
                anyPresent = true
                if value.confidence < weakest then weakest = value.confidence end
            elseif value.kind == "unknown" then
                anyUnknown = true
            end
        end
        if not anyPresent then
            if anyUnknown then unknown = true else absent = true end
        end
    end

    for _, item in noneGroup do
        local value = outcome(item)
        if value.kind == "present" then absent = true end
        if value.kind == "unknown" then unknown = true end
    end

    local kind = if absent then "absent" elseif unknown then "unknown" else "present"
    return { kind = kind, confidence = if kind == "present" then weakest else nil }
end

local function detectorRecords(detector: any): { any }
    local result = {}
    for _, group in { detector.all, detector.any, detector.none } do
        for _, predicate in group do table.insert(result, predicate) end
    end
    return result
end

local function evaluateVariant(binding: any, variant: any, recordsByKey: any, cycle: any): any
    local supplied = if type(recordsByKey) == "table" then recordsByKey else {}
    local used = {}
    local recordByPredicate: { [string]: any } = {}
    local outcomeByPredicate: { [string]: any } = {}
    for _, predicate in detectorRecords(variant.detector) do
        local predicateKey = model.predicate_key(predicate)
        local measurementKey = model.measurement_key(binding.id, predicate)
        local record = if type(recordsByKey) == "function"
            then recordsByKey(binding, predicate)
            else supplied[measurementKey]
        local outcome = evidence.result_of(record, cycle)
        if outcome == nil then
            record = evidence.unknown(cycle, "not_measured", {
                kind = "measurement",
                binding = binding.id,
                predicate = predicateKey,
            })
            outcome = evidence.result_of(record, cycle)
        end
        recordByPredicate[predicateKey] = record
        outcomeByPredicate[predicateKey] = outcome
        table.insert(used, record)
    end
    local combined = combine(
        variant.detector.all,
        variant.detector.any,
        variant.detector.none,
        function(predicate: any): any return outcomeByPredicate[model.predicate_key(predicate)] end
    )
    return {
        kind = combined.kind,
        confidence = combined.confidence,
        binding = binding,
        variant = variant,
        records = used,
        record_by_predicate = recordByPredicate,
        evidence = summaries(used, cycle),
    }
end

local function evaluateBinding(binding: any, recordsByKey: any, cycle: any): any
    local evaluations = {}
    local present = {}
    local unknown = false
    for _, variant in binding.variants do
        local evaluated = evaluateVariant(binding, variant, recordsByKey, cycle)
        table.insert(evaluations, evaluated)
        if evaluated.kind == "present" then table.insert(present, evaluated) end
        if evaluated.kind == "unknown" then unknown = true end
    end

    local kind = "absent"
    if #present > 1 then
        kind = "ambiguous"
    elseif unknown then
        kind = "unknown"
    elseif #present == 1 then
        kind = "present"
    end
    local selected = if #present == 1 then present[1] else nil
    local variants = {}
    for _, evaluated in present do table.insert(variants, evaluated.variant.name) end
    return {
        kind = kind,
        confidence = if selected == nil then nil else selected.confidence,
        binding = binding,
        variant = if selected == nil then nil else selected.variant.name,
        variants = freezeList(variants),
        records = if selected == nil then {} else selected.records,
        record_by_predicate = if selected == nil then {} else selected.record_by_predicate,
        evidence = allEvidence(evaluations),
    }
end

local function evaluateSurface(
    runtimeModel: any,
    surface: any,
    recordsByKey: any,
    cycle: any
): any
    local evaluations = {}
    local absent = false
    local unknown = false
    local ambiguous = false
    local weakest = 1
    for _, bindingId in surface.identity do
        local evaluated = evaluateBinding(runtimeModel.binding_by_id[bindingId], recordsByKey, cycle)
        table.insert(evaluations, evaluated)
        if evaluated.kind == "absent" then absent = true end
        if evaluated.kind == "unknown" then unknown = true end
        if evaluated.kind == "ambiguous" then ambiguous = true end
        if evaluated.kind == "present" and evaluated.confidence < weakest then
            weakest = evaluated.confidence
        end
    end
    local kind = if ambiguous
        then "ambiguous"
        elseif absent
        then "absent"
        elseif unknown
        then "unknown"
        else "present"
    return {
        kind = kind,
        confidence = if kind == "present" then weakest else nil,
        surface = surface,
        binding_evaluations = evaluations,
        evidence = allEvidence(evaluations),
    }
end

local function unknownState(reason: string, evaluations: { any }, diagnostic: string?): any
    return table.freeze({
        kind = "unknown_state",
        reason = reason,
        evidence = allEvidence(evaluations),
        diagnostic = diagnostic,
    })
end

local function ambiguousState(candidates: { any }, reason: string, evaluations: { any }): any
    return table.freeze({
        kind = "ambiguous_state",
        candidates = freezeList(candidates),
        conflicts = freezeList({ table.freeze({ kind = reason }) }),
        evidence = allEvidence(evaluations),
    })
end

local function resolvedState(
    runtimeModel: any,
    cycle: any,
    stack: { any },
    evaluations: { any }
): any
    nextStateId += 1
    local result = table.freeze({
        kind = "resolved_state",
        id = "state-resolution-" .. tostring(nextStateId),
        ordered_surface_stack = copyList(stack),
        evidence = allEvidence(evaluations),
    })
    stateLedger[result] = {
        cycle = cycle,
        model = runtimeModel,
        stack = result.ordered_surface_stack,
    }
    return result
end

local function confirmExpected(
    runtimeModel: any,
    cycle: any,
    recordsByKey: any,
    expectedStack: { any }
): any?
    local evaluations = {}
    for _, surfaceId in expectedStack do
        local evaluated = evaluateSurface(
            runtimeModel,
            runtimeModel.surface_by_id[surfaceId],
            recordsByKey,
            cycle
        )
        table.insert(evaluations, evaluated)
        if evaluated.kind ~= "present" then return nil end
    end
    return resolvedState(runtimeModel, cycle, expectedStack, evaluations)
end

function resolution.resolve_state(
    runtimeModel: any,
    cycle: any,
    recordsByKey: any,
    expectedStack: { any }?
): any
    if not model.is_runtime_model(runtimeModel) then error("resolve_state needs RuntimeModel v4", 2) end
    if not evidence.is_cycle_live(cycle) then error("resolve_state needs a live framework cycle", 2) end
    if expectedStack ~= nil then
        if not model.valid_surface_stack(runtimeModel, expectedStack) then
            error("resolve_state expected stack must be a declared Surface stack", 2)
        end
        local confirmed = confirmExpected(runtimeModel, cycle, recordsByKey, expectedStack)
        if confirmed ~= nil then return confirmed end
    end

    local evaluations = {}
    local scenes = {}
    local presentLayers = {}
    local unknownScenes = 0
    for _, surface in runtimeModel.surfaces do
        local evaluated = evaluateSurface(runtimeModel, surface, recordsByKey, cycle)
        table.insert(evaluations, evaluated)
        if #surface.covers == 0 then
            if evaluated.kind == "present" then table.insert(scenes, surface) end
            if evaluated.kind == "unknown" then unknownScenes += 1 end
        elseif evaluated.kind == "present" then
            table.insert(presentLayers, surface)
        end
    end

    for _, evaluated in evaluations do
        if evaluated.kind == "ambiguous" then
            return ambiguousState({}, "ambiguous_binding_variant", evaluations)
        end
    end

    if unknownScenes > 0 then return unknownState("unknown_scene_competitor", evaluations) end
    if #scenes == 0 then
        return unknownState(
            "no_scene_candidate",
            evaluations,
            if #presentLayers == 0 then "visible_content_matched_nothing" else nil
        )
    end
    if #scenes > 1 then
        local candidates = {}
        for _, scene in scenes do
            table.insert(candidates, table.freeze({ ordered_surface_stack = freezeList({ scene.id }) }))
        end
        return ambiguousState(candidates, "multiple_scenes", evaluations)
    end

    local scene = scenes[1]
    for _, evaluated in evaluations do
        local surface = evaluated.surface
        if #surface.covers > 0 and evaluated.kind == "unknown"
            and model.surface_covers(runtimeModel, surface.id, scene.id)
        then
            return unknownState("unknown_layer_competitor", evaluations)
        end
    end

    for _, surface in presentLayers do
        if not model.surface_covers(runtimeModel, surface.id, scene.id) then
            return ambiguousState({}, "incompatible_present_layer", evaluations)
        end
    end
    for left = 1, #presentLayers do
        for right = left + 1, #presentLayers do
            if not model.surface_covers(runtimeModel, presentLayers[left].id, presentLayers[right].id)
                and not model.surface_covers(runtimeModel, presentLayers[right].id, presentLayers[left].id)
            then
                return ambiguousState({}, "incomparable_present_layers", evaluations)
            end
        end
    end
    table.sort(presentLayers, function(left, right)
        return model.surface_covers(runtimeModel, right.id, left.id)
    end)
    local stack = { scene.id }
    for _, surface in presentLayers do table.insert(stack, surface.id) end
    if not model.valid_surface_stack(runtimeModel, stack) then
        return ambiguousState({}, "invalid_surface_stack", evaluations)
    end

    return resolvedState(runtimeModel, cycle, stack, evaluations)
end

-- Compares an offline observation with declared policy. The observation names
-- its declaration rather than duplicating its trigger, so a replay cannot
-- become a second policy authority. Both destinations in the returned report
-- are owned frozen copies: retaining the report cannot retain a mutable caller
-- table or rewrite the RuntimeModel it was compared with.
function resolution.compare_transition(
    runtimeModel: any,
    observedTransition: any
): (any?, string?)
    if not model.is_runtime_model(runtimeModel) or type(observedTransition) ~= "table" then
        return nil, "invalid_observed_transition"
    end
    for field in observedTransition do
        if field ~= "kind" and field ~= "transition" and field ~= "observed_to_surfaces" then
            return nil, "invalid_observed_transition"
        end
    end
    if observedTransition.kind ~= "observed_transition"
        or type(observedTransition.transition) ~= "string"
    then
        return nil, "invalid_observed_transition"
    end
    local declared = runtimeModel.transition_by_id[observedTransition.transition]
    if declared == nil then return nil, "unknown_transition" end
    if not model.valid_surface_stack(runtimeModel, observedTransition.observed_to_surfaces) then
        return nil, "invalid_observed_destination"
    end

    local declaredDestination = copyList(declared.to_surfaces)
    local observedDestination = copyList(observedTransition.observed_to_surfaces)
    return table.freeze({
        kind = "transition_comparison",
        transition = declared.id,
        declared_to_surfaces = declaredDestination,
        observed_to_surfaces = observedDestination,
        matches_policy = sameList(declaredDestination, observedDestination),
    }), nil
end

local function unknownBinding(reason: string, evaluations: { any }): any
    return table.freeze({
        kind = "unknown_binding",
        reason = reason,
        evidence = allEvidence(evaluations),
    })
end

function resolution.binding_candidates(
    runtimeModel: any,
    cycle: any,
    resolvedState: any,
    uiTarget: any
): { any }?
    if not model.is_runtime_model(runtimeModel)
        or type(uiTarget) ~= "string"
        or runtimeModel.ui_target_by_id[uiTarget] == nil
    then
        return nil
    end
    local stateEntry = stateLedger[resolvedState]
    if stateEntry == nil
        or stateEntry.cycle ~= cycle
        or stateEntry.model ~= runtimeModel
        or not evidence.is_cycle_live(cycle)
    then
        return nil
    end
    local result = {}
    local top = stateEntry.stack[#stateEntry.stack]
    for _, candidate in runtimeModel.bindings_by_surface[top] or {} do
        if candidate.ui_target == uiTarget then table.insert(result, candidate) end
    end
    return freezeList(result)
end

local function collectionTarget(
    runtimeModel: any,
    cycle: any,
    resolvedState: any,
    collectionId: any
): (any?, string?)
    if not model.is_runtime_model(runtimeModel)
        or type(collectionId) ~= "string"
        or runtimeModel.collection_by_id[collectionId] == nil
    then
        return nil, "unknown_collection"
    end
    local stateEntry = stateLedger[resolvedState]
    if stateEntry == nil
        or stateEntry.cycle ~= cycle
        or stateEntry.model ~= runtimeModel
        or not evidence.is_cycle_live(cycle)
    then
        return nil, "stale_state"
    end
    local collection = runtimeModel.collection_by_id[collectionId]
    for _, surfaceId in stateEntry.stack do
        if collection.surface == surfaceId then return collection, nil end
    end
    return nil, "collection_off_surface"
end

local function orderedCollectionLines(collection: any, lines: { any }): ({ any }?, string?)
    local ordered = {}
    for index, line in lines do ordered[index] = line end
    local axis = if collection.placement.order == "left_to_right" then 1 else 2
    local size = if axis == 1 then 3 else 4
    table.sort(ordered, function(left: any, right: any): boolean
        return left.rect[axis] < right.rect[axis]
    end)
    for index = 2, #ordered do
        local previous = ordered[index - 1].rect
        local current = ordered[index].rect
        if previous[axis] + previous[size] > current[axis] then
            return nil, "ambiguous_order"
        end
    end
    return ordered, nil
end

-- The declared slot coordinates of one cardinality, in integer pixels. A
-- layout of n items spans min(pitch * (n - 1), extent): the declared pitch
-- holds while the fan is short, and once extent binds the spacing shrinks with
-- the cardinality instead of the span growing. Each slot is the exact rational
-- coordinate rounded half up, computed from integers alone so the result never
-- depends on the order the divisions happen in; that per-slot rounding, not a
-- constraint on pitch, is what keeps every cardinality on integer pixels.
local function layoutSlots(slots: any, count: number): { number }
    if count == 1 then return { slots.origin } end
    local span = math.min(slots.pitch * (count - 1), slots.extent)
    local denominator = 2 * (count - 1)
    local result: { number } = table.create(count)
    for index = 0, count - 1 do
        local numerator = 2 * slots.origin * (count - 1)
            + (2 * index - (count - 1)) * span
        result[index + 1] = math.floor(
            (2 * numerator + denominator) / (2 * denominator)
        )
    end
    return result
end

-- The smallest distance between two adjacent slots of one layout, which is
-- what decides whether a measured origin can belong to exactly one of them.
-- A one-item layout has no adjacent pair and therefore no such distance.
local function smallestSlotGap(layout: { number }): number?
    local smallest: number? = nil
    for index = 2, #layout do
        local gap = layout[index] - layout[index - 1]
        if smallest == nil or gap < smallest then smallest = gap end
    end
    return smallest
end

local function fittedCollection(collection: any, ordered: { any }): (any?, string?)
    local measuredCount = #ordered
    if measuredCount == 0 then
        return table.freeze({
            completeness = "complete",
            count = 0,
            items = freezeList({}),
        }), nil
    end
    local axis = if collection.placement.order == "left_to_right" then 1 else 2
    local searchRect = collection.placement.search_rect
    local searchStart = searchRect[axis]
    local searchEnd = searchStart + searchRect[axis + 2]
    local slots = collection.placement.slots
    -- Two adjacent slots closer together than this cannot be told apart by a
    -- measurement the declaration allows to sit `tolerance` from either.
    local assignable = 2 * slots.tolerance + 1
    -- The search walks upward looking for a larger layout with missing slots,
    -- and `maximum_slots` is what stops it. The bound is the declaration's own:
    -- origin, pitch, extent and tolerance are pixels, so any slot count derived
    -- from them is arithmetic this framework invented over a declaration that
    -- said nothing about cardinality. Ruled in
    -- docs/decisions/2026-08-24-policy-is-the-axis-and-observation-holds-a-frame.md.
    --
    -- `stoppedAtBound` records WHY the walk ended, because only one of the three
    -- endings is the declaration disagreeing with the frame. A walk cut short by
    -- the search rectangle or by the ambiguity floor reached a layout it could
    -- judge and rejected it, which is the `unknown` this function has always
    -- reported; a walk that ran out of cardinalities never reached the layout
    -- that would have fitted, and says so by name.
    local maximumSlots = slots.maximum_slots
    local stoppedAtBound = true
    for layoutCount = measuredCount, maximumSlots do
        local layout = layoutSlots(slots, layoutCount)
        if layout[1] < searchStart or layout[layoutCount] >= searchEnd then
            stoppedAtBound = false
            break
        end
        local gap = smallestSlotGap(layout)
        if gap ~= nil and gap < assignable then
            -- A larger layout only packs its slots tighter, so this is the last
            -- cardinality worth trying. When it is the detected cardinality the
            -- declaration cannot place the items that were actually measured,
            -- and that is refused by name rather than reported as no-fit.
            if layoutCount == measuredCount then
                return nil, string.format(
                    "ambiguous_slots: %d items leave adjacent slots %d apart, "
                        .. "under the %d that tolerance %d requires",
                    layoutCount,
                    gap,
                    assignable,
                    slots.tolerance
                )
            end
            stoppedAtBound = false
            break
        end
        local items = {}
        local slotIndex = 0
        local fits = true
        for position, line in ordered do
            local coordinate = line.rect[axis]
            while slotIndex < layoutCount - 1
                and layout[slotIndex + 1] < coordinate - slots.tolerance
            do
                slotIndex += 1
            end
            if slotIndex >= layoutCount
                or math.abs(coordinate - layout[slotIndex + 1]) > slots.tolerance
            then
                fits = false
                break
            end
            items[position] = { index = slotIndex, rect = line.rect }
            slotIndex += 1
        end
        if fits then
            return table.freeze({
                completeness = if layoutCount == measuredCount then "complete" else "partial",
                count = measuredCount,
                items = items,
            }), nil
        end
    end
    if stoppedAtBound then
        return nil, string.format(
            "bounded_slots: %d measured items fit no layout of at most %d "
                .. "slots, which is the maximum_slots this collection declares",
            measuredCount,
            maximumSlots
        )
    end
    return table.freeze({ completeness = "unknown", count = measuredCount }), nil
end

local function collectionItems(collection: any, reading: any): (any?, string?)
    if type(reading) ~= "table" then return nil, "not_measured" end
    if reading.kind == "absent" then return fittedCollection(collection, {}) end
    if reading.kind ~= "read" or type(reading.lines) ~= "table" then
        local reason = if reading.kind == "unknown" and evidence.is_unknown_reason(reading.reason)
            then reading.reason
            else "ocr_unreadable"
        return nil, reason
    end
    local ordered, orderReason = orderedCollectionLines(collection, reading.lines)
    if ordered == nil then return nil, orderReason end
    return fittedCollection(collection, ordered)
end

-- Resolves one Collection click against one measured item. The item index is
-- the stable slot ordinal, not the detection's enumeration position. The point is
-- an item-origin delta and refuses the frame edge instead of being clamped.
function resolution.collection_action_point(
    runtimeModel: any,
    cycle: any,
    resolvedState: any,
    collectionId: any,
    itemIndex: any,
    actionId: any,
    readingsByKey: any
): ({ number }?, string?)
    local collection, targetReason = collectionTarget(
        runtimeModel,
        cycle,
        resolvedState,
        collectionId
    )
    if collection == nil then return nil, targetReason end
    if type(itemIndex) ~= "number" or itemIndex % 1 ~= 0 or itemIndex < 0 then
        return nil, "unknown_item"
    end
    if type(actionId) ~= "string" then return nil, "unknown_action" end
    local selectedAction = nil
    for _, action in collection.actions do
        if action.id == actionId then selectedAction = action end
    end
    if selectedAction == nil then return nil, "unknown_action" end
    if selectedAction.kind ~= "click" then return nil, "action_has_no_point" end

    local supplied = if type(readingsByKey) == "table" then readingsByKey else {}
    local reading = supplied[model.collection_detection_key(collection.id)]
    local fitted, itemReason = collectionItems(collection, reading)
    if fitted == nil then return nil, itemReason end
    if fitted.items == nil then return nil, "unknown_item" end
    local selectedItem = nil
    for _, item in fitted.items do
        if item.index == itemIndex then selectedItem = item end
    end
    if selectedItem == nil then return nil, "unknown_item" end

    local point = {
        selectedItem.rect[1] + selectedAction.offset[1],
        selectedItem.rect[2] + selectedAction.offset[2],
    }
    if point[1] < 0
        or point[2] < 0
        or point[1] >= runtimeModel.base_resolution[1]
        or point[2] >= runtimeModel.base_resolution[2]
    then
        return nil, "action_point_out_of_frame"
    end
    return freezeList(point), nil
end

local function offsetItemRect(
    runtimeModel: any,
    item: any,
    read: any
): ({ number }?, string?)
    local base = item.rect
    local offset = read.offset
    if type(base) ~= "table" or #base ~= 4 then return nil, "ocr_unreadable" end
    for _, value in base do
        if type(value) ~= "number" or value % 1 ~= 0 then return nil, "ocr_unreadable" end
    end
    local area = {
        base[1] + offset[1],
        base[2] + offset[2],
        read.size[1],
        read.size[2],
    }
    if area[1] < 0
        or area[2] < 0
        or area[3] < 1
        or area[4] < 1
        or area[1] + area[3] > runtimeModel.base_resolution[1]
        or area[2] + area[4] > runtimeModel.base_resolution[2]
    then
        return nil, "read_out_of_frame"
    end
    return freezeList(area), nil
end

function resolution.collection_read_targets(
    runtimeModel: any,
    cycle: any,
    resolvedState: any,
    collectionId: any,
    readingsByKey: any
): ({ any }?, string?)
    local collection, targetReason = collectionTarget(
        runtimeModel,
        cycle,
        resolvedState,
        collectionId
    )
    if collection == nil then return nil, targetReason end
    local supplied = if type(readingsByKey) == "table" then readingsByKey else {}
    local reading = supplied[model.collection_detection_key(collection.id)]
    local fitted, itemReason = collectionItems(collection, reading)
    if fitted == nil then return nil, itemReason end
    if fitted.items == nil then return freezeList({}), nil end
    local targets = {}
    for _, item in fitted.items do
        for _, read in collection.reads do
            local area, areaReason = offsetItemRect(runtimeModel, item, read)
            if area == nil then return nil, areaReason end
            table.insert(targets, table.freeze({
                key = model.collection_item_reading_key(
                    collection.id,
                    item.index,
                    read.id
                ),
                read = read,
                rect = area,
            }))
        end
    end
    return freezeList(targets), nil
end

-- One reported reading, named by the id of the site that declared it. That id
-- is the whole of the name: a Readout owns its rectangle, and a Collection's
-- per-item read is reported inside the item inside the Collection that already
-- names both. The member this used to carry, `ui_target`, was a Binding-shaped
-- name that a Collection filled with its own id, so a consumer reading it was
-- told a UiTarget that had never existed.
local function reportedReading(id: string, reading: any): any
    local kind = "unknown"
    local lines = nil
    local reason = "not_measured"
    if type(reading) == "table" then
        if reading.kind == "read" and type(reading.lines) == "table" then
            kind = "read"
            lines = reading.lines
            reason = nil
        elseif reading.kind == "absent" then
            kind = "absent"
            reason = nil
        elseif reading.kind == "unknown" and evidence.is_unknown_reason(reading.reason) then
            reason = reading.reason
        end
    end
    return table.freeze({
        id = id,
        kind = kind,
        lines = lines,
        reason = reason,
    })
end

local function collectionPredicateMatches(
    collection: any,
    itemIndex: number,
    supplied: any
): (boolean?, string?)
    local predicate = collection.predicate
    if predicate == nil then return true, nil end
    local reading = supplied[model.collection_item_reading_key(
        collection.id,
        itemIndex,
        predicate.read
    )]
    if type(reading) ~= "table" then return nil, "predicate_not_measured" end
    if reading.kind == "absent" then return false, nil end
    if reading.kind ~= "read" or type(reading.lines) ~= "table" then
        return nil, "predicate_unreadable"
    end
    for _, line in reading.lines do
        if type(line) == "table" and line.text == predicate.value then return true, nil end
    end
    return false, nil
end

-- Resolves one variable-cardinality collection against the same live state
-- that made its Surface available. A bare search rectangle contributes nothing:
-- only an absent or successful answer from the collection's own trusted
-- detection can resolve a count. Unknown reading and overlapping spatial spans
-- refuse instead of fabricating items or inheriting adapter enumeration order.
function resolution.resolve_collection(
    runtimeModel: any,
    cycle: any,
    resolvedState: any,
    collectionId: any,
    readingsByKey: any
): (any?, string?)
    local collection, targetReason = collectionTarget(
        runtimeModel,
        cycle,
        resolvedState,
        collectionId
    )
    if collection == nil then return nil, targetReason end
    local supplied = if type(readingsByKey) == "table" then readingsByKey else {}
    local reading = supplied[model.collection_detection_key(collection.id)]
    local fitted, itemReason = collectionItems(collection, reading)
    if fitted == nil then return nil, itemReason end
    if fitted.items == nil then
        return table.freeze({ completeness = fitted.completeness, count = fitted.count }), nil
    end
    local targets, targetReadReason = resolution.collection_read_targets(
        runtimeModel,
        cycle,
        resolvedState,
        collectionId,
        supplied
    )
    if targets == nil then return nil, targetReadReason end
    local items = {}
    for _, detected in fitted.items do
        local matches, predicateReason = collectionPredicateMatches(
            collection,
            detected.index,
            supplied
        )
        if matches == nil then return nil, predicateReason end
        if not matches then continue end
        local readings = {}
        for _, read in collection.reads do
            table.insert(readings, reportedReading(
                read.id,
                supplied[model.collection_item_reading_key(
                    collection.id,
                    detected.index,
                    read.id
                )]
            ))
        end
        table.insert(items, table.freeze({
            index = detected.index,
            rect = detected.rect,
            readings = freezeList(readings),
        }))
    end
    return table.freeze({
        completeness = fitted.completeness,
        count = #items,
        items = freezeList(items),
    }), nil
end

-- The Readouts a resolved state reports: every Readout on a Surface of its
-- stack, in stack order and then declaration order.  It is the reading sibling
-- of binding_candidates and refuses on the same terms, so a state from another
-- cycle or another model reports nothing rather than reporting readings taken
-- against a world it did not resolve.
--
-- The stack IS the gate, and there is nothing else to pass.  A Readout has no
-- detector of its own -- the value printed in its rectangle changes every
-- frame, so there is no stable ink there to measure -- and it needs none: the
-- Surface's identity Bindings were measured present to put that Surface on the
-- stack, and they are what proved this rectangle is this rectangle.
function resolution.stack_readouts(
    runtimeModel: any,
    cycle: any,
    resolvedState: any
): { any }?
    if not model.is_runtime_model(runtimeModel) then return nil end
    local stateEntry = stateLedger[resolvedState]
    if stateEntry == nil
        or stateEntry.cycle ~= cycle
        or stateEntry.model ~= runtimeModel
        or not evidence.is_cycle_live(cycle)
    then
        return nil
    end
    local result = {}
    for _, surfaceId in stateEntry.stack do
        for _, readout in runtimeModel.readouts_by_surface[surfaceId] or {} do
            table.insert(result, readout)
        end
    end
    return freezeList(result)
end

-- What a resolved state reports it read: one entry per Readout on its stack,
-- sorted by Readout id.  Each is `{ id, kind }` plus `lines` when it read and
-- `reason` when it could not.
--
-- A reading is a LIST of lines under either layout.  A `single_line` read
-- reports one element rather than a second shape, because a consumer that has
-- to ask which spelling it was handed has been handed two contracts.
--
-- All three outcomes travel.  A consumer told only about successful reads
-- cannot separate "nothing is written here" from "this frame was unreadable",
-- and has to fail closed on a single blurry capture; that is what dropping the
-- failures cost.  The length of the list is therefore decided by the model and
-- the resolved stack rather than by what the Host managed to answer, and a
-- reading nothing supplied is `not_measured` -- the rule evaluateBinding
-- already applies one function up to a predicate nothing measured.
--
-- The score still does not travel.  It has already been compared against the
-- confidence_floor the Readout declared, which is the declaration's judgement
-- and not the caller's to re-take; carrying the number as well would put a
-- value that differs between two captures of one unchanged screen inside a
-- hashed decision basis.  A bounded reason is safe there, a float is not.
--
-- Each line's RECT does travel, and is hashed with the rest of this document.
-- That is the ruling and not an oversight: a `block` read's rectangle is
-- measured, so it can move by a pixel between two captures of one unchanged
-- screen exactly as the score can -- but unlike the score it has a decision
-- left in it, because it is where the consumer acts.  A basis that omitted an
-- input the consumer branches on would let a frozen plan be re-derived into a
-- different action without the basis moving, and unsoundness outranks an extra
-- approval.  Under `single_line` the rect is the Readout's declared rectangle
-- and cannot move at all.
function resolution.resolve_readings(
    runtimeModel: any,
    cycle: any,
    resolvedState: any,
    readingsByKey: any
): { any }
    local supplied = if type(readingsByKey) == "table" then readingsByKey else {}
    local result = {}
    for _, readout in resolution.stack_readouts(runtimeModel, cycle, resolvedState) or {} do
        table.insert(result, reportedReading(
            readout.id,
            supplied[model.readout_key(readout.id)]
        ))
    end
    table.sort(result, function(left: any, right: any): boolean
        return left.id < right.id
    end)
    return freezeList(result)
end

function resolution.resolve_binding(
    runtimeModel: any,
    cycle: any,
    resolvedState: any,
    uiTarget: any,
    bindingId: any?,
    recordsByKey: any
): any
    if not model.is_runtime_model(runtimeModel) then error("resolve_binding needs RuntimeModel v4", 2) end
    if type(uiTarget) ~= "string" or runtimeModel.ui_target_by_id[uiTarget] == nil then
        error("resolve_binding needs a declared UiTarget", 2)
    end
    if bindingId ~= nil and type(bindingId) ~= "string" then error("binding_id must be a string", 2) end
    local stateEntry = stateLedger[resolvedState]
    if stateEntry == nil
        or stateEntry.cycle ~= cycle
        or stateEntry.model ~= runtimeModel
        or not evidence.is_cycle_live(cycle)
    then
        return unknownBinding("state_not_actionable", {})
    end

    local candidates = resolution.binding_candidates(runtimeModel, cycle, resolvedState, uiTarget) or {}
    if #candidates == 0 then return unknownBinding("no_binding_candidate", {}) end

    local evaluations = {}
    local present = {}
    local ambiguous = {}
    local hasUnknown = false
    for _, candidate in candidates do
        local evaluated = evaluateBinding(candidate, recordsByKey, cycle)
        table.insert(evaluations, evaluated)
        if evaluated.kind == "unknown" then hasUnknown = true end
        if evaluated.kind == "present" then table.insert(present, evaluated) end
        if evaluated.kind == "ambiguous" then table.insert(ambiguous, evaluated) end
    end
    if #ambiguous > 0 then
        local candidates = {}
        for _, evaluated in ambiguous do
            for _, variant in evaluated.variants do
                table.insert(candidates, evaluated.binding.id .. ":" .. variant)
            end
        end
        return table.freeze({
            kind = "ambiguous_binding",
            candidates = freezeList(candidates),
            evidence = allEvidence(evaluations),
        })
    end
    if hasUnknown then return unknownBinding("unknown_competitor", evaluations) end
    if #present == 0 then return unknownBinding("no_binding_candidate", evaluations) end
    if #present > 1 then
        local ids = {}
        for _, evaluated in present do table.insert(ids, evaluated.binding.id) end
        return table.freeze({
            kind = "ambiguous_binding",
            candidates = freezeList(ids),
            evidence = allEvidence(evaluations),
        })
    end
    if bindingId ~= nil and present[1].binding.id ~= bindingId then
        return unknownBinding("requested_binding_absent", evaluations)
    end

    nextBindingId += 1
    local selected = present[1]
    local result = table.freeze({
        kind = "resolved_binding",
        id = "binding-resolution-" .. tostring(nextBindingId),
        state_identity = resolvedState.id,
        surface = selected.binding.surface,
        ui_target = selected.binding.ui_target,
        binding = selected.binding.id,
        variant = selected.variant,
        evidence = selected.evidence,
    })
    bindingLedger[result] = {
        cycle = cycle,
        model = runtimeModel,
        state = resolvedState,
        binding = selected.binding,
        evaluation = selected,
    }
    return result
end

function resolution.authorize(
    runtimeModel: any,
    cycle: any,
    resolvedBinding: any,
    actionId: any
): (any?, string?)
    local entry = bindingLedger[resolvedBinding]
    if entry == nil
        or entry.cycle ~= cycle
        or entry.model ~= runtimeModel
        or not evidence.is_cycle_live(cycle)
    then
        return nil, "binding_not_actionable"
    end
    if type(actionId) ~= "string" then return nil, "unknown_action" end
    local selected
    for _, action in entry.binding.actions do
        if action.id == actionId then selected = action end
    end
    if selected == nil then return nil, "unknown_action" end

    local predicateKey = model.predicate_key({
        kind = "locator_present",
        locator = selected.proof_locator,
    })
    local proofRecord = entry.evaluation.record_by_predicate[predicateKey]
    local measured = evidence.result_of(proofRecord, cycle)
    local hostProof = evidence.host_proof_of(proofRecord, cycle)
    if measured == nil
        or measured.kind ~= "present"
        or measured.source.kind ~= "template"
        or measured.source.binding ~= entry.binding.id
        or measured.source.locator ~= selected.proof_locator
        or hostProof == nil
    then
        return nil, "proof_not_actionable"
    end

    nextRequestId += 1
    local evidenceIds = {}
    local seenEvidence: { [string]: boolean } = {}
    for _, group in { entry.state.evidence, resolvedBinding.evidence } do
        for _, summary in group do
            if not seenEvidence[summary.id] then
                seenEvidence[summary.id] = true
                table.insert(evidenceIds, summary.id)
            end
        end
    end
    local request = table.freeze({
        kind = "receipt_request",
        id = "receipt-request-" .. tostring(nextRequestId),
        state_identity = resolvedBinding.state_identity,
        surface = resolvedBinding.surface,
        ui_target = resolvedBinding.ui_target,
        binding = resolvedBinding.binding,
        variant = resolvedBinding.variant,
        action = selected.id,
        action_kind = selected.kind,
        -- Each of the four below is present for exactly the kinds that carry
        -- it and absent for the rest, so a request states what its own act
        -- needs and nothing more. The Host is told what to deliver rather than
        -- reading it back out of a model it does not interpret.
        key = selected.key,
        offset = selected.offset,
        duration_ms = selected.duration_ms,
        hold_ms = selected.hold_ms,
        notches = selected.notches,
        evidence_ids = freezeList(evidenceIds),
    })
    requestLedger[request] = {
        cycle = cycle,
        payload = table.freeze({
            request = request,
            placement = entry.binding.placement,
            proof_locator = selected.proof_locator,
            host_proof = hostProof,
        }),
    }
    return request, nil
end

-- One-shot transfer from the pure resolver to observe's private Host call.
function resolution.take_receipt_request(request: any, cycle: any): any?
    local entry = requestLedger[request]
    requestLedger[request] = nil
    if entry == nil or entry.cycle ~= cycle or not evidence.is_cycle_live(cycle) then return nil end
    return entry.payload
end

local function stitchedMapDeclaration(runtimeModel: any, mapId: any): (any?, string?)
    if not model.is_runtime_model(runtimeModel) then return nil, "invalid_runtime_model" end
    if type(mapId) ~= "string" then return nil, "unknown_stitched_map" end
    local declaration = runtimeModel.stitched_map_by_id[mapId]
    if declaration == nil then return nil, "unknown_stitched_map" end
    return declaration, nil
end

local function stitchedItems(collection: any): ({ any }?, string?)
    if type(collection) ~= "table"
        or collection.completeness == "unknown"
        or type(collection.items) ~= "table"
        or collection.count ~= #collection.items
    then
        return nil, "stitched_collection_unresolved"
    end
    local seen: { [number]: boolean } = {}
    for _, item in collection.items do
        if type(item) ~= "table"
            or type(item.index) ~= "number"
            or item.index % 1 ~= 0
            or item.index < 0
            or seen[item.index]
            or type(item.rect) ~= "table"
            or #item.rect ~= 4
        then
            return nil, "stitched_collection_invalid"
        end
        seen[item.index] = true
        for _, coordinate in item.rect do
            if type(coordinate) ~= "number" or coordinate % 1 ~= 0 then
                return nil, "stitched_collection_invalid"
            end
        end
        if item.rect[1] < 0 or item.rect[2] < 0 or item.rect[3] < 1 or item.rect[4] < 1 then
            return nil, "stitched_collection_invalid"
        end
    end
    return collection.items, nil
end

-- Returns every connectivity read the assembled-map evaluator requires. The
-- coordinates belong to the stitched image supplied by the project, so they
-- are deliberately not bounded by RuntimeModel.base_resolution: doing that
-- would silently reintroduce the one-screen page assumption this verb removes.
function resolution.stitched_map_connectivity_targets(
    runtimeModel: any,
    mapId: any,
    collection: any
): ({ any }?, string?)
    local declaration, declarationReason = stitchedMapDeclaration(runtimeModel, mapId)
    if declaration == nil then return nil, declarationReason end
    local items, itemsReason = stitchedItems(collection)
    if items == nil then return nil, itemsReason end
    local targets = {}
    for fromPosition = 1, #items do
        local from = items[fromPosition]
        for toPosition = fromPosition + 1, #items do
            local to = items[toPosition]
            table.insert(targets, table.freeze({
                from = from.index,
                to = to.index,
                start = table.freeze({
                    from.rect[1] + math.floor(from.rect[3] / 2),
                    from.rect[2] + math.floor(from.rect[4] / 2),
                }),
                finish = table.freeze({
                    to.rect[1] + math.floor(to.rect[3] / 2),
                    to.rect[2] + math.floor(to.rect[4] / 2),
                }),
                colour_key = declaration.colour_key,
            }))
        end
    end
    return freezeList(targets), nil
end

function resolution.evaluate_stitched_map(
    runtimeModel: any,
    mapId: any,
    collection: any,
    readings: any
): (any?, string?)
    local declaration, declarationReason = stitchedMapDeclaration(runtimeModel, mapId)
    if declaration == nil then return nil, declarationReason end
    local targets, targetsReason = resolution.stitched_map_connectivity_targets(
        runtimeModel,
        mapId,
        collection
    )
    if targets == nil then return nil, targetsReason end
    if type(readings) ~= "table" or #readings ~= #targets then
        return nil, "connectivity_incomplete"
    end
    local supplied: { [string]: number } = {}
    for _, reading in readings do
        if type(reading) ~= "table"
            or type(reading.from) ~= "number"
            or type(reading.to) ~= "number"
            or type(reading.ratio) ~= "number"
            or reading.ratio < 0
            or reading.ratio > 1
        then
            return nil, "connectivity_invalid"
        end
        local key = tostring(reading.from) .. ":" .. tostring(reading.to)
        if supplied[key] ~= nil then return nil, "connectivity_duplicate" end
        supplied[key] = reading.ratio
    end
    local edges = {}
    local evaluations = {}
    for _, target in targets do
        local key = tostring(target.from) .. ":" .. tostring(target.to)
        local ratio = supplied[key]
        if ratio == nil then return nil, "connectivity_incomplete" end
        local connected = ratio >= declaration.minimum_ratio
        table.insert(evaluations, table.freeze({
            from = target.from,
            to = target.to,
            ratio = ratio,
            connected = connected,
        }))
        if connected then
            table.insert(edges, table.freeze({ from = target.from, to = target.to }))
        end
    end
    return table.freeze({
        kind = "resolved_stitched_map",
        map = declaration.id,
        collection = declaration.collection,
        nodes = collection.items,
        edges = freezeList(edges),
        connectivity = freezeList(evaluations),
    }), nil
end

return table.freeze(resolution)
