#pragma once

#include "canonical-toml.hpp"
#include "catalog.hpp"

#include <core/error/result.hpp>
#include <core/safety/checked-access.hpp>

#include <domain/space.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace uf::annotation::detail
{
    [[nodiscard]]
    auto annotationTypeText(AnnotationType type) noexcept -> std::string_view;

    [[nodiscard]]
    auto annotationTypeFromText(
        std::string_view value
    ) noexcept -> std::optional<AnnotationType>;

    [[nodiscard]]
    auto parsePixelRectField(
        CanonicalTomlReader& reader,
        std::string_view key
    ) -> Result<PixelRect>;

    auto appendRectField(
        std::string& output,
        std::string_view key,
        PixelRect rect
    ) -> void;

    template <typename Id>
    auto appendIdArray(
        std::string& output,
        std::span<Id const> ids
    ) -> void
    {
        output.push_back('[');
        for (auto index = std::size_t{0}; index < ids.size(); ++index)
        {
            if (index != 0U)
            {
                output += ", ";
            }
            appendTomlString(output, checkedAt(ids, index).value().toString());
        }
        output.push_back(']');
    }
}
