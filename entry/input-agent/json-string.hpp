#pragma once

#include <string>
#include <string_view>

namespace uf::input_agent
{
    [[nodiscard]] auto escapeJsonString(std::string_view value) -> std::string;
}
