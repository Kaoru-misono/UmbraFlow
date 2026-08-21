#include <task/framework-bundle.hpp>

#include <domain/content-hash.hpp>

#include <script/engine.hpp>
#include <script/pure-data-program.hpp>

#include <json/value.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uf::task
{
    namespace
    {
        [[nodiscard]] auto digest(std::string_view text) -> std::string
        {
            auto result = sha256(std::as_bytes(std::span{text}));
            REQUIRE(result.has_value());
            return result->hex();
        }
    }

    TEST_CASE("business framework publication is fail closed")
    {
        CHECK(frameworkProjectGlobals().empty());
        CHECK(
            explorationProjectGlobals()
            == std::vector<std::string>{"explore"}
        );

        auto engine = script::Engine::create(
            script::EngineConfig{
                .frameworkModules        = frameworkScriptModules(),
                .projectGlobals          = {},
                .frameworkProjectGlobals = frameworkProjectGlobals(),
            }
        );
        REQUIRE(engine.has_value());
        auto result = engine->runNumber(
            R"lua(
                if ctx ~= nil or explore ~= nil or model ~= nil or observe ~= nil then return 0 end
                if project ~= nil or navigation ~= nil or input ~= nil or receipt ~= nil then return 0 end
                if require ~= nil or debug ~= nil or _G ~= nil or getfenv ~= nil then return 0 end
                if load ~= nil or loadstring ~= nil or package ~= nil or io ~= nil then return 0 end
                if native ~= nil or host ~= nil or ffi ~= nil or uf_private ~= nil then return 0 end
                return 1
            )lua",
            "business-surface-attack"
        );
        REQUIRE(result.has_value());
        CHECK(*result == doctest::Approx(1.0));
    }

    TEST_CASE("embedded framework identity remains deterministic")
    {
        auto names    = std::vector<std::string_view>{};
        auto preimage = std::string{};
        for (auto const& entry : frameworkBundleEntries())
        {
            names.emplace_back(entry.name);
            CHECK(entry.sourceHash == digest(entry.source));
            CHECK(checkFrameworkModuleSyntax(entry.source, entry.name).has_value());
            preimage += entry.name;
            preimage.push_back('\0');
            preimage += entry.source;
        }
        CHECK(std::ranges::is_sorted(names));
        CHECK(std::ranges::adjacent_find(names) == names.end());
        CHECK(frameworkBundleHash() == digest(preimage));
    }

    TEST_CASE("the Project pure SDK exposes embedded modules by reserved name")
    {
        auto sdk = pureFrameworkScriptModules();
        REQUIRE(sdk.has_value());
        REQUIRE(sdk->size() == 3U);
        CHECK(sdk->at(0).name == "@umbraflow/collections");
        CHECK(sdk->at(1).name == "@umbraflow/jcs");
        CHECK(sdk->at(2).name == "@umbraflow/result");

        constexpr auto entries = std::array{std::string_view{"derive"}};
        auto program = script::PureDataProgram::compile(
            "fixture.sdk",
            "main",
            {
                script::PureDataProgram::Module{
                    .name = "main",
                    .source = R"LUAU(
local jcs = require("@umbraflow/jcs")
local collections = require("@umbraflow/collections")
local result = require("@umbraflow/result")
return {
    plugin_id = "fixture.sdk",
    derive = function(input)
        local doubled = collections.map({ 3, 1, 2 }, function(value)
            return value * 2
        end)
        local ordered = collections.stable_sort(doubled, function(left, right)
            return left < right
        end)
        local stable = collections.stable_sort({
            { group = 1, id = "a" },
            { group = 1, id = "b" },
            { group = 0, id = "c" },
        }, function(left, right)
            return left.group < right.group
        end)
        local total = collections.fold(ordered, 0, function(sum, value)
            return sum + value
        end)
        local outcome = result.and_then(result.ok(total), function(value)
            return result.ok(value + 1)
        end)
        local failed = result.err("fixture", "expected")
        local skipped = 0
        local retained = result.and_then(failed, function(value)
            skipped += value
            return result.ok(skipped)
        end)
        local mappedFailure = result.map_error(retained, function(failure)
            return failure.code .. ".mapped", failure.message
        end)
        local set = collections.set({ "a", "b" })
        local frozen = not pcall(function() ordered[1] = 99 end)
        local errorFrozen = not pcall(function() failed.error.code = "forged" end)
        local invalidComparator = not pcall(function()
            collections.stable_sort({ 1, 2 }, function() return true end)
        end)
        local invalidFilter = not pcall(function()
            collections.filter({ 1 }, function() return "yes" end)
        end)
        local invalidList = not pcall(function()
            collections.list({ [1] = 1, [3] = 3 })
        end)
        local invalidMap = not pcall(function()
            collections.map({ 1 }, function() return nil end)
        end)
        local invalidResult = not pcall(function()
            return result.is_ok({ kind = "invented" })
        end)
        return {
            canonical = jcs.encode(input),
            error = result.match(mappedFailure, function() return "wrong" end,
                function(failure) return failure.code end),
            error_frozen = errorFrozen,
            frozen = frozen,
            has = collections.has(set, "b"),
            invalid_comparator = invalidComparator,
            invalid_filter = invalidFilter,
            invalid_list = invalidList,
            invalid_map = invalidMap,
            invalid_result = invalidResult,
            result = result.match(outcome, function(value) return value end,
                function(failure) return failure.code end),
            skipped = skipped,
            stable = stable[1].id .. stable[2].id .. stable[3].id,
            sorted = ordered,
        }
    end,
}
)LUAU",
                },
            },
            entries,
            {},
            *sdk
        );
        REQUIRE(program.has_value());
        auto input = json::parse(R"({"b":1,"a":2})");
        REQUIRE(input.has_value());
        auto result = program->invoke("derive", *input);
        REQUIRE(result.has_value());
        CHECK(
            json::canonicalBytes(*result)
            == R"({"canonical":"{\"a\":2,\"b\":1}","error":"fixture.mapped","error_frozen":true,"frozen":true,"has":true,"invalid_comparator":true,"invalid_filter":true,"invalid_list":true,"invalid_map":true,"invalid_result":true,"result":13,"skipped":0,"sorted":[2,4,6],"stable":"cab"})"
        );
    }
}
