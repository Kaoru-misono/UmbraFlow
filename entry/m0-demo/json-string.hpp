#pragma once

#include <string>
#include <string_view>

namespace uf::m0_demo
{
    [[nodiscard]] auto escapeJsonString(std::string_view value) -> std::string;
}
