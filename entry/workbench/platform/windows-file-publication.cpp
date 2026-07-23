#include "windows-file-publication.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>
#include <core/utility/scope-exit.hpp>

#include <domain/error.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace uf::workbench::platform
{
    namespace
    {
        constexpr auto g_temporaryFileAttempts = uint32{64};
        constexpr auto g_readBufferBytes       = std::size_t{64} * 1024U;

        class NativeHandle final
        {
            HANDLE m_value{INVALID_HANDLE_VALUE};

        public:
            NativeHandle() noexcept = default;

            explicit NativeHandle(HANDLE value) noexcept
                : m_value{value}
            {
            }

            NativeHandle(NativeHandle const&) = delete;
            auto operator=(NativeHandle const&) -> NativeHandle& = delete;
            NativeHandle(NativeHandle&&) = delete;
            auto operator=(NativeHandle&&) -> NativeHandle& = delete;

            ~NativeHandle() { static_cast<void>(close()); }

            [[nodiscard]] auto get() const noexcept -> HANDLE { return m_value; }

            [[nodiscard]]
            auto close() noexcept -> bool
            {
                if (m_value == INVALID_HANDLE_VALUE || m_value == nullptr)
                {
                    return true;
                }
                auto const value = std::exchange(m_value, INVALID_HANDLE_VALUE);
                // SAFETY: value is the unique live Windows handle adopted by this
                // object. CloseHandle consumes no pointer and is attempted once.
                return CloseHandle(value) != FALSE;
            }
        };

        [[nodiscard]]
        auto temporaryFileCleanup(std::wstring path)
        {
            return scopeExit(
                [owned = std::move(path)]() noexcept
                {
                    // SAFETY: owned holds a null-terminated UTF-16 path for the
                    // whole guard lifetime. This best-effort cleanup is
                    // synchronous and retains no pointer.
                    static_cast<void>(DeleteFileW(owned.c_str()));
                }
            );
        }

        [[nodiscard]]
        auto invalidProject(std::string message) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::move(message)
            );
        }

        [[nodiscard]]
        auto windowsIoFailure(
            std::string_view operation,
            std::filesystem::path const& path,
            DWORD nativeError,
            std::source_location location = std::source_location::current()
        ) -> std::unexpected<Error>
        {
            auto const nativeCode = systemErrorCode(nativeError);
            auto const message = nativeCode.message();
            return fail(
                AutomationErrorKind::IoFailure,
                std::format(
                    "workbench failed to {} {}: {}",
                    operation,
                    path.string(),
                    message
                ),
                nativeCode,
                location
            );
        }

        [[nodiscard]]
        auto extendedPath(std::filesystem::path const& path) -> std::wstring
        {
            auto preferred = path;
            preferred.make_preferred();
            auto const& native = preferred.native();
            if (
                native.starts_with(LR"(\\?\)")
                || native.starts_with(LR"(\\.\)")
            )
            {
                return native;
            }
            if (native.starts_with(LR"(\\)"))
            {
                return LR"(\\?\UNC\)" + native.substr(2U);
            }
            return LR"(\\?\)" + native;
        }

        [[nodiscard]]
        auto fileMatches(
            std::filesystem::path const& path,
            std::span<std::byte const> expected
        ) -> Result<std::optional<bool>>
        {
            auto const nativePath = extendedPath(path);
            // SAFETY: nativePath owns a null-terminated UTF-16 path for this
            // synchronous call. The returned handle is adopted immediately.
            auto const rawHandle = CreateFileW(
                nativePath.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr
            );
            if (rawHandle == INVALID_HANDLE_VALUE)
            {
                auto const nativeError = GetLastError();
                if (
                    nativeError == ERROR_FILE_NOT_FOUND
                    || nativeError == ERROR_PATH_NOT_FOUND
                )
                {
                    return std::nullopt;
                }
                return windowsIoFailure("open existing file", path, nativeError);
            }
            auto handle = NativeHandle{rawHandle};

            auto nativeSize = LARGE_INTEGER{};
            // SAFETY: handle uniquely owns a live synchronous file handle and
            // nativeSize is writable for the duration of the call.
            if (GetFileSizeEx(handle.get(), &nativeSize) == FALSE)
            {
                return windowsIoFailure(
                    "inspect existing file",
                    path,
                    GetLastError()
                );
            }
            if (nativeSize.QuadPart < 0)
            {
                return invalidProject(
                    std::format("workbench file {} has a negative size", path.string())
                );
            }
            auto const size = checkedCast<std::size_t>(nativeSize.QuadPart);
            if (!size)
            {
                return invalidProject(
                    std::format("workbench file {} is not addressable", path.string())
                );
            }
            if (*size != expected.size())
            {
                return false;
            }

            auto buffer    = std::array<std::byte, g_readBufferBytes>{};
            auto remaining = expected;
            while (!remaining.empty())
            {
                auto const chunkSize       = std::min(buffer.size(), remaining.size());
                auto const nativeChunkSize = checkedCast<DWORD>(chunkSize);
                UF_CHECK(nativeChunkSize.has_value());

                auto bytesRead = DWORD{};
                // SAFETY: buffer owns nativeChunkSize writable bytes and handle
                // is live. ReadFile completes synchronously and retains nothing.
                auto const succeeded = ReadFile(
                    handle.get(),
                    buffer.data(),
                    *nativeChunkSize,
                    &bytesRead,
                    nullptr
                );
                if (succeeded == FALSE)
                {
                    return windowsIoFailure(
                        "read existing file",
                        path,
                        GetLastError()
                    );
                }
                if (bytesRead != *nativeChunkSize)
                {
                    return false;
                }
                auto const readBytes = std::span<std::byte const>{
                    buffer.data(),
                    chunkSize
                };
                if (!std::ranges::equal(readBytes, remaining.first(chunkSize)))
                {
                    return false;
                }
                remaining = remaining.subspan(chunkSize);
            }
            return true;
        }

        [[nodiscard]]
        auto writeTemporaryFile(
            std::filesystem::path const& destination,
            std::span<std::byte const> bytes
        ) -> Result<std::filesystem::path>
        {
            static auto s_sequence = std::atomic<uint64>{1U};

            for (auto attempt = uint32{0}; attempt < g_temporaryFileAttempts; ++attempt)
            {
                auto temporary = destination;
                temporary += std::format(
                    L".umbraflow-tmp-{}-{}",
                    GetCurrentProcessId(),
                    s_sequence.fetch_add(1U, std::memory_order_relaxed)
                );
                auto const nativeTemporary = extendedPath(temporary);
                // SAFETY: nativeTemporary owns a null-terminated UTF-16 path for
                // this synchronous call. A successful handle is adopted below.
                auto const rawHandle = CreateFileW(
                    nativeTemporary.c_str(),
                    GENERIC_WRITE,
                    0,
                    nullptr,
                    CREATE_NEW,
                    (
                        FILE_ATTRIBUTE_NORMAL
                        | FILE_FLAG_WRITE_THROUGH
                        | FILE_FLAG_OPEN_REPARSE_POINT
                    ),
                    nullptr
                );
                if (rawHandle == INVALID_HANDLE_VALUE)
                {
                    auto const nativeError = GetLastError();
                    if (
                        nativeError == ERROR_FILE_EXISTS
                        || nativeError == ERROR_ALREADY_EXISTS
                    )
                    {
                        continue;
                    }
                    return windowsIoFailure(
                        "create temporary file",
                        temporary,
                        nativeError
                    );
                }

                auto cleanup   = temporaryFileCleanup(nativeTemporary);
                auto handle    = NativeHandle{rawHandle};
                auto remaining = bytes;
                while (!remaining.empty())
                {
                    auto const chunkSize = std::min(
                        remaining.size(),
                        static_cast<std::size_t>(
                            std::numeric_limits<DWORD>::max()
                        )
                    );
                    auto const nativeChunkSize = checkedCast<DWORD>(chunkSize);
                    UF_CHECK(nativeChunkSize.has_value());

                    auto bytesWritten = DWORD{};
                    // SAFETY: remaining owns nativeChunkSize readable bytes and
                    // handle is live. WriteFile completes synchronously.
                    auto const succeeded = WriteFile(
                        handle.get(),
                        remaining.data(),
                        *nativeChunkSize,
                        &bytesWritten,
                        nullptr
                    );
                    if (succeeded == FALSE)
                    {
                        return windowsIoFailure(
                            "write temporary file",
                            temporary,
                            GetLastError()
                        );
                    }
                    if (bytesWritten == 0U || bytesWritten > *nativeChunkSize)
                    {
                        return windowsIoFailure(
                            "write temporary file",
                            temporary,
                            ERROR_WRITE_FAULT
                        );
                    }
                    remaining = remaining.subspan(
                        static_cast<std::size_t>(bytesWritten)
                    );
                }

                // SAFETY: handle owns a live synchronous file handle and the call
                // retains no pointer or registration.
                if (FlushFileBuffers(handle.get()) == FALSE)
                {
                    return windowsIoFailure(
                        "flush temporary file",
                        temporary,
                        GetLastError()
                    );
                }
                if (!handle.close())
                {
                    return windowsIoFailure(
                        "close temporary file",
                        temporary,
                        GetLastError()
                    );
                }
                cleanup.release();
                return temporary;
            }
            return invalidProject(
                std::format(
                    "workbench could not allocate a temporary sibling for {}",
                    destination.string()
                )
            );
        }
    }

    auto publishImmutableFile(
        std::filesystem::path const& destination,
        std::span<std::byte const> bytes
    ) -> Status
    {
        UF_TRY_VALUE(existing, fileMatches(destination, bytes));
        if (existing.has_value())
        {
            if (*existing)
            {
                return ok();
            }
            return invalidProject(
                std::format(
                    "content-addressed file {} already exists with different bytes",
                    destination.string()
                )
            );
        }

        UF_TRY_VALUE(temporary, writeTemporaryFile(destination, bytes));
        auto const nativeTemporary   = extendedPath(temporary);
        auto const nativeDestination = extendedPath(destination);
        auto cleanup                 = temporaryFileCleanup(nativeTemporary);

        // SAFETY: both strings own null-terminated UTF-16 paths. MoveFileExW
        // consumes them synchronously and retains no pointer.
        if (
            MoveFileExW(
                nativeTemporary.c_str(),
                nativeDestination.c_str(),
                MOVEFILE_WRITE_THROUGH
            ) != FALSE
        )
        {
            cleanup.release();
            return ok();
        }
        auto const nativeError = GetLastError();
        if (
            nativeError == ERROR_FILE_EXISTS
            || nativeError == ERROR_ALREADY_EXISTS
        )
        {
            UF_TRY_VALUE(racedExisting, fileMatches(destination, bytes));
            if (racedExisting == std::optional<bool>{true})
            {
                return ok();
            }
        }
        return windowsIoFailure(
            "publish content-addressed file",
            destination,
            nativeError
        );
    }

    auto replaceFileAtomically(
        std::filesystem::path const& destination,
        std::span<std::byte const> bytes
    ) -> Status
    {
        UF_TRY_VALUE(temporary, writeTemporaryFile(destination, bytes));
        auto const nativeTemporary   = extendedPath(temporary);
        auto const nativeDestination = extendedPath(destination);
        auto cleanup                 = temporaryFileCleanup(nativeTemporary);

        // SAFETY: temporary is a flushed sibling of destination. MoveFileExW
        // performs the same-volume name switch synchronously and retains no pointer.
        if (
            MoveFileExW(
                nativeTemporary.c_str(),
                nativeDestination.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
            ) == FALSE
        )
        {
            return windowsIoFailure(
                "replace file atomically",
                destination,
                GetLastError()
            );
        }
        cleanup.release();
        return ok();
    }
}
