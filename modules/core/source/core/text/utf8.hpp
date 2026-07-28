#pragma once

#include <core/types/integer.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uf
{
    [[nodiscard]] auto isValidUtf8(std::string_view value) noexcept -> bool;

    // Decodes valid UTF-8 into Unicode scalar values. Invalid, overlong,
    // surrogate, truncated, and out-of-range sequences return no value.
    [[nodiscard]]
    auto decodeUtf8Scalars(
        std::string_view value
    ) -> std::optional<std::vector<uint32>>;

    // codePoint must be a Unicode scalar value.
    auto appendUtf8Scalar(std::string& output, uint32 codePoint) -> void;
}
