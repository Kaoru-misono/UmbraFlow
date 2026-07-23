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
        ContentHash m_hash;
        std::string m_relativePath{};

        std::vector<std::byte> m_pngBytes{};
        uint32                 m_width{};
        uint32                 m_height{};
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
