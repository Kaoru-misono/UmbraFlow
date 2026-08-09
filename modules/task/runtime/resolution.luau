--!strict
-- resolution: the v1 runtime resolver. It is deliberately independent of the
-- model constructors while P1 is being implemented: the resolver consumes the
-- frozen runtime-model shapes and a trusted measurement seam. That seam is what
-- the eventual model/observation adapter supplies; it is not exposed to project
-- code.
--
-- The resolver never uses declaration order as a decision. It evaluates every
-- relevant candidate from the supplied observation cycle, keeps present/absent/
-- unknown distinct, builds the visible surface stack, and refuses to turn a tie
-- or an unknown higher layer into an actionable result.

local resolution = {}

local freeze = table.freeze
if freeze == nil then
    -- This branch is only useful to the small host-independent test harness. The
    -- real Luau VM always supplies table.freeze.
    freeze = function(value)
        return value
    end
end

local evidenceReasons = {
    not_measured = true,
    low_confidence = true,
    ocr_unreadable = true,
    locator_failed = true,
    incompatible_geometry = true,
    stale_frame = true,
    internal_error = true,
}

-- A project can copy every field of a Resolved result.  Keep the authorization
-- gate on the identity of the framework result and the cycle that produced it.
local resolvedLedger: { [any]: any } = setmetatable({}, { __mode = "k" }) :: any

local resolutionReasons = {
    no_surface_candidate = true,
    insufficient_identity = true,
    unknown_evidence = true,
    incompatible_context = true,
    invalid_stack = true,
}

local function copyList(values)
    local copied = {}
    if type(values) == "table" then
        for index = 1, #values do
            copied[index] = values[index]
        end
    end
    return copied
end

local function contains(values, wanted)
    if type(values) ~= "table" then
        return false
    end
    for index = 1, #values do
        if values[index] == wanted then
            return true
        end
    end
    return false
end

local function appendUnique(destination, values)
    for index = 1, #values do
        local value = values[index]
        local already = false
        for existing = 1, #destination do
            if destination[existing].id == value.id then
                already = true
                break
            end
        end
        if not already then
            destination[#destination + 1] = value
        end
    end
end

local function clampConfidence(value)
    if type(value) ~= "number" then
        return 1
    end
    if value < 0 then
        return 0
    end
    if value > 1 then
        return 1
    end
    return value
end

local function defaultProof(target)
    return {
        {
            kind = "geometry",
            target = target,
        },
    }
end

local function normalizeResult(value, target)
    if value == "present" then
        return {
            kind = "present",
            value = true,
            confidence = 1,
            proof = defaultProof(target),
        }
    end
    if value == "absent" then
        return {
            kind = "absent",
            proof = defaultProof(target),
        }
    end
    if value == "unknown" or value == nil then
        return {
            kind = "unknown",
            reason = "not_measured",
        }
    end
    if type(value) ~= "table" then
        return {
            kind = "unknown",
            reason = "internal_error",
        }
    end

    local kind = value.kind
    if kind == "present" then
        return {
            kind = "present",
            value = value.value,
            confidence = clampConfidence(value.confidence),
            proof = copyList(value.proof or defaultProof(target)),
        }
    end
    if kind == "absent" then
        return {
            kind = "absent",
            proof = copyList(value.proof or defaultProof(target)),
        }
    end
    if kind == "unknown" then
        local reason = value.reason
        if not evidenceReasons[reason] then
            reason = "internal_error"
        end
        return {
            kind = "unknown",
            reason = reason,
            confidence = value.confidence,
            proof = copyList(value.proof or {}),
        }
    end
    return {
        kind = "unknown",
        reason = "internal_error",
    }
end

local function evidenceRecord(id, subject, value, cycle)
    if cycle ~= nil and evidence ~= nil and evidence.mint_evidence ~= nil then
        return evidence.mint_evidence(subject, value, cycle)
    end
    return freeze({
        id = id,
        subject = subject,
        result = freeze(value),
    })
end

local function predicateId(surface, binding, role, index)
    return surface.id .. "." .. binding.id .. "." .. role .. "." .. tostring(index)
end

local function textValue(value)
    if type(value) == "string" then
        return value
    end
    if type(value) == "table" and type(value.text) == "string" then
        return value.text
    end
    return nil
end

local function textPredicateMatches(predicate, result)
    if predicate.kind ~= "text_match" or result.kind ~= "present" then
        return result
    end
    local actual = textValue(result.value)
    if actual == nil then
        return {
            kind = "unknown",
            reason = "ocr_unreadable",
            confidence = result.confidence,
            proof = result.proof,
        }
    end
    local operator = predicate.operator
    local matched = false
    if operator == "present" then
        matched = actual ~= ""
    elseif operator == "equals" then
        matched = actual == predicate.value
    elseif operator == "contains_all" then
        matched = true
        for index = 1, #(predicate.value or {}) do
            if string.find(actual, predicate.value[index], 1, true) == nil then
                matched = false
                break
            end
        end
    else
        return {
            kind = "unknown",
            reason = "internal_error",
            confidence = result.confidence,
            proof = result.proof,
        }
    end
    if matched then
        return result
    end
    return {
        kind = "absent",
        proof = result.proof,
    }
end

local function evaluatePredicate(evaluator, predicate, binding, surface, role, index, cycle)
    if type(evaluator) ~= "function" then
        local unknown = normalizeResult(nil, binding.target)
        return evidenceRecord(
            predicateId(surface, binding, role, index),
            binding.target,
            unknown,
            cycle
        ), nil
    end

    local value, delivery = evaluator(predicate, binding, surface, role, index)
    local result = textPredicateMatches(
        predicate,
        normalizeResult(value, binding.target)
    )
    local record = evidenceRecord(
        predicateId(surface, binding, role, index),
        binding.target,
        result,
        cycle
    )
    return record, delivery
end

local function scoreEvidence(records)
    local total = 0
    local positive = 0
    for index = 1, #records do
        local result = records[index].result
        if result.kind == "present" then
            total += result.confidence
            positive += 1
        end
    end
    if positive == 0 then
        return 0
    end
    return total / positive
end

local function identityPredicates(binding)
    local identity = binding.identity or {}
    local all = identity.all or {}
    local any = identity.any or {}
    local none = identity.none or {}
    -- A `none` predicate is a guard, not positive identity evidence.  A
    -- binding whose only claim is "this marker is absent" cannot establish
    -- which Surface is visible and is therefore insufficient by itself.
    return all, any, none, #all + #any > 0
end

local function evaluateIdentity(evaluator, surface, binding, cycle)
    local all, any, none, hasPredicates = identityPredicates(binding)
    if not hasPredicates then
        return {
            status = "none",
            score = 0,
            evidence = {},
            deliveries = {},
        }
    end

    local records = {}
    local deliveries = {}
    local unknown = false
    local failed = false
    local positiveRecords = {}

    local function inspectList(list, role, requireAny)
        local presentCount = 0
        local unknownCount = 0
        for index = 1, #list do
            local record, delivery = evaluatePredicate(
                evaluator,
                list[index],
                binding,
                surface,
                role,
                index,
                cycle
            )
            records[#records + 1] = record
            deliveries[record.id] = delivery
            if record.result.kind == "present" then
                presentCount += 1
                positiveRecords[#positiveRecords + 1] = record
            elseif record.result.kind == "unknown" then
                unknownCount += 1
            end
        end

        if requireAny and #list > 0 then
            if presentCount == 0 then
                if unknownCount > 0 then
                    unknown = true
                else
                    failed = true
                end
            end
        elseif not requireAny then
            for index = 1, #list do
                local kind = records[#records - #list + index].result.kind
                if role == "none" then
                    if kind == "present" then
                        failed = true
                    elseif kind == "unknown" then
                        unknown = true
                    end
                elseif kind == "absent" then
                    failed = true
                elseif kind == "unknown" then
                    unknown = true
                end
            end
        end
    end

    inspectList(all, "all", false)
    inspectList(any, "any", true)
    inspectList(none, "none", false)

    local status = "matched"
    if failed then
        status = "absent"
    elseif unknown then
        status = "unknown"
    end

    return {
        status = status,
        score = scoreEvidence(positiveRecords),
        evidence = records,
        deliveries = deliveries,
    }
end

local function contextAllows(surface, context)
    local contexts = surface.contexts or {}
    if surface.kind == "interrupt" and #contexts == 0 then
        return true
    end
    if context == nil then
        return true
    end
    return contains(contexts, context)
end

local function candidateState(context, surfaces)
    return {
        context = context,
        surfaces = copyList(surfaces),
    }
end

local function candidateValue(surface, context, evidence, score, binding)
    return {
        surface = surface,
        score = score,
        evidence = evidence,
        binding = binding,
        state = candidateState(context, { surface.id }),
    }
end

local function evaluateSurface(evaluator, surface, model, cycle)
    local matched = {}
    local unknown = {}
    local identityCount = 0
    local records = {}
    local bindings = model.bindings or {}
    for index = 1, #bindings do
        local binding = bindings[index]
        if binding.surface == surface.id then
            local identity = evaluateIdentity(evaluator, surface, binding, cycle)
            appendUnique(records, identity.evidence)
            if identity.status ~= "none" then
                identityCount += 1
            end
            if identity.status == "matched" then
                matched[#matched + 1] = {
                    binding = binding,
                    score = identity.score,
                    evidence = identity.evidence,
                    deliveries = identity.deliveries,
                }
            elseif identity.status == "unknown" then
                unknown[#unknown + 1] = identity
            end
        end
    end

    if #matched == 0 then
        if #unknown > 0 then
            return {
                status = "unknown",
                evidence = records,
            }
        end
        return {
            status = if identityCount == 0 then "insufficient" else "absent",
            evidence = records,
        }
    end

    local best = matched[1]
    for index = 2, #matched do
        if matched[index].score > best.score then
            best = matched[index]
        end
    end
    return {
        status = "matched",
        score = best.score,
        evidence = records,
        binding = best.binding,
        deliveries = best.deliveries,
    }
end

local function surfaceIds(surfaces)
    local ids = {}
    for index = 1, #(surfaces or {}) do
        local value = surfaces[index]
        ids[index] = if type(value) == "table" then value.id else value
    end
    return ids
end

local function candidateResult(candidate, context, surfaces, allEvidence)
    local state = candidateState(context, surfaceIds(surfaces))
    if state.surfaces[#state.surfaces] ~= candidate.surface.id then
        state.surfaces[#state.surfaces + 1] = candidate.surface.id
    end
    return {
        surface = candidate.surface,
        score = candidate.score,
        state = state,
        evidence = candidate.evidence,
        allEvidence = allEvidence,
        binding = candidate.binding,
    }
end

local function ambiguous(candidates, conflicts, records)
    local publicCandidates = {}
    for index = 1, #candidates do
        local evidenceIds = {}
        for evidenceIndex = 1, #candidates[index].evidence do
            evidenceIds[evidenceIndex] = candidates[index].evidence[evidenceIndex].id
        end
        publicCandidates[index] = freeze({
            state = freeze({
                context = candidates[index].state.context,
                surfaces = freeze(copyList(candidates[index].state.surfaces)),
            }),
            score = candidates[index].score,
            evidence = freeze(evidenceIds),
        })
    end
    return freeze({
        kind = "ambiguous",
        candidates = freeze(publicCandidates),
        conflicts = freeze(copyList(conflicts)),
        evidence = freeze(copyList(records)),
    })
end

local function unknown(reason, records, diagnostic)
    if not resolutionReasons[reason] then
        reason = "internal_error"
    end
    local value = {
        kind = "unknown",
        reason = reason,
        evidence = freeze(copyList(records)),
    }
    if diagnostic ~= nil then
        value.diagnostic = diagnostic
    end
    return freeze(value)
end

local function choose(candidates, context, surfaces, options, records)
    if #candidates == 0 then
        return nil, nil
    end
    local bestScore = candidates[1].score
    for index = 2, #candidates do
        if candidates[index].score > bestScore then
            bestScore = candidates[index].score
        end
    end

    local margin = options.minimum_margin or 0
    local tied = {}
    for index = 1, #candidates do
        if bestScore - candidates[index].score <= margin then
            tied[#tied + 1] = candidateResult(
                candidates[index],
                context,
                surfaces,
                records
            )
        end
    end
    if #tied > 1 then
        return ambiguous(tied, {
            freeze({
                kind = "multiple_candidates",
                surfaces = (function()
                    local ids = {}
                    for index = 1, #tied do
                ids[index] = tied[index].surface.id
                    end
                    return freeze(ids)
                end)(),
                message = "multiple equally supported surfaces remain",
            }),
        }, records), nil
    end

    local selected = tied[1]
    return selected, nil
end

local function matchingCandidates(evaluator, surfaces, model, context, kind, records, cycle)
    local matched = {}
    local unknownSurfaces = {}
    local insufficientSurfaces = {}
    for index = 1, #surfaces do
        local surface = surfaces[index]
        if surface.kind == kind and contextAllows(surface, context) then
            local measured = evaluateSurface(evaluator, surface, model, cycle)
            appendUnique(records, measured.evidence)
            if measured.status == "matched" then
                matched[#matched + 1] = candidateValue(
                    surface,
                    context,
                    measured.evidence,
                    measured.score,
                    measured.binding
                )
            elseif measured.status == "unknown" then
                unknownSurfaces[#unknownSurfaces + 1] = surface.id
            elseif measured.status == "insufficient" then
                insufficientSurfaces[#insufficientSurfaces + 1] = surface.id
            end
        end
    end
    return matched, unknownSurfaces, insufficientSurfaces
end

local function inferContext(candidate, options)
    if options.context ~= nil then
        return options.context
    end
    local contexts = candidate.surface.contexts or {}
    if #contexts == 1 then
        return contexts[1]
    end
    return nil
end

local function surfaceById(model, id)
    for index = 1, #(model.surfaces or {}) do
        if model.surfaces[index].id == id then
            return model.surfaces[index]
        end
    end
    return nil
end

local function compatibleOverlay(surface, top)
    return surface.kind == "overlay"
        and contains(surface.covers or {}, top)
end

-- resolve(model, measure, options) -> Resolved | Ambiguous | UnknownResolution
--
-- `measure(predicate, binding, surface, role, index)` must return a v1 evidence
-- result and may return a private delivery payload as its second result. The
-- payload is deliberately not copied into runtime evidence; observe uses it
-- only when minting an action receipt for the same cycle.
function resolution.resolve(model, measure, options)
    options = options or {}
    if type(model) ~= "table" or type(model.surfaces) ~= "table" then
        return unknown(
            "invalid_stack",
            {},
            "resolution requires a runtime model with surfaces"
        )
    end

    local evaluator = measure
    local cycle = nil
    if type(measure) == "table" and type(measure.probe) == "function" then
        cycle = measure
        options.cycle_token = options.cycle_token or cycle.cycle_token
        evaluator = function(predicate, binding, surface, role, index)
            local query = table.clone(predicate)
            query.binding = binding
            query.surface = surface
            local value = cycle:probe(binding.target, query, surface, role, index)
            if type(value) == "table"
                and value.kind == "present"
                and type(value.value) == "table"
            then
                return value, value.value
            end
            return value
        end
    end

    local records = {}
    local context = options.context
    local sceneCandidates, unknownScenes, insufficientScenes = matchingCandidates(
        evaluator,
        model.surfaces,
        model,
        context,
        "scene",
        records,
        options.cycle_token
    )
    if #sceneCandidates == 0 then
        if #unknownScenes > 0 then
            return unknown(
                "unknown_evidence",
                records,
                "required scene identity evidence is unknown for: "
                    .. table.concat(unknownScenes, ", ")
            )
        end
        if #insufficientScenes > 0 then
            return unknown(
                "insufficient_identity",
                records,
                "known scene surfaces have no positive identity evidence: "
                    .. table.concat(insufficientScenes, ", ")
            )
        end
        return unknown(
            "no_surface_candidate",
            records,
            "no known scene surface matches; unmatched visible content may exist"
        )
    end

    local inferred = inferContext(sceneCandidates[1], options)
    if context == nil then
        context = inferred
        if context == nil then
            return unknown(
                "incompatible_context",
                records,
                "the matching scene does not identify one runtime context"
            )
        end
        for index = 1, #sceneCandidates do
            if inferContext(sceneCandidates[index], options) ~= context then
                return unknown(
                    "incompatible_context",
                    records,
                    "matching scene candidates belong to different contexts"
                )
            end
        end
    end

    local sceneChoice = choose(sceneCandidates, context, {}, options, records)
    if sceneChoice ~= nil and sceneChoice.kind == "ambiguous" then
        return sceneChoice
    end
    if sceneChoice == nil then
        return unknown(
            "no_surface_candidate",
            records,
            "no scene surface could be selected"
        )
    end

    local stack = { sceneChoice.surface }
    local selectedEvidence = {}
    appendUnique(selectedEvidence, sceneChoice.evidence)
    local overlays, unknownOverlays, insufficientOverlays
    local guard = 0
    while true do
        guard += 1
        if guard > #model.surfaces + 1 then
            return unknown(
                "invalid_stack",
                records,
                "overlay covers relations do not form a finite stack"
            )
        end

        local top = stack[#stack]
        local eligible = {}
        local unknownEligible = {}
        overlays, unknownOverlays, insufficientOverlays = matchingCandidates(
            evaluator,
            model.surfaces,
            model,
            context,
            "overlay",
            records,
            options.cycle_token
        )
        for index = 1, #overlays do
            if compatibleOverlay(overlays[index].surface, top.id) then
                eligible[#eligible + 1] = overlays[index]
            end
        end
        for index = 1, #unknownOverlays do
            local surface = surfaceById(model, unknownOverlays[index])
            if surface ~= nil and compatibleOverlay(surface, top.id) then
                unknownEligible[#unknownEligible + 1] = surface.id
            end
        end
        for index = 1, #insufficientOverlays do
            local surface = surfaceById(model, insufficientOverlays[index])
            if surface ~= nil and compatibleOverlay(surface, top.id) then
                unknownEligible[#unknownEligible + 1] = surface.id
            end
        end

        if #eligible == 0 then
            if #unknownEligible > 0 then
                return unknown(
                    "unknown_evidence",
                    records,
                    "a compatible overlay has unknown identity evidence: "
                        .. table.concat(unknownEligible, ", ")
                )
            end
            break
        end

        local choice = choose(eligible, context, stack, options, records)
        if choice ~= nil and choice.kind == "ambiguous" then
            return choice
        end
        if choice == nil then
            return unknown(
                "invalid_stack",
                records,
                "a compatible overlay could not be selected"
            )
        end
        stack[#stack + 1] = choice.surface
        appendUnique(selectedEvidence, choice.evidence)
    end

    local interrupts, unknownInterrupts, insufficientInterrupts = matchingCandidates(
        evaluator,
        model.surfaces,
        model,
        context,
        "interrupt",
        records,
        options.cycle_token
    )
    if #unknownInterrupts > 0 or #insufficientInterrupts > 0 then
        local names = copyList(unknownInterrupts)
        for index = 1, #insufficientInterrupts do
            names[#names + 1] = insufficientInterrupts[index]
        end
        return unknown(
            if #unknownInterrupts > 0 then "unknown_evidence" else "insufficient_identity",
            records,
            "an applicable interrupt has unknown identity evidence: "
                .. table.concat(names, ", ")
        )
    end
    if #interrupts > 0 then
        local choice = choose(interrupts, context, stack, options, records)
        if choice ~= nil and choice.kind == "ambiguous" then
            return choice
        end
        if choice == nil then
            return unknown(
                "invalid_stack",
                records,
                "an interrupt could not be selected"
            )
        end
        stack[#stack + 1] = choice.surface
        appendUnique(selectedEvidence, choice.evidence)
    end

    if stack[1].kind ~= "scene" then
        return unknown(
            "invalid_stack",
            records,
            "a runtime surface stack must start with a scene"
        )
    end

    local state = freeze({
        context = context,
        surfaces = freeze((function()
            local ids = {}
            for index = 1, #stack do
                ids[index] = stack[index].id
            end
            return ids
        end)()),
    })
    local resolved = freeze({
        kind = "resolved",
        state = state,
        evidence = freeze(copyList(selectedEvidence)),
    })
    if options.cycle_token ~= nil then
        resolvedLedger[resolved] = options.cycle_token
    end
    return resolved
end

local function actionRequest(request)
    request = request or {}
    local nested = request.action
    if type(nested) == "table" then
        return request.target or nested.target, nested.id, nested.kind
    end
    return request.target, request.action_id or nested, request.kind
end

local function matchingBinding(model, surface, target)
    local found = {}
    for index = 1, #(model.bindings or {}) do
        local binding = model.bindings[index]
        if binding.surface == surface and binding.target == target then
            found[#found + 1] = binding
        end
    end
    return found
end

local function matchingAction(binding, id, kind)
    local found = {}
    for index = 1, #(binding.actions or {}) do
        local action = binding.actions[index]
        if (id ~= nil and action.id == id) or (id == nil and kind ~= nil and action.kind == kind) then
            found[#found + 1] = action
        end
    end
    return found
end

-- authorize(model, resolved, request, measure) -> authorization result
--
-- This function does not mint a receipt. It proves the requested action in the
-- same cycle as resolution; observe/evidence owns the private mint and binds the
-- proof to ticket, cycle, frame and model hash.
function resolution.authorize(model, resolved, request, measure, cycle)
    local observationCycle = nil
    if type(resolved) == "table" and type(resolved.probe) == "function" then
        observationCycle = resolved
        local actualResolved = request
        local actualRequest = measure
        resolved = actualResolved
        request = actualRequest
        cycle = observationCycle.cycle_token
        measure = function(predicate, binding, surface, role, index)
            local query = table.clone(predicate)
            query.binding = binding
            query.surface = surface
            local value = observationCycle:probe(
                binding.target,
                query,
                surface,
                role,
                index
            )
            if type(value) == "table"
                and value.kind == "present"
                and type(value.value) == "table"
            then
                return value, value.value
            end
            return value
        end
    end

    local function deny(reason, details)
        if observationCycle ~= nil then
            return nil, reason
        end
        local result = details or {}
        result.authorized = false
        result.reason = reason
        return result
    end

    if type(resolved) ~= "table" or resolved.kind ~= "resolved" then
        return deny("resolution_not_actionable")
    end
    if cycle ~= nil and not rawequal(resolvedLedger[resolved], cycle) then
        return deny("resolution_not_framework_minted")
    end
    local target, actionId, actionKind = actionRequest(request)
    local state = resolved.state
    local surfaces = state and state.surfaces or {}
    local surface = surfaces[#surfaces]
    if target == nil or actionKind == nil and actionId == nil then
        return deny("action_request_incomplete")
    end

    local bindings = matchingBinding(model, surface, target)
    if #bindings == 0 then
        return deny("target_not_bound_to_active_surface")
    end
    if #bindings > 1 and actionId == nil then
        return deny("multiple_bindings_for_active_target")
    end

    local binding
    local action
    for index = 1, #bindings do
        local actions = matchingAction(bindings[index], actionId, actionKind)
        if #actions == 1 then
            if binding ~= nil then
                return deny("multiple_actions_for_active_target")
            end
            binding = bindings[index]
            action = actions[1]
        elseif #actions > 1 then
            return deny("multiple_actions_for_active_binding")
        end
    end
    if binding == nil or action == nil then
        return deny("action_not_declared_by_active_binding")
    end

    local records = {}
    local deliveries = {}
    local function check(predicate, role, index)
        local record, delivery = evaluatePredicate(
            measure,
            predicate,
            binding,
            { id = surface },
            role,
            index,
            cycle
        )
        records[#records + 1] = record
        deliveries[record.id] = delivery
        if record.result.kind ~= "present" then
            return false, record.result.kind
        end
        return true, nil
    end

    local checks = {}
    if action.locator ~= nil then
        checks[#checks + 1] = {
            kind = "locator_present",
            locator = action.locator,
            binding = binding,
        }
    end
    for index = 1, #(action.preconditions or {}) do
        local precondition = table.clone(action.preconditions[index])
        precondition.binding = binding
        checks[#checks + 1] = precondition
    end
    for index = 1, #checks do
        local ok, resultKind = check(checks[index], "action", index)
        if not ok then
            return deny(
                if resultKind == "unknown"
                    then "action_evidence_unknown"
                    else "action_precondition_absent",
                {
                    binding = binding,
                    action = action,
                    evidence = records,
                }
            )
        end
    end

    local authorization = {
        authorized = true,
        target = target,
        binding = binding,
        action = action,
        evidence = records,
        deliveries = deliveries,
    }

    if observationCycle == nil then
        return authorization
    end

    local allEvidence = {}
    local evidenceSeen = {}
    local function retain(recordsToAdd)
        for index = 1, #(recordsToAdd or {}) do
            local record = recordsToAdd[index]
            local retained = record
            if not evidenceSeen[record.id] then
                if not evidence.is_evidence_for(record, cycle) then
                    retained = evidence.mint_evidence(
                        record.subject,
                        record.result,
                        cycle
                    )
                end
                evidenceSeen[retained.id] = true
                allEvidence[#allEvidence + 1] = retained
            end
        end
    end
    retain(resolved.evidence)
    retain(authorization.evidence)

    local surfaceIds = resolved.state.surfaces
    local surface = surfaceIds[#surfaceIds]
    local delivery = nil
    -- The action locator is the first check for every positional action.  Do
    -- not iterate the delivery map: map iteration order is not a proof that a
    -- precondition's geometry belongs to the action target.
    if authorization.evidence[1] ~= nil then
        delivery = authorization.deliveries[authorization.evidence[1].id]
    end

    local receiptAuthorization = {
        kind = action.kind,
        ticket_id = observationCycle.ticket_id,
        cycle_id = observationCycle.cycle_id,
        target = authorization.target,
        action = action.id,
    }
    if type(delivery) == "table" then
        receiptAuthorization.match = delivery.match
        receiptAuthorization.click_point = delivery.click_point
    end

    local receipt = evidence.mint_receipt(
        {
            ticket_id = observationCycle.ticket_id,
            cycle_id = observationCycle.cycle_id,
            frame_id = observationCycle.frame_id,
            model_hash = observationCycle.model_hash,
            state = resolved.state,
            surface = surface,
            binding = binding.id,
            action = action.id,
            evidence_ids = (function()
                local ids = {}
                for index = 1, #allEvidence do
                    ids[index] = allEvidence[index].id
                end
                return ids
            end)(),
        },
        cycle,
        receiptAuthorization
    )
    return receipt, nil
end

resolution.present = function(value, confidence, proof)
    return normalizeResult({
        kind = "present",
        value = value,
        confidence = confidence,
        proof = proof,
    }, "unknown")
end

resolution.absent = function(proof)
    return normalizeResult({ kind = "absent", proof = proof }, "unknown")
end

resolution.unknown = function(reason, confidence, proof)
    return normalizeResult({
        kind = "unknown",
        reason = reason,
        confidence = confidence,
        proof = proof,
    }, "unknown")
end

return resolution
