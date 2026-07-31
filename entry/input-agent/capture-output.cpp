#include "capture-output.hpp"

#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>
#include <domain/error.hpp>
#include <image/pixels.hpp>
#include <image/png.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace uf::input_agent
{
    namespace
    {
        [[nodiscard]]
        auto frameRgba(Frame const& frame) -> Result<std::vector<std::byte>>
        {
            if (frame.pixelFormat() != PixelFormat::Bgra8)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    std::format(
                        "capture frame {} is not BGRA8",
                        frame.id().value()
                    )
                );
            }

            auto const width = checkedCast<std::size_t>(frame.width());
            auto const height = checkedCast<std::size_t>(frame.height());
            auto rowBytes = std::optional<std::size_t>{};
            if (width)
            {
                rowBytes = checkedMultiply(
                    *width,
                    bytesPerPixel(PixelFormat::Bgra8)
                );
            }
            auto expectedBytes = std::optional<std::size_t>{};
            if (rowBytes && height)
            {
                expectedBytes = checkedMultiply(*rowBytes, *height);
            }
            auto const pixels = frame.pixels();
            if (
                !rowBytes
                || !expectedBytes
                || frame.stride() != *rowBytes
                || pixels->size() != *expectedBytes
            )
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    std::format(
                        "capture frame {} is not tightly packed BGRA8",
                        frame.id().value()
                    )
                );
            }

            auto const bgra = pixels->bytes();
            auto bgraCopy = std::vector<std::byte>{
                bgra.begin(),
                bgra.end()
            };
            return image::bgra8ToRgba8(std::move(bgraCopy));
        }
    }

    auto writeFramePng(
        Frame const& frame,
        std::filesystem::path const& output
    ) -> Status
    {
        UF_TRY_VALUE(rgba, frameRgba(frame));
        return image::writeRgbaPng(
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
        return image::encodeRgbaPng(
            output.string(),
            frame.width(),
            frame.height(),
            rgba
        );
    }
}
