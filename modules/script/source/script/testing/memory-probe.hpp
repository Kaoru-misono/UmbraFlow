#pragma once

#include <cstddef>
#include <string_view>

namespace uf::script::testing
{
    // Ledger readout captured across one quota'd VM's whole life.
    struct MemoryUsage final
    {
        // High-water mark of live bytes while the VM ran. Zero only when the VM
        // could not be created.
        std::size_t peak{0};

        // Live bytes still charged to the ledger after the VM was closed. Zero
        // for a leak-free teardown.
        std::size_t residual{0};
    };

    // Build a quota'd, sandboxed VM (limitBytes caps the accounting allocator; 0
    // disables the ceiling), run `source` on a sandboxed task thread with its
    // result discarded, close the VM, and report the ledger's peak and post-close
    // residual. Luau-free so tests may include it; exercises the accounting
    // allocator before the real umbra.* host tables exist (phase 2). Not part of
    // the public Engine surface.
    [[nodiscard]]
    auto measureMemory(
        std::string_view source,
        std::string_view chunkName,
        std::size_t limitBytes
    ) -> MemoryUsage;
}
