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
        std::string capturedAt
    ) -> Result<IngestedSource>
    {
        UF_TRY_VALUE(frame, session.capture());
        return ingestSourceFromFrame(id, frame, std::move(capturedAt));
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

        return captureSourceFromSession(id, session, formatCapturedAtNow());
    }
}
