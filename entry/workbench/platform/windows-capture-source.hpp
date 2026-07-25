#pragma once

#include "source-ingestion.hpp"

#include <annotation/authoring-document.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <string>

namespace uf
{
    class WgcCaptureSession;
}

namespace uf::workbench::platform
{
    // Captures one frame from an active Windows Graphics Capture session and
    // ingests it into a canonical source asset carrying WGC provenance. dpi is
    // the target window's display density, stamped into the source fingerprint.
    // The session is borrowed for the duration of the call; the caller owns the
    // session lifetime and its bound target. Real-machine only: exercised
    // through the workbench, not unit tests.
    [[nodiscard]]
    auto captureSourceFromSession(
        annotation::SourceId id,
        WgcCaptureSession& session,
        uint32 dpi,
        std::string capturedAt
    ) -> Result<IngestedSource>;

    // Captures one frame from the first visible, non-minimized window whose title
    // contains titleSubstring and ingests it with WGC provenance. Enumerates
    // targets, resolves the matched window's client geometry, opens a one-shot
    // capture session, takes a single frame, and stamps the capture instant. Fails
    // when the substring is empty, no window matches, geometry cannot be resolved,
    // or capture fails. Real-machine only: exercised through the workbench.
    [[nodiscard]]
    auto captureSourceFromTargetTitle(
        annotation::SourceId id,
        std::string const& titleSubstring
    ) -> Result<IngestedSource>;
}
