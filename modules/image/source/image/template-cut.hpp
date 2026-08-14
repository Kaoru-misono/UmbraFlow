#pragma once

#include "png.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <span>

namespace uf::image
{
    enum class TemplateAlphaDerivation : uint8
    {
        SingleSourceOpaque,
        ObservedSpread,
    };

    struct TemplateCut final
    {
        RgbaImage               image{};
        TemplateAlphaDerivation alphaDerivation{};
    };

    // Uses the first source for the template colour plane. The alpha plane is
    // derived from Gray8 spread across every source crop. A pixel reaches full
    // weight only where the observed spread is zero. With one source there is
    // no observed spread, so the alpha plane is deliberately fully opaque.
    [[nodiscard]]
    auto cutRgba8Template(
        std::span<RgbaImage const> sources,
        PixelRect rect
    ) -> Result<TemplateCut>;
}
