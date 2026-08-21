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

        auto modules = frameworkScriptModules();
        auto const positionOf = [&modules](std::string_view name)
        {
            return std::ranges::find(
                modules,
                name,
                &script::FrameworkModule::name
            );
        };
        auto const jcs         = positionOf("jcs");
        auto const json        = positionOf("json");
        auto const text        = positionOf("text");
        auto const textData    = positionOf("unicode-text-data");
        auto const unicode     = positionOf("utf8");
        auto const unicodeData = positionOf("unicode-utf8-data");
        REQUIRE(jcs != modules.end());
        REQUIRE(json != modules.end());
        REQUIRE(text != modules.end());
        REQUIRE(textData != modules.end());
        REQUIRE(unicode != modules.end());
        REQUIRE(unicodeData != modules.end());
        CHECK(jcs->resolverName == "@umbraflow/jcs");
        CHECK(json->resolverName == "@umbraflow/json");
        CHECK(text->resolverName == "@umbraflow/text");
        CHECK(textData->resolverName == "@umbraflow/internal/unicode-text-data");
        CHECK(unicode->resolverName == "@umbraflow/utf8");
        CHECK(
            unicodeData->resolverName
            == "@umbraflow/internal/unicode-utf8-data"
        );
        CHECK(jcs < json);
        CHECK(textData < text);
        CHECK(unicodeData < unicode);
        CHECK(unicode < text);

        auto engine = script::Engine::create(
            script::EngineConfig{
                .frameworkModules        = std::move(modules),
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
        REQUIRE(sdk->size() == 8U);
        CHECK(sdk->at(0).name == "@umbraflow/collections");
        CHECK(sdk->at(1).name == "@umbraflow/jcs");
        CHECK(sdk->at(2).name == "@umbraflow/json");
        CHECK(sdk->at(3).name == "@umbraflow/result");
        CHECK(sdk->at(4).name == "@umbraflow/text");
        CHECK(sdk->at(5).name == "@umbraflow/utf8");
        CHECK(
            sdk->at(6).name
            == "@umbraflow/internal/unicode-text-data"
        );
        CHECK(
            sdk->at(7).name
            == "@umbraflow/internal/unicode-utf8-data"
        );
        for (auto index = std::size_t{0U}; index < 6U; ++index)
        {
            CHECK(sdk->at(index).projectVisible);
        }
        CHECK_FALSE(sdk->at(6).projectVisible);
        CHECK_FALSE(sdk->at(7).projectVisible);

        constexpr auto entries = std::array{std::string_view{"derive"}};
        auto program = script::PureDataProgram::compile(
            "fixture.sdk",
            "main",
            {
                script::PureDataProgram::Module{
                    .name = "main",
                    .source = R"LUAU(
local jcs = require("@umbraflow/jcs")
local json = require("@umbraflow/json")
local collections = require("@umbraflow/collections")
local result = require("@umbraflow/result")
local text = require("@umbraflow/text")
local unicode = require("@umbraflow/utf8")
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
        local points = unicode.codepoints("A🙂中")
        local pointsFrozen = not pcall(function() points[1] = 0 end)
        local invalidUtf8 = "\255"
        local invalidUtf8Rejected = not pcall(function()
            return unicode.validate(invalidUtf8)
        end)
        local split = text.split("a,,b", ",")
        local tokens = text.tokens("Go, 中🙂 42")
        local splitFrozen = not pcall(function() split[1] = "forged" end)
        local tokensFrozen = not pcall(function() tokens[1] = "forged" end)
        local invalidNormalization = not pcall(function()
            return text.normalize("value", "HOST")
        end)
        local invalidMatchOptions = not pcall(function()
            return text.equals("a", "a", { locale = "tr-TR" })
        end)
        local invalidMatchOptionType = not pcall(function()
            return text.equals("a", "a", { normalization = false })
        end)
        local invalidTextUtf8 = not pcall(function()
            return text.case_fold(invalidUtf8)
        end)
        local internalDataHidden = not pcall(function()
            return require("@umbraflow/internal/unicode-text-data")
        end)
        local emptyObject = json.parse("{}")
        local emptyArray = json.parse("[]")
        local immutableObject = json.object({
            nested = { 1, json.null, 3 },
        })
        local changedObject = json.set(immutableObject, "added", true)
        local appendedArray = json.append(json.get(immutableObject, "nested"), "tail")
        local parsedFrozen = not pcall(function() emptyObject.forged = true end)
        local nestedFrozen = not pcall(function()
            json.get(immutableObject, "nested")[1] = 99
        end)
        local duplicateRejected = not pcall(function()
            return json.parse('{"same":1,"same":2}')
        end)
        local badSurrogateRejected = not pcall(function()
            return json.parse('"\\uD800"')
        end)
        local badNumberRejected = not pcall(function()
            return json.parse("01")
        end)
        local invalidJsonUtf8Rejected = not pcall(function()
            return json.parse('"' .. invalidUtf8 .. '"')
        end)
        local mixedRejected = not pcall(function()
            return json.encode({ [1] = true, named = true })
        end)
        local sparseRejected = not pcall(function()
            return json.encode({ [1] = true, [3] = true })
        end)
        local cycle = {}
        cycle.self = cycle
        local cycleRejected = not pcall(function() return json.encode(cycle) end)
        local nonFiniteRejected = not pcall(function()
            return json.encode(math.huge)
        end)
        local ambiguousEmptyRejected = not pcall(function()
            return json.immutable({})
        end)
        local deep = json.array()
        for _ = 1, 130 do deep = json.array({ deep }) end
        local deepRejected = not pcall(function() return json.encode(deep) end)
        local tooLargeRejected = not pcall(function()
            return json.parse(string.rep(" ", 1024 * 1024 + 1))
        end)
        local expandedScalarRejected = not pcall(function()
            return json.encode(string.rep("\1", 200000))
        end)
        local valueCountRejected = not pcall(function()
            local wide = json.array({ true })
            for _ = 1, 16 do wide = json.array({ wide, wide }) end
            return wide
        end)
        local removedObject = json.remove(changedObject, "added")
        local removedArray = json.remove(appendedArray, 2)
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
            invalid_match_option_type = invalidMatchOptionType,
            invalid_match_options = invalidMatchOptions,
            invalid_normalization = invalidNormalization,
            invalid_result = invalidResult,
            invalid_text_utf8 = invalidTextUtf8,
            invalid_utf8 = not unicode.is_valid(invalidUtf8) and invalidUtf8Rejected,
            internal_data_hidden = internalDataHidden,
            json_appended = json.encode(appendedArray),
            json_ambiguous_empty = ambiguousEmptyRejected,
            json_bad_number = badNumberRejected,
            json_bad_surrogate = badSurrogateRejected,
            json_changed = json.encode(changedObject),
            json_cycle = cycleRejected,
            json_deep = deepRejected,
            json_duplicate = duplicateRejected,
            json_empty_array = json.encode(emptyArray),
            json_empty_object = json.encode(emptyObject),
            json_expanded_scalar = expandedScalarRejected,
            json_invalid_utf8 = invalidJsonUtf8Rejected,
            json_mixed = mixedRejected,
            json_nested_frozen = nestedFrozen,
            json_non_finite = nonFiniteRejected,
            json_null_shared = rawequal(json.null, jcs.null),
            json_parsed_frozen = parsedFrozen,
            json_removed_array = json.encode(removedArray),
            json_removed_object = json.encode(removedObject),
            json_sparse = sparseRejected,
            json_too_large = tooLargeRejected,
            json_value_count = valueCountRejected,
            json_constructed_array = json.encode(json.array()),
            json_constructed_object = json.encode(json.object()),
            letter = unicode.classify("A"),
            mark = unicode.classify(0x0301),
            number = unicode.classify("9"),
            other = unicode.classify(0x0378),
            points = points,
            points_frozen = pointsFrozen,
            result = result.match(outcome, function(value) return value end,
                function(failure) return failure.code end),
            skipped = skipped,
            split = split,
            split_compact = text.split("a,,b", ",", false),
            split_frozen = splitFrozen,
            stable = stable[1].id .. stable[2].id .. stable[3].id,
            sorted = ordered,
            separator = unicode.classify("　"),
            symbol = unicode.classify("🙂"),
            text_case_fold = text.case_fold("Straße"),
            text_collapse = text.collapse_whitespace("\u{00A0}  Menu\t Start　"),
            text_contains = text.contains("  MENU\t Start ", "menu start", {
                case_fold = true,
                collapse_whitespace = true,
            }),
            text_ends = text.ends_with("Straße", "SSE", { case_fold = true }),
            text_hangul = text.normalize("\u{1100}\u{1161}", "NFC"),
            text_nfd = text.normalize("Ǻ", "NFD") == "A\u{030A}\u{0301}",
            text_nfkc = text.normalize("ﬃ", "NFKC"),
            text_nfkd = text.normalize("①", "NFKD"),
            text_normalized = text.normalize("e\u{0301}", "NFC"),
            text_reordered = text.normalize("a\u{0315}\u{0300}", "NFD")
                == "a\u{0300}\u{0315}",
            text_special_fold = text.case_fold("İΣς"),
            text_starts = text.starts_with("Éclair", "e\u{0301}", {
                case_fold = true,
            }),
            text_trim = text.trim("\u{00A0} Menu 　"),
            tokens = tokens,
            tokens_frozen = tokensFrozen,
            unicode_length = unicode.length("A🙂中"),
            unicode_slice = unicode.slice("A🙂中", 2, 3),
            unicode_version = unicode.unicode_version,
            whitespace = unicode.is_whitespace("　")
                and not unicode.is_whitespace(0x001C),
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
            == R"({"canonical":"{\"a\":2,\"b\":1}","error":"fixture.mapped","error_frozen":true,"frozen":true,"has":true,"internal_data_hidden":true,"invalid_comparator":true,"invalid_filter":true,"invalid_list":true,"invalid_map":true,"invalid_match_option_type":true,"invalid_match_options":true,"invalid_normalization":true,"invalid_result":true,"invalid_text_utf8":true,"invalid_utf8":true,"json_ambiguous_empty":true,"json_appended":"[1,null,3,\"tail\"]","json_bad_number":true,"json_bad_surrogate":true,"json_changed":"{\"added\":true,\"nested\":[1,null,3]}","json_constructed_array":"[]","json_constructed_object":"{}","json_cycle":true,"json_deep":true,"json_duplicate":true,"json_empty_array":"[]","json_empty_object":"{}","json_expanded_scalar":true,"json_invalid_utf8":true,"json_mixed":true,"json_nested_frozen":true,"json_non_finite":true,"json_null_shared":true,"json_parsed_frozen":true,"json_removed_array":"[1,3,\"tail\"]","json_removed_object":"{\"nested\":[1,null,3]}","json_sparse":true,"json_too_large":true,"json_value_count":true,"letter":"letter","mark":"mark","number":"number","other":"other","points":[65,128578,20013],"points_frozen":true,"result":13,"separator":"separator","skipped":0,"sorted":[2,4,6],"split":["a","","b"],"split_compact":["a","b"],"split_frozen":true,"stable":"cab","symbol":"symbol","text_case_fold":"strasse","text_collapse":"Menu Start","text_contains":true,"text_ends":true,"text_hangul":"가","text_nfd":true,"text_nfkc":"ffi","text_nfkd":"1","text_normalized":"é","text_reordered":true,"text_special_fold":"i̇σσ","text_starts":true,"text_trim":"Menu","tokens":["Go",",","中","🙂","42"],"tokens_frozen":true,"unicode_length":3,"unicode_slice":"🙂中","unicode_version":"15.0.0","whitespace":true})"
        );
    }
}
