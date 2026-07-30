#pragma once

#include "canonical-toml.hpp"
#include "capabilities.hpp"
#include "catalog.hpp"
#include "resource.hpp"

#include <core/error/result.hpp>
#include <core/safety/checked-access.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uf::annotation::detail
{
    // One element's capability row exactly as it is spelled on disk. The click
    // offset stays raw here because a TemplateOffset has to be bounded by a
    // template, and the variant rows carrying the templates follow the element
    // row that owns them, in both schemas.
    struct CapabilityFields final
    {
        struct RawClickOffset final
        {
            uint32 x{};
            uint32 y{};
        };

        bool identify{};
        bool interact{};
        bool read{};

        std::optional<RawClickOffset>     clickOffset{};
        ReadLayout                        layout{ReadLayout::SingleLine};
        std::optional<CharsetRestriction> charset{};
    };

    [[nodiscard]]
    auto parseCapabilityFields(CanonicalTomlReader& reader) -> Result<CapabilityFields>;

    // Completes the parsed row once a template exists to bound the click offset
    // against. An absent boundingTemplate means the element declares no
    // variants, and then no click offset can be measured at all.
    [[nodiscard]]
    auto toElementCapabilities(
        CapabilityFields const& fields,
        std::optional<PixelRect> boundingTemplate
    ) -> Result<ElementCapabilities>;

    auto appendCapabilityFields(
        std::string& output,
        ElementCapabilities const& capabilities
    ) -> void;

    [[nodiscard]]
    auto parseExercisedFields(CanonicalTomlReader& reader) -> Result<ExercisedCapabilities>;

    auto appendExercisedFields(
        std::string& output,
        ExercisedCapabilities const& exercised
    ) -> void;

    [[nodiscard]]
    auto holdingText(Holding holding) noexcept -> std::string_view;

    [[nodiscard]]
    auto holdingFromText(std::string_view value) noexcept -> std::optional<Holding>;

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
    [[nodiscard]]
    auto parseId(std::string_view value) -> Result<Id>
    {
        UF_TRY_VALUE(resourceId, ResourceId::parse(value));
        return Id{resourceId};
    }

    template <typename Id>
    [[nodiscard]]
    auto parseIds(
        std::vector<std::string> const& encoded
    ) -> Result<std::vector<Id>>
    {
        auto ids = std::vector<Id>{};
        ids.reserve(encoded.size());
        for (auto const& value : encoded)
        {
            UF_TRY_VALUE(id, parseId<Id>(value));
            ids.emplace_back(id);
        }
        return ids;
    }

    [[nodiscard]]
    auto parseFingerprintFields(
        CanonicalTomlReader& reader,
        std::string_view resolutionKey,
        std::string_view dpiKey
    ) -> Result<ProjectFingerprint>;

    auto appendFingerprintFields(
        std::string& output,
        ProjectFingerprint fingerprint,
        std::string_view resolutionKey,
        std::string_view dpiKey
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
