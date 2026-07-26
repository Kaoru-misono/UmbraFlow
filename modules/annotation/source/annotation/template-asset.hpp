#pragma once

#include "content-hash.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace uf::annotation
{
    struct TemplateAsset final
    {
        ContentHash hash;
        std::string relativePath{};

        std::vector<std::byte> pngBytes{};
        uint32                 width{};
        uint32                 height{};
    };

    [[nodiscard]]
    auto generateTemplateAsset(
        std::span<std::byte const> sourceBgra,
        uint32 sourceWidth,
        uint32 sourceHeight,
        std::size_t sourceStride,
        PixelRect templateRect
    ) -> Result<TemplateAsset>;
}
