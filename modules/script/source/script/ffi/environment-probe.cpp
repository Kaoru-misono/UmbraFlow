#include <script/testing/environment-probe.hpp>

#include "environment.hpp"
#include "sandbox.hpp"

#include <script/engine.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>
#include <core/utility/scope-exit.hpp>
#include <domain/error.hpp>

#include <memory>
#include <string>
#include <utility>

// Luau's C headers are third-party and do not build clean under the project's
// /W4 /WX profile; wrap the includes exactly as the repo's other vendored FFI
// does (image/ffi/png-decoder.cpp).
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
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace uf::script::testing
{
    namespace
    {
        // Runs at VM teardown for the userdata probeInstallerFailure registers.
        auto bumpFinalizationWitness(void* storage) -> void
        {
            // SAFETY: `storage` is the userdata block lua_newuserdatadtor handed
            // the installer below, where exactly one uint64* was
            // placement-constructed. It points at a counter the caller owns and
            // keeps alive past lua_close, because it is declared before the
            // Engine whose destruction closes the VM.
            auto* p_counter = static_cast<uint64**>(storage);
            **p_counter += 1;
            std::destroy_at(p_counter);
        }
    }

    auto runInEnvironment(
        std::string_view frameworkSource,
        std::string_view source,
        ProbeEnvironment environment
    ) -> Result<double>
    {
        // SAFETY: luaL_newstate allocates the VM and returns null on failure;
        // the scope guard closes it on every exit path. Confined to this test
        // seam, which owns the whole VM lifetime locally.
        lua_State* state = luaL_newstate();
        if (state == nullptr)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "luaL_newstate returned null"
            );
        }
        auto stateGuard = scopeExit(
            [state]() noexcept
            {
                lua_close(state);
            }
        );

        luaL_openlibs(state);
        UF_TRY(
            installSandbox(
                state,
                EngineConfig{
                    .frameworkModules = {
                        FrameworkModule{
                            .name   = "probe",
                            .source = frameworkSource,
                        },
                    },
                },
                nullptr
            )
        );

        if (environment == ProbeEnvironment::Project)
        {
            return runNumberInProjectEnvironment(
                state,
                source,
                "probe-project",
                nullptr,
                nullptr
            );
        }

        int const stackBase = lua_gettop(state);
        auto stackGuard = scopeExit(
            [state, stackBase]() noexcept
            {
                lua_settop(state, stackBase);
            }
        );

        pushFrameworkEnvironment(state);
        return runNumberInEnvironment(
            state,
            lua_gettop(state),
            source,
            "probe-framework",
            nullptr,
            nullptr
        );
    }

    auto probeInstallerFailure(
        AutomationErrorKind kind,
        std::string_view message
    ) -> InstallerFailureProbe
    {
        // Declared before the Engine so it outlives the VM whichever way the
        // create goes: on failure the VM closes inside create(), and on an
        // unexpected success it closes when `engine` dies at the end of this
        // scope -- after this counter, either way.
        uint64 finalizedHostObjects = 0;

        auto engine = Engine::create(
            EngineConfig{
                .installHostTables =
                    [&finalizedHostObjects, kind, message](lua_State* state) -> Status
                    {
                        // SAFETY: lua_newuserdatadtor allocates sizeof(uint64*)
                        // VM-owned bytes, suitably aligned for a pointer, and we
                        // placement-construct exactly one uint64* into it. No Lua
                        // allocation runs in between, so a collection can never
                        // see the block uninitialised. The block stays rooted on
                        // the main stack until lua_close destroys it, which is
                        // the event this witness records.
                        void* storage = lua_newuserdatadtor(
                            state,
                            sizeof(uint64*),
                            &bumpFinalizationWitness
                        );
                        std::construct_at(
                            static_cast<uint64**>(storage),
                            &finalizedHostObjects
                        );
                        return fail(kind, std::string{message});
                    },
            }
        );

        auto probe = InstallerFailureProbe{
            .finalizedHostObjects = finalizedHostObjects,
        };
        if (!engine)
        {
            probe.failure = std::move(engine).error();
        }
        return probe;
    }
}
