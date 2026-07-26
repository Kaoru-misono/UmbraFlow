#pragma once

#include <annotation/authoring-compiler.hpp>

#include <core/error/result.hpp>

#include <filesystem>
#include <span>
#include <vector>

namespace uf::workbench
{
    // Validates and compiles the complete project before publishing any final
    // metadata. Content-addressed assets are installed first; annotations.toml
    // is saved next, and the runtime manifest commits the generated set last.
    // A runtime-manifest publication failure can therefore leave the new
    // authoring document saved while the previous runtime closure remains active.
    [[nodiscard]]
    auto saveAndGenerateAuthoringProject(
        std::filesystem::path const& projectRoot,
        annotation::AuthoringDocument const& document,
        std::span<annotation::AuthoringSourceAsset const> sourceAssets
    ) -> Status;

    // A project read back from disk: the parsed authoring document plus the
    // verbatim PNG bytes of every source it references. The bytes are returned
    // as-is, not re-encoded, so a subsequent save reproduces byte-identical
    // content-addressed files; callers decode them for display.
    struct LoadedAuthoringProject final
    {
        annotation::AuthoringDocument                 document;
        std::vector<annotation::AuthoringSourceAsset> sources{};
    };

    // Reads annotations.toml (capped at 16 MiB, parsed with parseAuthoringDocument)
    // and every referenced assets/sources/<hash>.png, verifying each source's
    // content hash and decoded fingerprint geometry before returning the parsed
    // document and its source assets in document order. This is the read half of
    // the round trip whose write half is saveAndGenerateAuthoringProject.
    [[nodiscard]]
    auto loadAuthoringProject(
        std::filesystem::path const& projectRoot
    ) -> Result<LoadedAuthoringProject>;
}
