--!strict
-- navigation: where a page leads, and what a run believes it is standing on.
-- `model` says what a page IS; this file says how pages RELATE -- the edges, the
-- collection that checks them against each other, and the stack a run keeps of
-- the pages an overlay is covering.
--
-- WHERE AN EDGE LEADS IS A FACT AND TAKING IT IS A STRATEGY, which is why edges
-- are data in the project file (docs/plans/2026-07-31-script-owned-page-model.md
-- 10, the 2026-08-01 ruling). Nothing here does pathfinding and no verb picks an
-- edge: layer three picks, and this file says what the choices are.
--
-- THE STACK IS BELIEF AND CARRIES NO GUARANTEE. A click is authorised by the
-- page's own reference row in `observe.click`, and the ticket, lease and
-- fingerprint are checked in C++ (the same document, 5), so a script that
-- corrupts a stack breaks its own run and cannot produce a click at a wrong
-- pixel. That is why the stack is the one mutable thing this framework hands out.
--
-- `observe.walk_edge` walks one of these edges and lives in `observe` because it
-- costs captures. This file is the nouns and the belief rules.
--
-- `navigation` sorts after `mint` and `model` and before `observe` and
-- `project`, which is the order the dependencies run in.

-- The three ways a screen changes, which is the whole of what a trigger can be.
local k_viaKnown: { [string]: boolean } = {
    click       = true,
    key         = true,
    spontaneous = true,
}

-- What the run should BELIEVE afterwards. Walking to another screen, covering
-- this one and uncovering it are three different answers, and no pixel says
-- which happened, so it is authored data rather than something a verb infers.
local k_edgeKindKnown: { [string]: boolean } = {
    navigate = true,
    push     = true,
    pop      = true,
}

local k_edgeKeys: { [string]: boolean } = {
    from        = true,
    to          = true,
    via         = true,
    via_element = true,
    via_key     = true,
    kind        = true,
    extra       = true,
    residual    = true,
}

local k_graphKeys: { [string]: boolean } = {
    pages = true,
    edges = true,
}

local k_stackKeys: { [string]: boolean } = {
    graph     = true,
    max_depth = true,
}

-- Pages, elements and references are spelled `any` throughout: there is no
-- module resolver in this VM, only a global binding, so a chunk cannot name
-- another chunk's exported types. The real checking is the mint predicates the
-- constructors call.

-- One way to leave one page.
--
-- `to` is a SET, and a pop has none. More than one destination is how an edge
-- says the outcome is not certain, rather than two edges sharing one trigger. A
-- pop declares nothing, because where it lands is the page underneath the one
-- being closed, which the run knows and the file cannot.
--
-- `via_reference` is the from page's own row for `via_element`, resolved once at
-- construction, so an edge naming an element its page never authorised fails
-- before any run.
export type Edge = {
    from: any,
    to: { any }?,
    via: string,
    via_element: any,
    via_reference: any,
    via_key: string?,
    kind: string,
    extra: { [string]: any },
    residual: { string },
}

-- Every page and every edge of one project, checked against each other, which is
-- what makes "this edge leads to a page nothing declares" a construction error
-- rather than a walk waiting for a page no frame can carry. `interrupts` is the
-- pages carrying the interrupt flag, in name order: the list a timed-out walk
-- sweeps.
export type Graph = {
    pages: { any },
    page_by_name: { [string]: any },
    edges: { Edge },
    interrupts: { any },
}

-- What a run believes about the screens underneath the one it is looking at.
-- Where a covered page was is not visible on the frame that shows the overlay,
-- so it is remembered: `entries[1]` is the base and the last entry is the page a
-- walk most recently arrived on.
--
-- It is an object and never a hidden global, so two runs hold two stacks. It is
-- the one table here left mutable, because it is belief and nothing rests on it.
export type PageStack = {
    graph: Graph,
    max_depth: number,
    entries: { any },
}

-- Weak keys, for the reason spelled out over `model`'s registries: an entry can
-- only vanish once the object is unreachable, and a graph nobody can reach is a
-- graph nobody can present.
local mintedEdges: { [any]: boolean } = setmetatable({}, { __mode = "k" }) :: any
local mintedGraphs: { [any]: boolean } = setmetatable({}, { __mode = "k" }) :: any
local mintedStacks: { [any]: boolean } = setmetatable({}, { __mode = "k" }) :: any

local navigation = {}

function navigation.is_edge(value: any): boolean
    return mint.derives_from(value, mintedEdges)
end

function navigation.is_graph(value: any): boolean
    return mint.derives_from(value, mintedGraphs)
end

function navigation.is_stack(value: any): boolean
    return mint.derives_from(value, mintedStacks)
end

-- ------------------------------------------------------------------- edges

local Edge = {}
navigation.Edge = Edge

-- Edge.new{ ... } -> a frozen edge.
--
-- Five rulings, all failing closed at construction rather than at the walk that
-- would have believed them:
--
--   * A click edge names an element the from page already references and already
--     exercises interact on, because `observe.click` enforces exactly that.
--   * A push only leads to an overlay: pushing onto a page that replaces the
--     screen strands a belief nothing can pop back to.
--   * A navigate never leads to an overlay, for the mirror reason.
--   * A pop declares no destination, which the run's stack remembers instead.
--   * A spontaneous pop is refused: no game annotated so far has one, so there
--     is no instance to design the shape against.
function Edge.new(spec: { [string]: any }): Edge
    if type(spec) ~= "table" then
        error("Edge.new needs a table of fields", 2)
    end

    local unknown = mint.unknown_key("Edge.new", spec, k_edgeKeys, true)
    if unknown ~= nil then
        error(unknown, 2)
    end

    if not model.is_page(spec.from) then
        error(
            "an edge needs from = the page it leaves, built by Page.new; an edge "
                .. "leaving a table nobody minted could name a click no page ever "
                .. "authorised",
            2
        )
    end
    local from = spec.from
    local what = "the edge leaving page '" .. from.name .. "'"

    if type(spec.via) ~= "string" or not k_viaKnown[spec.via] then
        error(
            what
                .. ' needs via = "click", "key" or "spontaneous"; those are the '
                .. "three ways a screen changes, and an edge that does not say "
                .. "which one is not something anything can walk",
            2
        )
    end
    local via = spec.via :: string

    if type(spec.kind) ~= "string" or not k_edgeKindKnown[spec.kind] then
        error(
            what
                .. ' needs kind = "navigate", "push" or "pop"; walking to another '
                .. "screen, covering this one, and uncovering it are three "
                .. "different things to believe afterwards, and no pixel says "
                .. "which happened",
            2
        )
    end
    local kind = spec.kind :: string

    local viaReference: any = nil
    if via == "click" then
        if spec.via_key ~= nil then
            error(
                what
                    .. " is taken by clicking, so it cannot also name a key to "
                    .. "press",
                2
            )
        end
        if not model.is_element(spec.via_element) then
            error(
                what
                    .. " is taken by clicking, so it needs via_element = the "
                    .. "element built by Element.new that gets clicked",
                2
            )
        end
        local element = spec.via_element
        viaReference = model.Page.reference_for(from, element)
        if viaReference == nil then
            error(
                what
                    .. " clicks element '"
                    .. element.name
                    .. "', which page '"
                    .. from.name
                    .. "' does not reference; an edge is walked from the page it "
                    .. "leaves, so the click has to be one that page declared",
                2
            )
        end
        if not viaReference.exercised.interact then
            error(
                what
                    .. " clicks element '"
                    .. element.name
                    .. "', which page '"
                    .. from.name
                    .. "' references without exercising interact; a page that may "
                    .. "not click an element cannot leave by clicking it",
                2
            )
        end
    elseif via == "key" then
        if spec.via_element ~= nil then
            error(
                what
                    .. " is taken by a keystroke, so it cannot also name an "
                    .. "element to click",
                2
            )
        end
        if type(spec.via_key) ~= "string" or spec.via_key == "" then
            error(
                what
                    .. " is taken by a keystroke, so it needs via_key = the name "
                    .. 'the target prints for that key, such as "E" or "3"',
                2
            )
        end
    else
        if spec.via_element ~= nil or spec.via_key ~= nil then
            error(
                what
                    .. " happens on its own, so it names neither an element to "
                    .. "click nor a key to press",
                2
            )
        end
        if kind == "pop" then
            error(
                what
                    .. " pops on its own, and that shape is refused: no game "
                    .. "annotated so far closes an overlay without a click or a "
                    .. "key, so there is no instance to design it against and a "
                    .. "guess would be wrong for free",
                2
            )
        end
    end

    local destinations: { any }? = nil
    if kind == "pop" then
        if spec.to ~= nil then
            error(
                what
                    .. " pops, so it declares no to; where a pop lands is the page "
                    .. "underneath the one being closed, which the run's page "
                    .. "stack knows and the file cannot",
                2
            )
        end
    else
        if type(spec.to) ~= "table" then
            error(
                what
                    .. " needs to = { page, ... }, the set of pages it can land "
                    .. "on; more than one is how an edge says the outcome is not "
                    .. "certain",
                2
            )
        end
        local declared = spec.to :: { any }
        if #declared == 0 then
            error(
                what
                    .. " has an empty to; an edge that leads nowhere is not an "
                    .. "edge, and a run walking it would wait for a page no frame "
                    .. "can carry",
                2
            )
        end

        local list: { any } = {}
        local seen: { [string]: boolean } = {}
        for index = 1, #declared do
            local target = declared[index]
            if not model.is_page(target) then
                error(
                    what
                        .. " destination "
                        .. tostring(index)
                        .. " is not a page built by Page.new",
                    2
                )
            end
            if seen[target.name] then
                error(
                    what
                        .. " names page '"
                        .. target.name
                        .. "' twice; to is a set, and naming one destination "
                        .. "again says nothing a second time",
                    2
                )
            end
            seen[target.name] = true
            if kind == "push" and not target.overlay then
                error(
                    what
                        .. " pushes page '"
                        .. target.name
                        .. "', which is not declared overlay = true; pushing is "
                        .. "what leaves the page underneath on the stack, so a "
                        .. "push onto a page that replaces the screen strands a "
                        .. "belief nothing can pop back to",
                    2
                )
            end
            if kind == "navigate" and target.overlay then
                error(
                    what
                        .. " navigates to page '"
                        .. target.name
                        .. "', which is declared overlay = true; walking to an "
                        .. "overlay forgets the page it covers, so the pop that "
                        .. "closes it would land on a stack that never recorded "
                        .. "where it came from -- push it instead",
                    2
                )
            end
            list[index] = target
        end
        -- Sorted, because a set has no order and two runs over one model must
        -- write one file.
        destinations = table.freeze(mint.sorted_by_name(list))
    end

    local extraProblem = mint.check_extra(what, spec.extra)
    if extraProblem ~= nil then
        error(extraProblem, 2)
    end
    local residualProblem = mint.check_residual(what, spec.residual)
    if residualProblem ~= nil then
        error(residualProblem, 2)
    end

    local edge = table.freeze({
        from          = from,
        to            = destinations,
        via           = via,
        via_element   = spec.via_element,
        via_reference = viaReference,
        via_key       = spec.via_key,
        kind          = kind,
        extra         = mint.frozen_extra(spec.extra),
        residual      = mint.frozen_residual(spec.residual),
    })
    mintedEdges[edge] = true
    return edge
end

-- What tells two edges leaving one page apart: the trigger, and nothing else.
-- One trigger on one page has one outcome -- an uncertain outcome is a `to` with
-- more than one page in it -- which makes this a total key over a graph's edges
-- and therefore what the file can be sorted by.
function Edge.trigger_key(edge: Edge): string
    if edge.via == "click" then
        return "click\0" .. edge.via_element.name
    end
    if edge.via == "key" then
        return "key\0" .. (edge.via_key :: string)
    end
    return "spontaneous"
end

-- Whether `page` is one of this edge's declared destinations. A pop has none, so
-- it leads to nothing by this reading -- where it lands is the run's business.
function Edge.leads_to(edge: Edge, page: any): boolean
    local destinations = edge.to
    if destinations == nil then
        return false
    end
    for index = 1, #destinations do
        if destinations[index].name == page.name then
            return true
        end
    end
    return false
end

-- ------------------------------------------------------------------- graph

local Graph = {}
navigation.Graph = Graph

-- Graph.new{ pages = { ... }, edges = { ... } } -> a frozen graph.
--
-- The collection checks what a single Edge.new cannot see: every page an edge
-- names is a page this project declares, and no two edges leaving one page share
-- a trigger. Both would otherwise surface as a walk waiting for a page no frame
-- can carry. A graph with no edges, or no pages, is legal -- that is what a
-- project which has drawn none loads as.
function Graph.new(spec: { [string]: any }): Graph
    if type(spec) ~= "table" then
        error("Graph.new needs a table of fields", 2)
    end

    local unknown = mint.unknown_key("Graph.new", spec, k_graphKeys, false)
    if unknown ~= nil then
        error(unknown, 2)
    end

    if type(spec.pages) ~= "table" then
        error("Graph.new needs pages = { ... }, every page this project declares", 2)
    end
    if type(spec.edges) ~= "table" then
        error(
            "Graph.new needs edges = { ... }; a project that has drawn none "
                .. "passes an empty list, which says so",
            2
        )
    end

    local pages: { any } = {}
    local pageByName: { [string]: any } = {}
    local declaredPages = spec.pages :: { any }
    for index = 1, #declaredPages do
        local page = declaredPages[index]
        if not model.is_page(page) then
            error(
                "Graph.new page "
                    .. tostring(index)
                    .. " is not a page built by Page.new",
                2
            )
        end
        if pageByName[page.name] ~= nil then
            error(
                "Graph.new was given page '"
                    .. page.name
                    .. "' twice; a graph names each page once",
                2
            )
        end
        pageByName[page.name] = page
        pages[index] = page
    end

    local function requireDeclared(page: any, what: string)
        if pageByName[page.name] == nil then
            error(
                what
                    .. " names page '"
                    .. page.name
                    .. "', which this graph does not declare; an edge into a page "
                    .. "nothing defines is a walk that waits for a screen no "
                    .. "frame can carry",
                3
            )
        end
    end

    local edges: { Edge } = {}
    local triggers: { [string]: boolean } = {}
    local declaredEdges = spec.edges :: { any }
    for index = 1, #declaredEdges do
        local edge = declaredEdges[index]
        if not navigation.is_edge(edge) then
            error(
                "Graph.new edge "
                    .. tostring(index)
                    .. " is not an edge built by Edge.new",
                2
            )
        end
        local what = "the edge leaving page '" .. edge.from.name .. "'"
        requireDeclared(edge.from, what)
        for _, target in edge.to or {} do
            requireDeclared(target, what)
        end

        local trigger = edge.from.name .. "\0" .. Edge.trigger_key(edge)
        if triggers[trigger] then
            error(
                what
                    .. " repeats a trigger page '"
                    .. edge.from.name
                    .. "' already leaves by; one trigger on one page has one "
                    .. "outcome, and an outcome that is not certain is a to with "
                    .. "more than one page in it",
                2
            )
        end
        triggers[trigger] = true
        edges[index] = edge
    end

    local interrupts: { any } = {}
    for _, page in pages do
        if page.interrupt then
            table.insert(interrupts, page)
        end
    end
    -- Sorted, because a timed-out walk reports the FIRST interrupt page that
    -- resolves, and which one that is must not depend on the order a project
    -- file listed its pages in.
    mint.sorted_by_name(interrupts)

    local graph = table.freeze({
        pages        = table.freeze(pages),
        page_by_name = table.freeze(pageByName),
        edges        = table.freeze(edges),
        interrupts   = table.freeze(interrupts),
    })
    mintedGraphs[graph] = true
    return graph
end

function Graph.page_named(graph: Graph, name: string): any
    return graph.page_by_name[name]
end

-- Whether this graph is the one `edge` was collected into. Identity rather than
-- name, because two graphs can carry two different edges with one trigger.
function Graph.has_edge(graph: Graph, edge: Edge): boolean
    for index = 1, #graph.edges do
        if graph.edges[index] == edge then
            return true
        end
    end
    return false
end

-- Every edge leaving `page`, in declaration order. Choosing among them is layer
-- three's business.
function Graph.edges_from(graph: Graph, page: any): { Edge }
    local found: { Edge } = {}
    for index = 1, #graph.edges do
        local edge = graph.edges[index]
        if edge.from.name == page.name then
            table.insert(found, edge)
        end
    end
    return found
end

-- -------------------------------------------------------------- page stack

-- navigation.stack_new{ graph = , max_depth = } -> a page stack.
--
-- Both fields are required and neither has a default. `max_depth` is the guard
-- against a script that pushes in a loop, and a ceiling the caller cannot see is
-- a decision the caller did not make.
--
-- The graph is bound once here because the interrupt pages a timed-out walk
-- falls back on are declared ON PAGES; it also lets a walk refuse an edge from
-- some other model.
--
-- A fresh stack is EMPTY, not seeded with a guess: where a run starts is
-- something the run knows, said out loud with `navigation.stack_reset`.
function navigation.stack_new(options: any): PageStack
    if type(options) ~= "table" then
        error("navigation.stack_new needs an options table", 2)
    end
    local unknown =
        mint.unknown_key("navigation.stack_new", options, k_stackKeys, false)
    if unknown ~= nil then
        error(unknown, 2)
    end
    if not navigation.is_graph(options.graph) then
        error(
            "navigation.stack_new needs graph = the page graph built by "
                .. "Graph.new; the stack is a stack of ITS pages, and the "
                .. "interrupt pages a timed-out walk falls back on are declared "
                .. "on them",
            2
        )
    end
    local depth = options.max_depth
    if not mint.whole_number(depth) or depth < 1 then
        error(
            "navigation.stack_new needs max_depth = a whole number of pages, at "
                .. "least one; it has no default, because a stack with no ceiling "
                .. "turns a script that pushes in a loop into a run that grows "
                .. "without ever saying why",
            2
        )
    end

    local stack = {
        graph     = options.graph,
        max_depth = depth,
        entries   = {},
    }
    mintedStacks[stack] = true
    return stack
end

local function requireStack(stack: any, verb: string)
    if not navigation.is_stack(stack) then
        error(
            verb
                .. " needs the page stack navigation.stack_new returned; a table "
                .. "nobody minted carries no depth cap and no graph, so the "
                .. "runaway push the cap exists to catch would go uncaught",
            3
        )
    end
end

function navigation.stack_depth(stack: any): number
    requireStack(stack, "navigation.stack_depth")
    return #stack.entries
end

-- The page a run believes it is looking at, or nil on a stack nothing seeded.
function navigation.stack_top(stack: any): any
    requireStack(stack, "navigation.stack_top")
    return stack.entries[#stack.entries]
end

-- The whole belief, base first. A fresh copy each call, so nothing outside can
-- reorder or truncate what a walk reads.
function navigation.stack_pages(stack: any): { any }
    requireStack(stack, "navigation.stack_pages")
    return table.clone(stack.entries)
end

-- navigation.stack_reset(stack, page) -> ()
--
-- Throws the belief away and starts again from one base page. It is public
-- because a run has to say where it starts and no observation can tell it, and
-- it is the same operation `believe_arrival` performs on a base page.
function navigation.stack_reset(stack: any, page: any)
    requireStack(stack, "navigation.stack_reset")
    if not model.is_page(page) then
        error("navigation.stack_reset needs the base page to start from", 2)
    end
    if Graph.page_named(stack.graph, page.name) == nil then
        error(
            "page '"
                .. page.name
                .. "' is not in the graph this stack was built over",
            2
        )
    end

    local entries = stack.entries
    for index = #entries, 1, -1 do
        entries[index] = nil
    end
    entries[1] = page
end

local function depthCapMessage(stack: any, what: string): string
    return "pushing "
        .. what
        .. " would make the page stack "
        .. tostring(#stack.entries + 1)
        .. " deep, past the max_depth of "
        .. tostring(stack.max_depth)
        .. " this stack was built with; a stack that keeps growing is a script "
        .. "pushing in a loop, and the cap says so at the push that crossed the "
        .. "line rather than at whatever runs out first"
end

-- navigation.require_push_room(stack, edge) -> ()
--
-- Raises if walking `edge` could not be believed, BEFORE its trigger is
-- delivered: a push whose belief has nowhere to go must not send the click, or
-- the run raises about a screen it has already left behind.
function navigation.require_push_room(stack: any, edge: any)
    requireStack(stack, "navigation.require_push_room")
    if edge.kind ~= "push" or #stack.entries < stack.max_depth then
        return
    end
    local pushed: { string } = {}
    for index, target in edge.to or {} do
        pushed[index] = "page '" .. target.name .. "'"
    end
    error(depthCapMessage(stack, table.concat(pushed, " or ")), 2)
end

-- navigation.pop_destination(stack, edge) -> the page a pop is walking to.
--
-- A pop declares no destination, so it is read off the stack at walk time: the
-- entry directly underneath the current one.
function navigation.pop_destination(stack: any, edge: any): any
    requireStack(stack, "navigation.pop_destination")
    local entries = stack.entries
    if #entries < 2 then
        error(
            "popping the edge leaving page '"
                .. edge.from.name
                .. "' needs a page underneath the one being closed, and the "
                .. "stack is "
                .. tostring(#entries)
                .. " deep; a pop declares no destination precisely because the "
                .. "destination is what the stack remembers",
            2
        )
    end
    return entries[#entries - 1]
end

-- navigation.believe_arrival(stack, page, closesTop) -> ()
--
-- The stack is belief and the observation is truth. One rule outranks every kind
-- an edge can declare: a page that does NOT cover the screen it appeared over IS
-- the whole screen, so arriving on a base page resets the stack to exactly that
-- page, unconditionally, however the run got there
-- (docs/plans/2026-07-31-script-owned-page-model.md 10, the 2026-08-01 ruling).
--
-- `closesTop` is true only for a pop. Everything else arriving on an overlay --
-- a push edge, or an interrupt page that appeared while a walk was waiting --
-- goes ON the stack, because that is what was observed.
function navigation.believe_arrival(stack: any, page: any, closesTop: boolean)
    requireStack(stack, "navigation.believe_arrival")
    if not model.is_page(page) then
        error("navigation.believe_arrival needs the page that was observed", 2)
    end

    if not page.overlay then
        local entries = stack.entries
        for index = #entries, 1, -1 do
            entries[index] = nil
        end
        entries[1] = page
        return
    end

    if closesTop then
        -- The page underneath was itself an overlay, so the stack keeps its base
        -- and loses exactly the entry that was closed.
        table.remove(stack.entries)
        return
    end

    if #stack.entries == 0 then
        error(
            "page '"
                .. page.name
                .. "' was pushed onto a stack with no base page; call "
                .. "navigation.stack_reset with the page the run starts on first, "
                .. "because an overlay with nothing underneath is a belief no pop "
                .. "can return to",
            0
        )
    end
    if #stack.entries >= stack.max_depth then
        error(depthCapMessage(stack, "page '" .. page.name .. "'"), 0)
    end
    table.insert(stack.entries, page)
end

return navigation
