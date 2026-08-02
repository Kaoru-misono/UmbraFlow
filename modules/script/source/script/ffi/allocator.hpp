#pragma once

#include <cstddef>

// Luau's opaque VM handle, forward-declared to keep the Luau C headers out of
// this header; global scope matches Luau's own typedef.
struct lua_State;

namespace uf::script
{
    // Memory ledger backing the accounting allocator. Lives in the heap-pinned
    // Engine::Impl, which keeps the pointer handed to lua_newstate valid and
    // address-stable and destroys it only after lua_close, so the frees Luau
    // runs during teardown still see a live ledger. The allocator runs only on
    // the VM's owning thread, GC included, so no field needs synchronization.
    struct MemoryQuota final
    {
        // Hard ceiling in bytes: the allocator refuses (returns null) any growth
        // that would push `used` past this, and Luau surfaces that null as a
        // catchable LUA_ERRMEM. Zero disables the ceiling.
        std::size_t limitBytes{0};

        // Live bytes currently vended by the allocator. Frees and shrinks lower
        // it; it returns to zero once the VM is closed.
        std::size_t used{0};

        // High-water mark of `used` across the VM's life. Diagnostic only.
        std::size_t peak{0};
    };

    // Create a lua_State whose allocator charges every allocation against
    // `quota` and refuses growth beyond quota.limitBytes, otherwise mirroring
    // luaL_newstate. Null on host allocation failure. The caller owns the state
    // and must close it with lua_close. `quota` must outlive the state: it is
    // read on every allocation, including those lua_close triggers.
    [[nodiscard]]
    auto createStateWithQuota(MemoryQuota* quota) -> lua_State*;

    // The ledger behind `state`, or null when `state` was not created by
    // createStateWithQuota. Luau hands back whatever userdata the state's
    // allocator was installed with, so a state built by luaL_newstate -- as
    // every probe in testing/ does -- would yield a pointer to something that is
    // not a ledger, and casting it blind would read another allocator's private
    // state as byte counts. Observation only: the ledger belongs to the
    // Engine::Impl that outlives the VM.
    [[nodiscard]]
    auto quotaFor(lua_State* state) noexcept -> MemoryQuota const*;
}
