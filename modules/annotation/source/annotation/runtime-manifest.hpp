#pragma once

#include "catalog.hpp"
#include "content-hash.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uf::annotation
{
    inline constexpr auto g_runtimeManifestSchema = std::string_view{
        "umbraflow-annotations/v1"
    };

    struct RuntimeRecognizerSpec final
    {
        RecognizerDefinition m_definition;
        ContentHash          m_templateHash;
        ContentHash          m_sourceHash;
    };

    struct RuntimeRecognizerAsset final
    {
        RecognizerId m_id;
        ContentHash  m_templateHash;
        ContentHash  m_sourceHash;
        std::string  m_templatePath;
    };

    class RuntimeManifest final
    {
        RecognitionCatalog                  m_catalog;
        std::vector<RuntimeRecognizerAsset> m_assets;

        RuntimeManifest(
            RecognitionCatalog catalog,
            std::vector<RuntimeRecognizerAsset> assets
        ) noexcept;

    public:
        [[nodiscard]]
        static auto create(
            ProjectId projectId,
            ProjectFingerprint fingerprint,
            std::vector<RuntimeRecognizerSpec> recognizers,
            std::vector<PageSignature> pages
        ) -> Result<RuntimeManifest>;

        [[nodiscard]]
        auto catalog() const noexcept UF_LIFETIME_BOUND -> RecognitionCatalog const&;

        [[nodiscard]]
        auto assets() const noexcept UF_LIFETIME_BOUND -> std::span<RuntimeRecognizerAsset const>;

        [[nodiscard]]
        auto findAsset(
            RecognizerId id
        ) const noexcept UF_LIFETIME_BOUND -> RuntimeRecognizerAsset const*;
    };

    [[nodiscard]]
    auto serializeRuntimeManifest(RuntimeManifest const& manifest) -> std::string;

    [[nodiscard]]
    auto parseRuntimeManifest(std::string_view canonicalToml) -> Result<RuntimeManifest>;
}
