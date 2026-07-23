#pragma once

#include <annotation/authoring-compiler.hpp>

#include <core/error/result.hpp>

#include <filesystem>
#include <span>

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
}
