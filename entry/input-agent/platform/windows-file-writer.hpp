#pragma once

#include <core/error/result.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>

namespace uf::input_agent::platform
{
    class FileWriter final
    {
        struct State;

        std::unique_ptr<State> m_state;

        explicit FileWriter(std::unique_ptr<State> p_state) noexcept;

    public:
        FileWriter(FileWriter const&) = delete;
        auto operator=(FileWriter const&) -> FileWriter& = delete;
        FileWriter(FileWriter&&) noexcept;
        auto operator=(FileWriter&&) noexcept -> FileWriter&;
        ~FileWriter();

        [[nodiscard]]
        static auto createExclusive(
            std::filesystem::path const& path,
            std::filesystem::path const& canonicalOutputDirectory
        ) -> Result<FileWriter>;

        [[nodiscard]]
        static auto openAppend(
            std::filesystem::path const& path
        ) -> Result<FileWriter>;

        // Replaces the whole file, for the small self-describing documents the
        // agent rewrites rather than appends to. Unlike createExclusive this
        // deliberately accepts an existing path and is confined to nothing, so
        // callers own the decision that the path is theirs to overwrite.
        [[nodiscard]]
        static auto createOrReplace(
            std::filesystem::path const& path
        ) -> Result<FileWriter>;

        [[nodiscard]]
        auto write(std::span<std::byte const> bytes) -> Status;

        [[nodiscard]] auto flushDurably() -> Status;
    };
}
