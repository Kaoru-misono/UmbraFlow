#include "windows-controller.hpp"

#include "controller/detail/discovery-logic.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::controller_platform
{
    namespace
    {
        // The Win32 path format is QueryFullProcessImageNameW's documented zero flag;
        // the C Windows SDK does not expose the name provided by the Rust binding.
        constexpr auto k_processNameWin32 = DWORD{0};

        // The location defaults at the call site so the reported origin is the
        // Win32 call that failed, not this helper.
        [[nodiscard]]
        auto win32Failure(
            std::string_view context,
            DWORD error,
            std::source_location location = std::source_location::current()
        ) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::TargetUnavailable,
                std::format("{} failed with win32 error {}", context, error),
                systemErrorCode(error),
                location
            );
        }

        [[nodiscard]]
        constexpr auto isBestEffortMetadataError(DWORD error) noexcept -> bool
        {
            // Process metadata is optional in the public discovery model. Access-denied
            // processes and processes that exit during enumeration therefore degrade to
            // absent metadata so one protected/racing process cannot hide other windows.
            return error == ERROR_ACCESS_DENIED || error == ERROR_INVALID_PARAMETER;
        }

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

            UniqueProcessHandle(UniqueProcessHandle&& other) noexcept
                : m_handle{std::exchange(other.m_handle, nullptr)}
            {
            }

            auto operator=(UniqueProcessHandle&&) -> UniqueProcessHandle& = delete;

            ~UniqueProcessHandle()
            {
                // SAFETY: OpenProcess returned this owned handle, it is closed exactly once
                // here, and no caller can retain the private native value afterwards.
                if (m_handle != nullptr)
                {
                    static_cast<void>(CloseHandle(m_handle));
                }
            }

            [[nodiscard]] auto get() const noexcept -> HANDLE { return m_handle; }
        };

        struct ProcessDetails final
        {
            std::optional<ProcessStartTime>      startTime{};
            std::optional<std::filesystem::path> executablePath{};
        };

        struct EnumerationState final
        {
            std::vector<WindowHandle> handles{};
            bool                      storageFailed{};
        };

        [[nodiscard]]
        constexpr auto copiedUtf16Unit(wchar_t unit) noexcept -> char16_t
        {
            static_assert(sizeof(wchar_t) == sizeof(char16_t));
            // SAFETY: Windows wchar_t and char16_t are equally sized UTF-16 code units.
            // bit_cast preserves every code-unit bit without aliasing or narrowing.
            return std::bit_cast<char16_t>(unit);
        }

        template <std::size_t Size>
        [[nodiscard]]
        auto copiedUtf16Units(
            std::array<wchar_t, Size> const& buffer
        ) -> std::array<char16_t, Size>
        {
            auto units = std::array<char16_t, Size>{};
            std::ranges::transform(
                buffer,
                units.begin(),
                copiedUtf16Unit
            );
            return units;
        }

        [[nodiscard]]
        auto copiedUtf16Units(
            std::span<wchar_t const> buffer
        ) -> std::vector<char16_t>
        {
            auto units = std::vector<char16_t>{};
            units.reserve(buffer.size());
            for (auto const unit : buffer)
            {
                units.emplace_back(copiedUtf16Unit(unit));
            }
            return units;
        }

        [[nodiscard]]
        auto toNativeHandle(WindowHandle handle) noexcept -> HWND
        {
            // SAFETY: WindowHandle stores the pointer-sized integer representation copied
            // from an HWND. The boundary restores that representation without dereferencing it.
            return std::bit_cast<HWND>(handle.value());
        }

        [[nodiscard]]
        auto fromNativeHandle(HWND handle) noexcept -> WindowHandle
        {
            // SAFETY: HWND is an opaque pointer-sized token. Copying its representation into
            // intptr preserves every bit and does not claim ownership or dereference it.
            return WindowHandle{std::bit_cast<intptr>(handle)};
        }

        auto CALLBACK enumerateWindow(HWND handle, LPARAM data) noexcept -> BOOL
        {
            // SAFETY: EnumWindows synchronously returns the exact pointer-sized token supplied
            // by enumerateWindowHandles; the state remains alive for the complete callback run.
            auto* p_state = std::bit_cast<EnumerationState*>(data);
            if (p_state == nullptr)
            {
                return FALSE;
            }

            try
            {
                p_state->handles.emplace_back(fromNativeHandle(handle));
            }
            catch (...)
            {
                p_state->storageFailed = true;
                return FALSE;
            }
            return TRUE;
        }

        [[nodiscard]]
        auto enumerateWindowHandles() -> Result<std::vector<WindowHandle>>
        {
            auto state = EnumerationState{};

            // SAFETY: Clearing the calling thread's last-error slot has no pointer or lifetime
            // precondition and lets an EnumWindows failure report an unambiguous native code.
            SetLastError(ERROR_SUCCESS);
            // SAFETY: The pointer is encoded in a pointer-sized LPARAM and will only be decoded
            // by the synchronous callback while state remains alive.
            auto const stateParameter = std::bit_cast<LPARAM>(&state);
            // SAFETY: EnumWindows invokes enumerateWindow synchronously with stateParameter;
            // the callback never retains the encoded pointer after returning.
            auto const enumerated = EnumWindows(
                enumerateWindow,
                stateParameter
            );
            if (state.storageFailed)
            {
                return fail(
                    AutomationErrorKind::TargetUnavailable,
                    "EnumWindows callback could not store a window handle"
                );
            }
            if (enumerated == FALSE)
            {
                // SAFETY: GetLastError reads calling-thread state immediately after the failed
                // EnumWindows call and does not access caller-owned memory.
                auto const error = GetLastError();
                return win32Failure("EnumWindows", error);
            }

            return std::move(state.handles);
        }

        [[nodiscard]]
        auto openProcessForQuery(
            ProcessId process
        ) -> Result<std::optional<UniqueProcessHandle>>
        {
            // SAFETY: OpenProcess receives a numeric process id and no pointers. A non-null
            // result is a newly owned handle placed under RAII before returning.
            auto const handle = OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE,
                process.value()
            );
            if (handle != nullptr)
            {
                return std::optional<UniqueProcessHandle>{std::in_place, handle};
            }

            // SAFETY: GetLastError reads calling-thread state immediately after the failed
            // OpenProcess call and does not access caller-owned memory.
            auto const error = GetLastError();
            if (isBestEffortMetadataError(error))
            {
                return std::optional<UniqueProcessHandle>{};
            }
            return win32Failure("OpenProcess", error);
        }

        [[nodiscard]]
        auto readStartTime(
            HANDLE process
        ) -> Result<std::optional<ProcessStartTime>>
        {
            auto creation = FILETIME{};
            auto exit     = FILETIME{};
            auto kernel   = FILETIME{};
            auto user     = FILETIME{};
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
                // SAFETY: GetLastError reads calling-thread state immediately after the
                // failed GetProcessTimes call and does not access caller-owned memory.
                auto const error = GetLastError();
                if (isBestEffortMetadataError(error))
                {
                    return std::optional<ProcessStartTime>{};
                }
                return win32Failure("GetProcessTimes", error);
            }

            return std::optional<ProcessStartTime>{
                std::in_place,
                controller_detail::fileTimeToTicks(
                    creation.dwHighDateTime,
                    creation.dwLowDateTime
                )
            };
        }

        [[nodiscard]]
        auto readExecutablePath(
            HANDLE process
        ) -> Result<std::optional<std::filesystem::path>>
        {
            constexpr auto maximumCapacity = std::size_t{32} * 1024U;
            auto buffer = std::vector<wchar_t>(512U);
            while (true)
            {
                auto const capacity = checkedCast<DWORD>(buffer.size());
                UF_CHECK(capacity.has_value());
                auto size = *capacity;
                // SAFETY: process is a live query handle; buffer owns size writable wchar_t
                // slots, and size is a live in/out count initialized to that exact capacity.
                auto const queried = QueryFullProcessImageNameW(
                    process,
                    k_processNameWin32,
                    buffer.data(),
                    &size
                );
                if (queried == FALSE)
                {
                    // SAFETY: GetLastError reads calling-thread state immediately after the
                    // failed QueryFullProcessImageNameW call and accesses no caller memory.
                    auto const error = GetLastError();
                    if (error == ERROR_INSUFFICIENT_BUFFER)
                    {
                        if (buffer.size() >= maximumCapacity)
                        {
                            return win32Failure(
                                "QueryFullProcessImageNameW exceeded the supported path capacity",
                                error
                            );
                        }
                        buffer.resize(
                            std::min(buffer.size() * 2U, maximumCapacity)
                        );
                        continue;
                    }
                    if (isBestEffortMetadataError(error))
                    {
                        return std::optional<std::filesystem::path>{};
                    }
                    return win32Failure("QueryFullProcessImageNameW", error);
                }

                auto const written = checkedCast<std::size_t>(size);
                UF_CHECK(written.has_value());
                if (*written > buffer.size())
                {
                    return fail(
                        AutomationErrorKind::TargetUnavailable,
                        "QueryFullProcessImageNameW returned an invalid path length"
                    );
                }
                auto const length = checkedCast<int32>(*written);
                UF_CHECK(length.has_value());
                auto const units = copiedUtf16Units(
                    std::span{buffer}.first(*written)
                );
                return std::optional<std::filesystem::path>{
                    controller_detail::utf16BufferToPath(units, *length)
                };
            }
        }

        [[nodiscard]]
        auto processDetails(ProcessId process) -> Result<ProcessDetails>
        {
            UF_TRY_VALUE(ownedHandle, openProcessForQuery(process));
            if (!ownedHandle)
            {
                return ProcessDetails{};
            }

            UF_TRY_VALUE(startTime, readStartTime(ownedHandle->get()));
            UF_TRY_VALUE(executablePath, readExecutablePath(ownedHandle->get()));
            return ProcessDetails{
                .startTime      = startTime,
                .executablePath = std::move(executablePath),
            };
        }

        [[nodiscard]]
        auto windowClassName(HWND handle) -> Result<std::string>
        {
            auto buffer = std::array<wchar_t, 256>{};
            auto const capacity = checkedCast<int>(buffer.size());
            UF_CHECK(capacity.has_value());
            // SAFETY: Clearing the calling thread's last-error slot has no pointer or lifetime
            // precondition and lets a failed query carry an unambiguous native code.
            SetLastError(ERROR_SUCCESS);
            // SAFETY: handle is an observed HWND and buffer owns capacity writable wchar_t
            // slots for the duration of GetClassNameW.
            auto const length = GetClassNameW(handle, buffer.data(), *capacity);
            if (length == 0)
            {
                // SAFETY: GetLastError reads calling-thread state immediately after the
                // failed GetClassNameW call and does not access caller-owned memory.
                return win32Failure("GetClassNameW", GetLastError());
            }
            auto const units = copiedUtf16Units(buffer);
            return controller_detail::utf16BufferToString(units, length);
        }

        [[nodiscard]]
        auto windowTitle(HWND handle) -> Result<std::string>
        {
            auto buffer = std::array<wchar_t, 512>{};
            auto const capacity = checkedCast<int>(buffer.size());
            UF_CHECK(capacity.has_value());
            // SAFETY: Clearing the calling thread's last-error slot has no pointer or lifetime
            // precondition and distinguishes an empty title from a failed GetWindowTextW.
            SetLastError(ERROR_SUCCESS);
            // SAFETY: handle is an observed HWND and buffer owns capacity writable wchar_t
            // slots for the duration of GetWindowTextW.
            auto const length = GetWindowTextW(handle, buffer.data(), *capacity);
            if (length == 0)
            {
                // SAFETY: GetLastError reads calling-thread state immediately after
                // GetWindowTextW and does not access caller-owned memory.
                auto const error = GetLastError();
                if (error != ERROR_SUCCESS)
                {
                    return win32Failure("GetWindowTextW", error);
                }
            }
            auto const units = copiedUtf16Units(buffer);
            return controller_detail::utf16BufferToString(units, length);
        }

        [[nodiscard]]
        auto windowDpi(HWND handle) -> Result<Dpi>
        {
            // SAFETY: Clearing the calling thread's last-error slot has no pointer or lifetime
            // precondition and lets a failed query carry an unambiguous native code.
            SetLastError(ERROR_SUCCESS);
            // SAFETY: GetDpiForWindow only inspects the opaque window token and retains no
            // caller state; zero is its documented failure value.
            auto const dpi = GetDpiForWindow(handle);
            if (dpi == 0)
            {
                // SAFETY: GetLastError reads calling-thread state immediately after the
                // failed GetDpiForWindow call and does not access caller-owned memory.
                return win32Failure("GetDpiForWindow", GetLastError());
            }
            return Dpi{dpi};
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
        auto probeCandidate(
            WindowHandle handle
        ) -> Result<std::optional<TargetCandidate>>
        {
            auto const nativeHandle = toNativeHandle(handle);
            if (!controller_platform::windowIsAlive(handle))
            {
                return std::optional<TargetCandidate>{};
            }

            auto process = controller_platform::windowProcess(handle);
            if (!process)
            {
                if (!controller_platform::windowIsAlive(handle))
                {
                    return std::optional<TargetCandidate>{};
                }
                return std::unexpected{std::move(process).error()};
            }

            auto details = processDetails(*process);
            if (!details)
            {
                if (!controller_platform::windowIsAlive(handle))
                {
                    return std::optional<TargetCandidate>{};
                }
                return std::unexpected{std::move(details).error()};
            }

            auto windowClass = windowClassName(nativeHandle);
            if (!windowClass)
            {
                if (!controller_platform::windowIsAlive(handle))
                {
                    return std::optional<TargetCandidate>{};
                }
                return std::unexpected{std::move(windowClass).error()};
            }

            auto title = windowTitle(nativeHandle);
            if (!title)
            {
                if (!controller_platform::windowIsAlive(handle))
                {
                    return std::optional<TargetCandidate>{};
                }
                return std::unexpected{std::move(title).error()};
            }

            auto clientSize = controller_platform::windowClientSize(handle);
            if (!clientSize)
            {
                if (!controller_platform::windowIsAlive(handle))
                {
                    return std::optional<TargetCandidate>{};
                }
                return std::unexpected{std::move(clientSize).error()};
            }

            auto dpi = windowDpi(nativeHandle);
            if (!dpi)
            {
                if (!controller_platform::windowIsAlive(handle))
                {
                    return std::optional<TargetCandidate>{};
                }
                return std::unexpected{std::move(dpi).error()};
            }

            if (!controller_platform::windowIsAlive(handle))
            {
                return std::optional<TargetCandidate>{};
            }

            return std::optional<TargetCandidate>{std::in_place,
                handle,
                *process,
                details->startTime,
                std::move(details->executablePath),
                *std::move(windowClass),
                *std::move(title),
                *clientSize,
                *dpi,
                windowIsVisible(nativeHandle),
                windowIsIconic(nativeHandle)
            };
        }
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
            UF_TRY_VALUE(candidate, probeCandidate(handle));
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
            return win32Failure("GetClientRect", error);
        }

        auto const width = checkedCast<uint32>(
            static_cast<int64>(rectangle.right)
            - static_cast<int64>(rectangle.left)
        );
        auto const height = checkedCast<uint32>(
            static_cast<int64>(rectangle.bottom)
            - static_cast<int64>(rectangle.top)
        );
        if (!width || !height)
        {
            return fail(
                AutomationErrorKind::TargetUnavailable,
                "GetClientRect returned invalid client dimensions"
            );
        }
        return ClientSize{*width, *height};
    }

    auto windowIsAlive(WindowHandle handle) noexcept -> bool
    {
        // SAFETY: IsWindow accepts stale opaque tokens and only reports whether the value
        // currently names a live window; it retains no caller state.
        return IsWindow(toNativeHandle(handle)) != FALSE;
    }

    auto windowProcess(WindowHandle handle) -> Result<ProcessId>
    {
        auto process = DWORD{0};
        // SAFETY: Clearing the calling thread's last-error slot has no pointer or lifetime
        // precondition and lets a failed query carry an unambiguous native code.
        SetLastError(ERROR_SUCCESS);
        // SAFETY: the converted HWND is an opaque token and process is a live writable
        // out-parameter for the duration of GetWindowThreadProcessId.
        auto const thread = GetWindowThreadProcessId(toNativeHandle(handle), &process);
        if (thread == 0)
        {
            // SAFETY: GetLastError reads calling-thread state immediately after the
            // failed GetWindowThreadProcessId call and accesses no caller memory.
            return win32Failure("GetWindowThreadProcessId", GetLastError());
        }
        return ProcessId{static_cast<uint32>(process)};
    }

    auto processStartTime(
        ProcessId process
    ) -> Result<std::optional<ProcessStartTime>>
    {
        UF_TRY_VALUE(ownedHandle, openProcessForQuery(process));
        if (!ownedHandle)
        {
            return std::optional<ProcessStartTime>{};
        }

        return readStartTime(ownedHandle->get());
    }
}
