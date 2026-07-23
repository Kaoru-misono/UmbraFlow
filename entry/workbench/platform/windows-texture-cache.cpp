#include "windows-texture-cache.hpp"

#include <core/numeric/checked-arithmetic.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <image/png.hpp>

#pragma warning(push, 0)
#include <Windows.h>

#include <d3d11.h>
#include <winrt/base.h>
#pragma warning(pop)

#include <bit>
#include <cstddef>
#include <utility>
#include <vector>

namespace uf::workbench::platform
{
    namespace
    {
        struct CachedTexture final
        {
            annotation::SourceId                     m_id;
            winrt::com_ptr<ID3D11ShaderResourceView> m_view{};
            uint32                                   m_width{};
            uint32                                   m_height{};
        };
    }

    struct TextureCache::Cache final
    {
        winrt::com_ptr<ID3D11Device> m_device{};
        std::vector<CachedTexture>   m_textures{};
    };

    TextureCache::TextureCache(std::unique_ptr<Cache> cache) noexcept
        : m_cache{std::move(cache)}
    {
    }

    TextureCache::TextureCache(TextureCache&&) noexcept = default;
    auto TextureCache::operator=(TextureCache&&) noexcept -> TextureCache& = default;
    TextureCache::~TextureCache() = default;

    auto TextureCache::create(ID3D11Device& device) -> Result<TextureCache>
    {
        auto cache = std::make_unique<Cache>();
        // SAFETY: copy_from adds an owning reference to the caller's device; the
        // com_ptr releases it when the cache is destroyed.
        cache->m_device.copy_from(&device);
        return TextureCache{std::move(cache)};
    }

    auto TextureCache::textureFor(
        annotation::AuthoringSourceAsset const& asset
    ) -> Result<GpuSourceTexture>
    {
        for (auto const& cached : m_cache->m_textures)
        {
            if (cached.m_id == asset.m_id)
            {
                return GpuSourceTexture{
                    // SAFETY: the view outlives the returned handle because the
                    // cache retains it; bit_cast only reinterprets the pointer bits
                    // as the opaque token ImGui stores and hands back unchanged.
                    .m_textureHandle = std::bit_cast<uint64>(cached.m_view.get()),
                    .m_width         = cached.m_width,
                    .m_height        = cached.m_height,
                };
            }
        }

        UF_TRY_VALUE(
            decoded,
            image::decodePng(asset.m_pngBytes, "workbench-canvas-source.png")
        );

        auto description                = D3D11_TEXTURE2D_DESC{};
        description.Width     = decoded.m_width;
        description.Height    = decoded.m_height;
        description.MipLevels = 1U;
        description.ArraySize = 1U;
        description.Format              = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count    = 1U;
        description.Usage               = D3D11_USAGE_DEFAULT;
        description.BindFlags           = D3D11_BIND_SHADER_RESOURCE;

        auto const pitch = checkedMultiply(
            static_cast<std::size_t>(decoded.m_width),
            std::size_t{4}
        );
        if (!pitch.has_value())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "canvas source row pitch overflowed addressable memory"
            );
        }

        auto initialData         = D3D11_SUBRESOURCE_DATA{};
        initialData.pSysMem     = decoded.m_pixels.data();
        initialData.SysMemPitch = static_cast<UINT>(*pitch);

        auto texture = winrt::com_ptr<ID3D11Texture2D>{};
        // SAFETY: description and initialData are live for the call and texture is
        // empty; the device writes the created texture into texture.put().
        auto const textureResult = m_cache->m_device->CreateTexture2D(
            &description,
            &initialData,
            texture.put()
        );
        if (FAILED(textureResult) || !texture)
        {
            return fail(
                AutomationErrorKind::UnsupportedCapability,
                "workbench canvas failed to create the source texture",
                systemErrorCode(static_cast<DWORD>(textureResult))
            );
        }

        auto viewDescription                    = D3D11_SHADER_RESOURCE_VIEW_DESC{};
        viewDescription.Format                  = description.Format;
        viewDescription.ViewDimension           = D3D11_SRV_DIMENSION_TEXTURE2D;
        viewDescription.Texture2D.MipLevels     = 1U;

        auto view = winrt::com_ptr<ID3D11ShaderResourceView>{};
        // SAFETY: texture is a live resource and view is empty; the device writes
        // the created shader-resource view into view.put().
        auto const viewResult = m_cache->m_device->CreateShaderResourceView(
            texture.get(),
            &viewDescription,
            view.put()
        );
        if (FAILED(viewResult) || !view)
        {
            return fail(
                AutomationErrorKind::UnsupportedCapability,
                "workbench canvas failed to create the source texture view",
                systemErrorCode(static_cast<DWORD>(viewResult))
            );
        }

        // SAFETY: see the cache-hit path; the freshly created view is owned by the
        // cache entry below, so the handle stays valid for the cache's lifetime.
        auto const handle = std::bit_cast<uint64>(view.get());
        m_cache->m_textures.emplace_back(
            CachedTexture{
                .m_id     = asset.m_id,
                .m_view   = std::move(view),
                .m_width  = decoded.m_width,
                .m_height = decoded.m_height,
            }
        );

        return GpuSourceTexture{
            .m_textureHandle = handle,
            .m_width         = decoded.m_width,
            .m_height        = decoded.m_height,
        };
    }
}
