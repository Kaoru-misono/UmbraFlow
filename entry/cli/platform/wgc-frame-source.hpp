#pragma once

#include <controller/capture.hpp>
#include <core/error/result.hpp>

#include <domain/frame.hpp>
#include <engine/ports.hpp>

#include <utility>

namespace uf::cli::platform
{
    // Adapts a WgcCaptureSession to the engine IFrameSource port. capture()
    // forwards the session capture and validateTargetInstance() forwards the
    // session's bound-target revalidation, so the engine stays platform-agnostic.
    class WgcFrameSource final : public engine::IFrameSource
    {
        WgcCaptureSession m_session;

    public:
        explicit WgcFrameSource(WgcCaptureSession session) noexcept
            : m_session{std::move(session)}
        {
        }

        [[nodiscard]] auto capture() -> Result<Frame> override
        {
            return m_session.capture();
        }

        [[nodiscard]] auto validateTargetInstance() -> Status override
        {
            return m_session.validateTargetInstance();
        }
    };
}
