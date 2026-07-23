#pragma once

#include <annotation/authoring-compiler.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <memory>

struct ID3D11Device;

namespace uf::workbench::platform
{
    // A GPU-resident source image the canvas can draw. The handle is the backing
    // Direct3D shader-resource view reinterpreted as an opaque integer, which the
    // caller passes to ImGui as an ImTextureID; the Direct3D type never crosses
    // this boundary. The handle stays valid for the cache's lifetime.
    struct GpuSourceTexture final
    {
        uint64 m_textureHandle{};
        uint32 m_width{};
        uint32 m_height{};
    };

    // Decodes and uploads workbench source images to Direct3D 11 textures on
    // demand, retaining each so a source is decoded and uploaded at most once. It
    // holds a share of the device used to create every view; all Direct3D state
    // lives in the implementation behind this boundary.
    class TextureCache final
    {
        struct Cache;

        std::unique_ptr<Cache> m_cache;

        explicit TextureCache(std::unique_ptr<Cache> cache) noexcept;

    public:
        TextureCache(TextureCache&&) noexcept;
        auto operator=(TextureCache&&) noexcept -> TextureCache&;
        TextureCache(TextureCache const&) = delete;
        auto operator=(TextureCache const&) -> TextureCache& = delete;
        ~TextureCache();

        // Borrows the device for the duration of the call and retains a reference
        // to it for later uploads; the caller must keep the device alive at least
        // as long as this cache.
        [[nodiscard]]
        static auto create(ID3D11Device& device) -> Result<TextureCache>;

        // Returns the texture for a source, uploading it on first request and
        // returning the cached view afterwards. Fails only when the asset's PNG
        // cannot be decoded or the device rejects the upload.
        [[nodiscard]]
        auto textureFor(
            annotation::AuthoringSourceAsset const& asset
        ) -> Result<GpuSourceTexture>;
    };
}
