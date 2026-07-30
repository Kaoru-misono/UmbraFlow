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

    // One authored element compiles to exactly one runtime recognizer, under
    // the element's own id, however many pages reference it and however many
    // appearances it declares.
    //
    // The rule it replaces minted a derived id per (element, page) as soon as
    // an element reached two pages, which meant an element on two pages had no
    // recognizer under its own id at all -- so a page signature naming that
    // element pointed at nothing in the runtime catalog. One recognizer per
    // element is also what the capability merge buys: a patch of pixels that
    // both names its page and can be clicked is searched once per cycle, and
    // minting a recognizer per page would put the second search straight back.
    // The element id is the one name scripts, authorisation, and the trace all
    // use, so it is the one the runtime catalog must answer to.
    //
    // Everything a page needs to say about an element -- which capabilities it
    // exercises, a refined search region, a pinned appearance -- is carried by
    // the page reference rows, which are the model's edge and reach the runtime
    // manifest unchanged.
    [[nodiscard]]
    auto compileAuthoringDocument(
        AuthoringDocument const& document,
        std::span<AuthoringSourceAsset const> sourceAssets
    ) -> Result<CompiledAuthoringProject>;
}
