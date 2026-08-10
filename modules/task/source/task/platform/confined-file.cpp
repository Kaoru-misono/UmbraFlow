#include "confined-file.hpp"

#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
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
#    include <cerrno>
#    include <dirent.h>
#    include <fcntl.h>
#    include <sys/stat.h>
#    include <unistd.h>
#endif

namespace uf::task_platform
{
    namespace
    {
        // A tree an installer produced is three levels deep at most. The
        // ceiling exists because the removal below walks whatever is actually
        // there, and what is actually there is writable by whoever can plant a
        // link in it.
        constexpr auto k_maximumRemovalDepth = uint32{32};

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

        // A child name must name a child. These are the shapes that would
        // otherwise leave the directory this root was opened on.
        [[nodiscard]]
        auto requireChildName(std::string_view name) -> Status
        {
            if (
                name.empty()
                || name == "."
                || name == ".."
                || name.contains('/')
                || name.contains('\\')
            )
            {
                return refuse(std::format("'{}' is not a single child name", name));
            }
            return ok();
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

        // FILE_SHARE_DELETE is deliberately absent from every open below: while
        // a handle lives its object cannot be renamed or removed by anyone
        // else, which is what keeps the accumulated path bound to the objects
        // already checked. That is also why the root is opened WITHOUT DELETE
        // in its mask -- a second open asking for an access the first one's
        // share mode denies is a sharing violation against ourselves.
        constexpr auto k_directoryAccess = static_cast<DWORD>(
            FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES
        );

        constexpr auto k_fileReadAccess = static_cast<DWORD>(GENERIC_READ);

        constexpr auto k_removalAccess = static_cast<DWORD>(
            DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES
        );

        constexpr auto k_enumerationBufferBytes = std::size_t{64U * 1024U};

        struct OpenedNode final
        {
            Handle handle{};
            bool   directory{};
        };

        // The one resolution every operation shares. An absent name is reported
        // as an empty optional rather than as a failure, because a removal that
        // finds its target already gone has the outcome it wanted.
        [[nodiscard]]
        auto openNode(
            std::wstring const& path,
            DWORD access
        ) -> Result<std::optional<OpenedNode>>
        {
            // FILE_FLAG_OPEN_REPARSE_POINT opens the link itself rather than
            // its target, so the attribute check below sees it.
            // SAFETY: path is a null-terminated wide string owned by the
            // caller for the duration of the call, and the returned handle is
            // adopted by Handle, which closes it exactly once.
            auto* const raw = ::CreateFileW(
                path.c_str(),
                access,
                static_cast<DWORD>(FILE_SHARE_READ),
                nullptr,
                static_cast<DWORD>(OPEN_EXISTING),
                static_cast<DWORD>(
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT
                ),
                nullptr
            );
            auto const openError = ::GetLastError();
            auto handle = Handle{raw};
            if (!handle.valid())
            {
                if (
                    openError == ERROR_FILE_NOT_FOUND
                    || openError == ERROR_PATH_NOT_FOUND
                )
                {
                    return std::optional<OpenedNode>{};
                }
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
            return std::optional<OpenedNode>{
                OpenedNode{
                    .handle    = std::move(handle),
                    .directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U,
                }
            };
        }

        // The walk's open: the name must exist and must be the kind the caller
        // declared it would be.
        [[nodiscard]]
        auto openNoFollow(
            std::wstring const& path,
            OpenKind kind
        ) -> Result<Handle>
        {
            auto const access = kind == OpenKind::Directory
                ? k_directoryAccess
                : k_fileReadAccess;
            UF_TRY_VALUE(opened, openNode(path, access));
            if (!opened)
            {
                return ioFailure("cannot open a confined path");
            }
            if (opened->directory != (kind == OpenKind::Directory))
            {
                return refuse("a confined path is not the kind its manifest declared");
            }
            return std::move(opened->handle);
        }

        struct RootIdentity final
        {
            uint32 volumeSerial{};
            uint64 fileIndex{};
        };

        [[nodiscard]]
        auto identityOf(Handle const& handle) -> Result<RootIdentity>
        {
            auto information = BY_HANDLE_FILE_INFORMATION{};
            // SAFETY: handle is valid and information is a local the API fills
            // completely on success.
            if (::GetFileInformationByHandle(handle.get(), &information) == 0)
            {
                return ioFailure("cannot identify a confined root");
            }
            auto index = static_cast<uint64>(information.nFileIndexHigh) << 32U;
            index |= static_cast<uint64>(information.nFileIndexLow);
            return RootIdentity{
                .volumeSerial = static_cast<uint32>(information.dwVolumeSerialNumber),
                .fileIndex    = index,
            };
        }

        // Enumeration goes THROUGH the handle rather than through the name
        // again: FindFirstFileW would reopen the directory, and a second open
        // is both another resolution to get right and a sharing question
        // against the handle already held.
        [[nodiscard]]
        auto childWideNames(Handle const& handle) -> Result<std::vector<std::wstring>>
        {
            auto buffer  = std::vector<std::byte>(k_enumerationBufferBytes);
            auto names   = std::vector<std::wstring>{};
            auto restart = true;
            while (true)
            {
                auto const informationClass = restart
                    ? FileIdBothDirectoryRestartInfo
                    : FileIdBothDirectoryInfo;
                restart = false;
                // SAFETY: buffer is a live vector and the size passed is its
                // own; the API fills it with a chain of records whose
                // NextEntryOffset bounds the walk below.
                auto const filled = ::GetFileInformationByHandleEx(
                    handle.get(),
                    informationClass,
                    buffer.data(),
                    static_cast<DWORD>(buffer.size())
                );
                if (filled == 0)
                {
                    if (::GetLastError() == ERROR_NO_MORE_FILES)
                    {
                        return names;
                    }
                    return ioFailure("cannot enumerate a confined directory");
                }

                auto offset = std::size_t{};
                while (true)
                {
                    if (offset + sizeof(FILE_ID_BOTH_DIR_INFO) > buffer.size())
                    {
                        return ioFailure("a directory enumeration record ran past its buffer");
                    }
                    // SAFETY: the bound above keeps the whole fixed part of the
                    // record inside the buffer, and the name length below is
                    // the one the same record declares.
                    auto const* const entry = reinterpret_cast<FILE_ID_BOTH_DIR_INFO const*>(
                        buffer.data() + offset
                    );
                    auto const nameBytes = static_cast<std::size_t>(entry->FileNameLength);
                    auto name = std::wstring{entry->FileName, nameBytes / sizeof(wchar_t)};
                    if (name != L"." && name != L"..")
                    {
                        names.emplace_back(std::move(name));
                    }
                    if (entry->NextEntryOffset == 0U)
                    {
                        break;
                    }
                    offset += entry->NextEntryOffset;
                }
            }
        }

        [[nodiscard]]
        auto removeTreeAt(std::wstring const& path, uint32 depth) -> Status
        {
            if (depth > k_maximumRemovalDepth)
            {
                return refuse("a confined tree is deeper than its ceiling");
            }
            UF_TRY_VALUE(opened, openNode(path, k_removalAccess));
            if (!opened)
            {
                return ok();
            }
            if (opened->directory)
            {
                UF_TRY_VALUE(names, childWideNames(opened->handle));
                for (auto const& name : names)
                {
                    auto child = path;
                    child += L'\\';
                    child += name;
                    UF_TRY(removeTreeAt(child, depth + 1U));
                }
            }

            // The handle already names the object the attribute check passed,
            // so the removal cannot be pointed at a different one. It takes
            // effect when the handle closes at the end of this call, which is
            // why a parent only marks itself once every child call returned.
            auto disposition = FILE_DISPOSITION_INFO{};
            disposition.DeleteFile = TRUE;
            // SAFETY: opened->handle carries DELETE access and disposition is a
            // local of exactly the size the information class declares.
            auto const marked = ::SetFileInformationByHandle(
                opened->handle.get(),
                FileDispositionInfo,
                &disposition,
                static_cast<DWORD>(sizeof(disposition))
            );
            if (marked == 0)
            {
                return ioFailure("cannot remove a confined path");
            }
            return ok();
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

        struct DirectoryStreamCloser final
        {
            auto operator()(DIR* stream) const noexcept -> void
            {
                static_cast<void>(::closedir(stream));
            }
        };

        using DirectoryStream = std::unique_ptr<DIR, DirectoryStreamCloser>;

        [[nodiscard]]
        auto childNamesAt(int directory) -> Result<std::vector<std::string>>
        {
            // SAFETY: fdopendir takes ownership of the descriptor it is given,
            // so it gets a duplicate and the caller's directory descriptor
            // stays valid after the stream closes.
            auto const duplicate = ::dup(directory);
            if (duplicate < 0)
            {
                return ioFailure("cannot enumerate a confined directory");
            }
            auto stream = DirectoryStream{::fdopendir(duplicate)};
            if (stream == nullptr)
            {
                // SAFETY: the duplicate is still ours because fdopendir failed.
                static_cast<void>(::close(duplicate));
                return ioFailure("cannot enumerate a confined directory");
            }

            auto names = std::vector<std::string>{};
            for (
                auto const* entry = ::readdir(stream.get());
                entry != nullptr;
                entry = ::readdir(stream.get())
            )
            {
                // SAFETY: d_name is a NUL-terminated name the stream owns until
                // the next readdir, and its array bound is not its length, so
                // the contract that ends it is the terminator rather than a
                // count. The cast states that decay instead of leaving it
                // implicit; the string copies out before the stream advances.
                auto name = std::string{static_cast<char const*>(entry->d_name)};
                if (name == "." || name == "..")
                {
                    continue;
                }
                names.emplace_back(std::move(name));
            }
            return names;
        }

        [[nodiscard]]
        auto removeTreeAt(
            int parent,
            std::string const& name,
            uint32 depth
        ) -> Status
        {
            if (depth > k_maximumRemovalDepth)
            {
                return refuse("a confined tree is deeper than its ceiling");
            }

            // Declared rather than `auto metadata = ...{}`: `stat` names a
            // function as well as the struct, so ordinary lookup finds the
            // function and the type is reachable only through its elaborated
            // form.
            struct stat metadata{};
            // SAFETY: parent is a directory descriptor owned by the caller and
            // name is null-terminated for the duration of the call.
            if (::fstatat(parent, name.c_str(), &metadata, AT_SYMLINK_NOFOLLOW) != 0)
            {
                if (errno == ENOENT)
                {
                    return ok();
                }
                return ioFailure("cannot inspect a confined path");
            }
            if (S_ISLNK(metadata.st_mode))
            {
                return refuse("a confined path is a link");
            }
            if (S_ISDIR(metadata.st_mode))
            {
                // SAFETY: parent is owned by the caller, and O_NOFOLLOW makes a
                // link at this component fail rather than resolve.
                auto child = Descriptor{
                    ::openat(
                        parent,
                        name.c_str(),
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC
                    )
                };
                if (!child.valid())
                {
                    return refuse("a confined directory is missing or is a link");
                }
                UF_TRY_VALUE(names, childNamesAt(child.get()));
                for (auto const& entry : names)
                {
                    UF_TRY(removeTreeAt(child.get(), entry, depth + 1U));
                }
                // SAFETY: parent is owned by the caller and name is
                // null-terminated for the duration of the call.
                if (::unlinkat(parent, name.c_str(), AT_REMOVEDIR) != 0)
                {
                    return ioFailure("cannot remove a confined directory");
                }
                return ok();
            }

            // SAFETY: parent is owned by the caller and name is null-terminated
            // for the duration of the call; unlinkat never follows a link.
            if (::unlinkat(parent, name.c_str(), 0) != 0)
            {
                return ioFailure("cannot remove a confined file");
            }
            return ok();
        }
#endif
    }

#if defined(_WIN32)
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

        // Every operation starts here: the name must still reach the object
        // this root was opened on, or a rename above it would silently move the
        // whole walk.
        [[nodiscard]]
        auto verifyAnchor() const -> Status
        {
            UF_TRY_VALUE(anchor, openNoFollow(rootPath, OpenKind::Directory));
            UF_TRY_VALUE(identity, identityOf(anchor));
            if (
                identity.volumeSerial != volumeSerial
                || identity.fileIndex != fileIndex
            )
            {
                return refuse(
                    "the confined root no longer resolves to the directory it was opened on"
                );
            }
            return ok();
        }
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
        UF_TRY_VALUE(identity, identityOf(handle));
        auto implementation = std::make_unique<Impl>();
        implementation->rootPath     = native;
        implementation->root         = std::move(handle);
        implementation->volumeSerial = identity.volumeSerial;
        implementation->fileIndex    = identity.fileIndex;
        return ConfinedRoot{std::move(implementation)};
    }

    auto ConfinedRoot::readFile(
        std::string_view relativeText,
        std::size_t maximumBytes
    ) const -> Result<std::vector<std::byte>>
    {
        UF_TRY_VALUE(parts, components(relativeText));
        UF_TRY(m_impl->verifyAnchor());

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
        UF_TRY(m_impl->verifyAnchor());

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

    auto ConfinedRoot::childNames() const -> Result<std::vector<std::string>>
    {
        UF_TRY(m_impl->verifyAnchor());
        UF_TRY_VALUE(wide, childWideNames(m_impl->root));
        auto names = std::vector<std::string>{};
        names.reserve(wide.size());
        for (auto const& value : wide)
        {
            auto const encoded = std::filesystem::path{value}.u8string();
            names.emplace_back(encoded.begin(), encoded.end());
        }
        return names;
    }

    auto ConfinedRoot::removeTree(std::string_view name) const -> Status
    {
        UF_TRY(requireChildName(name));
        UF_TRY(m_impl->verifyAnchor());

        // The name came back out of childNames as UTF-8, so it goes back in as
        // UTF-8; letting path interpret it as the active code page would break
        // the round trip for exactly the names an attacker chooses.
        auto decoded = std::u8string{};
        decoded.reserve(name.size());
        for (auto const value : name)
        {
            decoded.push_back(static_cast<char8_t>(value));
        }
        auto path = m_impl->rootPath;
        path += L'\\';
        path += std::filesystem::path{decoded}.native();
        return removeTreeAt(path, 0U);
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

        auto bytes     = std::vector<std::byte>(*total);
        auto remaining = std::span<std::byte>{bytes};
        while (!remaining.empty())
        {
            // SAFETY: the count offered is the size of what is left, so the
            // descriptor can only fill inside it; a return outside
            // (0, remaining.size()] is refused rather than stepped past.
            auto const read = ::read(file.get(), remaining.data(), remaining.size());
            if (read <= 0 || static_cast<std::size_t>(read) > remaining.size())
            {
                return ioFailure("cannot read a confined file");
            }
            remaining = remaining.subspan(static_cast<std::size_t>(read));
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

        auto remaining = bytes;
        while (!remaining.empty())
        {
            // SAFETY: the count offered is the size of what is left, so the
            // descriptor can only consume inside it; a return outside
            // (0, remaining.size()] is refused rather than stepped past.
            auto const wrote = ::write(file.get(), remaining.data(), remaining.size());
            if (wrote <= 0 || static_cast<std::size_t>(wrote) > remaining.size())
            {
                return ioFailure("cannot write a confined file");
            }
            remaining = remaining.subspan(static_cast<std::size_t>(wrote));
        }
        if (::fsync(file.get()) != 0)
        {
            return ioFailure("cannot flush a confined file");
        }
        return ok();
    }

    auto ConfinedRoot::childNames() const -> Result<std::vector<std::string>>
    {
        return childNamesAt(m_impl->root.get());
    }

    auto ConfinedRoot::removeTree(std::string_view name) const -> Status
    {
        UF_TRY(requireChildName(name));
        return removeTreeAt(m_impl->root.get(), std::string{name}, 0U);
    }
#endif
}
