#include "allocator.hpp"

#include <cstddef>
#include <cstdlib>

// Luau's C headers do not build clean under the project's /W4 /WX profile, and
// a manifest-driven module has no CMakeLists to mark them external; wrap them as
// image/ffi/png-decoder.cpp does.
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
        // Accounting allocator implementing Luau's documented frealloc contract:
        //   (ud, null, 0, n)     allocate a fresh block of n bytes
        //   (ud, p, osize, 0)    free p (osize is its recorded size), return null
        //   (ud, p, osize, n)    resize p from osize to n bytes
        // A resize to an equal-or-smaller size must never fail; only a fresh
        // allocation or a growth may return null. On top of that contract this
        // charges every byte against the ledger and refuses a growth past the
        // ceiling, which Luau surfaces as a catchable LUA_ERRMEM. It runs on the
        // VM's owning thread only, GC included, so it is noexcept and touches
        // nothing but the ledger and the C heap.
        auto accountingAlloc(
            void* ud,
            void* ptr,
            std::size_t osize,
            std::size_t nsize
        ) noexcept -> void*
        {
            // SAFETY: ud is the MemoryQuota handed to lua_newstate. It outlives
            // the VM (Impl closes the state before the ledger is destroyed) and
            // is confined to this thread, so this unsynchronized access is sound.
            // Two invariants keep every unsigned subtraction below non-wrapping:
            // used >= osize (each accounted block records its own size) and
            // used <= limitBytes (a growth past the ceiling is refused). The FFI
            // hot path therefore uses raw size_t arithmetic under proven
            // invariants rather than the Result-returning checked helpers, which
            // a noexcept C callback cannot consume.
            auto* quota = static_cast<MemoryQuota*>(ud);

            if (nsize == 0)
            {
                // SAFETY: ptr was vended by a prior call to this allocator, or
                // is null, which std::free treats as a no-op. Frees the C block.
                // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
                std::free(ptr);
                quota->used -= osize;
                return nullptr;
            }

            if (nsize > osize)
            {
                std::size_t const growth = nsize - osize;
                if (
                    quota->limitBytes != 0
                    && growth > quota->limitBytes - quota->used
                )
                {
                    // Over quota: return null so Luau raises a catchable
                    // LUA_ERRMEM. The old block, if any, is left intact.
                    quota->ceilingRefused = true;
                    return nullptr;
                }
            }

            // SAFETY: realloc of a block previously vended here, or null for a
            // fresh block; a shrink cannot fail per the frealloc contract.
            // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
            void* block = std::realloc(ptr, nsize);
            if (block == nullptr)
            {
                // Only a growth reaches here (a shrink cannot fail): the host
                // heap is exhausted. The ledger and the still-live old block are
                // left untouched; Luau raises a catchable LUA_ERRMEM.
                return nullptr;
            }

            if (nsize >= osize)
            {
                quota->used += nsize - osize;
            }
            else
            {
                quota->used -= osize - nsize;
            }
            if (quota->used > quota->peak)
            {
                quota->peak = quota->used;
            }
            return block;
        }
    }

    auto createStateWithQuota(MemoryQuota* quota) -> lua_State*
    {
        // SAFETY: installs the accounting allocator and its ledger on a new VM.
        // `quota` must outlive the returned state; Engine::Impl owns both and
        // closes the state before the ledger member is destroyed.
        return lua_newstate(&accountingAlloc, quota);
    }

    auto quotaFor(lua_State* state) noexcept -> MemoryQuota const*
    {
        if (state == nullptr)
        {
            return nullptr;
        }

        void* userdata            = nullptr;
        lua_Alloc const allocator = lua_getallocf(state, &userdata);
        if (allocator != &accountingAlloc)
        {
            return nullptr;
        }

        // SAFETY: this state's allocator IS accountingAlloc, and the only call
        // that installs it is createStateWithQuota just above, which passes a
        // live MemoryQuota as the userdata. The cast therefore recovers exactly
        // the type the void* was erased from, and the object outlives the state.
        return static_cast<MemoryQuota const*>(userdata);
    }
}
