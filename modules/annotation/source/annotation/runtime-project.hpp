#pragma once

#include <core/error/result.hpp>

#include "recognition-runtime.hpp"

#include <cstddef>
#include <filesystem>

namespace uf::annotation
{
    // The runtime manifest is a small canonical TOML document. A file larger
    // than this is rejected by its stat-reported size before any bytes are read,
    // so a malformed or hostile project cannot force an unbounded allocation.
    inline constexpr auto k_maximumRuntimeManifestBytes = std::size_t{16} * 1024U * 1024U;

    // Reads a published annotation project from disk and builds its recognition
    // runtime. It reads generated/annotations.runtime.toml (size-capped before
    // the read), parses it with parseRuntimeManifest, then reads each unique
    // assets/templates/<hex>.png the manifest references and hands the encoded
    // templates to RecognitionRuntime::create, which enforces the hash closure
    // and decodes every template. Each failure names the offending path. Extra
    // files on disk are ignored: the content-addressed store may retain history
    // the manifest no longer references.
    //
    // It lived in modules/engine until the script-owned page model retired the
    // v3 manifest from the runtime path
    // (docs/plans/2026-07-31-script-owned-page-model.md 9). Reading a v3 project
    // off disk is annotation's own business now, and the only callers left are
    // the v4 authoring line's.
    [[nodiscard]]
    auto loadRuntimeProject(
        std::filesystem::path const& projectRoot
    ) -> Result<RecognitionRuntime>;
}
