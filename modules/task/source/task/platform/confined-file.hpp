#pragma once

#include <core/error/result.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace uf::task_platform
{
    // A directory tree opened so that nothing can change what is read between
    // the moment a path is checked and the moment it is opened.
    //
    // Verifying bytes by hash defeats substitution but not confinement: a path
    // that is inspected and then opened by name is resolved twice, and only the
    // second resolution decides what the process actually reads. This type
    // resolves once. The root handle is held for the object's lifetime, every
    // directory on the way to a file is opened and held while that file is
    // opened, and no component is ever traversed through a reparse point --
    // checked by attribute rather than by tag, so a junction, an AppExecLink
    // and a cloud placeholder are refused alike.
    //
    // Handles are opened without delete sharing, so while one is held its
    // directory cannot be renamed or removed and the prefix cannot be swapped
    // underneath a later component.
    class ConfinedRoot final
    {
        class Impl;

        std::unique_ptr<Impl> m_impl;

        explicit ConfinedRoot(std::unique_ptr<Impl> implementation) noexcept;

    public:
        ConfinedRoot(ConfinedRoot&&) noexcept;
        auto operator=(ConfinedRoot&&) noexcept -> ConfinedRoot&;
        ConfinedRoot(ConfinedRoot const&) = delete;
        auto operator=(ConfinedRoot const&) -> ConfinedRoot& = delete;
        ~ConfinedRoot();

        [[nodiscard]]
        static auto open(
            std::filesystem::path const& root
        ) -> Result<ConfinedRoot>;

        // relativeText must already carry a validated manifest spelling:
        // forward slashes, and no empty, '.' or '..' component. Refusing those
        // is the caller's job because the caller knows what a manifest may say;
        // this type only guarantees that what it opens is what it checked.
        [[nodiscard]]
        auto readFile(
            std::string_view relativeText,
            std::size_t maximumBytes
        ) const -> Result<std::vector<std::byte>>;

        // Creates a file that must not already exist. Staging a deployment
        // needs the same confinement as reading one: a link planted at a
        // directory the installer creates would otherwise redirect the write,
        // and create-new means a link planted at the leaf fails outright
        // rather than being followed.
        [[nodiscard]]
        auto writeNewFile(
            std::string_view relativeText,
            std::span<std::byte const> bytes
        ) const -> Status;
    };
}
