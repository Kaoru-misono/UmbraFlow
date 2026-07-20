#include "ids.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace
{
    [[nodiscard]]
    auto isValidUtf8(std::string_view value) noexcept -> bool
    {
        auto codePoint = std::uint32_t{0};
        auto minimumCodePoint = std::uint32_t{0};
        auto continuationBytes = std::uint8_t{0};

        for (auto const character : value)
        {
            auto const byte = static_cast<std::uint32_t>(
                static_cast<unsigned char>(character)
            );
            if (continuationBytes == 0)
            {
                if (byte <= 0x7FU)
                {
                    continue;
                }

                if (byte >= 0xC2U && byte <= 0xDFU)
                {
                    codePoint = byte & 0x1FU;
                    minimumCodePoint = 0x80U;
                    continuationBytes = 1;
                    continue;
                }

                if (byte >= 0xE0U && byte <= 0xEFU)
                {
                    codePoint = byte & 0x0FU;
                    minimumCodePoint = 0x800U;
                    continuationBytes = 2;
                    continue;
                }

                if (byte >= 0xF0U && byte <= 0xF4U)
                {
                    codePoint = byte & 0x07U;
                    minimumCodePoint = 0x10000U;
                    continuationBytes = 3;
                    continue;
                }

                return false;
            }

            if ((byte & 0xC0U) != 0x80U)
            {
                return false;
            }

            codePoint = (codePoint << 6U) | (byte & 0x3FU);
            --continuationBytes;
            if (
                continuationBytes == 0
                && (
                    codePoint < minimumCodePoint
                    || codePoint > 0x10FFFFU
                    || (codePoint >= 0xD800U && codePoint <= 0xDFFFU)
                )
            )
            {
                return false;
            }
        }

        return continuationBytes == 0;
    }
}

namespace uf
{
    Label::Label(std::string value) noexcept
        : m_value{std::move(value)}
    {
    }

    auto Label::create(std::string value) -> Result<Label>
    {
        if (!isValidUtf8(value))
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "label must contain valid UTF-8"
            );
        }

        return Label{std::move(value)};
    }

    auto Label::value() const noexcept -> std::string const& { return m_value; }

    auto TargetGeneration::next() const -> Result<TargetGeneration>
    {
        auto const nextGeneration = m_generation.next();
        if (!nextGeneration)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "target generation " + std::to_string(value()) + " cannot be incremented"
            );
        }

        return TargetGeneration{*nextGeneration};
    }
}
