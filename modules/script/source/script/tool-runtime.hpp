#pragma once

#include <json/value.hpp>

#include <core/error/result.hpp>
#include <functional>
#include <string_view>

namespace uf::script
{
    // The single VM-facing Tool Runtime protocol: a Tool name and canonical
    // argument value in, canonical data out. Interactive scoped code calls this
    // exact type. Project Tool handlers are pure leaves and never receive it;
    // there is no callback or body re-entry protocol.
    using ToolRuntimeInvoke = std::move_only_function<
        Result<json::Value>(
            std::string_view toolName,
            json::Value const& arguments
        )
    >;

} // namespace uf::script
