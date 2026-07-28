#include "utf8.hpp"

#include "unsafe/unicode-code-unit.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace uf
{
    namespace
    {
        auto appendUtf8CodeUnit(
            std::string& output,
            uint32 value
        ) -> void
        {
            auto const codeUnit = checkedCast<uint8>(value);
            UF_CHECK(codeUnit.has_value());
            output.push_back(text_unsafe::utf8CodeUnit(*codeUnit));
        }

        template <typename ScalarSink>
        [[nodiscard]]
        auto decodeUtf8(
            std::string_view value,
            ScalarSink&& scalarSink
        ) noexcept(noexcept(scalarSink(uint32{}))) -> bool
        {
            auto codePoint         = uint32{0};
            auto minimumCodePoint  = uint32{0};
            auto continuationBytes = uint8{0};

            for (auto const character : value)
            {
                auto const byte = uint32{
                    text_unsafe::utf8CodeUnitValue(character)
                };
                if (continuationBytes == 0U)
                {
                    if (byte <= 0x7FU)
                    {
                        scalarSink(byte);
                        continue;
                    }
                    if (byte >= 0xC2U && byte <= 0xDFU)
                    {
                        codePoint         = byte & 0x1FU;
                        minimumCodePoint  = 0x80U;
                        continuationBytes = 1U;
                        continue;
                    }
                    if (byte >= 0xE0U && byte <= 0xEFU)
                    {
                        codePoint         = byte & 0x0FU;
                        minimumCodePoint  = 0x800U;
                        continuationBytes = 2U;
                        continue;
                    }
                    if (byte >= 0xF0U && byte <= 0xF4U)
                    {
                        codePoint         = byte & 0x07U;
                        minimumCodePoint  = 0x10000U;
                        continuationBytes = 3U;
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
                if (continuationBytes != 0U)
                {
                    continue;
                }
                if (
                    codePoint < minimumCodePoint
                    || codePoint > 0x10FFFFU
                    || (codePoint >= 0xD800U && codePoint <= 0xDFFFU)
                )
                {
                    return false;
                }
                scalarSink(codePoint);
            }

            return continuationBytes == 0U;
        }
    }

    auto isValidUtf8(std::string_view value) noexcept -> bool
    {
        return decodeUtf8(value, [](uint32) noexcept {});
    }

    auto decodeUtf8Scalars(
        std::string_view value
    ) -> std::optional<std::vector<uint32>>
    {
        auto scalars = std::vector<uint32>{};
        scalars.reserve(value.size());
        if (
            !decodeUtf8(
                value,
                [&scalars](uint32 scalar)
                {
                    scalars.emplace_back(scalar);
                }
            )
        )
        {
            return std::nullopt;
        }
        return scalars;
    }

    auto appendUtf8Scalar(
        std::string& output,
        uint32 codePoint
    ) -> void
    {
        UF_CHECK(
            codePoint <= 0x10FFFFU
            && (codePoint < 0xD800U || codePoint > 0xDFFFU)
        );

        if (codePoint <= 0x7FU)
        {
            appendUtf8CodeUnit(output, codePoint);
            return;
        }
        if (codePoint <= 0x7FFU)
        {
            appendUtf8CodeUnit(output, 0xC0U | (codePoint >> 6U));
            appendUtf8CodeUnit(output, 0x80U | (codePoint & 0x3FU));
            return;
        }
        if (codePoint <= 0xFFFFU)
        {
            appendUtf8CodeUnit(output, 0xE0U | (codePoint >> 12U));
            appendUtf8CodeUnit(
                output,
                0x80U | ((codePoint >> 6U) & 0x3FU)
            );
            appendUtf8CodeUnit(output, 0x80U | (codePoint & 0x3FU));
            return;
        }

        appendUtf8CodeUnit(output, 0xF0U | (codePoint >> 18U));
        appendUtf8CodeUnit(
            output,
            0x80U | ((codePoint >> 12U) & 0x3FU)
        );
        appendUtf8CodeUnit(
            output,
            0x80U | ((codePoint >> 6U) & 0x3FU)
        );
        appendUtf8CodeUnit(output, 0x80U | (codePoint & 0x3FU));
    }
}
