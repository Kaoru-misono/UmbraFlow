#include "windows-file-writer.hpp"

#include "path-validation.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-cast.hpp>
#include <domain/error.hpp>

#include <Windows.h>
#include <winternl.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::input_agent::platform
{
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
                if (
                    m_value != INVALID_HANDLE_VALUE
                    && m_value != nullptr
                )
                {
                    // SAFETY: m_value is the unique live Windows handle adopted by
                    // this wrapper. CloseHandle consumes no pointer, and the
                    // wrapper makes exactly one close attempt before invalidating
                    // it.
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
            DWORD nativeError,
            std::source_location location = std::source_location::current()
        ) -> std::unexpected<Error>
        {
            auto const nativeCode = systemErrorCode(nativeError);
            auto const message = nativeCode.message();
            return fail(
                AutomationErrorKind::IoFailure,
                std::format(
                    "input-agent failed to {} {}: {}",
                    operation,
                    path.string(),
                    message
                ),
                nativeCode,
                location
            );
        }

        [[nodiscard]]
        auto openFile(
            std::filesystem::path const& path,
            DWORD desiredAccess,
            DWORD shareMode,
            DWORD creationDisposition,
            std::string_view operation
        ) -> Result<NativeHandle>
        {
            auto const nativePath = extendedPath(path);
            // SAFETY: nativePath owns a null-terminated UTF-16 buffer for this
            // synchronous call. Null security/template pointers are permitted,
            // and the returned handle is transferred immediately to NativeHandle.
            auto const handle = CreateFileW(
                nativePath.c_str(),
                desiredAccess,
                shareMode,
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

        [[nodiscard]]
        auto openDirectory(
            std::filesystem::path const& path
        ) -> Result<NativeHandle>
        {
            auto const nativePath = extendedPath(path);
            // SAFETY: nativePath owns a null-terminated UTF-16 buffer for this
            // synchronous call. The final component is deliberately followed;
            // confinement is checked against the resulting directory handle.
            auto const handle = CreateFileW(
                nativePath.c_str(),
                FILE_READ_ATTRIBUTES | FILE_TRAVERSE,
                // Deny delete sharing so the verified directory cannot be renamed
                // or removed before the handle-relative create commits.
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS,
                nullptr
            );
            if (handle == INVALID_HANDLE_VALUE)
            {
                return fileFailure(
                    "open output parent directory",
                    path,
                    GetLastError()
                );
            }

            auto nativeHandle = NativeHandle{handle};
            auto information = FILE_STANDARD_INFO{};
            // SAFETY: nativeHandle owns a live handle. information is writable for
            // the synchronous query, and Windows retains no supplied pointer.
            if (
                GetFileInformationByHandleEx(
                    nativeHandle.get(),
                    FileStandardInfo,
                    &information,
                    static_cast<DWORD>(sizeof(information))
                ) == FALSE
            )
            {
                return fileFailure(
                    "inspect output parent directory",
                    path,
                    GetLastError()
                );
            }
            if (information.Directory == FALSE)
            {
                return fileFailure(
                    "open output parent directory",
                    path,
                    ERROR_DIRECTORY
                );
            }
            return nativeHandle;
        }

        [[nodiscard]]
        auto normalizedFinalPath(std::wstring_view native) -> std::filesystem::path
        {
            auto constexpr uncPrefix = std::wstring_view{LR"(\\?\UNC\)"};
            auto constexpr extendedPrefix = std::wstring_view{LR"(\\?\)"};
            if (native.starts_with(uncPrefix))
            {
                auto normalized = std::wstring{LR"(\\)"};
                normalized += native.substr(uncPrefix.size());
                return std::filesystem::path{normalized}.lexically_normal();
            }
            if (native.starts_with(extendedPrefix))
            {
                native.remove_prefix(extendedPrefix.size());
            }
            return std::filesystem::path{native}.lexically_normal();
        }

        [[nodiscard]]
        auto finalPathForHandle(
            NativeHandle const& handle,
            std::filesystem::path const& requestedPath
        ) -> Result<std::filesystem::path>
        {
            auto constexpr flags = DWORD{FILE_NAME_NORMALIZED | VOLUME_NAME_DOS};
            // SAFETY: handle owns a live file. A null buffer with size zero asks
            // Windows for the required UTF-16 buffer size and retains no pointer.
            auto const required = GetFinalPathNameByHandleW(
                handle.get(),
                nullptr,
                0U,
                flags
            );
            if (required == 0U)
            {
                return fileFailure(
                    "resolve final path for",
                    requestedPath,
                    GetLastError()
                );
            }

            auto buffer = std::wstring(required, L'\0');
            // SAFETY: buffer owns required writable wchar_t elements for the
            // synchronous call. Windows writes at most required elements and
            // retains no supplied pointer.
            auto const length = GetFinalPathNameByHandleW(
                handle.get(),
                buffer.data(),
                required,
                flags
            );
            if (length == 0U)
            {
                return fileFailure(
                    "resolve final path for",
                    requestedPath,
                    GetLastError()
                );
            }
            if (length >= required)
            {
                return fileFailure(
                    "resolve stable final path for",
                    requestedPath,
                    ERROR_INSUFFICIENT_BUFFER
                );
            }
            buffer.resize(length);
            return normalizedFinalPath(buffer);
        }

        struct RelativeFilename final
        {
            std::wstring value{};
            USHORT       byteLength{};
        };

        [[nodiscard]]
        auto relativeFilename(
            std::filesystem::path const& path
        ) -> Result<RelativeFilename>
        {
            auto value = path.filename().native();
            if (
                value.empty()
                || value == L"."
                || value == L".."
                || value.contains(L'\\')
                || value.contains(L'/')
                || value.contains(L':')
                || value.contains(L'\0')
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "input-agent output {} must have one safe filename "
                        "component",
                        path.string()
                    )
                );
            }

            auto constexpr maximumByteLength = static_cast<std::size_t>(
                std::numeric_limits<USHORT>::max()
            );
            if (
                value.size()
                > maximumByteLength / sizeof(std::wstring::value_type)
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "input-agent output filename {} is too long",
                        path.string()
                    )
                );
            }
            auto const byteLength = checkedCast<USHORT>(
                value.size() * sizeof(std::wstring::value_type)
            );
            UF_CHECK(byteLength.has_value());

            return RelativeFilename{
                .value      = std::move(value),
                .byteLength = *byteLength,
            };
        }

        [[nodiscard]]
        auto ntFileFailure(
            std::string_view operation,
            std::filesystem::path const& path,
            NTSTATUS status
        ) -> std::unexpected<Error>
        {
            // SAFETY: RtlNtStatusToDosError converts the value without retaining
            // pointers or registering state.
            auto const nativeError = RtlNtStatusToDosError(status);
            return fileFailure(
                operation,
                path,
                nativeError
            );
        }

        [[nodiscard]]
        auto openRelativeDirectory(
            NativeHandle const& parentHandle,
            std::filesystem::path const& component,
            std::filesystem::path const& requestedPath
        ) -> Result<NativeHandle>
        {
            UF_TRY_VALUE(name, relativeFilename(component));
            auto nativeName = UNICODE_STRING{
                .Length        = name.byteLength,
                .MaximumLength = name.byteLength,
                .Buffer        = name.value.data(),
            };
            auto attributes = OBJECT_ATTRIBUTES{};
            InitializeObjectAttributes(
                &attributes,
                &nativeName,
                OBJ_CASE_INSENSITIVE,
                parentHandle.get(),
                nullptr
            );
            auto ioStatus = IO_STATUS_BLOCK{};
            auto openedHandle = HANDLE{INVALID_HANDLE_VALUE};
            // SAFETY: parentHandle pins the containing directory, and nativeName
            // is one relative component backed by writable storage for this
            // synchronous call. NtOpenFile retains no supplied pointer.
            auto const status = NtOpenFile(
                &openedHandle,
                FILE_READ_ATTRIBUTES | FILE_TRAVERSE | SYNCHRONIZE,
                &attributes,
                &ioStatus,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT
            );
            auto handle = NativeHandle{openedHandle};
            if (!NT_SUCCESS(status))
            {
                return ntFileFailure(
                    "open output directory component",
                    requestedPath,
                    status
                );
            }
            UF_CHECK_MSG(
                openedHandle != INVALID_HANDLE_VALUE
                    && openedHandle != nullptr,
                "NtOpenFile succeeded without returning a directory handle"
            );
            return handle;
        }
    }

    struct FileWriter::State final
    {
        std::filesystem::path     path{};
        std::vector<NativeHandle> directoryHandles{};
        NativeHandle              handle{};
    };

    FileWriter::FileWriter(std::unique_ptr<State> p_state) noexcept
        : m_state{std::move(p_state)}
    {
    }

    FileWriter::FileWriter(FileWriter&&) noexcept = default;
    auto FileWriter::operator=(FileWriter&&) noexcept -> FileWriter& = default;
    FileWriter::~FileWriter() = default;

    auto FileWriter::createExclusive(
        std::filesystem::path const& path,
        std::filesystem::path const& canonicalOutputDirectory
    ) -> Result<FileWriter>
    {
        UF_TRY_VALUE(filename, relativeFilename(path));
        auto const requestedParent = path.parent_path();
        UF_TRY_VALUE(
            canonicalRequestedParent,
            canonicalizePathForComparison(
                requestedParent,
                "input-agent output parent"
            )
        );
        if (
            !isPathWithinDirectory(
                canonicalRequestedParent,
                canonicalOutputDirectory
            )
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "input-agent output parent {} is outside output "
                    "directory {}",
                    canonicalRequestedParent.string(),
                    canonicalOutputDirectory.string()
                )
            );
        }

        auto relativeDirectories = std::vector<std::filesystem::path>{};
        auto requestedComponent  = canonicalRequestedParent.begin();
        for (
            auto rootComponent = canonicalOutputDirectory.begin();
            rootComponent != canonicalOutputDirectory.end();
            ++rootComponent
        )
        {
            UF_CHECK(requestedComponent != canonicalRequestedParent.end());
            ++requestedComponent;
        }
        for (
            ; requestedComponent != canonicalRequestedParent.end();
            ++requestedComponent
        )
        {
            relativeDirectories.emplace_back(*requestedComponent);
        }

        UF_TRY_VALUE(
            rootHandle,
            openDirectory(canonicalOutputDirectory)
        );
        UF_TRY_VALUE(
            finalRootPath,
            finalPathForHandle(rootHandle, canonicalOutputDirectory)
        );
        if (
            !isPathWithinDirectory(finalRootPath, canonicalOutputDirectory)
            || !isPathWithinDirectory(
                canonicalOutputDirectory,
                finalRootPath
            )
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "input-agent output directory {} resolved as unexpected "
                    "directory {}",
                    canonicalOutputDirectory.string(),
                    finalRootPath.string()
                )
            );
        }

        auto directoryHandles = std::vector<NativeHandle>{};
        directoryHandles.reserve(relativeDirectories.size() + 1U);
        directoryHandles.emplace_back(std::move(rootHandle));
        auto openedPath = canonicalOutputDirectory;
        for (auto const& component : relativeDirectories)
        {
            openedPath /= component;
            UF_TRY_VALUE(
                directoryHandle,
                openRelativeDirectory(
                    directoryHandles.back(),
                    component,
                    openedPath
                )
            );
            UF_TRY_VALUE(
                finalDirectoryPath,
                finalPathForHandle(directoryHandle, openedPath)
            );
            if (
                !isPathWithinDirectory(finalDirectoryPath, openedPath)
                || !isPathWithinDirectory(
                    openedPath,
                    finalDirectoryPath
                )
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "input-agent output directory {} did not resolve as "
                        "requested directory {}",
                        finalDirectoryPath.string(),
                        openedPath.string()
                    )
                );
            }
            directoryHandles.emplace_back(std::move(directoryHandle));
        }

        // Allocate every owning object and path before the native create. Once
        // NtCreateFile succeeds, committing its handle cannot throw or fail.
        auto p_state = std::make_unique<State>(
            State{
                .path             = path,
                .directoryHandles = std::move(directoryHandles),
                .handle           = NativeHandle{},
            }
        );

        auto nativeName = UNICODE_STRING{
            .Length        = filename.byteLength,
            .MaximumLength = filename.byteLength,
            .Buffer        = filename.value.data(),
        };
        auto attributes = OBJECT_ATTRIBUTES{};
        InitializeObjectAttributes(
            &attributes,
            &nativeName,
            OBJ_CASE_INSENSITIVE,
            p_state->directoryHandles.back().get(),
            nullptr
        );
        auto ioStatus = IO_STATUS_BLOCK{};
        auto createdHandle = HANDLE{INVALID_HANDLE_VALUE};
        // SAFETY: p_state pins the verified root-to-parent directory chain;
        // nativeName is one relative filename backed by writable storage for
        // this synchronous call. p_state already owns all potentially
        // allocating state, and NtCreateFile retains no supplied pointer.
        auto const status = NtCreateFile(
            &createdHandle,
            GENERIC_WRITE | SYNCHRONIZE,
            &attributes,
            &ioStatus,
            nullptr,
            FILE_ATTRIBUTE_NORMAL,
            0U,
            FILE_CREATE,
            FILE_NON_DIRECTORY_FILE
                | FILE_SYNCHRONOUS_IO_NONALERT
                | FILE_OPEN_REPARSE_POINT,
            nullptr,
            0U
        );
        auto handle = NativeHandle{createdHandle};
        if (!NT_SUCCESS(status))
        {
            return ntFileFailure("create output file", path, status);
        }
        UF_CHECK_MSG(
            createdHandle != INVALID_HANDLE_VALUE
                && createdHandle != nullptr,
            "NtCreateFile succeeded without returning a file handle"
        );

        p_state->handle = std::move(handle);
        return FileWriter{std::move(p_state)};
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
                FILE_SHARE_READ,
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
                    .path   = path,
                    .handle = std::move(handle),
                }
            )
        };
    }

    auto FileWriter::createOrReplace(
        std::filesystem::path const& path
    ) -> Result<FileWriter>
    {
        UF_TRY_VALUE(
            handle,
            openFile(
                path,
                GENERIC_WRITE,
                // Share reads so the file stays readable while an annotation
                // session inspects the directory by hand.
                FILE_SHARE_READ,
                CREATE_ALWAYS,
                "create or replace file"
            )
        );
        return FileWriter{
            std::make_unique<State>(
                State{
                    .path   = path,
                    .handle = std::move(handle),
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
                m_state->handle.get(),
                remaining.data(),
                *nativeSize,
                &written,
                nullptr
            );
            if (succeeded == FALSE)
            {
                return fileFailure(
                    "write file",
                    m_state->path,
                    GetLastError()
                );
            }
            if (written == 0U || written > *nativeSize)
            {
                return fileFailure(
                    "write file",
                    m_state->path,
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
        if (FlushFileBuffers(m_state->handle.get()) == FALSE)
        {
            return fileFailure(
                "durably flush file",
                m_state->path,
                GetLastError()
            );
        }
        return ok();
    }
}
