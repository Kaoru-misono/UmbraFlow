#pragma once

#include <json/value.hpp>

#include <core/error/result.hpp>
#include <functional>
#include <string_view>

namespace uf::script
{
    // The single VM-facing Tool Runtime protocol: a Tool name and canonical
    // argument value in, canonical data out. Interactive scoped code calls this
    // exact type, as do registered Project handlers through their parent-bound
    // child issuing door. The callback is synchronous and never resumes a VM.
    using ToolRuntimeInvoke = std::move_only_function<
        Result<json::Value>(
            std::string_view toolName,
            json::Value const& arguments
        )
    >;

} // namespace uf::script
