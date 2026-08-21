--!strict
-- Deterministic list/set helpers and shallow immutable updates.

local collections = {}

local function reject(message: string): never
    error("collections " .. message, 3)
end

local function listLength(value: any, where: string): number
    if type(value) ~= "table" then reject(where .. " must be a list") end
    local count = rawlen(value)
    for key in next, value do
        if type(key) ~= "number" then reject(where .. " must be a list") end
        local index = key :: number
        if index % 1 ~= 0 or index < 1 or index > count then
            reject(where .. " must be a dense list")
        end
    end
    for index = 1, count do
        if rawget(value, index) == nil then reject(where .. " must be a dense list") end
    end
    return count
end

local function callable(value: any, where: string): any
    if type(value) ~= "function" then reject(where .. " must be a function") end
    return value
end

local function copyList(value: any, where: string): { any }
    local count = listLength(value, where)
    local copied: { any } = {}
    for index = 1, count do copied[index] = value[index] end
    return copied
end

local function primitiveKey(key: any): boolean
    local kind = type(key)
    return (kind == "string" or kind == "number" or kind == "boolean")
        and not (kind == "number" and key ~= key)
end

local function copyMap(value: any, where: string): { [any]: any }
    if type(value) ~= "table" then reject(where .. " must be a table") end
    local copied = {}
    for key, child in next, value do
        if not primitiveKey(key) then reject(where .. " has a non-primitive key") end
        copied[key] = child
    end
    return copied
end

function collections.list(value: any): any
    return table.freeze(copyList(value, "list"))
end

function collections.map(values: any, mapper: any): any
    local count = listLength(values, "map input")
    local apply = callable(mapper, "map callback")
    local mapped: { any } = {}
    for index = 1, count do
        local value = apply(values[index], index)
        if value == nil then reject("map callback returned nil") end
        mapped[index] = value
    end
    return table.freeze(mapped)
end

function collections.filter(values: any, predicate: any): any
    local count = listLength(values, "filter input")
    local keep = callable(predicate, "filter predicate")
    local filtered: { any } = {}
    for index = 1, count do
        local retained = keep(values[index], index)
        if type(retained) ~= "boolean" then
            reject("filter predicate must return a boolean")
        end
        if retained then table.insert(filtered, values[index]) end
    end
    return table.freeze(filtered)
end

function collections.fold(values: any, initial: any, reducer: any): any
    local count = listLength(values, "fold input")
    local reduce = callable(reducer, "fold reducer")
    local accumulator = initial
    for index = 1, count do
        accumulator = reduce(accumulator, values[index], index)
    end
    return accumulator
end

function collections.stable_sort(values: any, comparator: any): any
    local count = listLength(values, "stable_sort input")
    local less = callable(comparator, "stable_sort comparator")
    local decorated: { any } = {}
    for index = 1, count do
        decorated[index] = { index = index, value = values[index] }
    end
    table.sort(decorated, function(left: any, right: any): boolean
        local leftFirst = less(left.value, right.value)
        local rightFirst = less(right.value, left.value)
        if type(leftFirst) ~= "boolean" or type(rightFirst) ~= "boolean" then
            reject("stable_sort comparator must return a boolean")
        end
        if leftFirst and rightFirst then
            reject("stable_sort comparator orders both values before each other")
        end
        if leftFirst ~= rightFirst then return leftFirst end
        return left.index < right.index
    end)
    local sorted: { any } = {}
    for index = 1, count do sorted[index] = decorated[index].value end
    return table.freeze(sorted)
end

function collections.contains(values: any, expected: any): boolean
    local count = listLength(values, "contains input")
    for index = 1, count do
        if rawequal(values[index], expected) then return true end
    end
    return false
end

function collections.append(values: any, value: any): any
    if value == nil then reject("append value must not be nil") end
    local copied = copyList(values, "append input")
    table.insert(copied, value)
    return table.freeze(copied)
end

function collections.set_at(values: any, index: any, value: any): any
    local copied = copyList(values, "set_at input")
    if type(index) ~= "number" or index % 1 ~= 0 or index < 1 or index > #copied then
        reject("set_at index is outside the list")
    end
    if value == nil then reject("set_at value must not be nil") end
    copied[index] = value
    return table.freeze(copied)
end

function collections.map_set(values: any, key: any, value: any): any
    if not primitiveKey(key) then
        reject("map_set key is not representable")
    end
    if value == nil then reject("map_set value must not be nil") end
    local copied = copyMap(values, "map_set input")
    copied[key] = value
    return table.freeze(copied)
end

function collections.map_remove(values: any, key: any): any
    if not primitiveKey(key) then reject("map_remove key is not representable") end
    local copied = copyMap(values, "map_remove input")
    copied[key] = nil
    return table.freeze(copied)
end

function collections.set(values: any): any
    local count = listLength(values, "set input")
    local output = {}
    for index = 1, count do
        local value = values[index]
        if not primitiveKey(value) then
            reject("set value is not representable")
        end
        output[value] = true
    end
    return table.freeze(output)
end

function collections.has(values: any, value: any): boolean
    if type(values) ~= "table" then reject("has input must be a set") end
    if not primitiveKey(value) then reject("has value is not representable") end
    return values[value] == true
end

return table.freeze(collections)
