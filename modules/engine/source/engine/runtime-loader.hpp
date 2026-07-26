#pragma once

#include <core/error/result.hpp>

#include <annotation/recognition-runtime.hpp>

#include <cstddef>
#include <filesystem>

namespace uf::engine
{
    // The runtime manifest is a small canonical TOML document. A file larger
    // than this is rejected by its stat-reported size before any bytes are read,
    // so a malformed or hostile project cannot force an unbounded allocation.
    inline constexpr auto k_maximumRuntimeManifestBytes = std::size_t{16} * 1024U * 1024U;

    // A recognition runtime loaded from a published annotation project, holding
    // everything the engine needs to recognize pages and evaluate action
    // targets. RecognitionRuntime has no default state, so a LoadedRuntime only
    // ever exists once loadRuntimeProject has established its invariant.
    struct LoadedRuntime final
    {
        annotation::RecognitionRuntime runtime;
    };

    // Reads a published annotation project from disk and builds its recognition
    // runtime. It reads generated/annotations.runtime.toml (size-capped before
    // the read), parses it with annotation::parseRuntimeManifest, then reads each
    // unique assets/templates/<hex>.png the manifest references and hands the
    // encoded templates to RecognitionRuntime::create, which enforces the hash
    // closure and decodes every template. Each failure names the offending path.
    // Extra files on disk are ignored: the content-addressed store may retain
    // history the manifest no longer references.
    [[nodiscard]]
    auto loadRuntimeProject(
        std::filesystem::path const& projectRoot
    ) -> Result<LoadedRuntime>;
}
