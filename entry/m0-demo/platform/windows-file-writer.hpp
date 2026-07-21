#pragma once

#include <core/error/result.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>

namespace uf::m0_demo::platform
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

        [[nodiscard]]
        auto write(std::span<std::byte const> bytes) -> Status;

        [[nodiscard]] auto flushDurably() -> Status;
    };
}
