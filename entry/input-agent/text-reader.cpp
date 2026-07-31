#include "text-reader.hpp"

#include <core/error/result.hpp>
#include <core/types/enum-reflection.hpp>
#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <vision/bgra-image.hpp>

#include <format>
#include <string>

namespace uf::input_agent
{
    auto frameAsBgraImage(Frame const& frame) -> Result<BgraImage>
    {
        if (frame.pixelFormat() != PixelFormat::Bgra8)
        {
            return fail(
                AutomationErrorKind::UnsupportedCapability,
                std::format(
                    "input-agent cannot read text from a {} frame: recognition "
                    "runs on the BGRA8 pixels a capture session produces",
                    enumName(frame.pixelFormat()).value_or("unknown")
                )
            );
        }

        // Frame::create refuses a null buffer, so a constructed frame always has
        // one. The share taken here keeps it alive only for this call; the view
        // returned outlives it on the caller's own frame, which holds the other
        // share for as long as the contract in the header requires.
        auto const p_pixels = frame.pixels();
        return BgraImage::create(
            p_pixels->bytes(),
            frame.width(),
            frame.height(),
            frame.stride()
        );
    }
}
