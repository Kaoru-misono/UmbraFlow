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
-- WHAT IT DOES NOT CHECK, all three named so none is quietly missing:
--
--   * Mode purity (the plan's 3.4) and the preview contract (3.5) need
--     `page.state` and the `preview` edge flag to be CONSUMED by a routine; the
--     fields exist and nothing reads them yet.
--   * Whether a delivery had the effect the run expected (5.2). That needs the
--     four-beat step it belongs to, which is phase C, and no line of the stream
--     records an expectation today.
--
-- The front-end and model-identity refusals are not here either -- they are the
-- caller's, decided before a VM boots, and `replay.check` states what it assumes
-- about them.
--
-- `replay` sorts after `regress` and before `scribe`. It names `jsonl` and
-- `navigation`, and nothing that costs a capture.

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
--   * "unattributable" -- an input reached the target and nothing named what it
--                        landed on. It is its own answer and never a finding:
--                        the model is not contradicted by a move nobody can
--                        spell, and counting it as a missing edge would fill a
--                        report with edges that most likely exist. A bare
--                        coordinate press, a long press and a scroll all read
--                        this way, because none of them names an element and the
--                        edge schema can spell no trigger but a click or a key.
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

-- The trigger a run performed since the page last CHANGED, folded from the steps
-- in between. Nil until something was delivered.
--
-- `delivered` is the half an authorisation alone does not have. `observe.click`
-- names the element before the input is posted, so a click that was authorised
-- and then failed to reach the target leaves this false -- and a move that
-- follows one of those was not caused by it. Without the distinction a lease
-- that expired between the two lines would have the next page blamed on an
-- element nothing ever pressed.
type Pending = {
    trigger: string,
    element: string?,
    key: string?,
    seq: number,
    delivered: boolean,
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
    -- A preview declares that walking it changes no page, so it explains no
    -- transition at all -- and the page DID change here or this function would
    -- not have been called. It carries no `to` to compare, and iterating one
    -- crashed the checker at the exact moment it owed a finding.
    if edge.preview then
        return false
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
        -- A preview is not a way out of this page, so it cannot be the "leaves
        -- the page and lands elsewhere" half of the sentence either.
        if edge.via == pending.trigger and not edge.preview then
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
            -- Authorised, not yet delivered. The delivery below is what says it
            -- landed, and until then this explains nothing.
            pending = {
                trigger   = "click",
                element   = step.label,
                seq       = step.seq,
                delivered = false,
            }
        elseif step.kind == replay.action_delivered then
            if pending ~= nil and (pending :: Pending).trigger == "click" then
                pending = {
                    trigger   = "click",
                    element   = (pending :: Pending).element,
                    seq       = (pending :: Pending).seq,
                    delivered = true,
                }
            else
                -- An input that reached the target and named nothing: the bare
                -- coordinate `observe.wake` presses, a long press, a scroll.
                -- Recorded as a trigger with no name, so the transition it
                -- explains reads "unattributable" rather than "no edge".
                pending = {
                    trigger   = "click",
                    seq       = step.seq,
                    delivered = true,
                }
            end
        elseif step.kind == replay.key_delivered then
            pending = {
                trigger   = "key",
                key       = step.label,
                seq       = step.seq,
                delivered = true,
            }
        else
            verdict.resolutions += 1
            local arrived = step.label
            if current ~= nil and current ~= arrived then
                -- A pending that was authorised and never delivered explains
                -- nothing: the page moved for some other reason, and blaming it
                -- on an element nothing pressed would name the wrong edge.
                local moved: any = if pending ~= nil
                        and (pending :: Pending).delivered
                    then pending
                    else { trigger = "spontaneous", seq = step.seq }
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
            -- Cleared only when the page CHANGED. A run that resolves every
            -- cycle -- which a hand-written dispatcher does -- reports the page
            -- it is standing on again and again while an animation finishes, and
            -- clearing here would throw away the click that started the move and
            -- report the move that followed as spontaneous. The transition above
            -- is the only thing a trigger explains, so it is the only thing that
            -- may consume one.
            if current ~= arrived then
                pending = nil
            end
            current = arrived
        end
    end

    verdict.accepted = #verdict.findings == 0
    return verdict
end

-- ----------------------------------------------------------------- report

local function summaryLine(verdict: Verdict): string
    local matched        = 0
    local matchedPop     = 0
    local unattributable = 0
    for _, transition in verdict.transitions do
        if transition.verdict == "unattributable" then
            unattributable += 1
        elseif transition.verdict == "matched_pop" then
            matchedPop += 1
        else
            matched += 1
        end
    end

    return jsonl.object({
        { "replay", "summary" },
        { "accepted", verdict.accepted },
        { "steps", verdict.steps },
        { "resolutions", verdict.resolutions },
        { "transitions", #verdict.transitions },
        { "matched", matched },
        -- Counted apart from the matched ones, because a pop is matched on its
        -- trigger alone: where it lands is the run's stack and nothing here
        -- checked it. A reader adding the two together is claiming a check that
        -- did not happen.
        { "matched_pop", matchedPop },
        -- Counted apart from the matched ones and never among the findings: a
        -- move nobody can spell is not a move the model got wrong, and a reader
        -- watching this number fall is watching evidence arrive rather than
        -- defects being fixed.
        { "unattributable", unattributable },
        { "findings", #verdict.findings },
    })
end

local function transitionLine(transition: Transition): string
    return jsonl.object({
        { "replay", "transition" },
        { "from", transition.from },
        { "to", transition.to },
        { "seq", transition.seq },
        { "trigger", transition.trigger },
        { "element", transition.element },
        { "key", transition.key },
        { "verdict", transition.verdict },
    })
end

local function findingLine(finding: Finding): string
    return jsonl.object({
        { "replay", "finding" },
        { "kind", finding.kind },
        { "from", finding.from },
        { "to", finding.to },
        { "seq", finding.seq },
        { "trigger", finding.trigger },
        { "element", finding.element },
        { "key", finding.key },
        { "detail", finding.detail },
    })
end

export type Group = {
    rows: { any },
    line: (any) -> string,
}

-- replay.groups(verdict) -> the report in blocks, in report order.
--
-- Blocks rather than lines for `regress.groups`'s reason: a caller prints each
-- line as it is minted so the whole report never exists twice inside the VM
-- (docs/pitfalls/embedded-vm-memory-ceiling.md). A replay's report is far
-- smaller than a matrix's -- one row per page change rather than one per cell --
-- but the shape is shared so a caller does not have to know which is which.
--
-- The summary comes first so a reader piping this into a pager sees the answer
-- before the evidence, and the findings before the transitions that surround
-- them.
function replay.groups(verdict: Verdict): { Group }
    return {
        { rows = { verdict }, line = summaryLine :: any },
        { rows = verdict.findings, line = findingLine :: any },
        { rows = verdict.transitions, line = transitionLine :: any },
    }
end

-- replay.render(verdict) -> the report as one string, newline separated and with
-- no trailing newline.
function replay.render(verdict: Verdict): string
    local lines: { string } = {}
    for _, group in replay.groups(verdict) do
        for _, row in group.rows do
            table.insert(lines, group.line(row))
        end
    end
    return table.concat(lines, "\n")
end

return replay
