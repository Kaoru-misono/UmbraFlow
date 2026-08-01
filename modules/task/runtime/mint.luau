--!strict
-- mint: the checks and freezes every constructor in this framework repeats.
--
-- WHY THIS IS ITS OWN MODULE. `model` owns the element and page vocabulary and
-- `navigation` owns the page graph, and both of them answer the same four
-- questions about every spec table an author hands in: is this one of ours, does
-- it carry a key we do not own, is its `extra` a plain table, and are its
-- `residual` lines strings. Answering them twice would be two copies of a rule
-- that has to stay identical -- the `extra` reservation and the residual
-- carry-through are the whole of this format's forward compatibility
-- (docs/plans/2026-07-31-script-owned-page-model.md 8), and a second copy is
-- where they would drift apart.
--
-- NOTHING HERE DECIDES ANYTHING ABOUT PAGES OR PIXELS. Every function is about
-- the SHAPE of a table an author wrote, so this module names no other module and
-- can be read on its own.
--
-- THE SENTENCES COME BACK, THEY ARE NOT RAISED. Every checker returns a sentence
-- or nil, so the constructor that called it raises once at level 2 and the
-- author reads their own call site rather than a line of a helper file.
--
-- LOAD ORDER. Framework modules load in the sorted order of their file names and
-- each module's frozen exports are bound in the framework environment under its
-- own name. `mint` sorts before `model` and `navigation`, which is what lets
-- both of them name it. This module names nothing but the standard library.

local mint = {}

-- Whether `value` is one of `registry`'s tables, or inherits from one through a
-- chain of `__index` metatables.
--
-- The inheritance case is the point. Layer three extends what layer two built
-- the way Luau does it -- `setmetatable({ ... }, { __index = base })`
-- (docs/plans/2026-07-31-script-owned-page-model.md 3) -- so an extension does
-- not have to re-mint its base; what a registry check refuses is a table nobody
-- minted at all.
--
-- The walk is bounded because a cyclic chain is a table a script can build, and
-- this must answer rather than hang.
function mint.derives_from(value: any, registry: { [any]: boolean }): boolean
    local current = value
    local depth   = 0
    while type(current) == "table" and depth <= 8 do
        if registry[current] then
            return true
        end
        local meta = getmetatable(current)
        if type(meta) ~= "table" then
            return false
        end
        current = meta.__index
        depth  += 1
    end
    return false
end

function mint.whole_number(value: any): boolean
    return type(value) == "number" and value % 1 == 0
end

-- The sentence for the first key in `spec` that `known` does not list, or nil.
--
-- THE REFUSAL IS THE WHOLE REASON `extra` WORKS. A layer may add a key next
-- year, and a project that had quietly put its own key of that name at the top
-- level would silently lose it. So an unrecognised key is refused here, where it
-- can still be attributed to the line the author wrote, and `hasExtra` is what
-- lets the sentence point at the reserved subtable instead of just saying no.
function mint.unknown_key(
    what: string,
    spec: { [string]: any },
    known: { [string]: boolean },
    hasExtra: boolean
): string?
    for key in spec do
        if type(key) ~= "string" then
            return what .. " takes named fields only"
        end
        if not known[key] then
            if hasExtra then
                return what
                    .. ' does not have a field named "'
                    .. key
                    .. '"; a field this project owns goes under extra = { '
                    .. key
                    .. " = ... }"
            end
            return what .. ' does not have a field named "' .. key .. '"'
        end
    end
    return nil
end

function mint.check_extra(what: string, value: any): string?
    if value == nil then
        return nil
    end
    if type(value) ~= "table" then
        return what .. ".extra must be a table of the project's own fields"
    end
    if getmetatable(value) ~= nil then
        return what .. ".extra must be a plain table with no metatable"
    end
    return nil
end

function mint.frozen_extra(value: any): { [string]: any }
    if value == nil then
        return table.freeze({})
    end
    local copy: { [string]: any } = {}
    for key, entry in value :: { [string]: any } do
        copy[key] = entry
    end
    return table.freeze(copy)
end

-- The raw file lines a schema version did not understand, carried on the object
-- so a rewrite cannot drop them
-- (docs/plans/2026-07-31-script-owned-page-model.md 8: a newer file opened and
-- saved by an older build must not lose the newer build's keys, least of all
-- without saying so). A model built in code carries an empty list.
function mint.check_residual(what: string, value: any): string?
    if value == nil then
        return nil
    end
    if type(value) ~= "table" then
        return what .. ".residual must be a list of raw file lines"
    end
    local list = value :: { any }
    for index = 1, #list do
        if type(list[index]) ~= "string" then
            return what
                .. ".residual entry "
                .. tostring(index)
                .. " must be a raw file line"
        end
    end
    return nil
end

function mint.frozen_residual(value: any): { string }
    if value == nil then
        return table.freeze({})
    end
    local copy: { string } = {}
    local list = value :: { string }
    for index = 1, #list do
        copy[index] = list[index]
    end
    return table.freeze(copy)
end

-- Sorts a list of named objects by name, in place, and returns it. Elements,
-- pages and an edge's destination set are all written out sorted, because their
-- order carries no meaning and two runs over one model must produce one file.
function mint.sorted_by_name<T>(list: { T }): { T }
    table.sort(list, function(left: any, right: any): boolean
        return left.name < right.name
    end)
    return list
end

return mint
