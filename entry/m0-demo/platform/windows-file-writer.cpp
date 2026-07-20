#include "windows-file-writer.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-cast.hpp>
#include <domain/error.hpp>

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace
{
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
        NativeHandle(NativeHandle&& other) noexcept
            : m_value{std::exchange(other.m_value, INVALID_HANDLE_VALUE)}
        {
        }
        auto operator=(NativeHandle&& other) noexcept -> NativeHandle&
        {
            if (this != &other)
            {
                close();
                m_value = std::exchange(other.m_value, INVALID_HANDLE_VALUE);
            }
            return *this;
        }
        ~NativeHandle() { close(); }

        [[nodiscard]] auto get() const noexcept -> HANDLE { return m_value; }

    private:
        auto close() noexcept -> void
        {
            if (m_value != INVALID_HANDLE_VALUE)
            {
                // SAFETY: m_value is the unique live handle returned by
                // CreateFileW. CloseHandle consumes no pointer and the wrapper
                // makes exactly one close attempt before invalidating it.
                static_cast<void>(CloseHandle(m_value));
                m_value = INVALID_HANDLE_VALUE;
            }
        }
    };

    [[nodiscard]]
    auto extendedPath(std::filesystem::path const& path) -> std::wstring
    {
        auto const& native = path.native();
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
    auto fileFailure(
        std::string_view operation,
        std::filesystem::path const& path,
        DWORD nativeError
    ) -> std::unexpected<uf::Error>
    {
        auto message = std::format("Windows error {}", nativeError);
        if (auto const errorValue = uf::checkedCast<int>(nativeError))
        {
            message = std::error_code{
                *errorValue,
                std::system_category()
            }.message();
        }
        return uf::fail(
            uf::ErrorCode::Io,
            uf::automationErrorDetailCode(
                uf::AutomationErrorKind::InvalidResource
            ),
            std::format(
                "input-agent failed to {} {}: {}",
                operation,
                path.string(),
                message
            ),
            static_cast<std::int64_t>(nativeError)
        );
    }

    [[nodiscard]]
    auto openFile(
        std::filesystem::path const& path,
        DWORD desiredAccess,
        DWORD creationDisposition,
        std::string_view operation
    ) -> uf::Result<NativeHandle>
    {
        auto const nativePath = extendedPath(path);
        // SAFETY: nativePath owns a null-terminated UTF-16 buffer for this
        // synchronous call. Null security/template pointers are permitted,
        // and the returned handle is transferred immediately to NativeHandle.
        auto const handle = CreateFileW(
            nativePath.c_str(),
            desiredAccess,
            FILE_SHARE_READ,
            nullptr,
            creationDisposition,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr
        );
        if (handle == INVALID_HANDLE_VALUE)
        {
            return fileFailure(operation, path, GetLastError());
        }
        return NativeHandle{handle};
    }
}

namespace uf::m0_demo::platform
{
    struct FileWriter::State final
    {
        std::filesystem::path m_path;
        NativeHandle m_handle;
    };

    FileWriter::FileWriter(std::unique_ptr<State> state) noexcept
        : m_state{std::move(state)}
    {
    }

    FileWriter::FileWriter(FileWriter&&) noexcept = default;
    auto FileWriter::operator=(FileWriter&&) noexcept -> FileWriter& = default;
    FileWriter::~FileWriter() = default;

    auto FileWriter::createExclusive(
        std::filesystem::path const& path
    ) -> Result<FileWriter>
    {
        UF_TRY_VALUE(
            handle,
            openFile(
                path,
                GENERIC_WRITE,
                CREATE_NEW,
                "create output file"
            )
        );
        return FileWriter{
            std::make_unique<State>(
                State{
                    .m_path = path,
                    .m_handle = std::move(handle),
                }
            )
        };
    }

    auto FileWriter::openAppend(
        std::filesystem::path const& path
    ) -> Result<FileWriter>
    {
        UF_TRY_VALUE(
            handle,
            openFile(
                path,
                GENERIC_WRITE,
                OPEN_ALWAYS,
                "open results file"
            )
        );
        auto const endOffset = LARGE_INTEGER{};
        // SAFETY: handle uniquely owns a live synchronous file handle.
        // SetFilePointerEx reads endOffset by value, retains no pointer, and
        // moves this writer's file position to the existing end of the file.
        if (
            SetFilePointerEx(
                handle.get(),
                endOffset,
                nullptr,
                FILE_END
            ) == FALSE
        )
        {
            return fileFailure(
                "seek results file to end",
                path,
                GetLastError()
            );
        }
        return FileWriter{
            std::make_unique<State>(
                State{
                    .m_path = path,
                    .m_handle = std::move(handle),
                }
            )
        };
    }

    auto FileWriter::write(std::span<std::byte const> bytes) -> Status
    {
        UF_CHECK(m_state != nullptr);
        auto remaining = bytes;
        while (!remaining.empty())
        {
            auto const chunkSize = std::min(
                remaining.size(),
                static_cast<std::size_t>(
                    std::numeric_limits<DWORD>::max()
                )
            );
            auto const nativeSize = checkedCast<DWORD>(chunkSize);
            UF_CHECK(nativeSize.has_value());

            auto written = DWORD{};
            // SAFETY: remaining owns at least nativeSize readable bytes for
            // this synchronous call. State owns a live file handle, written is
            // writable for the call, and Windows retains no supplied pointer.
            auto const succeeded = WriteFile(
                m_state->m_handle.get(),
                remaining.data(),
                *nativeSize,
                &written,
                nullptr
            );
            if (succeeded == FALSE)
            {
                return fileFailure(
                    "write file",
                    m_state->m_path,
                    GetLastError()
                );
            }
            if (written == 0U || written > *nativeSize)
            {
                return fileFailure(
                    "write file",
                    m_state->m_path,
                    ERROR_WRITE_FAULT
                );
            }
            remaining = remaining.subspan(
                static_cast<std::size_t>(written)
            );
        }
        return ok();
    }

    auto FileWriter::flushDurably() -> Status
    {
        UF_CHECK(m_state != nullptr);
        // SAFETY: State owns a live synchronous file handle. FlushFileBuffers
        // uses only that handle and retains no pointer or registration.
        if (FlushFileBuffers(m_state->m_handle.get()) == FALSE)
        {
            return fileFailure(
                "durably flush file",
                m_state->m_path,
                GetLastError()
            );
        }
        return ok();
    }
}
