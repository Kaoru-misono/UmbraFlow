#pragma once

#include <annotation/authoring-compiler.hpp>
#include <annotation/authoring-document.hpp>
#include <annotation/resource.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <filesystem>
#include <string>

namespace uf
{
    class Frame;
}

namespace uf::workbench
{
    // An ingested source pairs the immutable document record that describes a
    // source image with the canonical PNG asset that backs it. The content hash
    // on the spec is always sha256 of the asset bytes, and the spec fingerprint
    // is the decoded geometry, so a caller can add the record to an
    // AuthoringDocument and hand the asset to compilation without recomputing
    // either. The caller mints the SourceId: authoring-time identity is a
    // higher-layer concern and the runtime never mints.
    struct IngestedSource final
    {
        annotation::AuthoringSourceSpec  spec;
        annotation::AuthoringSourceAsset asset;
    };

    // The density a PNG adopts when its caller states none. A file carries no
    // display density, and 96 is the conventional answer.
    inline constexpr auto k_defaultSourceDpi = uint32{96};

    // Decodes an external PNG and re-encodes it into the project's canonical PNG
    // form, so byte-identical images ingested through different encoders share a
    // content hash. Provenance is recorded as imported. Rejects any input the
    // image module cannot decode as a PNG.
    //
    // `dpi` must be the density of the window the screenshot came from, not the
    // density of the file, which does not have one. AuthoringDocument requires
    // every source's fingerprint to equal the project's, so importing at the
    // wrong density does not degrade the result -- it refuses the document. A
    // project authored from a 144-dpi window cannot ingest a file left at the 96
    // default at all, which is why this is a parameter rather than a constant.
    [[nodiscard]]
    auto importSourcePng(
        annotation::SourceId id,
        std::filesystem::path const& path,
        uint32 dpi = k_defaultSourceDpi
    ) -> Result<IngestedSource>;

    // Encodes an already-captured BGRA frame into a canonical source asset and
    // records Windows Graphics Capture provenance. The source fingerprint adopts
    // the captured window's display density in dpi, so a project authored from a
    // high-DPI target matches that target's runtime fingerprint; the caller
    // resolves the real DPI (the capture geometry must be read under per-monitor
    // DPI awareness for it to be correct). The wall-clock capture instant is
    // supplied by the caller because a monotonic frame timestamp cannot be
    // rendered as a stable calendar string.
    [[nodiscard]]
    auto ingestSourceFromFrame(
        annotation::SourceId id,
        Frame const& frame,
        uint32 dpi,
        std::string capturedAt
    ) -> Result<IngestedSource>;
}
