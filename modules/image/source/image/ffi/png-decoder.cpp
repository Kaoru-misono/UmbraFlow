#include <image/png.hpp>

#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
#define STB_IMAGE_IMPLEMENTATION

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wundef"
#endif

#include <stb_image.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace uf::image
{
    namespace
    {
        struct Stbi8ImageDeleter final
        {
            auto operator()(stbi_uc* p_image) const noexcept -> void
            {
                // SAFETY: p_image is either null or the unique allocation returned
                // by stbi_load_from_memory. stbi_image_free releases that allocation
                // exactly once and no observer is retained after this call.
                stbi_image_free(p_image);
            }
        };

        struct Stbi16ImageDeleter final
        {
            auto operator()(stbi_us* p_image) const noexcept -> void
            {
                // SAFETY: p_image is either null or the unique allocation returned
                // by stbi_load_16_from_memory. stbi_image_free releases that
                // allocation exactly once and no observer survives this call.
                stbi_image_free(p_image);
            }
        };

        [[nodiscard]]
        auto invalidResource(std::string message) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::move(message)
            );
        }

        struct PngMetadata final
        {
            uint32 m_width;
            uint32 m_height;
            uint8 m_bitDepth;
        };

        constexpr auto g_pngSignature = std::array{
            std::byte{0x89},
            std::byte{0x50},
            std::byte{0x4E},
            std::byte{0x47},
            std::byte{0x0D},
            std::byte{0x0A},
            std::byte{0x1A},
            std::byte{0x0A},
        };
        constexpr auto g_ihdrType = std::array{
            std::byte{0x49},
            std::byte{0x48},
            std::byte{0x44},
            std::byte{0x52},
        };
        constexpr auto g_iendType = std::array{
            std::byte{0x49},
            std::byte{0x45},
            std::byte{0x4E},
            std::byte{0x44},
        };
        constexpr auto g_pngChunkOverhead = std::size_t{12};
        constexpr auto g_ihdrDataBytes = uint32{13};

        [[nodiscard]]
        auto readBigEndianU32(
            std::span<std::byte const> encoded,
            std::size_t offset
        ) noexcept -> uint32
        {
            return (
                std::to_integer<uint32>(encoded[offset]) << 24U
                | std::to_integer<uint32>(encoded[offset + 1U]) << 16U
                | std::to_integer<uint32>(encoded[offset + 2U]) << 8U
                | std::to_integer<uint32>(encoded[offset + 3U])
            );
        }

        template <std::size_t Size>
        [[nodiscard]]
        auto bytesEqualAt(
            std::span<std::byte const> encoded,
            std::size_t offset,
            std::array<std::byte, Size> const& expected
        ) noexcept -> bool
        {
            return std::ranges::equal(encoded.subspan(offset, Size), expected);
        }

        [[nodiscard]]
        auto validatePngStructure(
            std::span<std::byte const> encoded,
            std::string_view resourceName
        ) -> Result<PngMetadata>
        {
            if (
                encoded.size() < g_pngSignature.size()
                || !bytesEqualAt(encoded, 0, g_pngSignature)
            )
            {
                return invalidResource(
                    std::format("failed to load template {}: not a PNG", resourceName)
                );
            }

            auto metadata = PngMetadata{};
            auto offset = g_pngSignature.size();
            auto firstChunk = true;
            auto sawIend = false;
            while (offset < encoded.size())
            {
                auto const remaining = encoded.size() - offset;
                if (remaining < g_pngChunkOverhead)
                {
                    return invalidResource(
                        std::format(
                            "failed to load template {}: malformed PNG (truncated chunk header)",
                            resourceName
                        )
                    );
                }

                auto const dataBytes = readBigEndianU32(encoded, offset);
                auto const dataSize = checkedCast<std::size_t>(dataBytes);
                if (!dataSize || *dataSize > remaining - g_pngChunkOverhead)
                {
                    return invalidResource(
                        std::format(
                            "failed to load template {}: malformed PNG (declared chunk length exceeds the input)",
                            resourceName
                        )
                    );
                }

                auto const isIhdr = bytesEqualAt(encoded, offset + 4U, g_ihdrType);
                auto const isIend = bytesEqualAt(encoded, offset + 4U, g_iendType);
                if (firstChunk)
                {
                    if (!isIhdr)
                    {
                        return invalidResource(
                            std::format(
                                "failed to load template {}: malformed PNG (IHDR is not first)",
                                resourceName
                            )
                        );
                    }
                    if (dataBytes != g_ihdrDataBytes)
                    {
                        return invalidResource(
                            std::format(
                                "failed to load template {}: malformed PNG (invalid IHDR length)",
                                resourceName
                            )
                        );
                    }

                    metadata = PngMetadata{
                        .m_width = readBigEndianU32(encoded, offset + 8U),
                        .m_height = readBigEndianU32(encoded, offset + 12U),
                        .m_bitDepth = std::to_integer<uint8>(encoded[offset + 16U]),
                    };
                    firstChunk = false;
                }
                else if (isIhdr)
                {
                    return invalidResource(
                        std::format(
                            "failed to load template {}: malformed PNG (duplicate IHDR)",
                            resourceName
                        )
                    );
                }

                offset += g_pngChunkOverhead + *dataSize;
                if (isIend)
                {
                    if (dataBytes != 0U || offset != encoded.size())
                    {
                        return invalidResource(
                            std::format(
                                "failed to load template {}: malformed PNG (invalid IEND)",
                                resourceName
                            )
                        );
                    }
                    sawIend = true;
                    break;
                }
            }

            if (firstChunk || !sawIend)
            {
                return invalidResource(
                    std::format(
                        "failed to load template {}: malformed PNG (missing IEND)",
                        resourceName
                    )
                );
            }
            return metadata;
        }
    }

    auto decodePng(
        std::span<std::byte const> encoded,
        std::string_view resourceName
    ) -> Result<RgbaImage>
    {
        if (encoded.empty())
        {
            return invalidResource(
                std::format("failed to load template {}: empty PNG", resourceName)
            );
        }
        if (encoded.size() > g_maximumPngFileBytes)
        {
            return invalidResource(
                std::format(
                    "template {} is oversized: {} encoded bytes exceeds {}",
                    resourceName,
                    encoded.size(),
                    g_maximumPngFileBytes
                )
            );
        }

        UF_TRY_VALUE(metadata, validatePngStructure(encoded, resourceName));

        auto const encodedSize = checkedCast<int>(encoded.size());
        if (!encodedSize)
        {
            return invalidResource(
                std::format("template {} is too large for the PNG decoder", resourceName)
            );
        }

        if (metadata.m_width == 0U || metadata.m_height == 0U)
        {
            return invalidResource(
                std::format(
                    "failed to load template {}: malformed PNG (zero dimension)",
                    resourceName
                )
            );
        }

        if (
            metadata.m_width > g_maximumPngDimension
            || metadata.m_height > g_maximumPngDimension
        )
        {
            return invalidResource(
                std::format(
                    "template {} is oversized: {}x{} exceeds {} pixels per axis",
                    resourceName,
                    metadata.m_width,
                    metadata.m_height,
                    g_maximumPngDimension
                )
            );
        }

        auto const widthSize = checkedCast<std::size_t>(metadata.m_width);
        auto const heightSize = checkedCast<std::size_t>(metadata.m_height);
        if (!widthSize || !heightSize)
        {
            return invalidResource(
                std::format("template {} dimensions are not addressable", resourceName)
            );
        }
        auto const pixelCount = checkedMultiply(*widthSize, *heightSize);
        if (!pixelCount || *pixelCount > g_maximumPngPixels)
        {
            return invalidResource(
                std::format(
                    "template {} is oversized: {}x{} exceeds the pixel quota",
                    resourceName,
                    metadata.m_width,
                    metadata.m_height
                )
            );
        }

        auto const decodedBytes = checkedMultiply(*pixelCount, std::size_t{4});
        if (!decodedBytes)
        {
            return invalidResource(
                std::format("template {} decoded size overflowed", resourceName)
            );
        }

        auto const expectedWidth = checkedCast<int>(metadata.m_width);
        auto const expectedHeight = checkedCast<int>(metadata.m_height);
        if (!expectedWidth || !expectedHeight)
        {
            return invalidResource(
                std::format("template {} has invalid PNG dimensions", resourceName)
            );
        }

        auto decodedWidth = int{};
        auto decodedHeight = int{};
        auto decodedChannels = int{};
        // validatePngStructure bounded every chunk before stb can inspect it.
        // SAFETY: encoded owns the live byte range that byte-sized stbi_uc reads
        // synchronously without retaining the converted pointer.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        auto const* encodedBytes = reinterpret_cast<stbi_uc const*>(encoded.data());
        auto pixels = std::vector<std::byte>(*decodedBytes);
        if (metadata.m_bitDepth == 16U)
        {
            // SAFETY: stb returns a unique RGBA16 allocation or null. The
            // custom-deleter owner takes it immediately and releases it once.
            auto decoded = std::unique_ptr<stbi_us, Stbi16ImageDeleter>{
                stbi_load_16_from_memory(
                    encodedBytes,
                    *encodedSize,
                    &decodedWidth,
                    &decodedHeight,
                    &decodedChannels,
                    STBI_rgb_alpha
                )
            };
            if (!decoded)
            {
                return invalidResource(
                    std::format(
                        "failed to load template {}: malformed PNG",
                        resourceName
                    )
                );
            }
            if (decodedWidth != *expectedWidth || decodedHeight != *expectedHeight)
            {
                return invalidResource(
                    std::format(
                        "template {} dimensions changed during PNG decode",
                        resourceName
                    )
                );
            }

            // SAFETY: the successful RGBA16 decode returned exactly one stbi_us
            // sample per byte in the validated RGBA8 destination size. decoded
            // owns that allocation for the lifetime of this call-scoped view.
            // Adopting a foreign pointer and length is the purpose of this FFI
            // boundary, so the bounds diagnostic is suppressed for that step
            // alone and the result is a bounded view for everything after it.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif
            auto const decodedSamples = std::span<stbi_us const>{
                decoded.get(),
                *decodedBytes
            };
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
            std::ranges::transform(
                decodedSamples,
                pixels.begin(),
                [](stbi_us sample) -> std::byte
                {
                    auto const widened = static_cast<uint32>(sample);
                    auto const rounded = (widened * 255U + 32'767U) / 65'535U;
                    return static_cast<std::byte>(rounded);
                }
            );
        }
        else
        {
            // SAFETY: stb returns a unique RGBA8 allocation or null. The
            // custom-deleter owner takes it immediately and releases it once.
            auto decoded = std::unique_ptr<stbi_uc, Stbi8ImageDeleter>{
                stbi_load_from_memory(
                    encodedBytes,
                    *encodedSize,
                    &decodedWidth,
                    &decodedHeight,
                    &decodedChannels,
                    STBI_rgb_alpha
                )
            };
            if (!decoded)
            {
                return invalidResource(
                    std::format(
                        "failed to load template {}: malformed PNG",
                        resourceName
                    )
                );
            }
            if (decodedWidth != *expectedWidth || decodedHeight != *expectedHeight)
            {
                return invalidResource(
                    std::format(
                        "template {} dimensions changed during PNG decode",
                        resourceName
                    )
                );
            }

            // SAFETY: decoded points to decodedBytes RGBA bytes because
            // STBI_rgb_alpha was requested. pixels owns exactly that many
            // writable bytes; the ranges are live and do not overlap. Reading
            // through a foreign pointer is this boundary's purpose, so the
            // bounds diagnostic is suppressed for the copy alone.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif
            std::memcpy(pixels.data(), decoded.get(), *decodedBytes);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
        }
        return RgbaImage{
            .m_width = metadata.m_width,
            .m_height = metadata.m_height,
            .m_pixels = std::move(pixels),
        };
    }

    auto loadPng(std::filesystem::path const& path) -> Result<RgbaImage>
    {
        auto fileError = std::error_code{};
        auto const fileBytes = std::filesystem::file_size(path, fileError);
        if (fileError)
        {
            return invalidResource(
                std::format(
                    "failed to load template {}: {}",
                    path.string(),
                    fileError.message()
                )
            );
        }
        if (fileBytes > g_maximumPngFileBytes)
        {
            return invalidResource(
                std::format(
                    "template {} is oversized: {} encoded bytes exceeds {}",
                    path.string(),
                    fileBytes,
                    g_maximumPngFileBytes
                )
            );
        }

        auto const size = checkedCast<std::size_t>(fileBytes);
        auto const streamSize = checkedCast<std::streamsize>(fileBytes);
        if (!size || !streamSize)
        {
            return invalidResource(
                std::format("template {} file size is not addressable", path.string())
            );
        }

        auto stream = std::ifstream{path, std::ios::binary};
        if (!stream.is_open())
        {
            return invalidResource(
                std::format("failed to load template {}: cannot open file", path.string())
            );
        }

        auto encoded = std::vector<std::byte>(*size);
        if (!encoded.empty())
        {
            // char and std::byte share byte alignment.
            // SAFETY: encoded owns the streamSize live bytes that ifstream::read
            // writes synchronously without retaining the converted pointer.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            auto* const destination = reinterpret_cast<char*>(encoded.data());
            stream.read(destination, *streamSize);
            if (!stream || stream.gcount() != *streamSize)
            {
                return invalidResource(
                    std::format("failed to load template {}: incomplete read", path.string())
                );
            }
        }

        return decodePng(encoded, path.string());
    }
}
