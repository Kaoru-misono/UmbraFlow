#pragma once

#include <core/types/integer.hpp>

#include <string>
#include <string_view>

namespace uf
{
    [[nodiscard]] auto isValidUtf8(std::string_view value) noexcept -> bool;

    // codePoint must be a Unicode scalar value.
    auto appendUtf8Scalar(std::string& output, uint32 codePoint) -> void;
}
