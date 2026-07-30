#pragma once

#include "catalog.hpp"
#include "content-hash.hpp"
#include "resource.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uf::annotation
{
    // Bumped from v1 when the three-way annotation type became a capability
    // set, an element grew an ordered list of appearances instead of one
    // template, and the page-membership list on the recognizer was replaced by
    // the page reference rows that already are the authorisation. The v1 read
    // path has been retired: the manifest is generated from the authoring
    // document, so migrating it is recompiling it.
    inline constexpr auto k_runtimeManifestSchema = std::string_view{
        "umbraflow-annotations/v2"
    };

    // The compiled bytes behind one appearance of one element.
    struct RuntimeVariantAsset final
    {
        ResourceName variantName;
        ContentHash  templateHash;
        ContentHash  sourceHash;
    };

    struct RuntimeRecognizerSpec final
    {
        RecognizerDefinition definition;

        // One entry per declared variant, in the definition's own order.
        std::vector<RuntimeVariantAsset> variants{};
    };

    struct RuntimeRecognizerAsset final
    {
        ElementId    elementId;
        ResourceName variantName;
        ContentHash  templateHash;
        ContentHash  sourceHash;
        std::string  templatePath{};
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
            std::vector<PageSpec> pages,
            std::vector<PageReference> references
        ) -> Result<RuntimeManifest>;

        [[nodiscard]]
        auto catalog() const noexcept UF_LIFETIME_BOUND -> RecognitionCatalog const&;

        [[nodiscard]]
        auto assets() const noexcept UF_LIFETIME_BOUND -> std::span<RuntimeRecognizerAsset const>;

        // One element now has one asset per appearance, so an element alone no
        // longer names a template.
        [[nodiscard]]
        auto findAsset(
            ElementId elementId,
            ResourceName const& variantName
        ) const noexcept UF_LIFETIME_BOUND -> RuntimeRecognizerAsset const*;
    };

    [[nodiscard]]
    auto serializeRuntimeManifest(RuntimeManifest const& manifest) -> std::string;

    [[nodiscard]]
    auto parseRuntimeManifest(std::string_view canonicalToml) -> Result<RuntimeManifest>;
}
