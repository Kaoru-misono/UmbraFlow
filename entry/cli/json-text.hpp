#pragma once

#include <string>
#include <string_view>

namespace uf::cli
{
    // Renders `value` as a JSON string literal, quotes included, escaping the six
    // named escapes and every remaining control byte as \u00XX. Bytes above 0x1F
    // pass through unchanged, so valid UTF-8 in a message survives. Shared by both
    // line protocols this CLI speaks, because two copies of the escaping rule is
    // one copy that can be wrong.
    [[nodiscard]] auto escapeJsonString(std::string_view value) -> std::string;
}
