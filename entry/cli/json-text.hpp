#pragma once

#include <string>
#include <string_view>

namespace uf::cli
{
    // Renders `value` as a JSON string literal, quotes included, escaping the
    // six named escapes and every remaining control byte as \u00XX.
    //
    // It is shared by the two line protocols this CLI speaks -- the operator's
    // and the agent's -- because a result line either escapes a message
    // correctly or produces a file the reader on the other side cannot parse,
    // and two copies of that rule is one copy that can be wrong. Bytes above
    // 0x1F pass through unchanged, so valid UTF-8 in a message survives.
    [[nodiscard]] auto escapeJsonString(std::string_view value) -> std::string;
}
