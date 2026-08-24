// What an exploration VM publishes, and what it can do with nothing behind it.
//
// EVERYTHING AN EXPLORATION CHUNK CAN ACTUALLY DO IS A TOOL CALL, so what an
// exploration verb does to the screen, the target or the project is asserted
// where those calls are admitted and recorded -- tests/cli/test-explore-door.cpp,
// against a real Operator root, a real ledger and a real policy artifact. The
// sixteen `explore_*` natives this file used to drive are gone, and with them
// the only reason a VM-level case could have observed one
// (docs/decisions/2026-08-24-there-is-no-annotation-phase.md).
//
// What is left here is the part that is genuinely a property of the VM: which
// module each environment publishes, and what the module says when it is loaded
// somewhere with no Tool Runtime behind it. Both are cheap -- no target, no
// ledger, no engine session -- which is why they stay separate from the door.

#include <task/framework-bundle.hpp>
#include <task/script-bindings.hpp>

#include <script/engine.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::task
{
    namespace
    {
        // One VM booted exactly as ExplorationSession boots one, minus the
        // private capability table: an exploration session installs the Tool
        // Runtime seam there, and a case that wants to know what the module
        // says WITHOUT one has to be able to leave it out.
        [[nodiscard]]
        auto runPublishing(
            std::vector<std::string> publishedGlobals,
            std::string_view chunk
        ) -> std::optional<std::string>
        {
            auto vm = script::Engine::create(
                script::EngineConfig{
                    .frameworkModules        = frameworkScriptModules(),
                    .installHostTables       = scriptHostTableInstaller(),
                    .projectGlobals          = scriptProjectGlobals(),
                    .frameworkProjectGlobals = std::move(publishedGlobals),
                    .classifyRaisedError     = scriptRaisedErrorClassifier(),
                }
            );
            REQUIRE(vm.has_value());
            auto const result = vm->runValue(chunk, "exploration-verbs-chunk");
            REQUIRE(result.has_value());
            REQUIRE(result->text() != nullptr);
            return std::string{*result->text()};
        }
    }

    // Which module each environment publishes. `explore` is in exactly one
    // whitelist, and nothing else publishes it.
    TEST_CASE("only the exploration whitelist publishes the acting module")
    {
        auto const exploration = explorationProjectGlobals();
        CHECK(std::ranges::contains(exploration, std::string{"explore"}));

        CHECK_MESSAGE(
            !std::ranges::contains(frameworkProjectGlobals(), std::string{"explore"}),
            "the business whitelist publishes the exploration acting module"
        );
        CHECK_MESSAGE(
            !std::ranges::contains(runtimeProjectGlobals(), std::string{"explore"}),
            "the Runtime plugin whitelist publishes the exploration acting module"
        );
    }

    // The same, as the VM actually enforces it. The chunk reports every
    // spelling it reached rather than a bare verdict, so a leak names itself.
    TEST_CASE("a Runtime-whitelist VM cannot name the exploration module")
    {
        auto const reached = runPublishing(
            runtimeProjectGlobals(),
            R"lua(
                local reached = {}
                if explore ~= nil then reached[#reached + 1] = "explore" end
                if native ~= nil then reached[#reached + 1] = "native" end
                if uf_private ~= nil then reached[#reached + 1] = "uf_private" end
                if #reached == 0 then return "isolated" end
                return table.concat(reached, ",")
            )lua"
        );
        REQUIRE(reached.has_value());
        CHECK_MESSAGE(
            *reached == std::string{"isolated"},
            "a plugin environment reached the exploration surface: ",
            *reached
        );
    }

    // The module without a Tool Runtime behind it.
    //
    // It LOADS -- every VM that boots the framework bundle loads it, including
    // the trusted runtime VM, and a module that refused while loading would take
    // that VM down with it. What it refuses is the CALL, and what is asserted
    // here is the refusal's wording as much as its existence: a VM that reached
    // an unbound primitive raises whatever the interpreter says about indexing
    // nothing, and a reader of that message cannot tell a misconfigured
    // environment from a typo.
    TEST_CASE("the acting module is inert without the Tool Runtime seam")
    {
        auto const answered = runPublishing(
            explorationProjectGlobals(),
            R"lua(
                if explore == nil then return "module absent" end
                local ok, err = pcall(function()
                    explore.observe()
                end)
                if ok then return "observed" end
                return tostring(err)
            )lua"
        );
        REQUIRE(answered.has_value());
        CHECK_MESSAGE(
            std::string_view{*answered}.find("does not provide")
                != std::string_view::npos,
            "an unbound seam was not refused by the module's own guard: ",
            *answered
        );
        CHECK_MESSAGE(
            std::string_view{*answered}.find("Tool Runtime seam")
                != std::string_view::npos,
            "the refusal does not name what is missing: ",
            *answered
        );
    }
}
