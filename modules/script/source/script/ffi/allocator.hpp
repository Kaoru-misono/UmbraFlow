#pragma once

#include <cstddef>

// Luau's opaque VM handle, forward-declared so this internal header never pulls
// in the Luau C headers (which compile only under the pragma-wrapped includes in
// the .cpp layer). Declared at global scope to match Luau's own typedef, so the
// name resolves to the same type once <lua.h> is visible in a .cpp.
struct lua_State;

namespace uf::script
{
    // Per-task-generation memory ledger backing the accounting allocator. It is
    // a member of the heap-pinned Engine::Impl, so the pointer handed to
    // lua_newstate as the allocator userdata stays valid and address-stable, and
    // it outlives lua_close: Impl closes the VM before this member is destroyed,
    // so the frees Luau runs during teardown still see a live ledger. The
    // allocator runs only on the VM's owning thread -- including during GC -- so
    // no field needs synchronization.
    struct MemoryQuota final
    {
        // Hard ceiling in bytes: the allocator refuses (returns null) any growth
        // that would push `used` past this. Luau surfaces that null as a
        // catchable LUA_ERRMEM, so an over-quota task fails without exhausting
        // host memory. Zero disables the ceiling (the host OS is the only limit).
        std::size_t limitBytes{0};

        // Live bytes currently vended by the allocator. Frees and shrinks lower
        // it; it returns to zero once the VM is closed.
        std::size_t used{0};

        // High-water mark of `used` across the VM's life. Diagnostic only.
        std::size_t peak{0};
    };

    // Create a lua_State whose allocator charges every allocation against
    // `quota` and refuses growth beyond quota.limitBytes, otherwise mirroring
    // luaL_newstate. Returns null on host allocation failure. The caller owns the
    // returned state and must close it with lua_close. `quota` must outlive the
    // state: it is read on every allocation, including those lua_close triggers.
    [[nodiscard]]
    auto createStateWithQuota(MemoryQuota* quota) -> lua_State*;
}
