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
        SourceId               m_id;
        std::vector<std::byte> m_pngBytes{};
    };

    struct CompiledAuthoringProject final
    {
        RuntimeManifest            m_runtimeManifest;
        std::string                m_runtimeManifestToml{};
        std::vector<TemplateAsset> m_templateAssets{};
    };

    [[nodiscard]]
    auto compileAuthoringDocument(
        AuthoringDocument const& document,
        std::span<AuthoringSourceAsset const> sourceAssets
    ) -> Result<CompiledAuthoringProject>;
}
