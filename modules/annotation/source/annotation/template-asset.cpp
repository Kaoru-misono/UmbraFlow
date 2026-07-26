#include "template-asset.hpp"

#include <core/types/integer.hpp>

#include <image/pixels.hpp>
#include <image/png.hpp>

#include <format>
#include <string>
#include <utility>

namespace uf::annotation
{
    auto generateTemplateAsset(
        std::span<std::byte const> sourceBgra,
        uint32 sourceWidth,
        uint32 sourceHeight,
        std::size_t sourceStride,
        PixelRect templateRect
    ) -> Result<TemplateAsset>
    {
        UF_TRY_VALUE(
            croppedBgra,
            image::cropBgra8(
                sourceBgra,
                sourceWidth,
                sourceHeight,
                sourceStride,
                templateRect
            )
        );
        UF_TRY_VALUE(
            croppedRgba,
            image::bgra8ToRgba8(std::move(croppedBgra))
        );
        UF_TRY_VALUE(
            encoded,
            image::encodeRgbaPng(
                std::format(
                    "template [{}, {}, {}, {}]",
                    templateRect.x(),
                    templateRect.y(),
                    templateRect.width(),
                    templateRect.height()
                ),
                templateRect.width(),
                templateRect.height(),
                croppedRgba
            )
        );
        UF_TRY_VALUE(hash, sha256(encoded));

        auto relativePath = std::string{"assets/templates/"};
        relativePath += hash.hex();
        relativePath += ".png";
        return TemplateAsset{
            .hash         = hash,
            .relativePath = std::move(relativePath),
            .pngBytes     = std::move(encoded),
            .width        = templateRect.width(),
            .height       = templateRect.height(),
        };
    }
}
