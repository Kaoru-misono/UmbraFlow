#pragma once

#include <controller/discovery.hpp>
#include <core/error/result.hpp>

#include <domain/space.hpp>

#include <engine/ports.hpp>

#include <memory>

namespace uf::cli::platform
{
    // The ports one session drives, plus the live fingerprint the engine's
    // fail-closed compatibility check compares against the manifest.
    // liveFingerprint carries no in-class initializer because ProjectFingerprint
    // refuses to invent one: a session that guessed the target's geometry would
    // defeat that check, so every construction site states it.
    struct BoundTarget final
    {
        std::unique_ptr<engine::IFrameSource> frameSource;
        std::unique_ptr<engine::IActionSink>  actionSink;

        ProjectFingerprint liveFingerprint;
    };

    // Declares per-monitor DPI awareness, resolves the window the handle names --
    // the enumeration still happens, because only the desktop can say whether it
    // is still there and still capturable -- builds the
    // live fingerprint from the resolved geometry, and wires the capture session
    // and the delivery target into the two engine ports. This is the whole of what
    // a front-end contributes below the capability surface, and `run` and
    // `explore` share it so the agent path cannot run under different guarantees
    // from the task path.
    [[nodiscard]] auto bindTarget(WindowHandle window) -> Result<BoundTarget>;
}
