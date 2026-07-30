#pragma once

#include "authoring-document.hpp"
#include "resource.hpp"
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

    // THE mapping from an authored element and one page it is placed on to the
    // id of the per-placement runtime recognizer that page receives. An element
    // placed on N >= 2 pages compiles into N runtime recognizers, one per page,
    // each needing an id distinct from the element's own and stable across
    // compiles; this is that id. It is the single source of truth: the compiler
    // emits these recognizers with it, and every consumer of the generated
    // manifest -- the workbench preview and model check -- maps a UI-facing
    // element id back to the runtime recognizer for a given page through the
    // same function. sha256 over the element id bytes concatenated with the page
    // id bytes, truncated to 16 through ResourceId::fromBytes: deterministic for
    // fixed inputs and collision-resistant enough that a clash with a real id is
    // astronomically unlikely -- and any clash fails loudly in
    // RecognitionCatalog::create's uniqueness guard rather than silently
    // dropping a recognizer.
    [[nodiscard]]
    auto derivedRuntimeRecognizerId(
        ElementId elementId,
        PageId pageId
    ) -> ElementId;
}
