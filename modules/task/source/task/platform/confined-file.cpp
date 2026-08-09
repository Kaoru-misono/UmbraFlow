#include "confined-file.hpp"

#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#else
#    include <fcntl.h>
#    include <sys/stat.h>
#    include <unistd.h>
#endif

namespace uf::task_platform
{
    namespace
    {
        [[nodiscard]]
        auto refuse(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        [[nodiscard]]
        auto ioFailure(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::IoFailure, std::move(message));
        }

        // The manifest spelling is already validated by the caller; this only
        // splits it, and refuses the three shapes that would let a component
        // leave the root even so.
        [[nodiscard]]
        auto components(
            std::string_view relativeText
        ) -> Result<std::vector<std::string>>
        {
            auto parts = std::vector<std::string>{};
            auto rest  = relativeText;
            while (!rest.empty())
            {
                auto const slash = rest.find('/');
                auto const part  = rest.substr(0U, slash);
                if (
                    part.empty()
                    || part == "."
                    || part == ".."
                    || part.contains('\\')
                )
                {
                    return refuse(
                        std::format("confined path '{}' has an unusable component", relativeText)
                    );
                }
                parts.emplace_back(part);
                if (slash == std::string_view::npos)
                {
                    break;
                }
                rest.remove_prefix(slash + 1U);
            }
            if (parts.empty())
            {
                return refuse("confined path is empty");
            }
            return parts;
        }

#if defined(_WIN32)
        class Handle final
        {
            void* m_value{INVALID_HANDLE_VALUE};

        public:
            Handle() noexcept = default;

            explicit Handle(void* value) noexcept
                : m_value{value}
            {
            }

            Handle(Handle&& other) noexcept
                : m_value{std::exchange(other.m_value, INVALID_HANDLE_VALUE)}
            {
            }

            auto operator=(Handle&& other) noexcept -> Handle&
            {
                if (this != &other)
                {
                    close();
                    m_value = std::exchange(other.m_value, INVALID_HANDLE_VALUE);
                }
                return *this;
            }

            Handle(Handle const&) = delete;
            auto operator=(Handle const&) -> Handle& = delete;

            ~Handle()
            {
                close();
            }

            [[nodiscard]] auto valid() const noexcept -> bool
            {
                return m_value != INVALID_HANDLE_VALUE;
            }

            [[nodiscard]] auto get() const noexcept -> void*
            {
                return m_value;
            }

        private:
            auto close() noexcept -> void
            {
                if (m_value != INVALID_HANDLE_VALUE)
                {
                    // SAFETY: m_value came from CreateFileW below and is closed
                    // exactly once, here, because this type is move-only and
                    // the moved-from value is reset to INVALID_HANDLE_VALUE.
                    static_cast<void>(::CloseHandle(m_value));
                    m_value = INVALID_HANDLE_VALUE;
                }
            }
        };

        enum class OpenKind : uint8
        {
            Directory,
            File,
        };

        [[nodiscard]]
        auto openNoFollow(
            std::wstring const& path,
            OpenKind kind
        ) -> Result<Handle>
        {
            // FILE_SHARE_DELETE is deliberately absent: while this handle
            // lives, the object cannot be renamed or deleted, which is what
            // stops a later component resolving through a different prefix.
            // FILE_FLAG_OPEN_REPARSE_POINT opens the link itself rather than
            // its target, so the attribute check below sees it.
            // SAFETY: path is a null-terminated wide string owned by the
            // caller for the duration of the call, and the returned handle is
            // adopted by Handle, which closes it exactly once.
            auto* const raw = ::CreateFileW(
                path.c_str(),
                kind == OpenKind::Directory
                    ? static_cast<DWORD>(FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES)
                    : static_cast<DWORD>(GENERIC_READ),
                static_cast<DWORD>(FILE_SHARE_READ),
                nullptr,
                static_cast<DWORD>(OPEN_EXISTING),
                static_cast<DWORD>(
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT
                ),
                nullptr
            );
            auto handle = Handle{raw};
            if (!handle.valid())
            {
                return ioFailure("cannot open a confined path");
            }

            auto information = BY_HANDLE_FILE_INFORMATION{};
            // SAFETY: handle is valid here, and information is a local the API
            // fills completely on success.
            if (::GetFileInformationByHandle(handle.get(), &information) == 0)
            {
                return ioFailure("cannot inspect a confined path");
            }
            auto const attributes = information.dwFileAttributes;
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
            {
                return refuse("a confined path is a reparse point");
            }
            auto const isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
            if (isDirectory != (kind == OpenKind::Directory))
            {
                return refuse("a confined path is not the kind its manifest declared");
            }
            return handle;
        }

        [[nodiscard]]
        auto readAll(
            Handle const& handle,
            std::size_t maximumBytes
        ) -> Result<std::vector<std::byte>>
        {
            auto size = LARGE_INTEGER{};
            // SAFETY: handle is a valid file handle and size is a local the
            // API fills on success.
            if (::GetFileSizeEx(handle.get(), &size) == 0)
            {
                return ioFailure("cannot measure a confined file");
            }
            if (size.QuadPart < 0)
            {
                return ioFailure("a confined file reported a negative size");
            }
            auto const total = checkedCast<std::size_t>(size.QuadPart);
            if (!total || *total == 0U || *total > maximumBytes)
            {
                return refuse("a confined file is empty or exceeds its ceiling");
            }

            auto bytes  = std::vector<std::byte>(*total);
            auto filled = std::size_t{};
            while (filled < *total)
            {
                auto const remaining = checkedCast<DWORD>(*total - filled);
                if (!remaining)
                {
                    return ioFailure("a confined file is larger than one read");
                }
                auto read = DWORD{};
                // SAFETY: bytes.data() + filled stays inside the vector because
                // filled < *total == bytes.size(), and the requested count is
                // exactly the remaining distance to the end.
                if (::ReadFile(handle.get(), bytes.data() + filled, *remaining, &read, nullptr) == 0)
                {
                    return ioFailure("cannot read a confined file");
                }
                if (read == 0U)
                {
                    return ioFailure("a confined file changed while it was read");
                }
                filled += read;
            }
            return bytes;
        }
#else
        class Descriptor final
        {
            int m_value{-1};

        public:
            Descriptor() noexcept = default;

            explicit Descriptor(int value) noexcept
                : m_value{value}
            {
            }

            Descriptor(Descriptor&& other) noexcept
                : m_value{std::exchange(other.m_value, -1)}
            {
            }

            auto operator=(Descriptor&& other) noexcept -> Descriptor&
            {
                if (this != &other)
                {
                    close();
                    m_value = std::exchange(other.m_value, -1);
                }
                return *this;
            }

            Descriptor(Descriptor const&) = delete;
            auto operator=(Descriptor const&) -> Descriptor& = delete;

            ~Descriptor()
            {
                close();
            }

            [[nodiscard]] auto valid() const noexcept -> bool
            {
                return m_value >= 0;
            }

            [[nodiscard]] auto get() const noexcept -> int
            {
                return m_value;
            }

        private:
            auto close() noexcept -> void
            {
                if (m_value >= 0)
                {
                    // SAFETY: m_value came from open/openat below and is closed
                    // exactly once, because this type is move-only and the
                    // moved-from value is reset to -1.
                    static_cast<void>(::close(m_value));
                    m_value = -1;
                }
            }
        };
#endif
    }

#if defined(_WIN32)
    namespace
    {
        [[nodiscard]]
        auto rootIdentity(Handle const& handle) -> Result<std::pair<uint32, uint64>>
        {
            auto information = BY_HANDLE_FILE_INFORMATION{};
            // SAFETY: handle is valid and information is a local the API fills
            // completely on success.
            if (::GetFileInformationByHandle(handle.get(), &information) == 0)
            {
                return ioFailure("cannot identify a confined root");
            }
            auto const index = (static_cast<uint64>(information.nFileIndexHigh) << 32U)
                | static_cast<uint64>(information.nFileIndexLow);
            return std::pair{static_cast<uint32>(information.dwVolumeSerialNumber), index};
        }
    }

    class ConfinedRoot::Impl final
    {
    public:
        std::wstring rootPath{};
        Handle       root{};

        // The identity the root had when it was opened. Windows has no openat,
        // so components are still reached through the accumulated path string;
        // re-checking this before each walk is what makes the held handle the
        // anchor rather than the name. Without it a rename of any directory
        // ABOVE the root redirects the whole prefix -- the one thing the POSIX
        // branch gets for free.
        uint32 volumeSerial{};
        uint64 fileIndex{};
    };
#else
    class ConfinedRoot::Impl final
    {
    public:
        Descriptor root{};
    };
#endif

    ConfinedRoot::ConfinedRoot(std::unique_ptr<Impl> implementation) noexcept
        : m_impl{std::move(implementation)}
    {
    }

    ConfinedRoot::ConfinedRoot(ConfinedRoot&&) noexcept = default;

    auto ConfinedRoot::operator=(ConfinedRoot&&) noexcept -> ConfinedRoot& = default;

    ConfinedRoot::~ConfinedRoot() = default;

#if defined(_WIN32)
    auto ConfinedRoot::open(
        std::filesystem::path const& root
    ) -> Result<ConfinedRoot>
    {
        auto const native = root.native();
        UF_TRY_VALUE(handle, openNoFollow(native, OpenKind::Directory));
        UF_TRY_VALUE(identity, rootIdentity(handle));
        auto implementation = std::make_unique<Impl>();
        implementation->rootPath     = native;
        implementation->root         = std::move(handle);
        implementation->volumeSerial = identity.first;
        implementation->fileIndex    = identity.second;
        return ConfinedRoot{std::move(implementation)};
    }

    auto ConfinedRoot::readFile(
        std::string_view relativeText,
        std::size_t maximumBytes
    ) const -> Result<std::vector<std::byte>>
    {
        UF_TRY_VALUE(parts, components(relativeText));

        // The name must still reach the object this root was opened on; a
        // rename above it would otherwise silently move the whole walk.
        UF_TRY_VALUE(anchor, openNoFollow(m_impl->rootPath, OpenKind::Directory));
        UF_TRY_VALUE(identity, rootIdentity(anchor));
        if (
            identity.first != m_impl->volumeSerial
            || identity.second != m_impl->fileIndex
        )
        {
            return refuse(
                "the confined root no longer resolves to the directory it was opened on"
            );
        }

        // Every ancestor is held open until the file itself has been opened, so
        // no directory on the way can be renamed or replaced in between.
        auto held = std::vector<Handle>{};
        auto path = m_impl->rootPath;
        for (auto index = std::size_t{0}; index + 1U < parts.size(); ++index)
        {
            path += L'\\';
            path += std::filesystem::path{parts[index]}.native();
            UF_TRY_VALUE(directory, openNoFollow(path, OpenKind::Directory));
            held.emplace_back(std::move(directory));
        }
        path += L'\\';
        path += std::filesystem::path{parts.back()}.native();

        UF_TRY_VALUE(file, openNoFollow(path, OpenKind::File));
        return readAll(file, maximumBytes);
    }

    auto ConfinedRoot::writeNewFile(
        std::string_view relativeText,
        std::span<std::byte const> bytes
    ) const -> Status
    {
        UF_TRY_VALUE(parts, components(relativeText));

        // The name must still reach the object this root was opened on; a
        // rename above it would otherwise silently move the whole walk.
        UF_TRY_VALUE(anchor, openNoFollow(m_impl->rootPath, OpenKind::Directory));
        UF_TRY_VALUE(identity, rootIdentity(anchor));
        if (
            identity.first != m_impl->volumeSerial
            || identity.second != m_impl->fileIndex
        )
        {
            return refuse(
                "the confined root no longer resolves to the directory it was opened on"
            );
        }

        auto held = std::vector<Handle>{};
        auto path = m_impl->rootPath;
        for (auto index = std::size_t{0}; index + 1U < parts.size(); ++index)
        {
            path += L'\\';
            path += std::filesystem::path{parts[index]}.native();
            // SAFETY: path is a null-terminated wide string owned here. A
            // directory that already exists is not an error; the no-follow open
            // below is what decides whether it is acceptable.
            static_cast<void>(::CreateDirectoryW(path.c_str(), nullptr));
            UF_TRY_VALUE(directory, openNoFollow(path, OpenKind::Directory));
            held.emplace_back(std::move(directory));
        }
        path += L'\\';
        path += std::filesystem::path{parts.back()}.native();

        // CREATE_NEW plus FILE_FLAG_OPEN_REPARSE_POINT: an existing name of any
        // kind, including a planted link, fails instead of being written
        // through.
        // SAFETY: path is null-terminated and owned here, and the handle is
        // adopted by Handle, which closes it once.
        auto file = Handle{
            ::CreateFileW(
                path.c_str(),
                static_cast<DWORD>(GENERIC_WRITE),
                static_cast<DWORD>(FILE_SHARE_READ),
                nullptr,
                static_cast<DWORD>(CREATE_NEW),
                static_cast<DWORD>(FILE_FLAG_OPEN_REPARSE_POINT),
                nullptr
            )
        };
        if (!file.valid())
        {
            return ioFailure("cannot create a confined file");
        }

        auto written = std::size_t{};
        while (written < bytes.size())
        {
            auto const remaining = checkedCast<DWORD>(bytes.size() - written);
            if (!remaining)
            {
                return ioFailure("a confined write is larger than one call");
            }
            auto wrote = DWORD{};
            // SAFETY: bytes.data() + written stays inside the span because
            // written < bytes.size(), and the count is the remaining distance.
            if (::WriteFile(file.get(), bytes.data() + written, *remaining, &wrote, nullptr) == 0)
            {
                return ioFailure("cannot write a confined file");
            }
            if (wrote == 0U)
            {
                return ioFailure("a confined write made no progress");
            }
            written += wrote;
        }
        // SAFETY: file is a valid handle; flushing before the handle closes is
        // what makes the staged bytes durable for the verification that follows.
        if (::FlushFileBuffers(file.get()) == 0)
        {
            return ioFailure("cannot flush a confined file");
        }
        return ok();
    }
#else
    auto ConfinedRoot::open(
        std::filesystem::path const& root
    ) -> Result<ConfinedRoot>
    {
        // SAFETY: root.c_str() is null-terminated and outlives the call; the
        // returned descriptor is adopted by Descriptor, which closes it once.
        auto descriptor = Descriptor{
            ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        };
        if (!descriptor.valid())
        {
            return ioFailure("cannot open a confined root");
        }
        auto implementation = std::make_unique<Impl>();
        implementation->root = std::move(descriptor);
        return ConfinedRoot{std::move(implementation)};
    }

    auto ConfinedRoot::readFile(
        std::string_view relativeText,
        std::size_t maximumBytes
    ) const -> Result<std::vector<std::byte>>
    {
        UF_TRY_VALUE(parts, components(relativeText));

        auto parent = Descriptor{};
        auto current = m_impl->root.get();
        for (auto index = std::size_t{0}; index + 1U < parts.size(); ++index)
        {
            // SAFETY: current is a directory descriptor this object owns, and
            // parts[index] is null-terminated for the duration of the call.
            auto next = Descriptor{
                ::openat(
                    current,
                    parts[index].c_str(),
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC
                )
            };
            if (!next.valid())
            {
                return refuse("a confined directory is missing or is a link");
            }
            parent  = std::move(next);
            current = parent.get();
        }

        // SAFETY: current is a directory descriptor owned above, and O_NOFOLLOW
        // makes a final-component symlink fail rather than resolve.
        auto file = Descriptor{
            ::openat(current, parts.back().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)
        };
        if (!file.valid())
        {
            return refuse("a confined file is missing or is a link");
        }

        // Declared rather than `auto metadata = ...{}`: `stat` names a function
        // as well as the struct, so ordinary lookup finds the function and the
        // type is reachable only through its elaborated form.
        struct stat metadata{};
        // SAFETY: file is a valid descriptor and metadata is a local the call
        // fills completely on success.
        if (::fstat(file.get(), &metadata) != 0 || !S_ISREG(metadata.st_mode))
        {
            return refuse("a confined path is not a regular file");
        }
        auto const total = checkedCast<std::size_t>(metadata.st_size);
        if (!total || *total == 0U || *total > maximumBytes)
        {
            return refuse("a confined file is empty or exceeds its ceiling");
        }

        auto bytes  = std::vector<std::byte>(*total);
        auto filled = std::size_t{};
        while (filled < *total)
        {
            // SAFETY: bytes.data() + filled stays inside the vector because
            // filled < *total == bytes.size().
            auto const read = ::read(file.get(), bytes.data() + filled, *total - filled);
            if (read <= 0)
            {
                return ioFailure("cannot read a confined file");
            }
            filled += static_cast<std::size_t>(read);
        }
        return bytes;
    }

    auto ConfinedRoot::writeNewFile(
        std::string_view relativeText,
        std::span<std::byte const> bytes
    ) const -> Status
    {
        UF_TRY_VALUE(parts, components(relativeText));

        auto parent  = Descriptor{};
        auto current = m_impl->root.get();
        for (auto index = std::size_t{0}; index + 1U < parts.size(); ++index)
        {
            // SAFETY: current is a directory descriptor owned here, and the
            // component is null-terminated for the duration of the call. An
            // existing directory is not an error; O_NOFOLLOW below decides.
            static_cast<void>(::mkdirat(current, parts[index].c_str(), 0700));
            auto next = Descriptor{
                ::openat(
                    current,
                    parts[index].c_str(),
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC
                )
            };
            if (!next.valid())
            {
                return refuse("a confined directory is missing or is a link");
            }
            parent  = std::move(next);
            current = parent.get();
        }

        // SAFETY: O_EXCL means an existing name of any kind, including a
        // planted link, fails rather than being written through.
        auto file = Descriptor{
            ::openat(
                current,
                parts.back().c_str(),
                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                0600
            )
        };
        if (!file.valid())
        {
            return refuse("a confined file already exists or is a link");
        }

        auto written = std::size_t{};
        while (written < bytes.size())
        {
            // SAFETY: bytes.data() + written stays inside the span because
            // written < bytes.size().
            auto const wrote = ::write(file.get(), bytes.data() + written, bytes.size() - written);
            if (wrote <= 0)
            {
                return ioFailure("cannot write a confined file");
            }
            written += static_cast<std::size_t>(wrote);
        }
        if (::fsync(file.get()) != 0)
        {
            return ioFailure("cannot flush a confined file");
        }
        return ok();
    }
#endif
}
