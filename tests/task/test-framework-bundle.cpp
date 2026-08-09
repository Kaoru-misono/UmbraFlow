#include <task/framework-bundle.hpp>

#include <domain/content-hash.hpp>

#include <script/engine.hpp>

#include <doctest/doctest.h>

#include <algorithm>
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
}
