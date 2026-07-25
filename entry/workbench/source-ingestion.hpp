#pragma once

#include <annotation/authoring-compiler.hpp>
#include <annotation/authoring-document.hpp>

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
        annotation::AuthoringSourceSpec  m_spec;
        annotation::AuthoringSourceAsset m_asset;
    };

    // Decodes an external PNG and re-encodes it into the project's canonical PNG
    // form, so byte-identical images ingested through different encoders share a
    // content hash. Provenance is recorded as imported. Rejects any input the
    // image module cannot decode as a PNG.
    [[nodiscard]]
    auto importSourcePng(
        annotation::SourceId id,
        std::filesystem::path const& path
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
