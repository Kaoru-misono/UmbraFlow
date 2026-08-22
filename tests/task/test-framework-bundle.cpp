#include <task/framework-bundle.hpp>
#include <task/script-bindings.hpp>

#include <domain/content-hash.hpp>

#include <script/engine.hpp>
#include <script/pure-data-program.hpp>
#include <script/testing/environment-probe.hpp>

#include <json/value.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <utility>
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

        // Every embedded module declares a reserved name and, with it, a
        // dependency depth. A new .luau file that declares neither would load at
        // depth 0 with nothing able to require it, which is a silent way to get
        // the load order wrong now that the resolver is the only route between
        // Framework modules.
        for (auto const& module : modules)
        {
            INFO("framework module: ", module.name);
            CHECK_FALSE(module.resolverName.empty());
        }

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

    // Both Luau execution paths now resolve a module's dependencies by the same
    // rule -- exact reserved @umbraflow/ names -- so a release-owned module has
    // one meaning wherever it runs. This is the smoke test that says the two
    // loaders have not diverged again: the probe runs unchanged on both and
    // renders every answer as ASCII hex, so two paths that both answer report
    // their disagreement as bytes rather than as a rendered string.
    TEST_CASE("the release SDK behaves identically on both Luau paths")
    {
        constexpr auto probe = std::string_view{R"LUAU(
local parts = {}
local function record(value: any)
    if type(value) ~= "string" then
        parts[#parts + 1] = "not-a-string"
        return
    end
    local bytes = {}
    for index = 1, #value do
        bytes[index] = string.format("%02X", string.byte(value, index))
    end
    parts[#parts + 1] = table.concat(bytes)
end
record(text.normalize("e\u{0301}", "NFC"))
record(text.normalize("\u{01FA}", "NFD"))
record(text.normalize("\u{1100}\u{1161}", "NFC"))
record(text.normalize("\u{FB03}", "NFKC"))
record(text.normalize("\u{2460}", "NFKD"))
record(text.case_fold("Stra\u{00DF}e"))
record(text.trim("\u{00A0} Menu \u{3000}"))
record(text.collapse_whitespace("\u{00A0}  Menu\t Start\u{3000}"))
record(table.concat(text.tokens("Go, \u{4E2D}\u{1F642} 42"), "/"))
record(table.concat(text.split("a,,b", ","), "/"))
record(json.encode(json.parse([[{"b":[1,2],"a":"\u00E9\uD83D\uDE42"}]])))
return table.concat(parts, " ")
)LUAU"};

        // Hand-checked: NFC of e+U+0301 is U+00E9 (C3 A9); NFD of U+01FA is
        // A U+030A U+0301 (41 CC 8A CC 81); NFC of the Hangul jamo pair is
        // U+AC00 (EA B0 80); NFKC of U+FB03 is "ffi"; NFKD of U+2460 is "1";
        // full case folding turns "Straße" into "strasse"; the trim and
        // collapse cases yield "Menu" and "Menu Start"; the token and split
        // lists join to "Go/,/<U+4E2D>/<U+1F642>/42" and "a//b"; and the JSON
        // round trip re-emits both escapes as literal UTF-8 in canonical key
        // order.
        constexpr auto expectation = std::string_view{
            "C3A9 41CC8ACC81 EAB080 666669 31 73747261737365 4D656E75"
            " 4D656E75205374617274 476F2F2C2FE4B8AD2FF09F99822F3432"
            " 612F2F62 7B2261223A22C3A9F09F9982222C2262223A5B312C325D7D"
        };

        auto sdk = pureFrameworkScriptModules();
        REQUIRE(sdk.has_value());

        constexpr auto pureHeader = std::string_view{R"LUAU(
local json = require("@umbraflow/json")
local text = require("@umbraflow/text")
return {
    plugin_id = "fixture.parity",
    probe = function(_)
)LUAU"};
        constexpr auto pureFooter = std::string_view{R"LUAU(
    end,
}
)LUAU"};

        auto pureSource = std::string{pureHeader};
        pureSource += probe;
        pureSource += pureFooter;

        constexpr auto entryPoints = std::array{std::string_view{"probe"}};
        auto program = script::PureDataProgram::compile(
            "fixture.parity",
            "main",
            {
                script::PureDataProgram::Module{
                    .name   = "main",
                    .source = pureSource,
                },
            },
            entryPoints,
            {},
            *sdk
        );
        REQUIRE(program.has_value());

        auto input = json::parse("{}");
        REQUIRE(input.has_value());
        auto pureResult = program->invoke("probe", *input);
        REQUIRE(pureResult.has_value());

        // Production publishes no framework export to project source at all --
        // the fail-closed case above binds that -- but publication is the only
        // seam by which a project script can name a framework export, and the
        // property under test is about the module sources rather than about
        // what production chooses to publish.
        auto engine = script::Engine::create(
            script::EngineConfig{
                .frameworkModules        = frameworkScriptModules(),
                .frameworkProjectGlobals = {"json", "text"},
            }
        );
        REQUIRE(engine.has_value());
        auto trusted = engine->runValue(probe, "cross-vm-parity");
        REQUIRE(trusted.has_value());
        auto const* trustedText = trusted->text();
        REQUIRE(trustedText != nullptr);

        CHECK(json::canonicalBytes(*pureResult) == "\"" + *trustedText + "\"");
        CHECK(*trustedText == std::string{expectation});
    }

    // The ratchet on the property the two loaders now share. Loading a Framework
    // module must add no name to any VM's global namespace: the trusted loader
    // keeps every module's frozen exports in its own registry, and the project
    // whitelist is the only projection out of it. Without this, a future
    // workstream re-adding a global for convenience would be caught by nothing
    // until some file stem collided with a standard-library table, silently and
    // at whichever call site happened to use the shadowed function.
    TEST_CASE("loading the framework publishes no trusted global")
    {
        auto const modules = frameworkScriptModules();
        auto const globals = script::testing::probeFrameworkGlobals(modules);
        REQUIRE(globals.has_value());

        // The control: a baseline that came back empty would make the equality
        // below hold for the wrong reason.
        CHECK_FALSE(globals->beforeLoad.empty());
        CHECK(globals->afterLoad == globals->beforeLoad);

        // The same rule at the projection boundary, which is where it can still
        // recur: a whitelist entry spelled like a standard-library global would
        // shadow that library for every project script the moment it published.
        auto const standard = script::projectStandardGlobals();
        auto const whitelists = std::array{
            frameworkProjectGlobals(),
            explorationProjectGlobals(),
            runtimeProjectGlobals(),
        };
        for (auto const& whitelist : whitelists)
        {
            for (auto const& name : whitelist)
            {
                INFO("projected framework global: ", name);
                CHECK_FALSE(
                    std::ranges::contains(standard, std::string_view{name})
                );
            }
        }
    }

    // The other half of the same rule, at the same boundary. Three sources are
    // copied into ONE flat project-environment table by
    // installProjectEnvironmentPrototype, in this order and with no collision
    // check between them: the Luau standard-library whitelist, the HOST
    // installer's own `projectGlobals`, and the framework projection. A name
    // carried by two of them is not refused; it is silently overwritten, and
    // whichever source is copied later wins. The assertion above bound only the
    // third source against the first, which left the host source -- written by
    // exactly the same lua_rawsetfield into exactly the same table -- covered by
    // nothing.
    TEST_CASE("no two sources may bind one project-environment name")
    {
        struct NameSource final
        {
            std::string_view         origin;
            std::vector<std::string> names;
        };

        auto standardNames = std::vector<std::string>{};
        for (auto const name : script::projectStandardGlobals())
        {
            standardNames.emplace_back(name);
        }

        auto const sources = std::vector<NameSource>{
            NameSource{"standard library", std::move(standardNames)},
            NameSource{"host installer", scriptProjectGlobals()},
            NameSource{"framework release", frameworkProjectGlobals()},
            NameSource{"framework exploration", explorationProjectGlobals()},
            NameSource{"framework runtime", runtimeProjectGlobals()},
        };

        // The whole matrix, not just framework-against-standard: any two of the
        // five colliding is one project global with two authors.
        for (auto left = std::size_t{}; left < sources.size(); ++left)
        {
            for (auto right = left + 1U; right < sources.size(); ++right)
            {
                for (auto const& name : sources[left].names)
                {
                    INFO(
                        "project global ",
                        name,
                        " from ",
                        sources[left].origin,
                        " and ",
                        sources[right].origin
                    );
                    CHECK_FALSE(
                        std::ranges::contains(sources[right].names, name)
                    );
                }
            }
        }

        // The control, run through the production prototype builder rather than
        // asserted about it: a projected name spelled like a standard-library
        // global is accepted in silence and REPLACES that library for every
        // project script. Without this the disjointness above would read as a
        // tidiness preference; with it, it is the only thing standing between a
        // whitelist edit and a project environment whose utf8 is not Luau's.
        constexpr auto shadowSource =
            std::string_view{"return table.freeze({ marker = 1 })\n"};
        auto engine = script::Engine::create(
            script::EngineConfig{
                .frameworkModules = {
                    script::FrameworkModule{
                        .name         = "utf8",
                        .source       = shadowSource,
                        .resolverName = "@umbraflow/internal/shadow-probe",
                    },
                },
                .frameworkProjectGlobals = {std::string{"utf8"}},
            }
        );
        REQUIRE(engine.has_value());
        auto const shadowed = engine->runNumber(
            R"lua(
                if utf8.char ~= nil then return 0 end
                if utf8.marker ~= 1 then return 0 end
                return 1
            )lua",
            "project-global-collision"
        );
        REQUIRE(shadowed.has_value());
        CHECK(*shadowed == doctest::Approx(1.0));
    }

    // The loader refuses a framework list that does not arrive in non-decreasing
    // dependency depth. Nothing constructed an out-of-order list before this, so
    // the refusal was a check that could not fail -- and it is load-bearing:
    // resolution is earlier-only, so a silently reordered list is a silently
    // different dependency graph rather than a boot error.
    TEST_CASE("the loader refuses a framework list out of dependency-depth order")
    {
        // The control on the production list, which is what makes the refusal
        // below about the ORDER rather than about the modules.
        auto const production = frameworkScriptModules();
        REQUIRE(production.size() > 1U);
        CHECK(
            std::ranges::is_sorted(
                production,
                {},
                &script::FrameworkModule::dependencyDepth
            )
        );

        constexpr auto leaf = std::string_view{"return table.freeze({})\n"};
        auto const shallow = script::FrameworkModule{
            .name            = "depth-probe-shallow",
            .source          = leaf,
            .resolverName    = "@umbraflow/internal/depth-probe-shallow",
            .dependencyDepth = 0U,
        };
        auto const deep = script::FrameworkModule{
            .name            = "depth-probe-deep",
            .source          = leaf,
            .resolverName    = "@umbraflow/internal/depth-probe-deep",
            .dependencyDepth = 1U,
        };

        auto ordered = script::Engine::create(
            script::EngineConfig{.frameworkModules = {shallow, deep}}
        );
        REQUIRE(ordered.has_value());

        auto reordered = script::Engine::create(
            script::EngineConfig{.frameworkModules = {deep, shallow}}
        );
        REQUIRE_FALSE(reordered.has_value());
        CHECK(
            std::string{reordered.error().message()}.find(
                "framework modules are not ordered by dependency depth: "
                "depth-probe-shallow"
            )
            != std::string::npos
        );
    }

    namespace
    {
        // One embedded module as the bundle-identity recipe sees it. The three
        // fields beside the source are release linkage that lives in C++ rather
        // than in the .luau file, which is exactly why they belong in the
        // digest and why this fixture derives them from the closure functions
        // rather than from the private tables that state them.
        struct BundleIdentityRow final
        {
            std::string_view name;
            std::string_view tier;
            std::string_view alias;
            std::size_t      dependencyDepth{};
            std::string_view source;
        };

        // Joins by SOURCE IDENTITY, not by name. Every closure hands out views
        // into the same static literal the bundle entry carries, so the pointer
        // is an exact join that assumes no naming convention between a
        // publication name and its reserved alias -- and there is none to
        // assume: docs/standards/luau.md says the alias appears neither in the
        // .luau file nor in its path.
        [[nodiscard]]
        auto sameSource(
            std::span<script::FrameworkModule const> modules,
            std::string_view source
        ) -> script::FrameworkModule const*
        {
            for (auto const& module : modules)
            {
                if (module.source.data() == source.data()
                    && module.source.size() == source.size())
                {
                    return &module;
                }
            }
            return nullptr;
        }

        // The bundle as the recipe in framework-bundle.hpp describes it,
        // assembled from the three published closures instead of from the
        // tables frameworkBundleHash() reads. A module the pure closure carries
        // is pure or internal-pure by its own projectVisible flag; one only the
        // scoped closure carries is scoped; one only the trusted list carries is
        // trusted.
        [[nodiscard]] auto bundleIdentityRows() -> std::vector<BundleIdentityRow>
        {
            auto pure = pureFrameworkScriptModules();
            REQUIRE(pure.has_value());
            auto scoped = scopedFrameworkScriptModules();
            REQUIRE(scoped.has_value());
            auto const trusted = frameworkScriptModules();

            auto rows = std::vector<BundleIdentityRow>{};
            for (auto const& entry : frameworkBundleEntries())
            {
                if (auto const* const module = sameSource(*pure, entry.source))
                {
                    rows.emplace_back(BundleIdentityRow{
                        .name            = entry.name,
                        .tier            = module->projectVisible
                            ? std::string_view{"pure"}
                            : std::string_view{"internal-pure"},
                        .alias           = module->name,
                        .dependencyDepth = module->dependencyDepth,
                        .source          = entry.source,
                    });
                    continue;
                }
                if (auto const* const module = sameSource(*scoped, entry.source))
                {
                    auto const alias = module->name;
                    auto const depth = module->dependencyDepth;
                    rows.emplace_back(BundleIdentityRow{
                        .name            = entry.name,
                        .tier            = "scoped",
                        .alias           = alias,
                        .dependencyDepth = depth,
                        .source          = entry.source,
                    });
                    continue;
                }
                auto const* const module = sameSource(trusted, entry.source);
                REQUIRE(module != nullptr);
                auto const alias = module->resolverName;
                auto const depth = module->dependencyDepth;
                rows.emplace_back(BundleIdentityRow{
                    .name            = entry.name,
                    .tier            = "trusted",
                    .alias           = alias,
                    .dependencyDepth = depth,
                    .source          = entry.source,
                });
            }
            return rows;
        }

        // The recipe, restated here from framework-bundle.hpp's prose rather
        // than shared with the implementation. Ten NUL-terminated fields per
        // entry, in bundle order.
        [[nodiscard]]
        auto bundleIdentityDigest(std::vector<BundleIdentityRow> const& rows)
            -> std::string
        {
            auto preimage = std::string{};
            for (auto const& row : rows)
            {
                preimage += row.name;
                preimage.push_back('\0');
                preimage += row.tier;
                preimage.push_back('\0');
                preimage += row.alias;
                preimage.push_back('\0');
                preimage += std::to_string(row.dependencyDepth);
                preimage.push_back('\0');
                preimage += row.source;
                preimage.push_back('\0');
            }
            return digest(preimage);
        }
    }

    TEST_CASE("embedded framework identity remains deterministic")
    {
        auto names = std::vector<std::string_view>{};
        for (auto const& entry : frameworkBundleEntries())
        {
            names.emplace_back(entry.name);
            CHECK(entry.sourceHash == digest(entry.source));
            CHECK(checkFrameworkModuleSyntax(entry.source, entry.name).has_value());
        }
        CHECK(std::ranges::is_sorted(names));
        CHECK(std::ranges::adjacent_find(names) == names.end());

        auto const rows = bundleIdentityRows();
        REQUIRE(rows.size() == frameworkBundleEntries().size());
        auto const published = frameworkBundleHash();
        REQUIRE(published.has_value());
        CHECK(*published == bundleIdentityDigest(rows));
    }

    // The other half of the case above, and the reason the recipe was widened.
    // The equality there says the shipped digest is the recipe applied to the
    // real bundle; these say the recipe is sensitive to each of the three
    // linkage fields that are not the module's bytes. Together they say a
    // renamed alias, a reordered depth or a module moved between declaration
    // tiers cannot ship under an unmoved digest -- which is precisely what the
    // old `name || NUL || source` recipe allowed.
    //
    // A production table cannot be mutated from a test, so the mutation is
    // applied to the row set the case above proved the shipped digest agrees
    // with. Dropping any one of the three fields from the preimage in
    // framework-bundle.cpp turns the case above red, and dropping it from the
    // restatement here turns these red.
    TEST_CASE("the bundle identity covers alias, depth, and declaration tier")
    {
        auto const rows = bundleIdentityRows();
        auto const baseline = bundleIdentityDigest(rows);
        REQUIRE(rows.size() > 1U);

        SUBCASE("a renamed reserved alias moves it")
        {
            auto renamed = rows;
            REQUIRE(renamed.front().alias != "@umbraflow/renamed");
            renamed.front().alias = "@umbraflow/renamed";
            CHECK(bundleIdentityDigest(renamed) != baseline);
        }

        SUBCASE("a changed dependency depth moves it")
        {
            auto redepthed = rows;
            redepthed.front().dependencyDepth += 1U;
            CHECK(bundleIdentityDigest(redepthed) != baseline);
        }

        SUBCASE("a module moved between declaration tiers moves it")
        {
            // The mutation the alias cannot catch: `internal-pure` and
            // `trusted` modules both carry an `@umbraflow/internal/` alias, so
            // exposing a trusted-only module to every Project VM changes the
            // closure it is in and nothing else about the row.
            auto const internalPure = std::ranges::find(
                rows,
                std::string_view{"internal-pure"},
                &BundleIdentityRow::tier
            );
            REQUIRE(internalPure != rows.end());
            auto retiered = rows;
            auto const position = static_cast<std::size_t>(
                internalPure - rows.begin()
            );
            retiered.at(position).tier = "trusted";
            CHECK(bundleIdentityDigest(retiered) != baseline);
        }

        SUBCASE("two entries swapped in bundle order move it")
        {
            auto reordered = rows;
            std::swap(reordered.at(0U), reordered.at(1U));
            CHECK(bundleIdentityDigest(reordered) != baseline);
        }
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
