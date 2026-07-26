#pragma once

#include "authoring-document.hpp"
#include "runtime-manifest.hpp"
#include "template-asset.hpp"

#include <core/error/result.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace uf::annotation
{
    struct AuthoringSourceAsset final
    {
        SourceId               id;
        std::vector<std::byte> pngBytes{};
    };

    struct CompiledAuthoringProject final
    {
        RuntimeManifest runtimeManifest;
        std::string     runtimeManifestToml{};

        std::vector<TemplateAsset> templateAssets{};
    };

    [[nodiscard]]
    auto compileAuthoringDocument(
        AuthoringDocument const& document,
        std::span<AuthoringSourceAsset const> sourceAssets
    ) -> Result<CompiledAuthoringProject>;
}
