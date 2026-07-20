#include "windows-controller.hpp"

#include "controller/detail/discovery-logic.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-cast.hpp>
#include <domain/error.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // The Win32 path format is QueryFullProcessImageNameW's documented zero flag;
    // the C Windows SDK does not expose the name provided by the Rust binding.
    constexpr auto processNameWin32 = DWORD{0};

    class UniqueProcessHandle final
    {
        HANDLE m_handle;

    public:
        explicit UniqueProcessHandle(HANDLE handle) noexcept
            : m_handle{handle}
        {
        }

        UniqueProcessHandle(UniqueProcessHandle const&) = delete;
        auto operator=(UniqueProcessHandle const&) -> UniqueProcessHandle& = delete;

        ~UniqueProcessHandle()
        {
            // SAFETY: OpenProcess returned this owned handle, it is closed exactly once
            // here, and no caller can retain the private native value afterwards.
            static_cast<void>(CloseHandle(m_handle));
        }

        [[nodiscard]] auto get() const noexcept -> HANDLE { return m_handle; }
    };

    struct ProcessDetails final
    {
        std::optional<uf::ProcessStartTime> m_startTime;
        std::optional<std::filesystem::path> m_executablePath;
    };

    struct EnumerationState final
    {
        std::vector<uf::WindowHandle> m_handles;
        bool m_storageFailed{};
    };

    template <std::size_t Size>
    [[nodiscard]]
    auto copiedUtf16Units(
        std::array<wchar_t, Size> const& buffer
    ) -> std::array<char16_t, Size>
    {
        static_assert(sizeof(wchar_t) == sizeof(char16_t));
        auto units = std::array<char16_t, Size>{};
        for (auto index = std::size_t{0}; index < buffer.size(); ++index)
        {
            units[index] = static_cast<char16_t>(buffer[index]);
        }
        return units;
    }

    [[nodiscard]]
    auto toNativeHandle(uf::WindowHandle handle) noexcept -> HWND
    {
        // SAFETY: WindowHandle stores the pointer-sized integer representation copied
        // from an HWND. The boundary restores that representation without dereferencing it.
        return reinterpret_cast<HWND>(handle.value());
    }

    [[nodiscard]]
    auto fromNativeHandle(HWND handle) noexcept -> uf::WindowHandle
    {
        // SAFETY: HWND is an opaque pointer-sized token. Copying its representation into
        // intptr_t preserves every bit and does not claim ownership or dereference it.
        return uf::WindowHandle{reinterpret_cast<std::intptr_t>(handle)};
    }

    auto CALLBACK enumerateWindow(HWND handle, LPARAM data) noexcept -> BOOL
    {
        // SAFETY: EnumWindows synchronously returns the exact pointer-sized token supplied
        // by enumerateWindowHandles; the state remains alive for the complete callback run.
        auto* p_state = reinterpret_cast<EnumerationState*>(data);
        if (p_state == nullptr)
        {
            return FALSE;
        }

        try
        {
            p_state->m_handles.emplace_back(fromNativeHandle(handle));
        }
        catch (...)
        {
            p_state->m_storageFailed = true;
            return FALSE;
        }
        return TRUE;
    }

    [[nodiscard]]
    auto enumerateWindowHandles() -> uf::Result<std::vector<uf::WindowHandle>>
    {
        auto state = EnumerationState{};

        // SAFETY: Clearing the calling thread's last-error slot has no pointer or lifetime
        // precondition and lets an EnumWindows failure report an unambiguous native code.
        SetLastError(ERROR_SUCCESS);
        // SAFETY: The pointer is encoded in a pointer-sized LPARAM and will only be decoded
        // by the synchronous callback while state remains alive.
        auto const stateParameter = reinterpret_cast<LPARAM>(&state);
        // SAFETY: EnumWindows invokes enumerateWindow synchronously with stateParameter;
        // the callback never retains the encoded pointer after returning.
        auto const enumerated = EnumWindows(
            enumerateWindow,
            stateParameter
        );
        if (state.m_storageFailed)
        {
            return uf::fail(
                uf::AutomationErrorKind::TargetUnavailable,
                "EnumWindows callback could not store a window handle"
            );
        }
        if (enumerated == FALSE)
        {
            // SAFETY: GetLastError reads calling-thread state immediately after the failed
            // EnumWindows call and does not access caller-owned memory.
            auto const error = GetLastError();
            return uf::fail(
                uf::AutomationErrorKind::TargetUnavailable,
                std::format("EnumWindows failed with win32 error {}", error)
            );
        }

        return std::move(state.m_handles);
    }

    [[nodiscard]]
    auto openProcessForQuery(uf::ProcessId process) noexcept -> HANDLE
    {
        // SAFETY: OpenProcess receives a numeric process id and no pointers. A non-null
        // result is a newly owned handle that the caller immediately places under RAII.
        return OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            process.value()
        );
    }

    [[nodiscard]]
    auto readStartTime(HANDLE process) noexcept -> std::optional<uf::ProcessStartTime>
    {
        auto creation = FILETIME{};
        auto exit = FILETIME{};
        auto kernel = FILETIME{};
        auto user = FILETIME{};
        // SAFETY: process is a live query handle and all four FILETIME pointers name
        // writable locals for the complete call.
        auto const queried = GetProcessTimes(
            process,
            &creation,
            &exit,
            &kernel,
            &user
        );
        if (queried == FALSE)
        {
            return std::nullopt;
        }

        return uf::ProcessStartTime{
            uf::controller_detail::fileTimeToTicks(
                creation.dwHighDateTime,
                creation.dwLowDateTime
            )
        };
    }

    [[nodiscard]]
    auto readExecutablePath(HANDLE process) -> std::optional<std::filesystem::path>
    {
        auto buffer = std::array<wchar_t, 512>{};
        auto size = static_cast<DWORD>(buffer.size());
        // SAFETY: process is a live query handle; buffer owns size writable wchar_t slots,
        // and size is a live in/out count initialized to that exact capacity.
        auto const queried = QueryFullProcessImageNameW(
            process,
            processNameWin32,
            buffer.data(),
            &size
        );
        if (queried == FALSE)
        {
            return std::nullopt;
        }

        auto const written = std::min(static_cast<std::size_t>(size), buffer.size());
        auto const length = uf::checkedCast<std::int32_t>(written);
        UF_CHECK(length.has_value());
        auto const units = copiedUtf16Units(buffer);
        return uf::controller_detail::utf16BufferToPath(units, *length);
    }

    [[nodiscard]]
    auto processDetails(uf::ProcessId process) -> ProcessDetails
    {
        auto const nativeHandle = openProcessForQuery(process);
        if (nativeHandle == nullptr)
        {
            return {};
        }

        auto const ownedHandle = UniqueProcessHandle{nativeHandle};
        return ProcessDetails{
            .m_startTime = readStartTime(ownedHandle.get()),
            .m_executablePath = readExecutablePath(ownedHandle.get()),
        };
    }

    [[nodiscard]]
    auto windowClassName(HWND handle) -> std::string
    {
        auto buffer = std::array<wchar_t, 256>{};
        auto const capacity = uf::checkedCast<int>(buffer.size());
        UF_CHECK(capacity.has_value());
        // SAFETY: handle is an observed HWND and buffer owns capacity writable wchar_t
        // slots for the duration of GetClassNameW.
        auto const length = GetClassNameW(handle, buffer.data(), *capacity);
        auto const units = copiedUtf16Units(buffer);
        return uf::controller_detail::utf16BufferToString(units, length);
    }

    [[nodiscard]]
    auto windowTitle(HWND handle) -> std::string
    {
        auto buffer = std::array<wchar_t, 512>{};
        auto const capacity = uf::checkedCast<int>(buffer.size());
        UF_CHECK(capacity.has_value());
        // SAFETY: handle is an observed HWND and buffer owns capacity writable wchar_t
        // slots for the duration of GetWindowTextW.
        auto const length = GetWindowTextW(handle, buffer.data(), *capacity);
        auto const units = copiedUtf16Units(buffer);
        return uf::controller_detail::utf16BufferToString(units, length);
    }

    [[nodiscard]]
    auto windowDpi(HWND handle) noexcept -> uf::Dpi
    {
        // SAFETY: GetDpiForWindow only inspects the opaque window token and retains no
        // caller state; zero is its documented failure value.
        return uf::Dpi{GetDpiForWindow(handle)};
    }

    [[nodiscard]]
    auto windowIsVisible(HWND handle) noexcept -> bool
    {
        // SAFETY: IsWindowVisible only inspects the opaque window token and retains no
        // caller-owned state.
        return IsWindowVisible(handle) != FALSE;
    }

    [[nodiscard]]
    auto windowIsIconic(HWND handle) noexcept -> bool
    {
        // SAFETY: IsIconic only inspects the opaque window token and retains no
        // caller-owned state.
        return IsIconic(handle) != FALSE;
    }

    [[nodiscard]]
    auto probeCandidate(uf::WindowHandle handle) -> std::optional<uf::TargetCandidate>
    {
        auto const nativeHandle = toNativeHandle(handle);
        auto const process = uf::controller_platform::windowProcess(handle);
        auto details = processDetails(process);
        auto windowClass = windowClassName(nativeHandle);
        auto title = windowTitle(nativeHandle);
        auto const clientSize = uf::controller_platform::windowClientSize(handle);
        if (!clientSize)
        {
            return std::nullopt;
        }

        return uf::TargetCandidate{
            handle,
            process,
            details.m_startTime,
            std::move(details.m_executablePath),
            std::move(windowClass),
            std::move(title),
            *clientSize,
            windowDpi(nativeHandle),
            windowIsVisible(nativeHandle),
            windowIsIconic(nativeHandle)
        };
    }
}

namespace uf::controller_platform
{
    auto enumerateCandidates() -> Result<std::vector<TargetCandidate>>
    {
        UF_TRY_VALUE(handles, enumerateWindowHandles());
        auto candidates = std::vector<TargetCandidate>{};
        candidates.reserve(handles.size());
        for (auto const handle : handles)
        {
            auto candidate = probeCandidate(handle);
            if (candidate)
            {
                candidates.emplace_back(std::move(*candidate));
            }
        }
        return candidates;
    }

    auto windowClientSize(WindowHandle handle) -> Result<ClientSize>
    {
        auto rectangle = RECT{};
        // SAFETY: the converted HWND is treated only as an opaque token and rectangle is
        // a live writable out-parameter for the duration of GetClientRect.
        auto const queried = GetClientRect(toNativeHandle(handle), &rectangle);
        if (queried == FALSE)
        {
            // SAFETY: GetLastError reads calling-thread state immediately after the failed
            // GetClientRect call and does not access caller-owned memory.
            auto const error = GetLastError();
            return fail(
                AutomationErrorKind::TargetUnavailable,
                std::format("GetClientRect failed with win32 error {}", error)
            );
        }

        auto const width = checkedCast<std::uint32_t>(
            static_cast<std::int64_t>(rectangle.right)
            - static_cast<std::int64_t>(rectangle.left)
        );
        auto const height = checkedCast<std::uint32_t>(
            static_cast<std::int64_t>(rectangle.bottom)
            - static_cast<std::int64_t>(rectangle.top)
        );
        return ClientSize{width.value_or(0), height.value_or(0)};
    }

    auto windowIsAlive(WindowHandle handle) noexcept -> bool
    {
        // SAFETY: IsWindow accepts stale opaque tokens and only reports whether the value
        // currently names a live window; it retains no caller state.
        return IsWindow(toNativeHandle(handle)) != FALSE;
    }

    auto windowProcess(WindowHandle handle) noexcept -> ProcessId
    {
        auto process = DWORD{0};
        // SAFETY: the converted HWND is an opaque token and process is a live writable
        // out-parameter for the duration of GetWindowThreadProcessId.
        static_cast<void>(GetWindowThreadProcessId(toNativeHandle(handle), &process));
        return ProcessId{static_cast<std::uint32_t>(process)};
    }

    auto processStartTime(ProcessId process) -> std::optional<ProcessStartTime>
    {
        auto const nativeHandle = openProcessForQuery(process);
        if (nativeHandle == nullptr)
        {
            return std::nullopt;
        }

        auto const ownedHandle = UniqueProcessHandle{nativeHandle};
        return readStartTime(ownedHandle.get());
    }
}
