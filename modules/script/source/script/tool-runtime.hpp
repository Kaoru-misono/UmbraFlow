#pragma once

#include <json/value.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <functional>
#include <stop_token>
#include <string_view>

namespace uf::script
{
    // The one structured Tool body every VM-facing Tool Runtime carries. It is
    // consumed exactly once and returns only after its framework-owned child
    // dispatch has finished, so no callback escapes the native call frame.
    // The Framework supplies the durable identity of the body-taking call when
    // it runs the body. Script never sees this argument; native adapters use it
    // to make body calls children of that call and restart their ordinal at 1.
    using ToolCallBody =
        std::move_only_function<Status(ContentHash const& owningCall)>;

    // The single VM-facing Tool Runtime protocol: canonical data and one
    // optional structured body in, canonical data out. Both interactive and
    // scoped Project code call this exact type.
    //
    // Synchronous and blocking by contract. No yield, coroutine suspension or
    // unstructured callback is permitted. Re-entry is approved only through
    // ToolCallBody: the Framework owns that child dispatch, records it in the
    // ledger's call tree and closes its Tool scope on every exit path.
    using ToolRuntimeInvoke = std::move_only_function<
        Result<json::Value>(
            std::string_view toolName,
            json::Value const& arguments,
            ToolCallBody body
        )
    >;

    // Where one scoped Tool call sits in the recorded call tree. Both halves
    // are assigned by the C++ adapter and neither is reachable from script.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct ToolCallCoordinate final
    {
        ContentHash parentPosition;
        uint64      childIndex{0};
    };

    // The coordinate-bearing host adapter injected into ScopedToolProgram.
    // This is not a second VM protocol: ScopedToolProgram binds these host-only
    // values and exposes ToolRuntimeInvoke to its VM.
    using ToolRuntimeDispatch = std::function<
        Result<json::Value>(
            std::string_view toolName,
            json::Value const& arguments,
            ToolCallCoordinate const& coordinate,
            std::stop_token cancellation,
            ToolCallBody body
        )
    >;
} // namespace uf::script
