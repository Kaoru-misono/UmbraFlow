#include "windows-guard.hpp"

#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <Windows.h>

#include <bit>
#include <cstddef>
#include <cstring>
#include <format>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace uf::m0_demo::platform
{
    namespace
    {
        class OwnedHandle final
        {
            HANDLE m_handle;

        public:
            explicit OwnedHandle(HANDLE handle) noexcept
                : m_handle{handle}
            {
            }

            OwnedHandle(OwnedHandle const&) = delete;
            auto operator=(OwnedHandle const&) -> OwnedHandle& = delete;
            OwnedHandle(OwnedHandle&& other) noexcept
                : m_handle{std::exchange(other.m_handle, nullptr)}
            {
            }
            auto operator=(OwnedHandle&&) -> OwnedHandle& = delete;

            ~OwnedHandle() noexcept
            {
                if (m_handle != nullptr)
                {
                    // SAFETY: m_handle is an owned Win32 process or token handle and
                    // this destructor closes it exactly once before discarding it.
                    static_cast<void>(CloseHandle(m_handle));
                }
            }

            [[nodiscard]] auto get() const noexcept -> HANDLE { return m_handle; }
            [[nodiscard]] auto valid() const noexcept -> bool { return m_handle != nullptr; }
        };

        [[nodiscard]]
        auto tokenIntegrityRid(HANDLE processHandle) -> std::optional<uint32>
        {
            auto tokenHandle = HANDLE{};
            // SAFETY: processHandle is a live process handle or current-process
            // pseudo-handle; tokenHandle is a live out-parameter and is wrapped in
            // OwnedHandle immediately on success.
            if (OpenProcessToken(processHandle, TOKEN_QUERY, &tokenHandle) == FALSE)
            {
                return std::nullopt;
            }
            auto token = OwnedHandle{tokenHandle};

            auto requiredBytes = DWORD{};
            // SAFETY: a null buffer and zero length request only the required byte
            // count through requiredBytes; no caller buffer is dereferenced.
            static_cast<void>(GetTokenInformation(
                token.get(),
                TokenIntegrityLevel,
                nullptr,
                0,
                &requiredBytes
            ));
            if (requiredBytes < sizeof(TOKEN_MANDATORY_LABEL))
            {
                return std::nullopt;
            }

            auto buffer = std::vector<std::byte>{requiredBytes};
            // SAFETY: buffer owns requiredBytes writable bytes for the entire call;
            // GetTokenInformation writes no more than the supplied byte count and
            // reports the actual count through requiredBytes.
            if (GetTokenInformation(
                token.get(),
                TokenIntegrityLevel,
                buffer.data(),
                requiredBytes,
                &requiredBytes
            ) == FALSE)
            {
                return std::nullopt;
            }
            if (requiredBytes < sizeof(TOKEN_MANDATORY_LABEL))
            {
                return std::nullopt;
            }

            static_assert(std::is_trivially_copyable_v<TOKEN_MANDATORY_LABEL>);
            auto label = TOKEN_MANDATORY_LABEL{};
            // SAFETY: the successful call initialized the leading bytes as a
            // TOKEN_MANDATORY_LABEL. memcpy avoids alignment and object-lifetime
            // assumptions while copying its trivially-copyable value fields; the
            // embedded SID continues to point into buffer, which remains alive.
            std::memcpy(&label, buffer.data(), sizeof(label));
            auto const sid = label.Label.Sid;
            if (sid == nullptr)
            {
                return std::nullopt;
            }

            // SAFETY: sid points into the live token-information buffer. The Win32
            // accessors validate the SID layout and return pointers into that same
            // buffer, which are checked before each bounded dereference.
            auto const count = GetSidSubAuthorityCount(sid);
            if (count == nullptr || *count == 0U)
            {
                return std::nullopt;
            }
            auto const rid = GetSidSubAuthority(sid, static_cast<DWORD>(*count - 1U));
            if (rid == nullptr)
            {
                return std::nullopt;
            }
            return *rid;
        }

        [[nodiscard]]
        auto windowFrom(WindowHandle handle) noexcept -> HWND
        {
            // SAFETY: WindowHandle stores the pointer-sized integer representation copied
            // from an HWND. bit_cast restores those exact bits as the opaque token without
            // dereferencing it.
            return std::bit_cast<HWND>(handle.value());
        }
    }

    auto observeGuard(GuardPolicy policy) -> Result<GuardBaseline>
    {
        // SAFETY: GetForegroundWindow takes no caller memory and returns an
        // opaque borrowed handle that may be null. Only its bit pattern is kept.
        auto const foregroundWindow = GetForegroundWindow();
        // SAFETY: HWND is an opaque pointer-sized token. bit_cast preserves its exact
        // bits in the signed machine-word representation without dereferencing or
        // extending its lifetime.
        auto const foreground = std::bit_cast<intptr>(foregroundWindow);

        auto cursor = std::pair<int32, int32>{0, 0};
        if (policy.m_compareCursor)
        {
            auto point = POINT{};
            // SAFETY: point is a live writable out-parameter for the duration of
            // GetCursorPos and is read only after the call reports success.
            if (GetCursorPos(&point) == FALSE)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    std::format("GetCursorPos failed with Win32 error {}", GetLastError())
                );
            }
            cursor = {point.x, point.y};
        }

        return GuardBaseline{foreground, cursor};
    }

    auto clientOriginDesktop(WindowHandle windowHandle) -> Result<Point<DesktopSpace>>
    {
        auto const window = windowFrom(windowHandle);
        auto windowRectangle = RECT{};
        // SAFETY: window is an opaque candidate handle and windowRectangle is a
        // live out-parameter. Failure is handled as target unavailability.
        if (GetWindowRect(window, &windowRectangle) == FALSE)
        {
            return fail(
                AutomationErrorKind::TargetUnavailable,
                std::format("GetWindowRect failed with Win32 error {}", GetLastError())
            );
        }

        auto origin = POINT{};
        // SAFETY: origin is initialized to client (0, 0), remains live for the
        // in-place translation, and is consumed only when the call succeeds.
        if (ClientToScreen(window, &origin) == FALSE)
        {
            return fail(
                AutomationErrorKind::TargetUnavailable,
                std::format("ClientToScreen failed with Win32 error {}", GetLastError())
            );
        }

        return Point<DesktopSpace>{
            static_cast<float>(origin.x),
            static_cast<float>(origin.y)
        };
    }

    auto currentProcessIntegrity() -> std::optional<IntegrityLevel>
    {
        // SAFETY: GetCurrentProcess returns a process pseudo-handle that is valid
        // for token queries and must not be closed by the caller.
        auto const process = GetCurrentProcess();
        auto const rid = tokenIntegrityRid(process);
        if (!rid)
        {
            return std::nullopt;
        }
        return IntegrityLevel::fromRid(*rid);
    }

    auto processIntegrity(ProcessId process) -> std::optional<IntegrityLevel>
    {
        // SAFETY: OpenProcess receives a numeric PID and no caller pointers. Its
        // returned owned handle is wrapped immediately and closed exactly once.
        auto handle = OwnedHandle{
            OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.value())
        };
        if (!handle.valid())
        {
            return std::nullopt;
        }

        auto const rid = tokenIntegrityRid(handle.get());
        if (!rid)
        {
            return std::nullopt;
        }
        return IntegrityLevel::fromRid(*rid);
    }
}
