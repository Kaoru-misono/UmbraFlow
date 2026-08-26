#include "durable-file.hpp"

#include <core/error/result.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace uf::operator_runtime::platform
{
    namespace
    {
        [[nodiscard]]
        auto ioFailure(
            std::string_view operation,
            std::filesystem::path const& path,
            std::error_code error
        ) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::IoFailure,
                std::format(
                    "Could not {} durable evidence file \"{}\"",
                    operation,
                    path.string()
                ),
                error
            );
        }

#if defined(_WIN32)
        class FileHandle final
        {
            HANDLE m_value;

        public:
            explicit FileHandle(HANDLE value) noexcept
                : m_value{value}
            {
            }

            FileHandle(FileHandle const&) = delete;
            FileHandle(FileHandle&&) = delete;
            auto operator=(FileHandle const&) -> FileHandle& = delete;
            auto operator=(FileHandle&&) -> FileHandle& = delete;

            ~FileHandle()
            {
                if (m_value != INVALID_HANDLE_VALUE)
                {
                    static_cast<void>(CloseHandle(m_value));
                }
            }

            [[nodiscard]] auto value() const noexcept -> HANDLE { return m_value; }
        };
#else
        class FileDescriptor final
        {
            int m_value;

        public:
            explicit FileDescriptor(int value) noexcept
                : m_value{value}
            {
            }

            FileDescriptor(FileDescriptor const&) = delete;
            FileDescriptor(FileDescriptor&&) = delete;
            auto operator=(FileDescriptor const&) -> FileDescriptor& = delete;
            auto operator=(FileDescriptor&&) -> FileDescriptor& = delete;

            ~FileDescriptor()
            {
                if (m_value >= 0)
                {
                    static_cast<void>(::close(m_value));
                }
            }

            [[nodiscard]] auto value() const noexcept -> int { return m_value; }
        };

        [[nodiscard]]
        auto synchronizeDirectory(std::filesystem::path const& directory) -> Status
        {
            auto descriptor = FileDescriptor{
                ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)
            };
            if (descriptor.value() < 0)
            {
                return ioFailure(
                    "open the parent directory of",
                    directory,
                    std::error_code{errno, std::generic_category()}
                );
            }
            if (::fsync(descriptor.value()) != 0)
            {
                return ioFailure(
                    "synchronize the parent directory of",
                    directory,
                    std::error_code{errno, std::generic_category()}
                );
            }
            return ok();
        }
#endif
    } // namespace

    auto writeDurableNewFile(
        std::filesystem::path const& path,
        std::span<std::byte const> bytes
    ) -> Status
    {
#if defined(_WIN32)
        auto file = FileHandle{CreateFileW(
            path.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
            nullptr
        )};
        if (file.value() == INVALID_HANDLE_VALUE)
        {
            return ioFailure(
                "create",
                path,
                std::error_code{
                    static_cast<int>(GetLastError()),
                    std::system_category()
                }
            );
        }

        auto remaining = bytes;
        while (!remaining.empty())
        {
            auto const blockSize = static_cast<DWORD>(
                (std::min)(
                    remaining.size(),
                    static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())
                )
            );
            auto written = DWORD{};
            // SAFETY: remaining names blockSize readable bytes for this
            // synchronous WriteFile call, and written is a live DWORD result.
            if (
                WriteFile(
                    file.value(),
                    remaining.data(),
                    blockSize,
                    &written,
                    nullptr
                ) == 0
                || written == 0U
            )
            {
                return ioFailure(
                    "write",
                    path,
                    std::error_code{
                        static_cast<int>(GetLastError()),
                        std::system_category()
                    }
                );
            }
            remaining = remaining.subspan(written);
        }
        if (FlushFileBuffers(file.value()) == 0)
        {
            return ioFailure(
                "synchronize",
                path,
                std::error_code{
                    static_cast<int>(GetLastError()),
                    std::system_category()
                }
            );
        }
        return ok();
#else
        auto descriptor = FileDescriptor{
            ::open(
                path.c_str(),
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                0600
            )
        };
        if (descriptor.value() < 0)
        {
            return ioFailure(
                "create",
                path,
                std::error_code{errno, std::generic_category()}
            );
        }

        auto remaining = bytes;
        while (!remaining.empty())
        {
            // SAFETY: remaining is a live contiguous byte span and write is
            // synchronous; its size bounds every byte the OS may inspect.
            auto const written = ::write(
                descriptor.value(),
                remaining.data(),
                remaining.size()
            );
            if (written <= 0)
            {
                return ioFailure(
                    "write",
                    path,
                    std::error_code{errno, std::generic_category()}
                );
            }
            remaining = remaining.subspan(static_cast<std::size_t>(written));
        }
        if (::fsync(descriptor.value()) != 0)
        {
            return ioFailure(
                "synchronize",
                path,
                std::error_code{errno, std::generic_category()}
            );
        }
        return ok();
#endif
    }

    auto publishDurableNewFile(
        std::filesystem::path const& staging,
        std::filesystem::path const& destination
    ) -> Result<DurablePublishResult>
    {
#if defined(_WIN32)
        if (
            MoveFileExW(
                staging.c_str(),
                destination.c_str(),
                MOVEFILE_WRITE_THROUGH
            ) != 0
        )
        {
            return DurablePublishResult::Published;
        }
        auto const nativeError = GetLastError();
        if (
            nativeError == ERROR_ALREADY_EXISTS
            || nativeError == ERROR_FILE_EXISTS
        )
        {
            return DurablePublishResult::AlreadyExists;
        }
        return ioFailure(
            "publish",
            destination,
            std::error_code{
                static_cast<int>(nativeError),
                std::system_category()
            }
        );
#else
        auto error        = std::error_code{};
        auto const status = std::filesystem::symlink_status(destination, error);
        if (!error && std::filesystem::exists(status))
        {
            return DurablePublishResult::AlreadyExists;
        }
        if (error && error != std::errc::no_such_file_or_directory)
        {
            return ioFailure("inspect", destination, error);
        }

        std::filesystem::rename(staging, destination, error);
        if (error)
        {
            return ioFailure("publish", destination, error);
        }
        UF_TRY(synchronizeDirectory(destination.parent_path()));
        return DurablePublishResult::Published;
#endif
    }
}
