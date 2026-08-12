#include <script/pure-data-program.hpp>

#include <json/value.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The pure data boundary as a value rather than as bytes, and the identity of
// the environment that boundary runs in. Both were unobservable before: a
// plugin received a byte string and re-derived structure from it, and nothing
// anywhere named the set of globals it could see.
namespace uf::script
{
    namespace
    {
        constexpr auto k_entryPoints = std::array{std::string_view{"derive"}};

        [[nodiscard]]
        auto parsed(std::string_view text) -> json::Value
        {
            auto value = json::parse(text);
            REQUIRE(value.has_value());
            return *std::move(value);
        }

        [[nodiscard]]
        auto pluginReturning(std::string_view moduleId, std::string_view deriveBody) -> std::string
        {
            return "return {\n    plugin_id = \"" + std::string{moduleId}
                 + "\",\n    derive = function(input)\n" + std::string{deriveBody}
                 + "\n    end,\n}\n";
        }

        [[nodiscard]]
        auto artifactOf(std::string name, std::string bytes) -> PureDataProgram::Artifact
        {
            return PureDataProgram::Artifact{
                .name  = std::move(name),
                .bytes = std::move(bytes),
            };
        }

        [[nodiscard]]
        auto derive(std::string_view moduleId,
                    std::string_view deriveBody,
                    json::Value const& input,
                    std::vector<PureDataProgram::Artifact> artifacts = {}) -> Result<json::Value>
        {
            auto const source = pluginReturning(moduleId, deriveBody);
            auto const program =
                PureDataProgram::compile(moduleId, source, k_entryPoints, std::move(artifacts));
            REQUIRE(program.has_value());
            return program->invoke("derive", input);
        }

        [[nodiscard]]
        auto deriveBytes(std::string_view moduleId,
                         std::string_view deriveBody,
                         json::Value const& input,
                         std::vector<PureDataProgram::Artifact> artifacts = {}) -> std::string
        {
            auto const output = derive(moduleId, deriveBody, input, std::move(artifacts));
            REQUIRE(output.has_value());
            return json::canonicalBytes(*output);
        }

        [[nodiscard]]
        auto refusal(std::string_view moduleId,
                     std::string_view deriveBody,
                     json::Value const& input) -> std::string
        {
            auto const output = derive(moduleId, deriveBody, input);
            REQUIRE_FALSE(output.has_value());
            return std::string{output.error().message()};
        }

        // A registration that must not succeed, which refusal() cannot express:
        // that one requires a program and refuses the call it then makes.
        [[nodiscard]]
        auto admissionRefusal(std::string_view moduleId,
                              std::vector<PureDataProgram::Artifact> artifacts) -> std::string
        {
            auto const source = pluginReturning(moduleId, "        return input");
            auto const program =
                PureDataProgram::compile(moduleId, source, k_entryPoints, std::move(artifacts));
            REQUIRE_FALSE(program.has_value());
            return std::string{program.error().message()};
        }
    } // namespace

    // uf-chaos's project-layer design quotes the size of this set as a measured
    // fact, and until this case existed nothing in this repository would have
    // gone red if it changed: the plugin environment has no _G and no getfenv,
    // so a plugin cannot enumerate its own globals and the set was observable
    // only by reading pure-data-program.cpp.
    TEST_CASE("the pure environment publishes exactly the whitelisted globals")
    {
        constexpr auto expected = std::array{
            std::string_view{"assert"},       std::string_view{"error"},
            std::string_view{"getmetatable"}, std::string_view{"ipairs"},
            std::string_view{"next"},         std::string_view{"pairs"},
            std::string_view{"pcall"},        std::string_view{"rawequal"},
            std::string_view{"rawget"},       std::string_view{"rawlen"},
            std::string_view{"rawset"},       std::string_view{"select"},
            std::string_view{"tonumber"},     std::string_view{"tostring"},
            std::string_view{"type"},         std::string_view{"typeof"},
            std::string_view{"unpack"},       std::string_view{"xpcall"},
            std::string_view{"bit32"},        std::string_view{"math"},
            std::string_view{"string"},       std::string_view{"table"},
            std::string_view{"utf8"},
        };

        auto const published = pureEnvironmentGlobals();
        CHECK(published.size() == 23U);
        CHECK(std::ranges::equal(published, expected));

        // The list above says what the whitelist states; this says the VM
        // publishes it. Without the second half the first would pass over a
        // whitelist the environment never applied.
        auto probe = std::string{"        local missing = {}\n"};
        for (auto const name : expected)
        {
            probe += "        if ";
            probe += name;
            probe += " == nil then missing[#missing + 1] = \"";
            probe += name;
            probe += "\" end\n";
        }
        probe += "        if #missing == 0 then return canon.emptyObject end\n";
        probe += "        return { missing = missing }";
        CHECK(derive("fixture.whitelist", probe, parsed("{}")).has_value());
        CHECK(deriveBytes("fixture.whitelist", probe, parsed("{}")) == "{}");
    }

    // The negative control for the case above: names the whitelist does not
    // carry are absent even though the same libraries define them.
    TEST_CASE("the pure environment publishes nothing outside the whitelist")
    {
        constexpr auto probe = std::string_view{
            R"(        local reachable = require ~= nil or load ~= nil or loadstring ~= nil
            or dofile ~= nil or debug ~= nil or _G ~= nil or os ~= nil or io ~= nil
            or coroutine ~= nil or newproxy ~= nil or collectgarbage ~= nil
            or getfenv ~= nil or setfenv ~= nil or setmetatable ~= nil
            or buffer ~= nil or vector ~= nil or print ~= nil or _VERSION ~= nil
        return { reachable = reachable })"
        };
        CHECK(deriveBytes("fixture.denied", probe, parsed("{}")) == "{\"reachable\":false}");
    }

    TEST_CASE("published tostring has no pointer-derived spelling")
    {
        constexpr auto probe = std::string_view{R"(
        return {
            boolean = tostring(true),
            callable = tostring(function() end),
            nil_value = tostring(nil),
            number = tostring(12.5),
            object = tostring({}),
            text = tostring("value"),
        })"};
        CHECK(
            deriveBytes("fixture.deterministic-tostring", probe, parsed("{}"))
            == R"({"boolean":"true","callable":"function","nil_value":"nil","number":"12.5","object":"table","text":"value"})"
        );
    }

    // Beside the whitelist stand exactly two frozen tables, and this is what
    // says frozen is a property of the objects rather than of the comment on
    // them.
    TEST_CASE("the input value and the published tables are frozen")
    {
        constexpr auto probe = std::string_view{
            R"(        local wrote = pcall(function() input.request = "no" end)
        local raw_wrote = pcall(function() rawset(input, "extra", 1) end)
        local nested = pcall(function() input.nested[1] = 9 end)
        local canon_wrote = pcall(function() canon.null = 1 end)
        local canon_raw = pcall(function() rawset(canon, "extra", 1) end)
        local sentinel_raw = pcall(function() rawset(canon.null, "extra", 1) end)
        local empty_raw = pcall(function() rawset(canon.emptyObject, "extra", 1) end)
        local artifact_wrote = pcall(function() artifact.read = function() return 1 end end)
        return {
            frozen = not wrote and not raw_wrote and not nested and not canon_wrote
                and not canon_raw and not sentinel_raw and not empty_raw
                and not artifact_wrote,
            unchanged = input.request == "ok" and input.nested[1] == 1,
        })"
        };
        CHECK(
            deriveBytes("fixture.frozen", probe, parsed(R"({"nested":[1,2],"request":"ok"})"))
            == R"({"frozen":true,"unchanged":true})"
        );

        // The same write without pcall around it. A plugin that mutates what it
        // was handed does not quietly hand back a changed document: the whole
        // call is refused.
        auto const mutated = refusal(
            "fixture.mutating",
            "        input.request = \"no\"\n        return input",
            parsed(R"({"request":"ok"})")
        );
        CHECK(mutated.find("pure data program failed") != std::string::npos);
        CHECK(mutated.find("readonly") != std::string::npos);
    }

    // The two JSON values Lua cannot spell for itself. Both directions use the
    // same two objects, so a document that goes in comes back out unchanged --
    // which a boundary that mapped an empty object to an empty array could not
    // do.
    TEST_CASE("null and the empty object survive both directions")
    {
        constexpr auto k_document =
            std::string_view{R"({"empty":{},"list":[],"nested":{"a":null},"nothing":null})"};
        CHECK(deriveBytes("fixture.echo", "        return input", parsed(k_document))
              == k_document);

        constexpr auto recognize = std::string_view{
            R"(        return {
            null_is_sentinel = rawequal(input.nothing, canon.null),
            empty_is_sentinel = rawequal(input.empty, canon.emptyObject),
            list_is_plain = not rawequal(input.list, canon.emptyObject)
                and next(input.list) == nil,
        })"
        };
        CHECK(
            deriveBytes("fixture.sentinels", recognize, parsed(k_document))
            == R"({"empty_is_sentinel":true,"list_is_plain":true,"null_is_sentinel":true})"
        );

        constexpr auto mint = std::string_view{
            R"(        return { a = canon.null, b = canon.emptyObject, c = {} })"
        };
        CHECK(deriveBytes("fixture.mint", mint, parsed("{}")) == R"({"a":null,"b":{},"c":[]})");
    }

    // A plugin no longer emits bytes, so it cannot emit non-canonical bytes.
    // What it can still do is hand back a Lua value no JSON document has, and
    // each of those is refused by name.
    TEST_CASE("a returned value that no JSON document has is refused")
    {
        auto const empty = parsed("{}");
        CHECK(
            refusal("fixture.mixed", "        return { 1, a = 2 }", empty)
                .find("table that is neither array nor object")
            != std::string::npos
        );
        CHECK(
            refusal("fixture.sparse", "        return { [1] = 1, [3] = 3 }", empty)
                .find("sparse table no JSON array has")
            != std::string::npos
        );
        CHECK(
            refusal("fixture.callable", "        return { f = function() return 1 end }", empty)
                .find("type no JSON document has")
            != std::string::npos
        );
        CHECK(
            refusal("fixture.nan", "        return { n = 0 / 0 }", empty)
                .find("number no JSON document can spell")
            != std::string::npos
        );
        CHECK(
            refusal("fixture.badname", "        return { [\"\\255\"] = 1 }", empty)
                .find("member name that is not UTF-8")
            != std::string::npos
        );
        CHECK(
            refusal("fixture.badtext", "        return { s = \"\\255\" }", empty)
                .find("string that is not UTF-8")
            != std::string::npos
        );
        CHECK(
            refusal("fixture.scalar", "        return nil", empty)
                .find("must exchange decoded JSON values")
            != std::string::npos
        );
    }

    // The ceilings the byte boundary used to carry, restated over the value.
    TEST_CASE("a returned value has fixed depth and byte ceilings")
    {
        auto const empty = parsed("{}");
        constexpr auto deep = std::string_view{
            R"(        local value = {}
        for _ = 1, 70 do value = { value } end
        return value)"
        };
        CHECK(
            refusal("fixture.deep", deep, empty).find("fixed nesting ceiling")
            != std::string::npos
        );

        constexpr auto wide = std::string_view{
            R"(        return { blob = string.rep("x", 1024 * 1024 + 1) })"
        };
        CHECK(
            refusal("fixture.wide", wide, empty).find("fixed byte ceiling")
            != std::string::npos
        );
    }

    // What a plugin is handed when it reads an artifact. The environment
    // publishes no JSON decoder and a consuming project ships no C++, so bytes
    // carrying JSON were a capability no plugin could express. Each half of
    // what replaced them is probed on its own: one plugin returning three flags
    // goes red at the same assertion whichever of the three broke.
    TEST_CASE("an artifact is handed over decoded, frozen, and once per VM")
    {
        constexpr auto k_map =
            std::string_view{R"({"depth":{"summit":3},"regions":["harbour","pass"]})"};
        auto const empty = parsed("{}");

        constexpr auto decoded = std::string_view{
            R"(        local map = artifact.read("map")
        return {
            decoded = type(map) == "table" and map.regions[2] == "pass"
                and map.depth.summit == 3,
        })"
        };
        CHECK(
            deriveBytes("fixture.decoded", decoded, empty, {artifactOf("map", std::string{k_map})})
            == R"({"decoded":true})"
        );

        constexpr auto frozen = std::string_view{
            R"(        local map = artifact.read("map")
        local wrote = pcall(function() map.regions[1] = "moved" end)
        local raw_wrote = pcall(function() rawset(map, "extra", 1) end)
        local nested = pcall(function() map.depth.summit = 9 end)
        return { frozen = not wrote and not raw_wrote and not nested })"
        };
        CHECK(
            deriveBytes("fixture.frozen-map", frozen, empty, {artifactOf("map", std::string{k_map})})
            == R"({"frozen":true})"
        );

        // One artifact is one value per VM, so a plugin that reads it twice
        // gets the object it already holds rather than a second copy charged
        // against the same memory quota.
        constexpr auto once = std::string_view{
            R"(        return { same = rawequal(artifact.read("map"), artifact.read("map")) })"
        };
        CHECK(
            deriveBytes("fixture.once", once, empty, {artifactOf("map", std::string{k_map})})
            == R"({"same":true})"
        );
    }

    // Admission is two-stage and this is the first stage. Registration is where
    // a document that cannot become a value is refused, because the bytes are
    // pinned by the artifact root hash: their value is a fact about the
    // registration, so a call must never be the first to discover there is not
    // one.
    TEST_CASE("an artifact is admitted only if it is JSON this VM can build")
    {
        CHECK(
            admissionRefusal("fixture.notjson",
                             {artifactOf("map", "expedition-map-bytes")})
                .find("pure data artifact is not JSON")
            != std::string::npos
        );

        // Valid JSON, inside every byte ceiling, and beyond what a fresh VM may
        // allocate: 400,000 empty arrays are 1.14 MiB of text and one Luau
        // table each. Repeated bytes would have been refused by the parse
        // instead, which is a refusal this case is not about.
        auto wide = std::string{"["};
        for (auto index = std::size_t{0}; index < 400000U; ++index)
        {
            wide += index == 0U ? "[]" : ",[]";
        }
        wide += "]";
        CHECK(
            admissionRefusal("fixture.wide-artifact", {artifactOf("map", std::move(wide))})
                .find("cannot be materialized inside its VM quota")
            != std::string::npos
        );
    }

    TEST_CASE("quota exhaustion while the host pushes an input is a refusal")
    {
        // 300,000 empty arrays spell less than one MiB of JSON, yet require one
        // Luau table apiece. The quota is reached while pushValue is preparing
        // the argument, before the plugin's entry point can be resumed.
        auto items = std::vector<json::Value>{};
        items.reserve(300'000U);
        for (auto index = std::size_t{0}; index < 300'000U; ++index)
        {
            items.emplace_back(json::Value::ofArray({}));
        }
        auto const input = json::Value::ofArray(std::move(items));
        auto const output = derive(
            "fixture.host-push-quota",
            "        return input",
            input
        );
        REQUIRE_FALSE(output.has_value());
        CHECK(std::string{output.error().message()}.find("memory quota")
              != std::string::npos);
    }

    // The pin change 2 rests on. The literal is what a framework upgrade has to
    // move deliberately: the bridge source, the whitelist, the frozen table
    // surface and the contract each published function answers to are its whole
    // preimage, so one byte anywhere in them lands here.
    TEST_CASE("the plugin environment has one pinned identity")
    {
        auto const first  = pluginEnvironmentHash();
        auto const second = pluginEnvironmentHash();
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        CHECK(*first == *second);

        // That the digest moved is one claim; what it covers is another, and
        // only this one names it. A preimage over published NAMES alone leaves
        // a build that changed what artifact.read RETURNS with an unmoved
        // digest, and every session_manifest_hash and decision_basis_hash
        // beneath it unmoved with it -- exactly the upgrade the pin exists to
        // catch.
        CHECK(
            pluginEnvironmentMaterial().find(R"("artifact.read":"decoded_json_value_v1")")
            != std::string::npos
        );
        CHECK(pluginEnvironmentMaterial().find(
                  R"("tostring":"json_scalar_or_type_name_v1")")
              != std::string::npos);
        CHECK(pluginEnvironmentMaterial().find(
                  R"("luau_implementation":"luau-0.730+5bc7f4b23756f69f4669b419fa9034f117ccd6fe")")
              != std::string::npos);
        CHECK(
            first->hex()
            == "0eca9b6b1d364eee24aa0c7b6500010b96d5656365b79d718d5add9963a55dee"
        );
    }
} // namespace uf::script
