#include "capture-output.hpp"

#include "ffi/png-encoder.hpp"

#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/safety/checked-access.hpp>
#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    [[nodiscard]]
    auto frameRgba(uf::Frame const& frame) -> uf::Result<std::vector<std::byte>>
    {
        if (frame.pixelFormat() != uf::PixelFormat::Bgra8)
        {
            return uf::fail(
                uf::AutomationErrorKind::InternalInvariant,
                std::format(
                    "capture frame {} is not BGRA8",
                    frame.id().value()
                )
            );
        }

        auto const width = uf::checkedCast<std::size_t>(frame.width());
        auto const height = uf::checkedCast<std::size_t>(frame.height());
        auto rowBytes = std::optional<std::size_t>{};
        if (width)
        {
            rowBytes = uf::checkedMultiply(
                *width,
                uf::bytesPerPixel(uf::PixelFormat::Bgra8)
            );
        }
        auto expectedBytes = std::optional<std::size_t>{};
        if (rowBytes && height)
        {
            expectedBytes = uf::checkedMultiply(*rowBytes, *height);
        }
        auto const pixels = frame.pixels();
        if (
            !rowBytes
            || !expectedBytes
            || frame.stride() != *rowBytes
            || pixels->size() != *expectedBytes
        )
        {
            return uf::fail(
                uf::AutomationErrorKind::InternalInvariant,
                std::format(
                    "capture frame {} is not tightly packed BGRA8",
                    frame.id().value()
                )
            );
        }

        auto const bgra = pixels->bytes();
        auto rgba = std::vector<std::byte>{
            bgra.begin(),
            bgra.end()
        };
        for (auto index = std::size_t{0}; index < rgba.size(); index += 4U)
        {
            std::swap(
                uf::checkedAt(rgba, index),
                uf::checkedAt(rgba, index + 2U)
            );
        }
        return rgba;
    }
}

namespace uf::m0_demo
{
    auto indexedOutputPath(
        std::filesystem::path const& output,
        uint32 index,
        uint32 frameCount
    ) -> std::filesystem::path
    {
        if (frameCount == 1U)
        {
            return output;
        }

        auto filename = output.stem();
        filename += std::filesystem::path{
            "-" + std::to_string(index + 1U)
        };
        filename += output.extension();
        return output.parent_path() / filename;
    }

    auto writeFramePng(
        Frame const& frame,
        std::filesystem::path const& output
    ) -> Status
    {
        UF_TRY_VALUE(rgba, frameRgba(frame));
        return ffi::writeRgbaPng(
            output,
            frame.width(),
            frame.height(),
            rgba
        );
    }

    auto encodeFramePng(
        Frame const& frame,
        std::filesystem::path const& output
    ) -> Result<std::vector<std::byte>>
    {
        UF_TRY_VALUE(rgba, frameRgba(frame));
        return ffi::encodeRgbaPng(
            output,
            frame.width(),
            frame.height(),
            rgba
        );
    }

    auto captureFramePng(
        WgcCaptureSession& session,
        std::filesystem::path const& output
    ) -> Result<Frame>
    {
        UF_TRY_VALUE(frame, session.capture());
        UF_TRY(writeFramePng(frame, output));
        return frame;
    }
}
