#include "annotation-fields.hpp"

#include <core/error/contracts.hpp>
#include <core/safety/checked-access.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace uf::annotation::detail
{
    auto annotationTypeText(AnnotationType type) noexcept -> std::string_view
    {
        switch (type)
        {
        case AnnotationType::PageAnchor:
            return "page_anchor";
        case AnnotationType::ActionTarget:
            return "action_target";
        case AnnotationType::InfoRegion:
            return "info_region";
        }
        UF_UNREACHABLE_MSG("unknown annotation type");
    }

    auto annotationTypeFromText(
        std::string_view value
    ) noexcept -> std::optional<AnnotationType>
    {
        if (value == "page_anchor")
        {
            return AnnotationType::PageAnchor;
        }
        if (value == "action_target")
        {
            return AnnotationType::ActionTarget;
        }
        if (value == "info_region")
        {
            return AnnotationType::InfoRegion;
        }
        return std::nullopt;
    }

    auto parsePixelRectField(
        CanonicalTomlReader& reader,
        std::string_view key
    ) -> Result<PixelRect>
    {
        UF_TRY_VALUE(values, reader.takeUnsigned32ArrayField(key));
        if (values.size() != 4U)
        {
            return reader.invalid(
                std::format("'{}' must have four integers", key)
            );
        }
        auto const rect = PixelRect::create(
            checkedAt(values, 0),
            checkedAt(values, 1),
            checkedAt(values, 2),
            checkedAt(values, 3)
        );
        if (!rect)
        {
            return reader.invalid(
                std::format("'{}' is not a valid rectangle", key)
            );
        }
        return *rect;
    }

    auto appendRectField(
        std::string& output,
        std::string_view key,
        PixelRect rect
    ) -> void
    {
        output += key;
        output += " = ";
        auto const values = std::array{
            rect.x(),
            rect.y(),
            rect.width(),
            rect.height(),
        };
        appendUnsigned32Array(output, values);
        output.push_back('\n');
    }

    auto parseFingerprintFields(
        CanonicalTomlReader& reader,
        std::string_view resolutionKey,
        std::string_view dpiKey
    ) -> Result<ProjectFingerprint>
    {
        UF_TRY_VALUE(
            resolution,
            reader.takeUnsigned32ArrayField(resolutionKey)
        );
        UF_TRY_VALUE(dpi, reader.takeUnsigned32ArrayField(dpiKey));
        if (resolution.size() != 2U || dpi.size() != 2U)
        {
            return reader.invalid(
                std::format(
                    "'{}' and '{}' must each have two integers",
                    resolutionKey,
                    dpiKey
                )
            );
        }
        return ProjectFingerprint::create(
            checkedAt(resolution, 0),
            checkedAt(resolution, 1),
            checkedAt(dpi, 0),
            checkedAt(dpi, 1)
        );
    }

    auto appendFingerprintFields(
        std::string& output,
        ProjectFingerprint fingerprint,
        std::string_view resolutionKey,
        std::string_view dpiKey
    ) -> void
    {
        output += resolutionKey;
        output += " = ";
        auto const resolution = std::array{
            fingerprint.width(),
            fingerprint.height(),
        };
        appendUnsigned32Array(output, resolution);
        output.push_back('\n');

        output += dpiKey;
        output += " = ";
        auto const dpi = std::array{
            fingerprint.dpiX(),
            fingerprint.dpiY(),
        };
        appendUnsigned32Array(output, dpi);
        output.push_back('\n');
    }
}
