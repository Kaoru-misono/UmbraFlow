#include "ocr.hpp"

#include "platform/ocr-engine-binding.hpp"

#include <core/error/result.hpp>

#include <domain/error.hpp>

#include <image/pixels.hpp>
#include <image/png.hpp>

#include <ocr/engine.hpp>

#include <vision/bgra-image.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <utility>

namespace uf::cli
{
    auto ocrProduct(OcrArgs const& args) -> Result<ImageText>
    {
        UF_TRY_VALUE_CONTEXT(
            decoded,
            image::loadPng(args.image),
            std::format("--image {}", args.image.string())
        );

        // The recognition model was trained on OpenCV's channel order, and a
        // frame handed over in the wrong one comes back as confident nonsense
        // rather than as a failure. loadPng yields RGBA; a live capture is
        // already BGRA, which is why the swizzle belongs here rather than in
        // the engine.
        UF_TRY_VALUE(pixels, image::rgba8ToBgra8(std::move(decoded.pixels)));

        // Refused here as well as inside the engine, which reports only that
        // the rect does not fit. This one names both the rectangle and the
        // extent it missed, and it answers before twenty-one megabytes of
        // weights are loaded to say the same thing.
        if (args.rect)
        {
            UF_TRY_CONTEXT(
                args.rect->ensureWithinExtent(decoded.width, decoded.height),
                std::format("--rect against --image {}", args.image.string())
            );
        }

        auto const modelDirectory = std::optional<std::filesystem::path>{
            args.ocrModels
        };
        UF_TRY_VALUE_CONTEXT(
            engine,
            platform::bindOcrEngine(modelDirectory),
            std::format("--ocr-models {}", args.ocrModels.string())
        );
        // bindOcrEngine answers an absent directory with a null engine. This
        // verb always names one, so reaching here would mean that contract
        // changed; it must not become an image that silently holds no text.
        if (engine == nullptr)
        {
            return fail(
                AutomationErrorKind::UnsupportedCapability,
                std::format(
                    "no ocr engine was bound for --ocr-models \"{}\"",
                    args.ocrModels.string()
                )
            );
        }

        // loadPng packs rows tightly, so the stride is the row itself. `pixels`
        // is declared above and outlives the view for the rest of this scope,
        // which is the backing-owner contract BgraImage::create states.
        auto const stride = static_cast<std::size_t>(decoded.width) * 4U;
        UF_TRY_VALUE(
            view,
            BgraImage::create(pixels, decoded.width, decoded.height, stride)
        );

        UF_TRY_VALUE_CONTEXT(
            readout,
            engine->read(
                view,
                ocr::ReadSpec{
                    .rect         = args.rect,
                    .layout       = args.layout,
                    .maximumLines = args.maximumLines,
                }
            ),
            std::format("reading --image {}", args.image.string())
        );

        return ImageText{
            .width  = decoded.width,
            .height = decoded.height,
            .lines  = std::move(readout.lines),
        };
    }
}
