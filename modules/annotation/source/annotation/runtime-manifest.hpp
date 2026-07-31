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
    // Bumped from v2 on 2026-07-31 by the vocabulary rename: `[[recognizer]]` is
    // now `[[element]]`, `[[variant]]` is now `[[appearance]]`, and a
    // reference's `variant` field is now `appearance`. v2 was itself the bump
    // for the capability set, the ordered appearance list, and the page
    // reference rows that replaced the page-membership list. Every earlier read
    // path is retired: the manifest is generated from the authoring document, so
    // migrating it is recompiling it.
    inline constexpr auto k_runtimeManifestSchema = std::string_view{
        "umbraflow-annotations/v3"
    };

    // The compiled bytes behind one appearance of one element.
    struct RuntimeAppearanceAsset final
    {
        ResourceName appearanceName;
        ContentHash  templateHash;
        ContentHash  sourceHash;
    };

    struct RuntimeElementSpec final
    {
        CompiledElement definition;

        // One entry per declared appearance, in the definition's own order.
        std::vector<RuntimeAppearanceAsset> appearances{};
    };

    struct RuntimeElementAsset final
    {
        ElementId    elementId;
        ResourceName appearanceName;
        ContentHash  templateHash;
        ContentHash  sourceHash;
        std::string  templatePath{};
    };

    class RuntimeManifest final
    {
        RecognitionCatalog               m_catalog;
        std::vector<RuntimeElementAsset> m_assets;

        RuntimeManifest(
            RecognitionCatalog catalog,
            std::vector<RuntimeElementAsset> assets
        ) noexcept;

    public:
        [[nodiscard]]
        static auto create(
            ProjectId projectId,
            ProjectFingerprint fingerprint,
            std::vector<RuntimeElementSpec> elements,
            std::vector<PageSpec> pages,
            std::vector<PageReference> references
        ) -> Result<RuntimeManifest>;

        [[nodiscard]]
        auto catalog() const noexcept UF_LIFETIME_BOUND -> RecognitionCatalog const&;

        [[nodiscard]]
        auto assets() const noexcept UF_LIFETIME_BOUND -> std::span<RuntimeElementAsset const>;

        // One element now has one asset per appearance, so an element alone no
        // longer names a template.
        [[nodiscard]]
        auto findAsset(
            ElementId elementId,
            ResourceName const& appearanceName
        ) const noexcept UF_LIFETIME_BOUND -> RuntimeElementAsset const*;
    };

    [[nodiscard]]
    auto serializeRuntimeManifest(RuntimeManifest const& manifest) -> std::string;

    [[nodiscard]]
    auto parseRuntimeManifest(std::string_view canonicalToml) -> Result<RuntimeManifest>;
}
