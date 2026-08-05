--!strict
-- replay: what one recorded run says about the model it ran against.
--
-- The mirror of `regress`. That one asks whether the SCREENS a model claims say
-- what it claims -- can a page be told apart, does a mark hit only where it
-- belongs -- and this one asks whether the MOVES a run made are moves the model
-- draws. Two evidence libraries and one question each: the screen library makes
-- "recognise" and "permit" falsifiable, the trace library makes "change"
-- falsifiable (docs/plans/2026-08-04-state-layer-and-policy-slots.md 4.2).
--
-- IT COSTS NO CAPTURE AND TAKES NO ctx. Every verb here reads a projection of a
-- stream that was recorded earlier and the model as it is now. That is what lets
-- a run recorded on a real target be checked on a machine that has none.
--
-- WHAT IT DOES NOT CHECK, and why not yet: mode purity needs `page.state` and
-- the preview contract needs a `preview` edge flag, and both are phase B schema
-- (the same plan, 3.4 and 3.5). The two front-end and model-identity refusals
-- are not here either -- they are the caller's, decided before a VM boots, and
-- `replay.check` states what it assumes about them.
--
-- `replay` sorts after `regress` and before `scribe`, so every module this
-- framework has is already bound when it loads. It names `navigation` alone.

local replay = {}

-- One projected line of a recorded run. The kinds are the host's
-- (`trace::ReplayStepKind`), respelled as strings because a chunk cannot name
-- another chunk's enum and the wire has no numbers here.
--
-- `label` is the page for a resolution, the element for an authorisation, the
-- key for a keystroke, and empty for a delivered click -- which carries no name
-- of its own and is read together with the authorisation before it.
export type Step = {
    kind: string,
    seq: number,
    label: string,
}

replay.page_resolved    = "page_resolved"
replay.element_clicked  = "element_clicked"
replay.action_delivered = "action_delivered"
replay.key_delivered    = "key_delivered"

local k_stepKinds: { [string]: boolean } = {
    [replay.page_resolved]    = true,
    [replay.element_clicked]  = true,
    [replay.action_delivered] = true,
    [replay.key_delivered]    = true,
}

-- One move the run made, from the page it had resolved to the page it resolved
-- next, and what the model says about it.
--
-- `trigger` is what the run did in between, spelled as an edge spells it:
-- "click" naming an element, "key" naming a key, or "spontaneous" when the run
-- delivered nothing at all and the frame changed under it.
--
-- `verdict` is one of:
--
--   * "matched"       -- an edge leaves `from` on this trigger and names `to`.
--   * "matched_pop"   -- an edge leaves `from` on this trigger and pops. Where a
--                        pop lands is the run's stack and not the file's to
--                        know, so the destination is not checked and must not be
--                        reported as agreement.
--   * "unattributable" -- a click reached the target and nothing named the
--                        element. It is its own answer and never a finding: the
--                        model is not contradicted by a move nobody can spell.
--                        A run recorded before `framework.element_clicked`
--                        existed reads entirely like this, which is why it is a
--                        row rather than silence.
export type Transition = {
    from: string,
    to: string,
    seq: number,
    trigger: string,
    element: string?,
    key: string?,
    verdict: string,
}

-- What the model got wrong, which here is one thing: the run went somewhere the
-- model draws no way to. `trigger` and its name are carried so the reader knows
-- which edge is missing rather than that one is.
export type Finding = {
    kind: string,
    from: string,
    to: string,
    seq: number,
    trigger: string,
    element: string?,
    key: string?,
    detail: string,
}

-- `accepted` is findings alone, as it is in `regress`: transitions are a
-- measurement, and an unattributable one is a measurement this build cannot
-- sharpen.
export type Verdict = {
    accepted: boolean,
    steps: number,
    resolutions: number,
    transitions: { Transition },
    findings: { Finding },
}

-- The trigger a run performed between two resolutions, folded from the steps in
-- between. Nil until something was delivered.
type Pending = {
    trigger: string,
    element: string?,
    key: string?,
    seq: number,
}

-- Whether `edge` is the one the run took: same trigger, same name, and -- unless
-- it pops -- a declared destination the run actually reached.
local function edgeExplains(edge: any, pending: Pending, arrived: string): boolean
    if edge.via ~= pending.trigger then
        return false
    end
    if edge.via == "click" and edge.via_element.name ~= pending.element then
        return false
    end
    if edge.via == "key" and edge.via_key ~= pending.key then
        return false
    end
    if edge.kind == "pop" then
        return true
    end
    for _, page in edge.to do
        if page.name == arrived then
            return true
        end
    end
    return false
end

-- Whether any edge leaving `from` carries this trigger at all, whatever it says
-- about where it lands. It separates "the model draws no such move" from "the
-- model draws it and it goes somewhere else", which are different mistakes and
-- read the same in a bare refusal.
local function triggerDeclared(edges: { any }, pending: Pending): boolean
    for _, edge in edges do
        if edge.via == pending.trigger then
            if edge.via == "click" then
                if edge.via_element.name == pending.element then
                    return true
                end
            elseif edge.via == "key" then
                if edge.via_key == pending.key then
                    return true
                end
            else
                return true
            end
        end
    end
    return false
end

-- replay.check(built, steps) -> Verdict
--
-- Walks the projection and reports every page transition against the graph.
--
-- WHAT THE CALLER HAS ALREADY DECIDED, because neither is decidable here: that
-- this stream is a task run rather than a measuring one -- a check resolves
-- every page it cares about against one frame, so its resolutions are not a walk
-- -- and that the model on disk is the model the run read. The first is
-- `run.started`'s front end and the second its `modelHash`, and both are
-- compared before a VM boots.
--
-- A transition is only counted between two SUCCESSFUL resolutions. A frame that
-- resolved to nothing writes no line at all, so a run that lost the page for
-- three frames and found it again reads here as one move -- which is what the
-- model claims about it too.
function replay.check(built: any, steps: { Step }): Verdict
    if type(built) ~= "table" or built.graph == nil then
        error(
            "replay.check needs the model project.load_project returned; the "
                .. "edges it judges a run against are the graph on it",
            2
        )
    end
    if type(steps) ~= "table" then
        error(
            "replay.check needs the steps of the run it replays, as a list",
            2
        )
    end

    local verdict: Verdict = {
        accepted    = true,
        steps       = #steps,
        resolutions = 0,
        transitions = {},
        findings    = {},
    }

    local graph   = built.graph
    local current: string? = nil
    local pending: Pending? = nil

    for index = 1, #steps do
        local step = steps[index]
        if type(step) ~= "table" or not k_stepKinds[step.kind] then
            error(
                "replay.check was handed a step "
                    .. tostring(index)
                    .. " that is not one of the four kinds a run projects",
                2
            )
        end

        if step.kind == replay.element_clicked then
            -- Authorised, not yet delivered. It is kept rather than acted on
            -- because a click that was authorised and never landed changes no
            -- page, and the delivery below is what says it did.
            pending = {
                trigger = "click",
                element = step.label,
                seq     = step.seq,
            }
        elseif step.kind == replay.action_delivered then
            if pending == nil or (pending :: Pending).trigger ~= "click" then
                -- A click nothing named: a long press, or a stream older than
                -- the line that names one. Recorded as a trigger with no name,
                -- so the transition it explains reads "unattributable" rather
                -- than "no edge".
                pending = { trigger = "click", seq = step.seq }
            end
        elseif step.kind == replay.key_delivered then
            pending = {
                trigger = "key",
                key     = step.label,
                seq     = step.seq,
            }
        else
            verdict.resolutions += 1
            local arrived = step.label
            if current ~= nil and current ~= arrived then
                local moved = pending
                    or { trigger = "spontaneous", seq = step.seq }
                local standing = navigation.Graph.page_named(graph, current)

                -- Only reachable when the trace and the model disagree about
                -- what pages exist, which the caller's model-hash comparison
                -- already refuses. Reported rather than raised, so a caller that
                -- skipped that comparison learns why its report is empty.
                if standing == nil then
                    table.insert(verdict.findings, {
                        kind    = "unknown_page",
                        from    = current,
                        to      = arrived,
                        seq     = step.seq,
                        trigger = moved.trigger,
                        element = moved.element,
                        key     = moved.key,
                        detail  = "the run stood on a page this model does not "
                            .. "declare, so no edge of its could explain the move",
                    })
                    current = arrived
                    pending = nil
                    continue
                end

                local edges     = navigation.Graph.edges_from(graph, standing)
                local explained = false
                for _, edge in edges do
                    if edgeExplains(edge, moved, arrived) then
                        explained = true
                        table.insert(verdict.transitions, {
                            from    = current,
                            to      = arrived,
                            seq     = step.seq,
                            trigger = moved.trigger,
                            element = moved.element,
                            key     = moved.key,
                            verdict = if edge.kind == "pop"
                                then "matched_pop"
                                else "matched",
                        })
                        break
                    end
                end

                if not explained then
                    if moved.trigger == "click" and moved.element == nil then
                        table.insert(verdict.transitions, {
                            from    = current,
                            to      = arrived,
                            seq     = step.seq,
                            trigger = "click",
                            verdict = "unattributable",
                        })
                    else
                        table.insert(verdict.findings, {
                            kind    = "no_edge",
                            from    = current,
                            to      = arrived,
                            seq     = step.seq,
                            trigger = moved.trigger,
                            element = moved.element,
                            key     = moved.key,
                            detail  = if triggerDeclared(edges, moved)
                                then "this trigger leaves the page in the model "
                                    .. "and lands somewhere else"
                                else "the model draws no way out of this page "
                                    .. "on this trigger",
                        })
                    end
                end
            end
            current = arrived
            pending = nil
        end
    end

    verdict.accepted = #verdict.findings == 0
    return verdict
end

return replay
