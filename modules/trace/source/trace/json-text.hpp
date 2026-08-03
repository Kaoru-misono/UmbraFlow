#pragma once

#include <string>
#include <string_view>

namespace uf::trace
{
    // Renders `value` as a JSON string literal, quotes included: the seven named
    // escapes, every remaining byte below 0x20 as \u00XX, and every byte above it
    // unchanged so valid UTF-8 in a message survives. Public because the CLI's
    // line protocols write the same schema this module does, and two copies of an
    // escaping rule is one copy that can be wrong.
    [[nodiscard]] auto escapeJsonString(std::string_view value) -> std::string;
}
