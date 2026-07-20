#pragma once

#include "args.hpp"

#include <controller/capture.hpp>
#include <controller/discovery.hpp>
#include <controller/target.hpp>
#include <core/error/result.hpp>

namespace uf::m0_demo
{
    [[nodiscard]] auto ensureClientAreaUsable(ClientSize client) -> Status;

    [[nodiscard]] auto buildSelector(SelectorArgs const& selector) -> TargetSelector;

    [[nodiscard]]
    auto createCaptureSession(
        ResolvedTarget const& target,
        SessionId sessionId,
        WgcCaptureOptions options = {}
    ) -> Result<WgcCaptureSession>;
}
