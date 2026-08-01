#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace uf::task
{
    // The largest project file the host will read or write in one call.
    //
    // A primitive has to be bounded, and this is the bound for the two that
    // touch the disk: without it a script could ask the host to hold an
    // arbitrary file in memory, and a run's memory ceiling would be a property
    // of whatever happens to be in the project directory. Eight mebibytes is far
    // above any page model a project writes -- the largest authored model this
    // repository has ever produced is measured in tens of kilobytes -- and
    // far below the frame budget a run already lives inside.
    inline constexpr auto k_maximumProjectFileBytes = std::size_t{8} * 1024U * 1024U;

    // Reads and writes files inside ONE project directory, and refuses every
    // name that would leave it.
    //
    // WHY THIS EXISTS AT ALL. The page model moved into the trusted Luau layer,
    // and a model that cannot be written down is not a model. So layer one has
    // to hand the script layer a way to reach its own project's bytes -- and the
    // moment it does, "which bytes" becomes an argument a script supplies. The
    // confinement below is the whole of what stops that argument from naming the
    // rest of the disk. It has the shape the retired input agent's output
    // confinement had, restated here rather than shared, because the two had
    // opposite freshness rules: an agent capture had to be a file that did not
    // exist yet, while a project file is rewritten in place every time the model
    // changes.
    //
    // The root is stored, not the canonical form of it, and every call
    // canonicalises afresh. A generation outlives the individual calls, and a
    // directory replaced by a symbolic link between two of them must be caught
    // by the second call rather than trusted because the first one passed.
    class ProjectFileStore final
    {
        std::filesystem::path m_root;

    public:
        explicit ProjectFileStore(std::filesystem::path root) noexcept;

        [[nodiscard]]
        auto root() const noexcept UF_LIFETIME_BOUND -> std::filesystem::path const&;

        // The absolute path `name` denotes inside this store, or the refusal that
        // stops it.
        //
        // `name` is a project-relative name: it must not be empty, must not be
        // absolute or drive-relative, must contain no parent traversal, and must
        // not name an alternate data stream. Its parent directory must already
        // exist and must canonicalise to a directory inside the root, which is
        // what closes the symbolic-link route out of the project.
        //
        // Creating missing directories is deliberately NOT part of this: a name
        // whose parent does not exist is a typo far more often than it is an
        // intent to lay out a new tree, and a store that silently created
        // directories would turn that typo into litter nothing later reads.
        [[nodiscard]]
        auto resolve(std::string_view name) const -> Result<std::filesystem::path>;

        [[nodiscard]]
        auto read(std::string_view name) const -> Result<std::vector<std::byte>>;

        [[nodiscard]]
        auto write(
            std::string_view name,
            std::span<std::byte const> bytes
        ) const -> Status;
    };
}
