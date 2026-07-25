#include "windows-capture-source.hpp"

#include "source-ingestion.hpp"

#include <controller/capture.hpp>
#include <controller/discovery.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#pragma warning(push, 0)
#include <Windows.h>
#pragma warning(pop)

#include <algorithm>
#include <bit>
#include <chrono>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::workbench::platform
{
    namespace
    {
        // Makes the calling thread per-monitor-DPI-aware (v2) for the capture
        // scope and restores the prior context on destruction. The workbench UI
        // thread is otherwise DPI-unaware, which virtualizes a high-DPI target's
        // client rectangle (for example 1066x600 for a 1600x900 window at 150%)
        // and both desyncs it from the physical captured frame and hides the real
        // display density. Scoping the awareness keeps the workbench window itself
        // rendering at its comfortable system-scaled size.
        class ScopedPerMonitorDpiAwareness final
        {
            DPI_AWARENESS_CONTEXT m_previous{nullptr};

        public:
            ScopedPerMonitorDpiAwareness() noexcept
            {
                // SAFETY: SetThreadDpiAwarenessContext takes an opaque context
                // token, mutates only calling-thread state, and returns the prior
                // context (null when unsupported). No pointer is dereferenced.
                m_previous = SetThreadDpiAwarenessContext(
                    DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
                );
            }

            ScopedPerMonitorDpiAwareness(ScopedPerMonitorDpiAwareness const&) = delete;
            auto operator=(ScopedPerMonitorDpiAwareness const&)
                -> ScopedPerMonitorDpiAwareness& = delete;
            ScopedPerMonitorDpiAwareness(ScopedPerMonitorDpiAwareness&&) = delete;
            auto operator=(ScopedPerMonitorDpiAwareness&&)
                -> ScopedPerMonitorDpiAwareness& = delete;

            ~ScopedPerMonitorDpiAwareness() noexcept
            {
                if (m_previous != nullptr)
                {
                    // SAFETY: restores the exact context captured in the
                    // constructor on the same thread; the token is opaque and is
                    // never dereferenced.
                    static_cast<void>(SetThreadDpiAwarenessContext(m_previous));
                }
            }
        };

        [[nodiscard]]
        auto clientOriginDesktop(
            WindowHandle windowHandle
        ) -> Result<Point<DesktopSpace>>
        {
            // SAFETY: WindowHandle stores the pointer-sized bits copied from an
            // HWND; bit_cast restores the opaque token without dereferencing it.
            auto const window = std::bit_cast<HWND>(windowHandle.value());

            auto origin = POINT{};
            // SAFETY: origin starts at client (0, 0), stays live for the in-place
            // translation, and is read only when the call reports success.
            if (ClientToScreen(window, &origin) == FALSE)
            {
                return fail(
                    AutomationErrorKind::TargetUnavailable,
                    std::format(
                        "ClientToScreen failed with Win32 error {}",
                        GetLastError()
                    )
                );
            }

            return Point<DesktopSpace>{
                static_cast<float>(origin.x),
                static_cast<float>(origin.y),
            };
        }

        [[nodiscard]]
        auto formatCapturedAtNow() -> std::string
        {
            auto const now = std::chrono::floor<std::chrono::seconds>(
                std::chrono::system_clock::now()
            );
            return std::format("{:%Y-%m-%dT%H:%M:%SZ}", now);
        }
    }

    auto captureSourceFromSession(
        annotation::SourceId id,
        WgcCaptureSession& session,
        uint32 dpi,
        std::string capturedAt
    ) -> Result<IngestedSource>
    {
        UF_TRY_VALUE(frame, session.capture());
        return ingestSourceFromFrame(id, frame, dpi, std::move(capturedAt));
    }

    auto captureSourceFromTargetTitle(
        annotation::SourceId id,
        std::string const& titleSubstring
    ) -> Result<IngestedSource>
    {
        if (titleSubstring.empty())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "capture requires a non-empty target title"
            );
        }

        // Window discovery and WGC geometry below must read the target's physical
        // client rectangle and true display density; the UI thread is DPI-unaware,
        // so raise per-monitor awareness for just this capture.
        auto const dpiScope = ScopedPerMonitorDpiAwareness{};

        UF_TRY_VALUE(candidates, enumerateCandidates());
        auto const match = std::ranges::find_if(
            candidates,
            [&titleSubstring](TargetCandidate const& candidate) -> bool
            {
                return candidate.isVisible()
                    && !candidate.isIconic()
                    && candidate.title().find(titleSubstring)
                        != std::string::npos;
            }
        );
        if (match == candidates.end())
        {
            return fail(
                AutomationErrorKind::TargetUnavailable,
                std::format(
                    "no visible window title contains \"{}\"",
                    titleSubstring
                )
            );
        }

        UF_TRY_VALUE(origin, clientOriginDesktop(match->handle()));
        auto const clientSize = match->clientSize();
        UF_TRY_VALUE(
            geometry,
            ClientGeometry::create(
                origin,
                static_cast<float>(clientSize.width()),
                static_cast<float>(clientSize.height())
            )
        );
        UF_TRY_VALUE(
            session,
            WgcCaptureSession::create(
                match->handle(),
                SessionId{1},
                TargetGeneration::fromValue(1),
                geometry
            )
        );

        return captureSourceFromSession(
            id,
            session,
            match->dpi().value(),
            formatCapturedAtNow()
        );
    }
}
