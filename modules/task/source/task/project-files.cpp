#include "project-files.hpp"

#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::task
{
    namespace
    {
        [[nodiscard]]
        auto pathFailure(
            std::string_view operation,
            std::filesystem::path const& path,
            std::error_code error
        ) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::IoFailure,
                std::format(
                    "cannot {} project file {}: {}",
                    operation,
                    path.string(),
                    error.message()
                )
            );
        }

        [[nodiscard]]
        auto refuse(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        // Whether `canonicalPath` is `canonicalDirectory` or sits under it,
        // compared component by component so a prefix that merely shares
        // characters ("project-backup" against "project") is not a match.
        [[nodiscard]]
        auto isWithinDirectory(
            std::filesystem::path const& canonicalPath,
            std::filesystem::path const& canonicalDirectory
        ) -> bool
        {
            auto component = canonicalPath.begin();
            for (auto const& expected : canonicalDirectory)
            {
                if (component == canonicalPath.end() || *component != expected)
                {
                    return false;
                }
                ++component;
            }
            return true;
        }

        // The project-relative spelling of `relative`'s parent directory, which
        // is what a refusal names it by: the caller wrote the name, and telling
        // it about an absolute path it never chose is telling it about this
        // host's disk layout instead of about its own mistake. A name that sits
        // at the project root has no parent component, and "." is what that is.
        [[nodiscard]]
        auto parentSpellingOf(std::filesystem::path const& relative) -> std::string
        {
            auto const parent = relative.parent_path();
            if (parent.empty())
            {
                return std::string{"."};
            }
            return parent.generic_string();
        }
    }

    ProjectFileStore::ProjectFileStore(std::filesystem::path root) noexcept
        : m_root{std::move(root)}
    {
    }

    auto ProjectFileStore::root() const noexcept -> std::filesystem::path const&
    {
        return m_root;
    }

    auto ProjectFileStore::resolve(
        std::string_view name
    ) const -> Result<std::filesystem::path>
    {
        if (m_root.empty())
        {
            return refuse(
                "this generation has no project directory, so it can neither "
                "read nor write project files"
            );
        }
        if (name.empty())
        {
            return refuse("a project file name must not be empty");
        }

        auto const relative = std::filesystem::path{name};
        if (relative.is_absolute() || relative.has_root_path())
        {
            return refuse(
                std::format(
                    "project file name {} must be relative to the project "
                    "directory",
                    name
                )
            );
        }
        for (auto const& component : relative)
        {
            if (component == std::filesystem::path{".."})
            {
                return refuse(
                    std::format(
                        "project file name {} contains forbidden parent traversal",
                        name
                    )
                );
            }
        }

        auto const filename = relative.filename();
        if (
            filename.empty()
            || filename == std::filesystem::path{"."}
            || filename == std::filesystem::path{".."}
        )
        {
            return refuse(
                std::format("project file name {} does not name a file", name)
            );
        }
        if (filename.native().contains(std::filesystem::path::value_type{':'}))
        {
            return refuse(
                std::format(
                    "project file name {} must not name an alternate data stream",
                    name
                )
            );
        }

        auto error         = std::error_code{};
        auto const canonicalRoot = std::filesystem::canonical(m_root, error);
        if (error)
        {
            return pathFailure("canonicalize the project directory of", m_root, error);
        }

        auto const candidate = canonicalRoot / relative;

        // The parent is inspected BEFORE it is canonicalized, because a parent
        // that is not there is what canonicalization fails on -- and its own
        // failure names the wrong fact. "cannot canonicalize the parent of
        // <absolute path>: the system cannot find the path specified" reads as a
        // broken path when what happened is that nobody has laid this directory
        // out yet, and an agent authoring a project from nothing meets it once
        // per directory. So the refusal below says which directory is missing,
        // in the project-relative spelling the caller wrote, and says that this
        // store will not create it.
        auto const parent         = candidate.parent_path();
        auto const parentSpelling = parentSpellingOf(relative);

        error                   = std::error_code{};
        auto const parentStatus = std::filesystem::status(parent, error);
        if (error && error != std::errc::no_such_file_or_directory)
        {
            return pathFailure("inspect the parent directory of", candidate, error);
        }
        if (parentStatus.type() == std::filesystem::file_type::not_found)
        {
            return refuse(
                std::format(
                    "project file name {} has no directory to sit in: \"{}\" does "
                    "not exist inside the project at {}. Proving a write stayed "
                    "inside the project means canonicalizing a parent that is "
                    "really there, so this store creates no directory of its own "
                    "-- the project's skeleton has to be laid out first",
                    name,
                    parentSpelling,
                    canonicalRoot.string()
                )
            );
        }
        if (!std::filesystem::is_directory(parentStatus))
        {
            return refuse(
                std::format(
                    "project file name {} has no directory to sit in: \"{}\" "
                    "exists inside the project at {} and is not a directory",
                    name,
                    parentSpelling,
                    canonicalRoot.string()
                )
            );
        }

        error                      = std::error_code{};
        auto const canonicalParent = std::filesystem::canonical(parent, error);
        if (error)
        {
            return pathFailure("canonicalize the parent of", candidate, error);
        }
        // Asked a second time, on what canonicalization actually resolved to. The
        // check above rules on the parent as it was a moment ago; this rules on
        // the object the write will really open, which is what a directory
        // replaced by a file in between would otherwise slip past.
        if (!std::filesystem::is_directory(canonicalParent, error) || error)
        {
            return refuse(
                std::format(
                    "project file name {} must have an existing directory parent",
                    name
                )
            );
        }
        if (!isWithinDirectory(canonicalParent, canonicalRoot))
        {
            return refuse(
                std::format(
                    "project file name {} escapes the project directory {}",
                    name,
                    canonicalRoot.string()
                )
            );
        }
        return canonicalParent / filename;
    }

    auto ProjectFileStore::read(
        std::string_view name
    ) const -> Result<std::vector<std::byte>>
    {
        UF_TRY_VALUE(path, resolve(name));

        auto error       = std::error_code{};
        auto const status = std::filesystem::symlink_status(path, error);
        if (error)
        {
            return pathFailure("inspect", path, error);
        }
        if (status.type() != std::filesystem::file_type::regular)
        {
            return refuse(
                std::format("project file {} is not a regular file", name)
            );
        }

        auto const size = std::filesystem::file_size(path, error);
        if (error)
        {
            return pathFailure("measure", path, error);
        }
        auto const byteCount = checkedCast<std::size_t>(size);
        if (!byteCount || *byteCount > k_maximumProjectFileBytes)
        {
            return refuse(
                std::format(
                    "project file {} is larger than the host's {} byte ceiling",
                    name,
                    k_maximumProjectFileBytes
                )
            );
        }

        auto stream = std::ifstream{path, std::ios::binary};
        if (!stream)
        {
            return fail(
                AutomationErrorKind::IoFailure,
                std::format("cannot open project file {} for reading", name)
            );
        }
        // Read as characters and widen afterwards. The stream's own interface is
        // char-shaped, and going through a std::string keeps the whole path free
        // of a pointer cast that this module would have no boundary to justify.
        auto contents = std::string(*byteCount, '\0');
        if (*byteCount != 0U)
        {
            stream.read(contents.data(), static_cast<std::streamsize>(*byteCount));
        }
        if (!stream || stream.gcount() != static_cast<std::streamsize>(*byteCount))
        {
            return fail(
                AutomationErrorKind::IoFailure,
                std::format("project file {} could not be read to its end", name)
            );
        }

        auto bytes = std::vector<std::byte>{};
        bytes.reserve(contents.size());
        for (auto const character : contents)
        {
            bytes.emplace_back(
                static_cast<std::byte>(static_cast<unsigned char>(character))
            );
        }
        return bytes;
    }

    auto ProjectFileStore::write(
        std::string_view name,
        std::span<std::byte const> bytes
    ) const -> Status
    {
        if (bytes.size() > k_maximumProjectFileBytes)
        {
            return refuse(
                std::format(
                    "writing {} bytes to project file {} exceeds the host's {} "
                    "byte ceiling",
                    bytes.size(),
                    name,
                    k_maximumProjectFileBytes
                )
            );
        }
        UF_TRY_VALUE(path, resolve(name));

        auto error        = std::error_code{};
        auto const status = std::filesystem::symlink_status(path, error);
        if (error && error != std::errc::no_such_file_or_directory)
        {
            return pathFailure("inspect", path, error);
        }
        if (
            status.type() != std::filesystem::file_type::not_found
            && status.type() != std::filesystem::file_type::regular
        )
        {
            // A symbolic link or a directory already sitting at the resolved
            // name is refused rather than followed: the parent was proven to be
            // inside the project, but the leaf itself has not been, and a link
            // there is the one remaining way out.
            return refuse(
                std::format(
                    "project file {} already exists and is not a regular file",
                    name
                )
            );
        }

        auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
        if (!stream)
        {
            return fail(
                AutomationErrorKind::IoFailure,
                std::format("cannot open project file {} for writing", name)
            );
        }
        if (!bytes.empty())
        {
            // Narrowed into characters for the same reason read widens out of
            // them: the stream speaks char, and a copy of at most the ceiling
            // above buys a path with no pointer cast in it.
            auto contents = std::string{};
            contents.reserve(bytes.size());
            for (auto const value : bytes)
            {
                contents.push_back(static_cast<char>(std::to_integer<uint8>(value)));
            }
            stream.write(
                contents.data(),
                static_cast<std::streamsize>(contents.size())
            );
        }
        stream.flush();
        if (!stream)
        {
            return fail(
                AutomationErrorKind::IoFailure,
                std::format("project file {} could not be written in full", name)
            );
        }
        return ok();
    }
}
