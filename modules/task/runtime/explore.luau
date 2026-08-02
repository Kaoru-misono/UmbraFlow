--!strict
-- explore: the verbs an agent has and a business script does not.
--
-- WHY THIS IS A MODULE AND NOT MORE METHODS ON `ctx`. `ctx` is published into
-- EVERY project environment, business runs included, so a forward added there is
-- a forward every task can call. These verbs must be reachable from one
-- environment and unreachable from the other, and the mechanism this framework
-- has for that is which module names a project environment publishes
-- (docs/plans/2026-07-29-three-layer-task-system.md 7). So the privileged
-- forwards live in a module of their own, and `explore` is in the exploration
-- environment's publish list and in no other.
--
-- The isolation is therefore structural rather than checked. A project
-- environment is a whitelist with no metatable: a run VM has no `explore`, no
-- `__index` chain that could reach one, and -- for `crop` and `probe` -- no
-- primitive behind it either, because a run VM's private surface never binds
-- `cycle_crop` or `probe` at all. `click_point` and `long_press` do have their
-- primitives on both surfaces, because the trusted framework needs both to act
-- on an element the page model placed; what confines those is that only this
-- module forwards them, and only the exploration environment has this module.
--
-- WHAT IS NOT HERE. Frame differencing. The plan defers it deliberately: two
-- crops and an agent's own eyes cover the cases anyone has actually met, and a
-- primitive minted before the first real instance would be a guess
-- (docs/plans/2026-08-01-agent-front-end-and-exploration.md 1).
--
-- NOTHING HERE DECIDES ANYTHING. Every function below forwards its arguments and
-- returns what the host returned. Judging a probe's counts, choosing a
-- tolerance, deciding where to click -- all of that is the agent's, or `scribe`'s
-- when the agent asks for the whole authoring loop.
--
-- LOAD ORDER. Framework modules load in the sorted order of their file names.
-- `explore` sorts after `ctx` and before every other module, which is fine
-- because it names none of them: it names the private surface and the standard
-- library, and nothing else.

local native = ...

local explore = {}

-- Whether the host bound `name` on this VM's private capability surface.
--
-- It exists so the environment split can be FALSIFIED from the outside. "The
-- run surface does not carry cycle_crop" is a claim about a table no test can
-- name, and a test that could only observe it through a raised error would pass
-- just as well against a surface that carried the key and refused the call --
-- which is precisely the shape section 2 rule 2 rejects. This answers the
-- question directly, and it confers nothing: the answer is a boolean, and a
-- caller that can ask it could have called the verb instead.
function explore.has(name: string): boolean
    if type(name) ~= "string" then
        error("explore.has needs the name of a primitive", 2)
    end
    if native == nil then
        return false
    end
    return (native :: any)[name] ~= nil
end

local function requirePrimitive(name: string, verb: string)
    if not explore.has(name) then
        error(
            verb
                .. " needs the '"
                .. name
                .. "' primitive, which this environment does not load; it is an "
                .. "exploration verb and this VM is not an exploration VM",
            3
        )
    end
end

-- explore.click_point(ticket, x, y) -> ()
--
-- Clicks a bare coordinate and spends the cycle. This is the verb the two trust
-- modes differ on most plainly: a business script clicks an element its page
-- authorised, and an agent standing in front of a game it has no model of yet
-- clicks wherever it decided to look next
-- (docs/plans/2026-08-01-three-layers-and-agent-operator.md 2).
--
-- The host still fences it. The frame must be this ticket's, its observation
-- lease must still be fresh, the live geometry must match the project's
-- fingerprint, and the cycle is spent so one frame delivers one input. What is
-- gone is only the part that needed a model, because the agent is here to build
-- the model.
--
-- It is recorded as `annotation.click_delivered`, never as
-- `engine.action_delivered`: there is no element and no page behind this
-- coordinate, and the evidence stream has to say so.
function explore.click_point(ticket: any, x: number, y: number)
    requirePrimitive("cycle_click_point", "explore.click_point")
    return (native :: any).cycle_click_point(ticket, x, y)
end

-- explore.long_press(ticket, x, y, hold_ms) -> ()
--
-- Presses a bare coordinate, holds it for `hold_ms`, releases it, and spends the
-- cycle. It is `click_point` with a duration, and it is here for `click_point`'s
-- reason: an agent standing in front of a game it has no model of yet acts on
-- coordinates it chose, and a business script acts on elements its page
-- authorised.
--
-- WHY AN AGENT NEEDS THIS SPECIFICALLY. A long press is how a target volunteers
-- what it will not show otherwise -- the one this framework was built for prints
-- "long-press a card to see its details" and magnifies the card, which turns a
-- name clipped by the card in front of it into a name that can simply be read.
-- Discovering that a screen has such a gesture, and what it reveals, is
-- measurement, and measurement is what this module is for.
--
-- The host fences it exactly as it fences click_point: the frame must be this
-- ticket's, the observation lease must still be fresh, the live geometry must
-- match the project's fingerprint, and the cycle is spent so one frame delivers
-- one input. The hold is bounded by the host and has NO DEFAULT here: how long
-- this target wants a press held is the thing being measured, so a number this
-- module supplied would be a measurement nobody took.
function explore.long_press(ticket: any, x: number, y: number, holdMillis: number)
    requirePrimitive("cycle_long_press", "explore.long_press")
    if type(holdMillis) ~= "number" then
        error(
            "explore.long_press needs hold_ms = how long to hold the button "
                .. "down, in milliseconds; it has no default, because how long "
                .. "a target wants a press held is a fact about that target",
            2
        )
    end
    return (native :: any).cycle_long_press(ticket, x, y, holdMillis)
end

-- explore.crop(ticket, x, y, width, height, key?, tolerance?)
--     -> (blob, hash, mask?)
--
-- The pixels of one rectangle as a PNG, and the lowercase hex SHA-256 of exactly
-- those bytes. The hash comes back because a template asset is NAMED by it and
-- this layer has no hash function of its own; see the primitive.
--
-- The blob is the same kind of value `project_read` hands back, so it goes
-- straight into `project_write` with no conversion -- which is how a crop
-- becomes a file an agent can look at.
--
-- WITH A KEY THE CROP IS A MASKED TEMPLATE. The key is the same optional
-- { red, green, blue } table `explore.probe` takes, with the same optional
-- tolerance, and it is unpacked here for the same reason -- the private surface
-- takes scalars and a Luau table is neither. What the host does with it is bake
-- the weights into the PNG's alpha channel, which is what makes the matcher
-- compare the glyph rather than the whole rectangle. The third return says what
-- the key took: `rect_pixels`, `selected_pixels`, `ramp_selected_pixels`, and a
-- `warning` when those counts look like a mask that cannot measure anything. It
-- is nil when no key was given, and a key that selects NO pixel is refused by
-- the host rather than reported here.
function explore.crop(
    ticket: any,
    x: number,
    y: number,
    width: number,
    height: number,
    key: { red: number, green: number, blue: number }?,
    tolerance: number?
): (string, string, any)
    requirePrimitive("cycle_crop", "explore.crop")
    if key == nil then
        return (native :: any).cycle_crop(ticket, x, y, width, height)
    end
    if type(key) ~= "table" then
        error(
            "explore.crop takes key = { red = r, green = g, blue = b } or no "
                .. "key at all",
            2
        )
    end
    return (native :: any).cycle_crop(
        ticket,
        x,
        y,
        width,
        height,
        key.red,
        key.green,
        key.blue,
        tolerance
    )
end

-- explore.probe(blob, x, y, width, height, key?, tolerance?) -> stats
--
-- Colour statistics over one rectangle of one PNG. `key` is an optional
-- { red, green, blue } table, and `tolerance` an optional whole number; with no
-- key the answer carries the census fields alone, which is what an agent reads
-- to CHOOSE a key.
--
-- The key arrives here as a table and leaves as three scalars, because the
-- private surface takes only host-minted handles and scalars and a Luau table is
-- neither. Unpacking it is this layer's whole contribution: the numbers are the
-- host's and the judgement is the caller's.
function explore.probe(
    blob: string,
    x: number,
    y: number,
    width: number,
    height: number,
    key: { red: number, green: number, blue: number }?,
    tolerance: number?
): any
    requirePrimitive("probe", "explore.probe")
    if key == nil then
        return (native :: any).probe(blob, x, y, width, height)
    end
    if type(key) ~= "table" then
        error(
            "explore.probe takes key = { red = r, green = g, blue = b } or no "
                .. "key at all",
            2
        )
    end
    return (native :: any).probe(
        blob,
        x,
        y,
        width,
        height,
        key.red,
        key.green,
        key.blue,
        tolerance
    )
end

return explore
