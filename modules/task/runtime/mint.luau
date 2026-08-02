--!strict
-- mint: the checks and freezes every constructor in this framework repeats.
--
-- `model` and `navigation` both ask the same four questions of every spec table
-- an author hands in: is this one of ours, does it carry a key we do not own, is
-- its `extra` a plain table, are its `residual` lines strings. The `extra`
-- reservation and the residual carry-through are the whole of this format's
-- forward compatibility (docs/plans/2026-07-31-script-owned-page-model.md 8), so
-- a second copy of them is where they would drift apart.
--
-- Nothing here decides anything about pages or pixels; every function is about
-- the SHAPE of a table an author wrote, so this module names nothing but the
-- standard library.
--
-- Every checker RETURNS a sentence or nil rather than raising, so the
-- constructor raises once at level 2 and the author reads their own call site.
--
-- Framework modules load in the sorted order of their file names, and each
-- module's frozen exports are bound in the framework environment under its own
-- name. `mint` sorts before `model` and `navigation`, which is what lets both
-- name it.

local mint = {}

-- Whether `value` is one of `registry`'s tables, or inherits from one through a
-- chain of `__index` metatables. The inheritance case is the point: layer three
-- extends layer two with `setmetatable({ ... }, { __index = base })`
-- (docs/plans/2026-07-31-script-owned-page-model.md 3), so what a registry check
-- refuses is a table nobody minted at all. The walk is bounded because a cyclic
-- chain is a table a script can build.
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
-- The refusal is what makes `extra` work: a layer may add a key next year, and a
-- project that had quietly put its own key of that name at the top level would
-- silently lose it. `hasExtra` lets the sentence point at the reserved subtable.
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

-- How many tables deep an `extra` may nest, itself counted as the first. The
-- bound is there for the reason `derives_from`'s is: a table that refers back to
-- itself is one a project script can build, and neither the check nor the copy
-- below would return on one. It is the same 8, and authored project data does
-- not come near it.
local k_extraDepth = 8

-- The sentence for the first table under `value` that nests past the bound, or
-- nil. A cycle reaches the bound rather than running forever, so it arrives here
-- as the same sentence.
local function nestingProblem(what: string, value: { [any]: any }, depth: number): string?
    for key, entry in value do
        if type(entry) == "table" then
            if depth >= k_extraDepth then
                return what
                    .. ".extra nests more than "
                    .. tostring(k_extraDepth)
                    .. ' tables deep at "'
                    .. tostring(key)
                    .. '", or refers back to itself there'
            end
            local problem = nestingProblem(what, entry :: { [any]: any }, depth + 1)
            if problem ~= nil then
                return problem
            end
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
    return nestingProblem(what, value :: { [any]: any }, 1)
end

local function frozenCopy(value: { [any]: any }, depth: number): any
    local copy: { [any]: any } = {}
    for key, entry in value do
        if type(entry) == "table" then
            if depth >= k_extraDepth then
                error(
                    "an extra table nests deeper than a snapshot can copy",
                    0
                )
            end
            copy[key] = frozenCopy(entry :: { [any]: any }, depth + 1)
        else
            copy[key] = entry
        end
    end
    return table.freeze(copy)
end

-- A snapshot ALL THE WAY DOWN. Freezing one level would leave a nested table as
-- the caller's own writable one, so the value handed back would still change
-- under whoever was given it. `check_extra` refuses deeper nesting first, with a
-- sentence carrying the author's own call site; the raise above is what keeps
-- this function bounded for a caller that skipped that check.
function mint.frozen_extra(value: any): { [string]: any }
    if value == nil then
        return table.freeze({})
    end
    return frozenCopy(value :: { [any]: any }, 1)
end

-- The raw file lines a schema version did not understand, carried on the object
-- so an older build cannot silently drop a newer build's keys when it rewrites
-- the file (docs/plans/2026-07-31-script-owned-page-model.md 8). A model built in
-- code carries an empty list.
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
