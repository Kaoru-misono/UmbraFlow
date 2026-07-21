#include "png-encoder.hpp"

#include "png-limits.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#define STBI_WRITE_NO_STDIO
#if !defined(__clang_analyzer__)
// Clang cannot associate a call-site suppression with stb's internal ArrayBound
// false positive. Keep only the third-party implementation opaque to analysis;
// the production compiler still compiles the original implementation below.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
// stb's stretchy buffer otherwise continues after a failed realloc in release
// builds. A release-active check terminates before that buffer can be written.
#define STBIW_ASSERT(condition) \
    UF_CHECK_MSG((condition), "stb_image_write failed closed")

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

#include <stb_image_write.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#undef STBIW_ASSERT

namespace uf::m0_demo::ffi
{
    namespace
    {
        constexpr auto g_rgbaChannels = std::size_t{4};
        // stb stores (row bytes + one filter byte) * height in signed working
        // integers. The shared decoder quotas keep that total far below INT_MAX.
        constexpr auto g_maximumFilteredPngBytes = (
            g_maximumPngPixels * g_rgbaChannels
            + g_maximumPngDimension
        );

        static_assert(
            g_maximumFilteredPngBytes
            < static_cast<std::size_t>(std::numeric_limits<int>::max()) / 4U
        );

        struct EncodedPng final
        {
            std::vector<std::byte> m_bytes;
            bool m_callbackFailed{};
        };

        auto appendEncodedPng(
            void* p_context,
            void* p_data,
            int size
        ) noexcept -> void
        {
            if (p_context == nullptr)
            {
                return;
            }

            // SAFETY: encodeRgbaPng passes a live EncodedPng as the synchronous stb
            // callback context. stb retains neither the context nor the data after
            // this call, and no exception is allowed to cross the C callback boundary.
            auto& encoded = *static_cast<EncodedPng*>(p_context);
            if (encoded.m_callbackFailed)
            {
                return;
            }
            if (p_data == nullptr || size <= 0)
            {
                encoded.m_callbackFailed = true;
                return;
            }

            auto const byteCount = checkedCast<std::size_t>(size);
            auto finalSize = std::optional<std::size_t>{};
            if (byteCount)
            {
                finalSize = checkedAdd(encoded.m_bytes.size(), *byteCount);
            }
            if (!byteCount || !finalSize)
            {
                encoded.m_callbackFailed = true;
                return;
            }

            try
            {
                auto const offset = encoded.m_bytes.size();
                encoded.m_bytes.resize(*finalSize);
                auto destination = std::span<std::byte>{encoded.m_bytes}.subspan(
                    offset,
                    *byteCount
                );
                // SAFETY: stb promises size readable bytes in p_data for this
                // synchronous callback. destination owns exactly byteCount writable
                // bytes, the ranges do not overlap, and neither pointer escapes.
                std::memcpy(destination.data(), p_data, *byteCount);
            }
            catch (...)
            {
                encoded.m_callbackFailed = true;
            }
        }

        [[nodiscard]]
        auto invalidPng(std::string message) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::move(message)
            );
        }

        [[nodiscard]]
        auto currentIoError() -> std::error_code
        {
            if (errno != 0)
            {
                return std::error_code{errno, std::generic_category()};
            }
            return std::make_error_code(std::io_errc::stream);
        }

        [[nodiscard]]
        auto pngIoFailure(
            std::string_view operation,
            std::filesystem::path const& path,
            std::error_code error
        ) -> std::unexpected<Error>
        {
            return fail(
                ErrorCode::Io,
                automationErrorDetailCode(
                    AutomationErrorKind::InvalidResource
                ),
                std::format(
                    "failed to {} PNG {}: {}",
                    operation,
                    path.string(),
                    error.message()
                ),
                static_cast<int64>(error.value())
            );
        }
    }

    auto encodeRgbaPng(
        std::filesystem::path const& path,
        uint32 width,
        uint32 height,
        std::span<std::byte const> pixels
    ) -> Result<std::vector<std::byte>>
    {
        if (width == 0U || height == 0U)
        {
            return invalidPng(
                std::format(
                    "cannot encode zero-sized PNG {}x{} for {}",
                    width,
                    height,
                    path.string()
                )
            );
        }

        if (
            width > g_maximumPngDimension
            || height > g_maximumPngDimension
        )
        {
            return invalidPng(
                std::format(
                    "PNG dimensions {}x{} for {} exceed {} pixels per axis",
                    width,
                    height,
                    path.string(),
                    g_maximumPngDimension
                )
            );
        }

        auto const widthSize = checkedCast<std::size_t>(width);
        auto const heightSize = checkedCast<std::size_t>(height);
        auto pixelCount = std::optional<std::size_t>{};
        if (widthSize && heightSize)
        {
            pixelCount = checkedMultiply(*widthSize, *heightSize);
        }
        if (!pixelCount || *pixelCount > g_maximumPngPixels)
        {
            return invalidPng(
                std::format(
                    "PNG dimensions {}x{} for {} exceed the pixel quota",
                    width,
                    height,
                    path.string()
                )
            );
        }

        auto rowBytes = std::optional<std::size_t>{};
        if (widthSize)
        {
            rowBytes = checkedMultiply(*widthSize, g_rgbaChannels);
        }
        auto filteredRowBytes = std::optional<std::size_t>{};
        if (rowBytes)
        {
            filteredRowBytes = checkedAdd(*rowBytes, std::size_t{1});
        }
        auto filteredBytes = std::optional<std::size_t>{};
        if (filteredRowBytes && heightSize)
        {
            filteredBytes = checkedMultiply(*filteredRowBytes, *heightSize);
        }
        if (
            !filteredBytes
            || *filteredBytes > g_maximumFilteredPngBytes
        )
        {
            return invalidPng(
                std::format(
                    "PNG dimensions {}x{} for {} exceed the filtered-byte quota",
                    width,
                    height,
                    path.string()
                )
            );
        }
        auto expectedBytes = std::optional<std::size_t>{};
        if (rowBytes && heightSize)
        {
            expectedBytes = checkedMultiply(*rowBytes, *heightSize);
        }
        if (!rowBytes || !expectedBytes || pixels.size() != *expectedBytes)
        {
            return invalidPng(
                std::format(
                    "RGBA pixels for {} do not match tightly packed {}x{} geometry",
                    path.string(),
                    width,
                    height
                )
            );
        }

        auto const encodedWidth = checkedCast<int>(width);
        auto const encodedHeight = checkedCast<int>(height);
        auto const encodedStride = checkedCast<int>(*rowBytes);
        if (!encodedWidth || !encodedHeight || !encodedStride)
        {
            return invalidPng(
                std::format(
                    "PNG dimensions {}x{} for {} exceed stb_image_write limits",
                    width,
                    height,
                    path.string()
                )
            );
        }

        auto encoded = EncodedPng{};
        // SAFETY: geometry validation proves pixels contains exactly height
        // tightly packed RGBA rows of encodedStride bytes. stb reads the span
        // synchronously, invokes the non-throwing callback with the live encoded
        // context, and retains neither pointer after this call returns. The
        // dimension quota also proves stb's signed row-buffer arithmetic remains
        // positive and bounded.
        auto const encodedOk = stbi_write_png_to_func(
            appendEncodedPng,
            &encoded,
            *encodedWidth,
            *encodedHeight,
            static_cast<int>(g_rgbaChannels),
            pixels.data(),
            *encodedStride
        );
        if (
            encodedOk == 0
            || encoded.m_callbackFailed
            || encoded.m_bytes.empty()
        )
        {
            return fail(
                ErrorCode::External,
                automationErrorDetailCode(
                    AutomationErrorKind::InvalidResource
                ),
                std::format(
                    "failed to encode PNG {} with stb_image_write",
                    path.string()
                )
            );
        }

        return std::move(encoded.m_bytes);
    }

    auto writeRgbaPng(
        std::filesystem::path const& path,
        uint32 width,
        uint32 height,
        std::span<std::byte const> pixels
    ) -> Status
    {
        UF_TRY_VALUE(
            encoded,
            encodeRgbaPng(path, width, height, pixels)
        );

        errno = 0;
        auto stream = std::ofstream{
            path,
            std::ios::binary | std::ios::trunc
        };
        if (!stream.is_open())
        {
            return pngIoFailure("open", path, currentIoError());
        }

        auto const streamSize = checkedCast<std::streamsize>(encoded.size());
        if (!streamSize)
        {
            return invalidPng(
                std::format(
                    "encoded PNG {} is too large for file output",
                    path.string()
                )
            );
        }

        errno = 0;
        // char and std::byte share byte alignment.
        // SAFETY: encoded owns the streamSize live bytes that ofstream::write
        // reads synchronously without retaining the converted pointer.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        auto const* source = reinterpret_cast<char const*>(encoded.data());
        stream.write(source, *streamSize);
        if (!stream)
        {
            return pngIoFailure("write", path, currentIoError());
        }

        errno = 0;
        stream.flush();
        if (!stream)
        {
            return pngIoFailure("flush", path, currentIoError());
        }

        errno = 0;
        stream.close();
        if (!stream)
        {
            return pngIoFailure("close", path, currentIoError());
        }
        return ok();
    }
}
