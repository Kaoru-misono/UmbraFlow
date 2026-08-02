#include "environment.hpp"

#include <core/utility/scope-exit.hpp>
#include <domain/error.hpp>

#include <cstddef>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <utility>

// Luau's C headers are third-party and do not build clean under the project's
// /W4 /WX profile; a manifest-driven module has no CMakeLists to mark them
// external, so wrap the includes exactly as the repo's other vendored FFI does
// (image/ffi/png-decoder.cpp).
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
#include <lua.h>
#include <lualib.h>
#include <luacode.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace uf::script
{
    namespace
    {
        // Registry slots for the two environments. The registry is the only
        // place they live: neither is reachable from any global table, so a
        // script has no name to start from.
        constexpr auto k_frameworkEnvironmentKey = "uf.script.framework_env";
        constexpr auto k_projectEnvironmentKey   = "uf.script.project_env";

        // The framework environment's __metatable value. Its presence is what
        // stops getmetatable from handing the real metatable -- and with it the
        // __index chain to the main globals -- to anything holding the table.
        constexpr auto k_frameworkEnvironmentLabel = "uf.framework_env";

        // The project environment's whitelist: every deterministic base
        // function and library a project script may see, named one by one.
        //
        // This is a whitelist rather than the complement of a denial list on
        // purpose. A denial list is only as good as its author's memory of what
        // luaL_openlibs registered, and a Luau bump that adds a global would
        // silently widen the project surface. Here a new global is invisible
        // until someone writes it down.
        //
        // What is deliberately absent, and matches the design's denial list
        // exactly: _G, getfenv, setfenv, newproxy, gcinfo, coroutine and debug
        // (installSandbox also nils each of them, so they are gone from the main
        // globals this copies from), plus os.time / os.clock / os.date /
        // math.random / math.randomseed, which installSandbox removes from the
        // shared os and math tables listed here.
        constexpr std::string_view k_projectStandardGlobals[] = {
            "_VERSION",
            "assert",
            "error",
            "getmetatable",
            "ipairs",
            "next",
            "pairs",
            "pcall",
            "print",
            "rawequal",
            "rawget",
            "rawlen",
            "rawset",
            "select",
            "setmetatable",
            "tonumber",
            "tostring",
            "type",
            "typeof",
            "unpack",
            "xpcall",
            "bit32",
            "buffer",
            "math",
            "os",
            "string",
            "table",
            "utf8",
            "vector",
        };

        [[nodiscard]]
        auto topError(lua_State* thread) -> std::string
        {
            char const* text = lua_tostring(thread, -1);
            return text != nullptr
                ? std::string{text}
                : std::string{"(non-string error value)"};
        }

        // Copies one field by name from the table at `source` into the table at
        // `destination`. A name that resolves to nil fails: an environment
        // quietly missing a whitelisted entry would look like a script bug at
        // the point of use rather than like the boot problem it is.
        [[nodiscard]]
        auto copyFieldInto(
            lua_State* state,
            int source,
            int destination,
            std::string_view name,
            std::string_view origin
        ) -> Status
        {
            auto const key = std::string{name};
            lua_rawgetfield(state, source, key.c_str());
            if (lua_isnil(state, -1))
            {
                lua_pop(state, 1);
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "project environment whitelist names a value that is absent"
                    " from this VM (" + std::string{origin} + "): " + key
                );
            }
            lua_rawsetfield(state, destination, key.c_str());
            return ok();
        }

        // Compiles `source`, loads it as a closure whose environment is the
        // table at `environmentIndex`, and resumes it on a fresh thread spun
        // from `mainState`.
        //
        // The returned pointer is a non-owning observation of the VM-owned
        // thread, which stays alive because `mainState` still holds it on its
        // stack: the caller reads the thread's results and then restores
        // `mainState`'s stack top, after which the pointer is dead. Every caller
        // in this file installs that restoring guard before calling.
        [[nodiscard]]
        auto resumeChunkOnThread(
            lua_State* mainState,
            int environmentIndex,
            std::optional<int> argumentIndex,
            std::string_view source,
            std::string_view chunkName,
            InterruptState const* control,
            RaisedErrorClassifier const* classify
        ) -> Result<lua_State*>
        {
            int const environment = lua_absindex(mainState, environmentIndex);

            // Absolutized before lua_newthread pushes onto `mainState`, because
            // a relative index would name a different slot afterwards.
            auto const argument = argumentIndex.transform(
                [mainState](int index)
                {
                    return lua_absindex(mainState, index);
                }
            );

            auto options              = lua_CompileOptions{};
            options.optimizationLevel = 1;
            options.debugLevel        = 1;

            std::size_t bytecodeSize = 0;
            // SAFETY: luau_compile allocates the bytecode buffer with malloc; the
            // caller owns it. The scope guard frees it on every exit path (safe
            // after load, which copies the bytecode into the VM). A SYNTAX error
            // does NOT return null -- it is encoded as error bytecode and surfaces
            // at luau_load below; null is returned ONLY on allocation failure,
            // hence InternalInvariant.
            char* bytecode = luau_compile(
                source.data(),
                source.size(),
                &options,
                &bytecodeSize
            );
            if (bytecode == nullptr)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "luau_compile allocation failed (returned null)"
                );
            }
            auto bytecodeGuard = scopeExit(
                [bytecode]() noexcept
                {
                    // SAFETY: pairs the malloc inside luau_compile; freeing a
                    // caller-owned C buffer at the FFI boundary is intentional here.
                    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
                    std::free(bytecode);
                }
            );

            lua_State* thread = lua_newthread(mainState);

            // Move the environment onto the thread, where luau_load can name it,
            // and make it the thread's globals table as well.
            //
            // Binding the thread is the C++-side setfenv path the sandbox needs:
            // Lua's own setfenv is removed, and without this the thread would
            // keep the globals table lua_newthread copied from its parent (the
            // main globals). That matters twice over. LUA_GLOBALSINDEX on this
            // thread -- what every C function sees -- would otherwise reach the
            // main globals; and luau_load pre-resolves import constants against
            // L->gt when that table is marked safeenv, which the frozen main
            // globals are, so a chunk could be handed constants it must not see.
            lua_xpush(mainState, thread, environment);
            lua_pushthread(thread);
            lua_pushvalue(thread, 1);
            lua_setfenv(thread, -2);
            lua_pop(thread, 1);

            auto const name = std::string{chunkName};
            int const loadStatus = luau_load(
                thread,
                name.c_str(),
                bytecode,
                bytecodeSize,
                1
            );
            if (loadStatus != LUA_OK)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "luau_load failed: " + topError(thread)
                );
            }

            // The closure now holds the environment, so drop the stack copy and
            // leave the thread's stack holding exactly the function to resume.
            lua_remove(thread, 1);

            // The chunk's one vararg, when the caller supplied one. A framework
            // module reads it as `local native = ...`, which is how the private
            // capability surface becomes an upvalue of trusted code without ever
            // being bound to a name either environment could reach.
            int argumentCount = 0;
            if (argument.has_value())
            {
                lua_xpush(mainState, thread, *argument);
                argumentCount = 1;
            }

            int const runStatus = lua_resume(thread, nullptr, argumentCount);
            if (runStatus == LUA_BREAK || (control != nullptr && control->broken()))
            {
                // The interrupt hard-cancelled this thread (stop token, instruction
                // budget, or deadline). The thread is abandoned -- the caller's
                // guard pops it and it is never resumed. A break is not a
                // script-level error and cannot be caught by pcall.
                //
                // The broken flag is checked alongside LUA_BREAK because a break
                // raised inside a non-yieldable C frame surfaces as an ordinary
                // runtime error (LUA_ERRRUN, "attempt to break across C-call
                // boundary") instead. Classifying that as a recoverable script
                // error would misreport a host control signal as a Tier B
                // failure, and it is this disjunct alone that prevents it:
                // removing it turns six of the seven non-yieldable forms in
                // tests/script/test-adversarial-substrate.cpp, plus veto #6's own
                // two cases, from Cancelled into InvalidResource.
                //
                // The trigger is NAMED. All three land here identically, and a
                // sentence that says only "hard-cancelled" leaves the reader to
                // guess between a stop token, a spent budget and an expired
                // clock -- which cost three wrong diagnoses of one session.
                auto reason = std::string{
                    "task hard-cancelled (lua_break); the task thread is abandoned"
                };
                if (control != nullptr)
                {
                    reason += " [" + describeBreak(*control) + "]";
                }
                return fail(AutomationErrorKind::Cancelled, std::move(reason));
            }
            if (runStatus == LUA_YIELD)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "script yielded; the substrate does not resume yields"
                );
            }
            if (runStatus != LUA_OK)
            {
                // Reached only after the cancellation branch above, so a hard
                // cancel is never handed to the classifier. What is left is a
                // raise nobody caught: the host's own carrier names its kind,
                // and anything else -- a project string, a project table -- is
                // the script's own failure and stays InvalidResource.
                //
                // That ordering is the fail-closed default and NOT the mechanism
                // that stops a cancel being downgraded, because the state it
                // would rule on cannot arise: `broken` is set only inside the
                // interrupt, which then calls lua_break, and every trigger it
                // reads is monotone. Once it is set, the next interrupt breaks
                // again -- and Luau's interrupt sites are the call, return and
                // loop-back ops, all of which run BEFORE the op completes. So no
                // host C function can be entered to mint a carrier afterwards,
                // and a carrier already in hand can only be raised through
                // error(), which is itself a call. Measured, not assumed: moving
                // this block above the cancellation branch reddens nothing, even
                // against the script in
                // tests/task/test-adversarial-surface.cpp that holds a real
                // carrier and re-raises it from inside a cancelled sort
                // comparator. Keep the ordering anyway -- it costs one branch and
                // it is what a future yield protocol, or a Luau that adds an
                // interrupt site, would need.
                //
                // The stack is grown first. A thread that has just failed is
                // unwound to exactly the error value, with no spare slots, so a
                // classifier that pushes even one temporary would run off the
                // end -- measured, not assumed: a classifier calling
                // luaL_getmetafield here crashed the process until this line
                // existed. Growing it costs nothing on the success path, which
                // never reaches this branch.
                static_cast<void>(lua_checkstack(thread, LUA_MINSTACK));
                auto raised = std::optional<RaisedError>{};
                if (classify != nullptr && *classify)
                {
                    raised = (*classify)(thread, -1);
                }
                if (raised.has_value() && !raised->message.empty())
                {
                    // The host's own sentence, which lua_tostring cannot reach:
                    // the carrier is a userdata and that call runs no metamethod,
                    // so without the classifier's copy every host refusal reads
                    // "(non-string error value)" at this boundary and the reason
                    // survives only in the trace.
                    return fail(raised->kind, "script error: " + raised->message);
                }
                auto kind = AutomationErrorKind::InvalidResource;
                if (raised.has_value())
                {
                    kind = raised->kind;
                }
                return fail(kind, "script error: " + topError(thread));
            }

            return thread;
        }
    }

    auto installFrameworkEnvironment(lua_State* state) -> void
    {
        lua_newtable(state);
        int const environment = lua_gettop(state);

        lua_newtable(state);
        int const metatable = lua_gettop(state);
        lua_pushvalue(state, LUA_GLOBALSINDEX);
        lua_setfield(state, metatable, "__index");
        lua_pushstring(state, k_frameworkEnvironmentLabel);
        lua_setfield(state, metatable, "__metatable");
        // Frozen with a shallow lua_setreadonly rather than deepFreezeMetatable,
        // which would follow __index into the main globals and freeze them here
        // -- before the host installer has registered its tables and before
        // luaL_sandbox, which owns that freeze. The shape rules the walk would
        // check are satisfied by construction two lines up.
        lua_setreadonly(state, metatable, 1);
        lua_setmetatable(state, environment);

        lua_setfield(state, LUA_REGISTRYINDEX, k_frameworkEnvironmentKey);
    }

    auto pushFrameworkEnvironment(lua_State* state) -> void
    {
        lua_getfield(state, LUA_REGISTRYINDEX, k_frameworkEnvironmentKey);
    }

    auto loadFrameworkModules(
        lua_State* state,
        std::span<FrameworkModule const> modules,
        std::optional<int> privateCapabilities,
        InterruptState const* control
    ) -> Status
    {
        for (auto const& module : modules)
        {
            int const stackBase = lua_gettop(state);
            auto stackGuard = scopeExit(
                [state, stackBase]() noexcept
                {
                    lua_settop(state, stackBase);
                }
            );

            pushFrameworkEnvironment(state);
            int const environment = lua_gettop(state);
            if (!lua_istable(state, environment))
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "the framework environment is missing from the VM registry"
                );
            }

            UF_TRY_VALUE(
                thread,
                resumeChunkOnThread(
                    state,
                    environment,
                    privateCapabilities,
                    module.source,
                    module.name,
                    control,
                    nullptr
                )
            );
            if (lua_gettop(thread) < 1)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "framework module returned no exports: "
                        + std::string{module.name}
                );
            }

            // Take the module's first return value as its exports, freeze it,
            // and bind it in the framework environment under the module name.
            // Only a table can be frozen; a module exporting a scalar or a
            // function exports something already immutable or already opaque.
            lua_xpush(thread, state, 1);
            if (lua_istable(state, -1))
            {
                UF_TRY(deepFreeze(state, -1));
            }
            lua_rawsetfield(state, environment, std::string{module.name}.c_str());
        }
        return ok();
    }

    auto installProjectEnvironmentPrototype(
        lua_State* state,
        std::span<std::string const> hostGlobals,
        std::span<std::string const> frameworkGlobals
    ) -> Status
    {
        int const stackBase = lua_gettop(state);
        auto stackGuard = scopeExit(
            [state, stackBase]() noexcept
            {
                lua_settop(state, stackBase);
            }
        );

        lua_newtable(state);
        int const prototype = lua_gettop(state);

        for (std::string_view const name : k_projectStandardGlobals)
        {
            UF_TRY(
                copyFieldInto(
                    state,
                    LUA_GLOBALSINDEX,
                    prototype,
                    name,
                    "standard library"
                )
            );
        }
        for (auto const& name : hostGlobals)
        {
            UF_TRY(
                copyFieldInto(
                    state,
                    LUA_GLOBALSINDEX,
                    prototype,
                    name,
                    "host installer"
                )
            );
        }

        if (!frameworkGlobals.empty())
        {
            pushFrameworkEnvironment(state);
            int const framework = lua_gettop(state);
            if (!lua_istable(state, framework))
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "the framework environment is missing from the VM registry"
                );
            }
            for (auto const& name : frameworkGlobals)
            {
                UF_TRY(
                    copyFieldInto(
                        state,
                        framework,
                        prototype,
                        name,
                        "framework bundle"
                    )
                );
            }
            lua_remove(state, framework);
        }

        // No metatable is attached, here or in pushProjectEnvironment. That
        // absence is the whole isolation property: with no __index there is no
        // chain to the framework environment or to the main globals, so the
        // denial list holds structurally instead of by enumeration.
        lua_setreadonly(state, prototype, 1);
        lua_pushvalue(state, prototype);
        lua_setfield(state, LUA_REGISTRYINDEX, k_projectEnvironmentKey);
        return ok();
    }

    auto pushProjectEnvironment(lua_State* state) -> Status
    {
        lua_getfield(state, LUA_REGISTRYINDEX, k_projectEnvironmentKey);
        int const prototype = lua_gettop(state);
        if (!lua_istable(state, prototype))
        {
            lua_pop(state, 1);
            return fail(
                AutomationErrorKind::InternalInvariant,
                "the project environment prototype is missing from the VM registry"
            );
        }

        lua_newtable(state);
        int const environment = lua_gettop(state);

        lua_pushnil(state);
        while (lua_next(state, prototype) != 0)
        {
            // key is at -2 and value at -1; copy both, raw-set them into the
            // fresh environment, then drop the value and keep the key for the
            // next lua_next.
            lua_pushvalue(state, -2);
            lua_pushvalue(state, -2);
            lua_rawset(state, environment);
            lua_pop(state, 1);
        }

        // The copy is deliberately shallow and left WRITABLE: a script may
        // define its own globals, and they die with this table at the end of the
        // run. The values it shares with the prototype are already frozen.
        lua_remove(state, prototype);
        return ok();
    }

    auto runNumberInEnvironment(
        lua_State* mainState,
        int environmentIndex,
        std::string_view source,
        std::string_view chunkName,
        InterruptState const* control,
        RaisedErrorClassifier const* classify
    ) -> Result<double>
    {
        // The thread lua_newthread pushes onto the main state's stack, and every
        // value the run leaves behind, are released here on EVERY exit --
        // including a throw from the std::string allocations below -- so threads
        // can never accumulate across repeated calls.
        int const stackBase = lua_gettop(mainState);
        auto stackGuard = scopeExit(
            [mainState, stackBase]() noexcept
            {
                lua_settop(mainState, stackBase);
            }
        );

        UF_TRY_VALUE(
            thread,
            resumeChunkOnThread(
                mainState,
                environmentIndex,
                std::nullopt,
                source,
                chunkName,
                control,
                classify
            )
        );

        return lua_gettop(thread) >= 1
            ? lua_tonumber(thread, -1)
            : 0.0;
    }

    auto runValueInProjectEnvironment(
        lua_State* mainState,
        std::string_view source,
        std::string_view chunkName,
        InterruptState const* control,
        RaisedErrorClassifier const* classify
    ) -> Result<ScriptValue>
    {
        int const stackBase = lua_gettop(mainState);
        auto stackGuard = scopeExit(
            [mainState, stackBase]() noexcept
            {
                lua_settop(mainState, stackBase);
            }
        );

        UF_TRY(pushProjectEnvironment(mainState));
        UF_TRY_VALUE(
            thread,
            resumeChunkOnThread(
                mainState,
                lua_gettop(mainState),
                std::nullopt,
                source,
                chunkName,
                control,
                classify
            )
        );

        if (lua_gettop(thread) < 1)
        {
            return ScriptValue{};
        }

        switch (lua_type(thread, -1))
        {
        case LUA_TNIL:
            return ScriptValue{};
        case LUA_TBOOLEAN:
            return ScriptValue{lua_toboolean(thread, -1) != 0};
        case LUA_TNUMBER:
            return ScriptValue{lua_tonumber(thread, -1)};
        case LUA_TSTRING:
        {
            std::size_t length = 0;
            // SAFETY: the value was just confirmed to be a string, so
            // lua_tolstring performs no conversion and returns the VM-owned
            // bytes with their length. They are copied into the ScriptValue
            // before the stack guard drops the thread.
            char const* p_text = lua_tolstring(thread, -1, &length);
            return ScriptValue{std::string{p_text, length}};
        }
        default:
            break;
        }

        return fail(
            AutomationErrorKind::InvalidResource,
            std::format(
                "the chunk returned a {}, which no result line can carry; return "
                "nil, a boolean, a number or a string",
                lua_typename(thread, lua_type(thread, -1))
            )
        );
    }

    auto runNumberInProjectEnvironment(
        lua_State* mainState,
        std::string_view source,
        std::string_view chunkName,
        InterruptState const* control,
        RaisedErrorClassifier const* classify
    ) -> Result<double>
    {
        int const stackBase = lua_gettop(mainState);
        auto stackGuard = scopeExit(
            [mainState, stackBase]() noexcept
            {
                lua_settop(mainState, stackBase);
            }
        );

        UF_TRY(pushProjectEnvironment(mainState));
        return runNumberInEnvironment(
            mainState,
            lua_gettop(mainState),
            source,
            chunkName,
            control,
            classify
        );
    }
}
