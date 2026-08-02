#pragma once

#include <script/engine.hpp>

#include <string>

namespace uf::script::testing
{
    // Builds an EngineConfig::installPrivateCapabilities installer whose surface
    // a LUAU chunk assembled, so a suite can boot the real framework bundle
    // against primitives it wrote itself. The framework's policy layer -- how
    // long a wait polls, which cycle an interrupt handler is offered, whether a
    // retry pauses between attempts -- is only observable as the sequence of
    // primitive calls it produces, and the real surface is a closure upvalue of
    // trusted code that reaches no trace. A fake in its place records the calls
    // and answers them from a script.
    //
    // `inner` is the surface the chunk receives as its one argument, so a fake
    // can forward the primitives it does not want to fake -- notably `raise`,
    // whose Tier B carrier is host-minted userdata a Luau chunk cannot forge.
    // An empty `inner` runs the chunk with no argument at all.
    //
    // The chunk's single returned table becomes the private capability surface
    // and is NOT frozen, unlike the real one: a fake's recorded call log is
    // state it must keep writing. It is still unreachable from a project script,
    // because the boot drops the host's reference once every framework module
    // has been handed it. Both strings are taken by value and owned by the
    // returned installer, so a caller may build one from a temporary.
    [[nodiscard]]
    auto scriptedPrivateCapabilities(
        PrivateCapabilityInstaller inner,
        std::string source,
        std::string chunkName
    ) -> PrivateCapabilityInstaller;
}
