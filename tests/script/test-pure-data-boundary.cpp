#include <script/pure-data-program.hpp>
#include <script/ffi/pure-data-admission.hpp>

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
        auto pluginReturning(std::string_view pluginId, std::string_view deriveBody) -> std::string
        {
            return "return {\n    plugin_id = \"" + std::string{pluginId}
                 + "\",\n    derive = function(input)\n" + std::string{deriveBody}
                 + "\n    end,\n}\n";
        }

        [[nodiscard]]
        auto resourceOf(PureDataProgram::ResourceKind kind,
                        std::string name,
                        std::string bytes) -> PureDataProgram::Resource
        {
            return PureDataProgram::Resource{
                .kind  = kind,
                .name  = std::move(name),
                .bytes = std::move(bytes),
            };
        }

        [[nodiscard]]
        auto compileProgram(
            std::string_view pluginId,
            std::string_view entryModule,
            std::vector<PureDataProgram::Module> modules,
            std::vector<PureDataProgram::Resource> resources = {}
        ) -> Result<PureDataProgram>
        {
            return PureDataProgram::compile(
                pluginId,
                entryModule,
                std::move(modules),
                k_entryPoints,
                std::move(resources)
            );
        }

        [[nodiscard]]
        auto derive(std::string_view pluginId,
                    std::string_view deriveBody,
                    json::Value const& input,
                    std::vector<PureDataProgram::Resource> resources = {}) -> Result<json::Value>
        {
            auto const source = pluginReturning(pluginId, deriveBody);
            auto modules      = std::vector<PureDataProgram::Module>{
                PureDataProgram::Module{.name = "main", .source = source},
            };
            auto const program =
                compileProgram(pluginId, "main", std::move(modules), std::move(resources));
            REQUIRE(program.has_value());
            return program->invoke("derive", input);
        }

        [[nodiscard]]
        auto deriveBytes(std::string_view pluginId,
                         std::string_view deriveBody,
                         json::Value const& input,
                         std::vector<PureDataProgram::Resource> resources = {}) -> std::string
        {
            auto const output = derive(pluginId, deriveBody, input, std::move(resources));
            REQUIRE(output.has_value());
            return json::canonicalBytes(*output);
        }

        [[nodiscard]]
        auto refusal(std::string_view pluginId,
                     std::string_view deriveBody,
                     json::Value const& input) -> std::string
        {
            auto const output = derive(pluginId, deriveBody, input);
            REQUIRE_FALSE(output.has_value());
            return std::string{output.error().message()};
        }

        // A registration that must not succeed, which refusal() cannot express:
        // that one requires a program and refuses the call it then makes.
        [[nodiscard]]
        auto admissionRefusal(std::string_view pluginId,
                              std::vector<PureDataProgram::Resource> resources) -> std::string
        {
            auto modules = std::vector<PureDataProgram::Module>{
                PureDataProgram::Module{
                    .name   = "main",
                    .source = pluginReturning(pluginId, "        return input"),
                },
            };
            auto const program =
                compileProgram(pluginId, "main", std::move(modules), std::move(resources));
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
            std::string_view{"rawset"},       std::string_view{"require"},
            std::string_view{"select"},       std::string_view{"tonumber"},
            std::string_view{"tostring"},     std::string_view{"type"},
            std::string_view{"typeof"},       std::string_view{"unpack"},
            std::string_view{"xpcall"},       std::string_view{"bit32"},
            std::string_view{"math"},         std::string_view{"string"},
            std::string_view{"table"},        std::string_view{"utf8"},
        };

        auto const published = pureEnvironmentGlobals();
        CHECK(published.size() == 24U);
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
            R"(        local reachable = load ~= nil or loadstring ~= nil or dofile ~= nil
            or debug ~= nil or _G ~= nil or os ~= nil or io ~= nil or package ~= nil
            or game ~= nil or workspace ~= nil or socket ~= nil
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
        local resource_wrote = pcall(function() resource.readJson = function() return 1 end end)
        return {
            frozen = not wrote and not raw_wrote and not nested and not canon_wrote
                and not canon_raw and not sentinel_raw and not empty_raw
                and not resource_wrote,
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

    TEST_CASE("a closed module graph resolves root and relative names once per fresh VM")
    {
        auto const program = compileProgram(
            "fixture.modules",
            "app/main",
            {
                PureDataProgram::Module{
                    .name   = "shared",
                    .source = "return { value = 'parent' }",
                },
                PureDataProgram::Module{
                    .name   = "lib/root",
                    .source = R"LUAU(
local load_count = 0
load_count += 1
return {
    value = "root",
    load_count = function() return load_count end,
}
)LUAU",
                },
                PureDataProgram::Module{
                    .name   = "app/sibling",
                    .source = "return { value = 'sibling' }",
                },
                PureDataProgram::Module{
                    .name = "app/main",
                    .source = R"LUAU(
local root = require("lib/root")
local sibling = require("./sibling")
local parent = require("../shared")
local computed = "../lib/" .. "root"
local same_root = require(computed)
return {
    plugin_id = "fixture.modules",
    derive = function(_input)
        return {
            root = root.value,
            sibling = sibling.value,
            parent = parent.value,
            same = rawequal(root, same_root),
            load_count = root.load_count(),
        }
    end,
}
)LUAU",
                },
            }
        );
        REQUIRE(program.has_value());

        constexpr auto expected = std::string_view{
            R"({"load_count":1,"parent":"parent","root":"root","same":true,"sibling":"sibling"})"
        };
        auto const first = program->invoke("derive", parsed("{}"));
        auto const second = program->invoke("derive", parsed("{}"));
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        CHECK(json::canonicalBytes(*first) == expected);
        CHECK(json::canonicalBytes(*second) == expected);
    }

    TEST_CASE("module environments keep require caller-bound and frozen")
    {
        SUBCASE("a direct replacement is refused")
        {
            auto const program = compileProgram(
                "fixture.frozen-require-direct",
                "main",
                {
                    PureDataProgram::Module{
                        .name = "main",
                        .source = R"LUAU(
require = function() return { value = "forged" } end
return {
    plugin_id = "fixture.frozen-require-direct",
    derive = function(input) return input end,
}
)LUAU",
                    },
                }
            );
            REQUIRE_FALSE(program.has_value());
            CHECK(std::string{program.error().message()}.find("readonly")
                  != std::string::npos);
        }

        SUBCASE("pcall cannot replace the caller-bound function")
        {
            auto const program = compileProgram(
                "fixture.frozen-require-pcall",
                "main",
                {
                    PureDataProgram::Module{
                        .name = "main",
                        .source = R"LUAU(
local original_require = require
local replaced = pcall(function()
    require = function() return { value = "forged" } end
end)
local dependency = require("dependency")
return {
    plugin_id = "fixture.frozen-require-pcall",
    derive = function()
        return {
            replacement_refused = not replaced,
            binding_unchanged = rawequal(require, original_require),
            value = dependency.value,
        }
    end,
}
)LUAU",
                    },
                    PureDataProgram::Module{
                        .name   = "dependency",
                        .source = "return { value = 'registered' }",
                    },
                }
            );
            REQUIRE(program.has_value());
            auto const result = program->invoke("derive", parsed("{}"));
            REQUIRE(result.has_value());
            CHECK(
                json::canonicalBytes(*result)
                == R"({"binding_unchanged":true,"replacement_refused":true,"value":"registered"})"
            );
        }
    }

    TEST_CASE("computed require first loads an unreached module during invocation")
    {
        SUBCASE("the resolved module is cached once in each fresh invocation VM")
        {
            auto const program = compileProgram(
                "fixture.dynamic-module",
                "main",
                {
                    PureDataProgram::Module{
                        .name = "main",
                        .source = R"LUAU(
return {
    plugin_id = "fixture.dynamic-module",
    derive = function(input)
        local first = require(input.module)
        local second = require("./later")
        return {
            same = rawequal(first, second),
            load_count = first.load_count(),
        }
    end,
}
)LUAU",
                    },
                    PureDataProgram::Module{
                        .name = "later",
                        .source = R"LUAU(
local count = 0
count += 1
return { load_count = function() return count end }
)LUAU",
                    },
                }
            );
            REQUIRE(program.has_value());
            constexpr auto expected = std::string_view{R"({"load_count":1,"same":true})"};
            for (auto const input : {R"({"module":"later"})", R"({"module":"./later"})"})
            {
                auto const result = program->invoke("derive", parsed(input));
                REQUIRE(result.has_value());
                CHECK(json::canonicalBytes(*result) == expected);
            }
        }

        SUBCASE("a deterministic invocation-time failure is cached")
        {
            auto const program = compileProgram(
                "fixture.dynamic-failure",
                "main",
                {
                    PureDataProgram::Module{
                        .name = "main",
                        .source = R"LUAU(
return {
    plugin_id = "fixture.dynamic-failure",
    derive = function(input)
        local first_ok, first_error = pcall(function() return require(input.module) end)
        local second_ok, second_error = pcall(function() return require(input.module) end)
        return {
            refused = not first_ok and not second_ok,
            same = first_error == second_error,
            first_attempt = string.find(first_error, "dynamic refusal 1", 1, true) ~= nil,
        }
    end,
}
)LUAU",
                    },
                    PureDataProgram::Module{
                        .name = "broken",
                        .source = R"LUAU(
local state = require("state")
state.attempt += 1
error("stable dynamic refusal " .. state.attempt, 0)
)LUAU",
                    },
                    PureDataProgram::Module{
                        .name   = "state",
                        .source = "return { attempt = 0 }",
                    },
                }
            );
            REQUIRE(program.has_value());
            auto const result =
                program->invoke("derive", parsed(R"({"module":"broken"})"));
            REQUIRE(result.has_value());
            CHECK(
                json::canonicalBytes(*result)
                == R"({"first_attempt":true,"refused":true,"same":true})"
            );
        }

        SUBCASE("an invocation-time cancellation remains terminal across pcall")
        {
            auto const program = compileProgram(
                "fixture.dynamic-terminal",
                "main",
                {
                    PureDataProgram::Module{
                        .name = "main",
                        .source = R"LUAU(
return {
    plugin_id = "fixture.dynamic-terminal",
    derive = function(input)
        pcall(function() return require(input.module) end)
        return input
    end,
}
)LUAU",
                    },
                    PureDataProgram::Module{
                        .name   = "runaway",
                        .source = "while true do end",
                    },
                }
            );
            REQUIRE(program.has_value());
            auto const result =
                program->invoke("derive", parsed(R"({"module":"runaway"})"));
            REQUIRE_FALSE(result.has_value());
            CHECK(std::string{result.error().message()}.find("hard-cancelled")
                  != std::string::npos);
        }
    }

    TEST_CASE("module failures are canonical, bounded, and closed over the registered graph")
    {
        SUBCASE("a missing dependency is refused")
        {
            auto const program = compileProgram(
                "fixture.missing-module",
                "main",
                {
                    PureDataProgram::Module{
                        .name = "main",
                        .source = R"LUAU(
local missing = require("missing")
return { plugin_id = "fixture.missing-module", derive = function() return missing end }
)LUAU",
                    },
                }
            );
            REQUIRE_FALSE(program.has_value());
            CHECK(std::string{program.error().message()}.find("unknown module")
                  != std::string::npos);
        }

        SUBCASE("a dependency cycle is refused")
        {
            auto const selfCycle = compileProgram(
                "fixture.module-self-cycle",
                "main",
                {
                    PureDataProgram::Module{
                        .name = "main",
                        .source = R"LUAU(
local value = require("main")
return { plugin_id = "fixture.module-self-cycle", derive = function() return value end }
)LUAU",
                    },
                }
            );
            REQUIRE_FALSE(selfCycle.has_value());
            CHECK(std::string{selfCycle.error().message()}.find("dependency cycle")
                  != std::string::npos);

            auto const program = compileProgram(
                "fixture.module-cycle",
                "main",
                {
                    PureDataProgram::Module{
                        .name   = "main",
                        .source = R"LUAU(
local value = require("a")
return { plugin_id = "fixture.module-cycle", derive = function() return value end }
)LUAU",
                    },
                    PureDataProgram::Module{
                        .name   = "a",
                        .source = "return require('b')",
                    },
                    PureDataProgram::Module{
                        .name   = "b",
                        .source = "return require('a')",
                    },
                }
            );
            REQUIRE_FALSE(program.has_value());
            CHECK(std::string{program.error().message()}.find("dependency cycle")
                  != std::string::npos);
        }

        SUBCASE("requests are not normalized and cannot traverse above root")
        {
            for (auto const request : {"./a/../b", "../outside"})
            {
                auto const source = std::string{"local value = require('"} + request
                                  + "')\nreturn { plugin_id = 'fixture.bad-request', "
                                    "derive = function() return value end }";
                auto const program = compileProgram(
                    "fixture.bad-request",
                    "main",
                    {PureDataProgram::Module{.name = "main", .source = source}}
                );
                REQUIRE_FALSE(program.has_value());
                auto const message = std::string{program.error().message()};
                auto const isGrammarRefusal =
                    message.find("not canonical") != std::string::npos
                    || message.find("above its logical root") != std::string::npos;
                CHECK(isGrammarRefusal);
            }
        }

        SUBCASE("duplicate and absent entry modules are refused before execution")
        {
            auto const duplicate = compileProgram(
                "fixture.duplicate-module",
                "main",
                {
                    PureDataProgram::Module{.name = "main", .source = "return {}"},
                    PureDataProgram::Module{.name = "main", .source = "return {}"},
                }
            );
            REQUIRE_FALSE(duplicate.has_value());
            CHECK(std::string{duplicate.error().message()}.find("must be unique")
                  != std::string::npos);

            auto const absent = compileProgram(
                "fixture.absent-entry",
                "main",
                {PureDataProgram::Module{.name = "other", .source = "return {}"}}
            );
            REQUIRE_FALSE(absent.has_value());
            CHECK(std::string{absent.error().message()}.find("absent from its closure")
                  != std::string::npos);
        }

        SUBCASE("an unreached module with invalid syntax is refused at admission")
        {
            auto const program = compileProgram(
                "fixture.invalid-unreached-module",
                "main",
                {
                    PureDataProgram::Module{
                        .name = "main",
                        .source = pluginReturning(
                            "fixture.invalid-unreached-module",
                            "        return input"
                        ),
                    },
                    PureDataProgram::Module{
                        .name   = "unreached",
                        .source = "local = broken syntax",
                    },
                }
            );
            REQUIRE_FALSE(program.has_value());
            CHECK(std::string{program.error().message()}.find(
                      "source failed to compile: unreached"
                  )
                  != std::string::npos);
        }

        SUBCASE("a deterministic dependency failure is cached for plugin pcall")
        {
            auto const program = compileProgram(
                "fixture.cached-failure",
                "main",
                {
                    PureDataProgram::Module{
                        .name = "main",
                        .source = R"LUAU(
local first_ok, first_error = pcall(function() return require("broken") end)
local second_ok, second_error = pcall(function() return require("broken") end)
return {
    plugin_id = "fixture.cached-failure",
    derive = function()
        return {
            refused = not first_ok and not second_ok,
            same = first_error == second_error,
            first_attempt = string.find(first_error, "dependency refusal 1", 1, true) ~= nil,
        }
    end,
}
)LUAU",
                    },
                    PureDataProgram::Module{
                        .name = "broken",
                        .source = R"LUAU(
local state = require("state")
state.attempt += 1
error("stable dependency refusal " .. state.attempt, 0)
)LUAU",
                    },
                    PureDataProgram::Module{
                        .name   = "state",
                        .source = "return { attempt = 0 }",
                    },
                }
            );
            REQUIRE(program.has_value());
            auto const result = program->invoke("derive", parsed("{}"));
            REQUIRE(result.has_value());
            CHECK(
                json::canonicalBytes(*result)
                == R"({"first_attempt":true,"refused":true,"same":true})"
            );
        }

        SUBCASE("a dependency instruction cancellation is terminal across plugin pcall")
        {
            auto const program = compileProgram(
                "fixture.terminal-module-cancel",
                "main",
                {
                    PureDataProgram::Module{
                        .name = "main",
                        .source = R"LUAU(
pcall(function() return require("runaway") end)
return {
    plugin_id = "fixture.terminal-module-cancel",
    derive = function(input) return input end,
}
)LUAU",
                    },
                    PureDataProgram::Module{
                        .name   = "runaway",
                        .source = "while true do end",
                    },
                }
            );
            REQUIRE_FALSE(program.has_value());
            CHECK(std::string{program.error().message()}.find("hard-cancelled")
                  != std::string::npos);
        }

        SUBCASE("a dependency memory failure is terminal across plugin pcall")
        {
            auto const program = compileProgram(
                "fixture.terminal-module-memory",
                "main",
                {
                    PureDataProgram::Module{
                        .name = "main",
                        .source = R"LUAU(
pcall(function() return require("hungry") end)
return {
    plugin_id = "fixture.terminal-module-memory",
    derive = function(input) return input end,
}
)LUAU",
                    },
                    PureDataProgram::Module{
                        .name = "hungry",
                        .source = R"LUAU(
local values = {}
while true do
    values[#values + 1] = string.rep("x", 1024)
end
)LUAU",
                    },
                }
            );
            REQUIRE_FALSE(program.has_value());
            CHECK(std::string{program.error().message()}.find("memory quota")
                  != std::string::npos);
        }
    }

    TEST_CASE("module names and require requests use only the exact ASCII grammar")
    {
        auto overlongSegment = std::string(65U, 'a');
        auto tooManySegments = std::string{"a"};
        for (auto index = std::size_t{1U}; index < 17U; ++index)
        {
            tooManySegments += "/a";
        }
        auto overlongName = std::string(16U, 'a');
        for (auto index = std::size_t{1U}; index < 16U; ++index)
        {
            overlongName += '/' + std::string(16U, 'a');
        }

        auto invalidNames = std::vector<std::string>{
            "Upper",
            "1lead",
            "a//b",
            "a.b",
            "a\\b",
            "/a",
            "a/",
            std::string{"a\nb"},
            std::string{"a\xc3\xa9"},
            std::move(overlongSegment),
            std::move(tooManySegments),
            std::move(overlongName),
        };
        for (auto const& invalidName : invalidNames)
        {
            auto const program = compileProgram(
                "fixture.invalid-module-name",
                "main",
                {
                    PureDataProgram::Module{
                        .name = "main",
                        .source = pluginReturning(
                            "fixture.invalid-module-name",
                            "        return input"
                        ),
                    },
                    PureDataProgram::Module{
                        .name   = invalidName,
                        .source = "return true",
                    },
                }
            );
            REQUIRE_FALSE(program.has_value());
            CHECK(std::string{program.error().message()}.find("name is not canonical")
                  != std::string::npos);
        }

        auto invalidRequests = std::vector<std::string>{
            "",
            "Upper",
            "1lead",
            "a//b",
            "a.b",
            "a\\b",
            "/a",
            "a/",
            "./",
            "../",
            "./a/../b",
            "a.lua",
            std::string(257U, 'a'),
        };
        for (auto const& invalidRequest : invalidRequests)
        {
            auto const source = std::string{"local value = require("}
                              + '"' + invalidRequest
                              + "\")\nreturn {\n"
                                "    plugin_id = \"fixture.invalid-module-request\",\n"
                                "    derive = function() return value end,\n"
                                "}";
            auto const program = compileProgram(
                "fixture.invalid-module-request",
                "main",
                {PureDataProgram::Module{.name = "main", .source = source}}
            );
            REQUIRE_FALSE(program.has_value());
            auto const message = std::string{program.error().message()};
            auto const isRequestRefusal =
                message.find("not canonical") != std::string::npos
                || message.find("not a bounded module name") != std::string::npos;
            CHECK(isRequestRefusal);
        }

        for (auto const requestExpression : {
                 R"("a" .. string.char(1) .. "b")",
                 R"(string.char(0xc3, 0xa9))",
             })
        {
            auto const source = std::string{"local value = require("}
                              + requestExpression
                              + R"()
return {
    plugin_id = "fixture.invalid-module-request-bytes",
    derive = function() return value end,
})";
            auto const program = compileProgram(
                "fixture.invalid-module-request-bytes",
                "main",
                {PureDataProgram::Module{.name = "main", .source = source}}
            );
            REQUIRE_FALSE(program.has_value());
            CHECK(std::string{program.error().message()}.find("not canonical")
                  != std::string::npos);
        }

        for (auto const call : {"require()", "require({})", "require('other', 'extra')"})
        {
            auto const source = std::string{"local value = "} + call
                              + R"(
return {
    plugin_id = "fixture.invalid-require-call",
    derive = function() return value end,
})";
            auto const program = compileProgram(
                "fixture.invalid-require-call",
                "main",
                {
                    PureDataProgram::Module{.name = "main", .source = source},
                    PureDataProgram::Module{.name = "other", .source = "return true"},
                }
            );
            REQUIRE_FALSE(program.has_value());
            CHECK(std::string{program.error().message()}.find("exactly one module-name string")
                  != std::string::npos);
        }
    }

    TEST_CASE("entry and dependency modules enforce distinct return contracts")
    {
        auto const acceptedFalse = compileProgram(
            "fixture.false-dependency",
            "main",
            {
                PureDataProgram::Module{
                    .name = "main",
                    .source = R"LUAU(
local flag = require("flag")
return {
    plugin_id = "fixture.false-dependency",
    derive = function() return { accepted = flag == false } end,
}
)LUAU",
                },
                PureDataProgram::Module{.name = "flag", .source = "return false"},
            }
        );
        REQUIRE(acceptedFalse.has_value());
        auto const accepted = acceptedFalse->invoke("derive", parsed("{}"));
        REQUIRE(accepted.has_value());
        CHECK(json::canonicalBytes(*accepted) == R"({"accepted":true})");

        for (auto const dependencySource : {"return nil", "return 1, 2"})
        {
            auto const program = compileProgram(
                "fixture.bad-dependency-return",
                "main",
                {
                    PureDataProgram::Module{
                        .name = "main",
                        .source = R"LUAU(
local value = require("dependency")
return {
    plugin_id = "fixture.bad-dependency-return",
    derive = function() return value end,
}
)LUAU",
                    },
                    PureDataProgram::Module{
                        .name   = "dependency",
                        .source = dependencySource,
                    },
                }
            );
            REQUIRE_FALSE(program.has_value());
            CHECK(std::string{program.error().message()}.find(
                      "dependency must return exactly one non-nil value"
                  )
                  != std::string::npos);
        }

        auto const scalarEntry = compileProgram(
            "fixture.scalar-entry",
            "main",
            {PureDataProgram::Module{.name = "main", .source = "return 1"}}
        );
        REQUIRE_FALSE(scalarEntry.has_value());
        CHECK(std::string{scalarEntry.error().message()}.find(
                  "entry module must return exactly one table"
              )
              != std::string::npos);
    }

    TEST_CASE("module and resource closure admission enforces every public byte ceiling")
    {
        auto const oversizedSource = compileProgram(
            "fixture.module-source-ceiling",
            "main",
            {
                PureDataProgram::Module{
                    .name   = "main",
                    .source = std::string(
                        PureDataProgram::k_maximumModuleSourceBytes + 1U,
                        ' '
                    ),
                },
            }
        );
        REQUIRE_FALSE(oversizedSource.has_value());
        CHECK(std::string{oversizedSource.error().message()}.find("bounded UTF-8")
              != std::string::npos);

        auto sourceClosure = std::vector<PureDataProgram::Module>{};
        for (auto index = std::size_t{0U}; index < 17U; ++index)
        {
            sourceClosure.emplace_back(PureDataProgram::Module{
                .name   = "m" + std::to_string(index),
                .source = std::string(PureDataProgram::k_maximumModuleSourceBytes, ' '),
            });
        }
        auto const oversizedSourceClosure = compileProgram(
            "fixture.module-source-total",
            "m0",
            std::move(sourceClosure)
        );
        REQUIRE_FALSE(oversizedSourceClosure.has_value());
        CHECK(std::string{oversizedSourceClosure.error().message()}.find(
                  "total byte ceiling"
              )
              != std::string::npos);

        auto moduleClosure = std::vector<PureDataProgram::Module>{
            PureDataProgram::Module{
                .name = "main",
                .source = pluginReturning(
                    "fixture.module-count",
                    "        return input"
                ),
            },
        };
        for (auto index = std::size_t{0U}; index < 64U; ++index)
        {
            moduleClosure.emplace_back(PureDataProgram::Module{
                .name   = "m" + std::to_string(index),
                .source = "return true",
            });
        }
        auto const tooManyModules = compileProgram(
            "fixture.module-count",
            "main",
            std::move(moduleClosure)
        );
        REQUIRE_FALSE(tooManyModules.has_value());
        CHECK(std::string{tooManyModules.error().message()}.find("module count")
              != std::string::npos);

        auto const mainSource = pluginReturning(
            "fixture.resource-limits",
            "        return input"
        );
        auto resources = std::vector<PureDataProgram::Resource>{};
        for (auto index = std::size_t{0U}; index < 65U; ++index)
        {
            resources.emplace_back(resourceOf(
                PureDataProgram::ResourceKind::Bytes,
                "r" + std::to_string(index),
                {}
            ));
        }
        auto const tooManyResources = compileProgram(
            "fixture.resource-limits",
            "main",
            {PureDataProgram::Module{.name = "main", .source = mainSource}},
            std::move(resources)
        );
        REQUIRE_FALSE(tooManyResources.has_value());
        CHECK(std::string{tooManyResources.error().message()}.find("resource count")
              != std::string::npos);

        auto const oversizedResource = compileProgram(
            "fixture.resource-limits",
            "main",
            {PureDataProgram::Module{.name = "main", .source = mainSource}},
            {resourceOf(
                PureDataProgram::ResourceKind::Bytes,
                "large",
                std::string(PureDataProgram::k_maximumResourceBytes + 1U, 'x')
            )}
        );
        REQUIRE_FALSE(oversizedResource.has_value());
        CHECK(std::string{oversizedResource.error().message()}.find(
                  "resource exceeds its fixed byte ceiling"
              )
              != std::string::npos);

        resources.clear();
        for (auto index = std::size_t{0U}; index < 5U; ++index)
        {
            resources.emplace_back(resourceOf(
                PureDataProgram::ResourceKind::Bytes,
                "r" + std::to_string(index),
                std::string(PureDataProgram::k_maximumResourceBytes, 'x')
            ));
        }
        auto const oversizedResourceClosure = compileProgram(
            "fixture.resource-limits",
            "main",
            {PureDataProgram::Module{.name = "main", .source = mainSource}},
            std::move(resources)
        );
        REQUIRE_FALSE(oversizedResourceClosure.has_value());
        CHECK(std::string{oversizedResourceClosure.error().message()}.find(
                  "resources exceed their total byte ceiling"
              )
              != std::string::npos);

        using detail::ModuleBytecodeAdmission;
        CHECK(
            detail::classifyModuleBytecode(
                0U,
                0U,
                PureDataProgram::k_maximumModuleBytecodeBytes,
                PureDataProgram::k_maximumModuleClosureBytecodeBytes
            )
            == ModuleBytecodeAdmission::Empty
        );
        CHECK(
            detail::classifyModuleBytecode(
                PureDataProgram::k_maximumModuleBytecodeBytes + 1U,
                0U,
                PureDataProgram::k_maximumModuleBytecodeBytes,
                PureDataProgram::k_maximumModuleClosureBytecodeBytes
            )
            == ModuleBytecodeAdmission::ModuleCeiling
        );
        CHECK(
            detail::classifyModuleBytecode(
                PureDataProgram::k_maximumModuleBytecodeBytes,
                PureDataProgram::k_maximumModuleClosureBytecodeBytes
                    - PureDataProgram::k_maximumModuleBytecodeBytes,
                PureDataProgram::k_maximumModuleBytecodeBytes,
                PureDataProgram::k_maximumModuleClosureBytecodeBytes
            )
            == ModuleBytecodeAdmission::Accepted
        );
        CHECK(
            detail::classifyModuleBytecode(
                1U,
                PureDataProgram::k_maximumModuleClosureBytecodeBytes,
                PureDataProgram::k_maximumModuleBytecodeBytes,
                PureDataProgram::k_maximumModuleClosureBytecodeBytes
            )
            == ModuleBytecodeAdmission::ClosureCeiling
        );
    }

    // What a plugin is handed when it reads a JSON resource. The environment
    // publishes no JSON decoder and a consuming project ships no C++, so the
    // host owns decoding at this trust boundary.
    TEST_CASE("a JSON resource is handed over decoded, frozen, and once per VM")
    {
        constexpr auto k_map =
            std::string_view{R"({"depth":{"summit":3},"regions":["harbour","pass"]})"};
        auto const empty = parsed("{}");

        constexpr auto decoded = std::string_view{
            R"(        local map = resource.readJson("map")
        return {
            decoded = type(map) == "table" and map.regions[2] == "pass"
                and map.depth.summit == 3,
        })"
        };
        CHECK(
            deriveBytes(
                "fixture.decoded",
                decoded,
                empty,
                {resourceOf(PureDataProgram::ResourceKind::Json, "map", std::string{k_map})}
            )
            == R"({"decoded":true})"
        );

        constexpr auto frozen = std::string_view{
            R"(        local map = resource.readJson("map")
        local wrote = pcall(function() map.regions[1] = "moved" end)
        local raw_wrote = pcall(function() rawset(map, "extra", 1) end)
        local nested = pcall(function() map.depth.summit = 9 end)
        return { frozen = not wrote and not raw_wrote and not nested })"
        };
        CHECK(
            deriveBytes(
                "fixture.frozen-map",
                frozen,
                empty,
                {resourceOf(PureDataProgram::ResourceKind::Json, "map", std::string{k_map})}
            )
            == R"({"frozen":true})"
        );

        // One resource is one value per VM, so a plugin that reads it twice
        // gets the object it already holds rather than a second copy charged
        // against the same memory quota.
        constexpr auto once = std::string_view{
            R"(        return {
            same = rawequal(resource.readJson("map"), resource.readJson("map")),
        })"
        };
        CHECK(
            deriveBytes(
                "fixture.once",
                once,
                empty,
                {resourceOf(PureDataProgram::ResourceKind::Json, "map", std::string{k_map})}
            )
            == R"({"same":true})"
        );
    }

    TEST_CASE("typed resource readers preserve JSON, UTF-8 text, and exact bytes")
    {
        auto binary = std::string{};
        binary.push_back('\0');
        binary.push_back(static_cast<char>(0xFFU));
        binary.push_back('A');

        constexpr auto body = std::string_view{R"LUAU(
        local document = resource.readJson("catalog.map")
        local text = resource.readText("notes.readme")
        local bytes = resource.readBytes("payload.bin")
        return {
            json_value = document.value,
            text = text,
            byte_count = #bytes,
            byte_1 = string.byte(bytes, 1),
            byte_2 = string.byte(bytes, 2),
            byte_3 = string.byte(bytes, 3),
        }
)LUAU"};
        auto const result = derive(
            "fixture.typed-resources",
            body,
            parsed("{}"),
            {
                resourceOf(PureDataProgram::ResourceKind::Bytes,
                           "payload.bin",
                           std::move(binary)),
                resourceOf(PureDataProgram::ResourceKind::Utf8,
                           "notes.readme",
                           "line one\nline two"),
                resourceOf(PureDataProgram::ResourceKind::Json,
                           "catalog.map",
                           R"({"value":7})"),
            }
        );
        REQUIRE(result.has_value());
        CHECK(
            json::canonicalBytes(*result)
            == R"({"byte_1":0,"byte_2":255,"byte_3":65,"byte_count":3,"json_value":7,"text":"line one\nline two"})"
        );
    }

    TEST_CASE("resource readers reject kind mismatches and invalid UTF-8")
    {
        auto const mismatched = derive(
            "fixture.resource-kind",
            "        return { value = resource.readText(\"payload\") }",
            parsed("{}"),
            {resourceOf(PureDataProgram::ResourceKind::Bytes, "payload", "bytes")}
        );
        REQUIRE_FALSE(mismatched.has_value());
        CHECK(std::string{mismatched.error().message()}.find("resource kind mismatch")
              != std::string::npos);

        auto invalidUtf8 = std::string{};
        invalidUtf8.push_back(static_cast<char>(0xFFU));
        auto const rejected = compileProgram(
            "fixture.resource-utf8",
            "main",
            {
                PureDataProgram::Module{
                    .name   = "main",
                    .source = pluginReturning("fixture.resource-utf8", "        return input"),
                },
            },
            {resourceOf(PureDataProgram::ResourceKind::Utf8,
                        "invalid",
                        std::move(invalidUtf8))}
        );
        REQUIRE_FALSE(rejected.has_value());
        CHECK(std::string{rejected.error().message()}.find("UTF-8 resource is invalid")
              != std::string::npos);

        auto const invalidKind = compileProgram(
            "fixture.resource-invalid-kind",
            "main",
            {
                PureDataProgram::Module{
                    .name = "main",
                    .source = pluginReturning(
                        "fixture.resource-invalid-kind",
                        "        return input"
                    ),
                },
            },
            {resourceOf(static_cast<PureDataProgram::ResourceKind>(255U),
                        "invalid",
                        "bytes")}
        );
        REQUIRE_FALSE(invalidKind.has_value());
        CHECK(std::string{invalidKind.error().message()}.find("resource kind is invalid")
              != std::string::npos);
    }

    TEST_CASE("resource names and reader calls use only the exact dotted ASCII grammar")
    {
        auto overlongSegment = std::string(65U, 'a');
        auto tooManySegments = std::string{"a"};
        for (auto index = std::size_t{1U}; index < 17U; ++index)
        {
            tooManySegments += ".a";
        }
        auto overlongName = std::string(8U, 'a');
        for (auto index = std::size_t{1U}; index < 16U; ++index)
        {
            overlongName += '.' + std::string(8U, 'a');
        }

        auto invalidNames = std::vector<std::string>{
            "Upper",
            "1lead",
            "a/b",
            "a..b",
            ".a",
            "a.",
            std::string{"a\nb"},
            std::string{"a\xc3\xa9"},
            std::move(overlongSegment),
            std::move(tooManySegments),
            std::move(overlongName),
        };
        auto const source = pluginReturning(
            "fixture.invalid-resource-name",
            "        return input"
        );
        for (auto const& invalidName : invalidNames)
        {
            auto const program = compileProgram(
                "fixture.invalid-resource-name",
                "main",
                {PureDataProgram::Module{.name = "main", .source = source}},
                {resourceOf(PureDataProgram::ResourceKind::Bytes,
                            invalidName,
                            "bytes")}
            );
            REQUIRE_FALSE(program.has_value());
            CHECK(std::string{program.error().message()}.find(
                      "resource name is not canonical"
                  )
                  != std::string::npos);
        }

        for (auto const call : {
                 "resource.readJson()",
                 "resource.readJson(1)",
                 "resource.readJson('known', 'extra')",
                 "resource.readJson('')",
                 "resource.readJson('bad/name')",
                 "resource.readJson('missing')",
                 "resource.readJson('a' .. string.char(1) .. 'b')",
                 "resource.readJson(string.char(0xc3, 0xa9))",
             })
        {
            auto const body = std::string{"        return "} + call;
            auto const output = derive(
                "fixture.invalid-resource-call",
                body,
                parsed("{}"),
                {resourceOf(PureDataProgram::ResourceKind::Json, "known", "{}")}
            );
            REQUIRE_FALSE(output.has_value());
            auto const message = std::string{output.error().message()};
            auto const isReaderRefusal =
                message.find("requires exactly one resource name") != std::string::npos
                || message.find("non-canonical name") != std::string::npos
                || message.find("unknown resource") != std::string::npos;
            CHECK(isReaderRefusal);
        }
    }

    // Admission is two-stage and this is the first stage. Registration is where
    // a document that cannot become a value is refused, because the bytes are
    // pinned by the resource root hash: their value is a fact about the
    // registration, so a call must never be the first to discover there is not
    // one.
    TEST_CASE("a JSON resource is admitted only if this VM can build it")
    {
        CHECK(
            admissionRefusal(
                "fixture.notjson",
                {resourceOf(PureDataProgram::ResourceKind::Json,
                            "map",
                            "expedition-map-bytes")}
            )
                .find("pure data JSON resource is invalid")
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
            admissionRefusal(
                "fixture.wide-resource",
                {resourceOf(PureDataProgram::ResourceKind::Json, "map", std::move(wide))}
            )
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
        auto const material = pluginEnvironmentMaterial();
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        CHECK(*first == *second);
        CHECK(json::canonicalBytes(parsed(material)) == material);

        // That the digest moved is one claim; what it covers is another, and
        // only this one names it. A preimage over published NAMES alone leaves
        // a build that changed what resource.readJson RETURNS with an unmoved
        // digest, and every session_manifest_hash and decision_basis_hash
        // beneath it unmoved with it -- exactly the upgrade the pin exists to
        // catch.
        CHECK(
            material.find(
                R"("resource.readJson":"exact_name_kind_checked_cached_frozen_json_value_v1")"
            )
            != std::string::npos
        );
        CHECK(material.find(
                  R"("resource.readText":"exact_name_kind_checked_cached_utf8_string_v1")"
              )
              != std::string::npos);
        CHECK(material.find(
                  R"("require":"closed_ascii_relative_resolver_cached_value_v1")"
              )
              != std::string::npos);
        CHECK(material.find(
                  R"("tostring":"json_scalar_or_type_name_v1")")
              != std::string::npos);
        CHECK(material.find(
                  R"("luau_implementation":"luau-0.730+5bc7f4b23756f69f4669b419fa9034f117ccd6fe")")
              != std::string::npos);
        CHECK(material.find(
                  R"("module_bytecode_bytes":1048576,"module_bytecode_total_bytes":16777216)"
              )
              != std::string::npos);
        CHECK(material.find(
                  R"("resource_bytes":4194304,"resource_count":64)"
              )
              != std::string::npos);
        CHECK(
            first->hex()
            == "db836678c3c0de5a36a5b2db86174b943db716c3c8b05203be8ec6a19bfc2a06"
        );
    }
} // namespace uf::script
