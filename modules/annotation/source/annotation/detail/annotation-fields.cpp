#include "annotation-fields.hpp"

#include <core/error/contracts.hpp>
#include <core/safety/checked-access.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::annotation::detail
{
    namespace
    {
        // The one canonical order for a capability array, on both the element
        // and the reference side. Serialization walks it and parsing compares
        // against it, so the array a reader accepts is the array a writer
        // emits and the round-trip check has one shape to enforce.
        constexpr auto k_identifyText = std::string_view{"identify"};
        constexpr auto k_interactText = std::string_view{"interact"};
        constexpr auto k_readText     = std::string_view{"read"};

        struct CapabilityNames final
        {
            bool identify{};
            bool interact{};
            bool read{};
        };

        [[nodiscard]]
        auto invalidField(std::string message) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::move(message)
            );
        }

        [[nodiscard]]
        auto parseCapabilityNames(
            std::vector<std::string> const& encoded
        ) -> Result<CapabilityNames>
        {
            auto names = CapabilityNames{};
            for (auto const& value : encoded)
            {
                auto* p_slot = [&names, &value]() noexcept -> bool*
                {
                    if (value == k_identifyText)
                    {
                        return &names.identify;
                    }
                    if (value == k_interactText)
                    {
                        return &names.interact;
                    }
                    if (value == k_readText)
                    {
                        return &names.read;
                    }
                    return nullptr;
                }();
                if (p_slot == nullptr)
                {
                    return invalidField(
                        std::format("unknown capability '{}'", value)
                    );
                }
                if (*p_slot)
                {
                    return invalidField(
                        std::format("capability '{}' is listed twice", value)
                    );
                }
                *p_slot = true;
            }
            if (!names.identify && !names.interact && !names.read)
            {
                return invalidField("a capability set must not be empty");
            }
            return names;
        }

        auto appendCapabilityArray(
            std::string& output,
            std::string_view key,
            CapabilityNames names
        ) -> void
        {
            auto listed = std::vector<std::string_view>{};
            if (names.identify)
            {
                listed.emplace_back(k_identifyText);
            }
            if (names.interact)
            {
                listed.emplace_back(k_interactText);
            }
            if (names.read)
            {
                listed.emplace_back(k_readText);
            }

            output += key;
            output += " = [";
            for (auto index = std::size_t{0}; index < listed.size(); ++index)
            {
                if (index != 0U)
                {
                    output += ", ";
                }
                appendTomlString(output, checkedAt(listed, index));
            }
            output += "]\n";
        }

        [[nodiscard]]
        auto readLayoutText(ReadLayout layout) noexcept -> std::string_view
        {
            switch (layout)
            {
            case ReadLayout::SingleLine: return "single_line";
            case ReadLayout::Block: return "block";
            }
            UF_UNREACHABLE_MSG("unknown read layout");
        }

        [[nodiscard]]
        auto readLayoutFromText(
            std::string_view value
        ) noexcept -> std::optional<ReadLayout>
        {
            constexpr auto k_layouts = std::array{
                std::pair{std::string_view{"single_line"}, ReadLayout::SingleLine},
                std::pair{std::string_view{"block"}, ReadLayout::Block},
            };
            auto const found = std::ranges::find(k_layouts, value, &std::pair<std::string_view, ReadLayout>::first);
            if (found == k_layouts.end())
            {
                return std::nullopt;
            }
            return found->second;
        }

        [[nodiscard]]
        auto charsetText(CharsetRestriction charset) noexcept -> std::string_view
        {
            switch (charset)
            {
            case CharsetRestriction::Digits: return "digits";
            }
            UF_UNREACHABLE_MSG("unknown charset restriction");
        }

        [[nodiscard]]
        auto charsetFromText(
            std::string_view value
        ) noexcept -> std::optional<CharsetRestriction>
        {
            if (value == "digits")
            {
                return CharsetRestriction::Digits;
            }
            return std::nullopt;
        }

        [[nodiscard]]
        auto signatureRoleText(SignatureRole role) noexcept -> std::string_view
        {
            switch (role)
            {
            case SignatureRole::Required: return "required";
            case SignatureRole::Forbidden: return "forbidden";
            }
            UF_UNREACHABLE_MSG("unknown signature role");
        }

        [[nodiscard]]
        auto signatureRoleFromText(
            std::string_view value
        ) noexcept -> std::optional<SignatureRole>
        {
            constexpr auto k_roles = std::array{
                std::pair{std::string_view{"required"}, SignatureRole::Required},
                std::pair{std::string_view{"forbidden"}, SignatureRole::Forbidden},
            };
            auto const found = std::ranges::find(k_roles, value, &std::pair<std::string_view, SignatureRole>::first);
            if (found == k_roles.end())
            {
                return std::nullopt;
            }
            return found->second;
        }
    }

    auto parseCapabilityFields(
        CanonicalTomlReader& reader
    ) -> Result<CapabilityFields>
    {
        UF_TRY_VALUE(encoded, reader.takeStringArrayField("capabilities"));
        UF_TRY_VALUE(names, parseCapabilityNames(encoded));

        auto fields = CapabilityFields{
            .identify = names.identify,
            .interact = names.interact,
            .read     = names.read,
        };

        UF_TRY_VALUE(hasDefaultClick, reader.nextIsField("default_click"));
        if (hasDefaultClick)
        {
            if (!fields.interact)
            {
                return invalidField(
                    "only an element that interacts may define a default click"
                );
            }
            UF_TRY_VALUE(values, reader.takeUnsigned32ArrayField("default_click"));
            if (values.size() != 2U)
            {
                return invalidField("default_click must have two integers");
            }
            fields.clickOffset = CapabilityFields::RawClickOffset{
                .x = checkedAt(values, 0),
                .y = checkedAt(values, 1),
            };
        }

        if (fields.read)
        {
            UF_TRY_VALUE(layoutText, reader.takeStringField("read_layout"));
            auto const layout = readLayoutFromText(layoutText);
            if (!layout)
            {
                return invalidField(
                    std::format("unknown read_layout '{}'", layoutText)
                );
            }
            fields.layout = *layout;

            UF_TRY_VALUE(hasCharset, reader.nextIsField("read_charset"));
            if (hasCharset)
            {
                UF_TRY_VALUE(charsetName, reader.takeStringField("read_charset"));
                auto const charset = charsetFromText(charsetName);
                if (!charset)
                {
                    return invalidField(
                        std::format("unknown read_charset '{}'", charsetName)
                    );
                }
                fields.charset = *charset;
            }
        }

        return fields;
    }

    auto toElementCapabilities(
        CapabilityFields const& fields,
        std::optional<PixelRect> boundingTemplate
    ) -> Result<ElementCapabilities>
    {
        auto identify = std::optional<Identify>{};
        if (fields.identify)
        {
            identify = Identify{};
        }

        auto interact = std::optional<Interact>{};
        if (fields.interact)
        {
            auto clickOffset = std::optional<TemplateOffset>{};
            if (auto const raw = fields.clickOffset)
            {
                if (!boundingTemplate)
                {
                    return invalidField(
                        "a default click needs an appearance template to be measured against"
                    );
                }
                UF_TRY_VALUE(
                    offset,
                    TemplateOffset::create(
                        raw->x,
                        raw->y,
                        boundingTemplate->width(),
                        boundingTemplate->height()
                    )
                );
                clickOffset = offset;
            }
            interact = Interact{.clickOffset = clickOffset};
        }

        auto read = std::optional<Read>{};
        if (fields.read)
        {
            read = Read{
                .layout  = fields.layout,
                .charset = fields.charset,
            };
        }

        return ElementCapabilities::create(identify, interact, read);
    }

    auto appendCapabilityFields(
        std::string& output,
        ElementCapabilities const& capabilities
    ) -> void
    {
        appendCapabilityArray(
            output,
            "capabilities",
            CapabilityNames{
                .identify = capabilities.hasIdentify(),
                .interact = capabilities.hasInteract(),
                .read     = capabilities.hasRead(),
            }
        );

        if (auto const& interact = capabilities.interact())
        {
            if (auto const click = interact->clickOffset)
            {
                output += "default_click = ";
                auto const values = std::array{click->x(), click->y()};
                appendUnsigned32Array(output, values);
                output.push_back('\n');
            }
        }

        if (auto const& read = capabilities.read())
        {
            appendStringField(output, "read_layout", readLayoutText(read->layout));
            if (auto const charset = read->charset)
            {
                appendStringField(output, "read_charset", charsetText(*charset));
            }
        }
    }

    auto parseExercisedFields(
        CanonicalTomlReader& reader
    ) -> Result<ExercisedCapabilities>
    {
        UF_TRY_VALUE(encoded, reader.takeStringArrayField("exercised"));
        UF_TRY_VALUE(names, parseCapabilityNames(encoded));

        auto identify = std::optional<ExercisedIdentify>{};
        if (names.identify)
        {
            UF_TRY_VALUE(roleText, reader.takeStringField("signature_role"));
            auto const role = signatureRoleFromText(roleText);
            if (!role)
            {
                return invalidField(
                    std::format("unknown signature_role '{}'", roleText)
                );
            }
            identify = ExercisedIdentify{.role = *role};
        }

        auto interact = std::optional<ExercisedInteract>{};
        if (names.interact)
        {
            interact = ExercisedInteract{};
        }

        auto read = std::optional<ExercisedRead>{};
        if (names.read)
        {
            read = ExercisedRead{};
        }

        return ExercisedCapabilities::create(identify, interact, read);
    }

    auto appendExercisedFields(
        std::string& output,
        ExercisedCapabilities const& exercised
    ) -> void
    {
        appendCapabilityArray(
            output,
            "exercised",
            CapabilityNames{
                .identify = exercised.hasIdentify(),
                .interact = exercised.hasInteract(),
                .read     = exercised.hasRead(),
            }
        );

        if (auto const& identify = exercised.identify())
        {
            appendStringField(
                output,
                "signature_role",
                signatureRoleText(identify->role)
            );
        }
    }

    auto holdingText(Holding holding) noexcept -> std::string_view
    {
        switch (holding)
        {
        case Holding::Owned: return "owned";
        case Holding::Referenced: return "referenced";
        }
        UF_UNREACHABLE_MSG("unknown holding");
    }

    auto holdingFromText(std::string_view value) noexcept -> std::optional<Holding>
    {
        constexpr auto k_holdings = std::array{
            std::pair{std::string_view{"owned"}, Holding::Owned},
            std::pair{std::string_view{"referenced"}, Holding::Referenced},
        };
        auto const found = std::ranges::find(k_holdings, value, &std::pair<std::string_view, Holding>::first);
        if (found == k_holdings.end())
        {
            return std::nullopt;
        }
        return found->second;
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
