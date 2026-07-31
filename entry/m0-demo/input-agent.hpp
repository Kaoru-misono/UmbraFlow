#pragma once

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace uf::m0_demo
{
    class InputAgentQueueReader final
    {
    public:
        // One framed command together with the queue position that command
        // occupies. consumedBytes ends just past its newline, so recording it
        // once the command has run is exactly what a restart must not undo.
        struct Entry final
        {
            std::string text{};
            uintmax     consumedBytes{};

            auto operator==(Entry const&) const -> bool = default;
        };

    private:
        std::filesystem::path m_path;

        uintmax     m_offset;
        std::string m_pending{};

        InputAgentQueueReader(
            std::filesystem::path path,
            uintmax startBytes
        ) noexcept;

        [[nodiscard]]
        auto extractEntries() -> Result<std::vector<Entry>>;

    public:
        InputAgentQueueReader(InputAgentQueueReader const&) = delete;
        auto operator=(InputAgentQueueReader const&)
            -> InputAgentQueueReader& = delete;
        InputAgentQueueReader(InputAgentQueueReader&&) noexcept = default;
        auto operator=(InputAgentQueueReader&&) noexcept
            -> InputAgentQueueReader& = default;
        ~InputAgentQueueReader() = default;

        // startBytes is stated rather than defaulted: where a reader begins
        // decides whether an operator's history is delivered a second time, so
        // no caller gets to leave it unsaid.
        [[nodiscard]]
        static auto create(
            std::filesystem::path path,
            uintmax startBytes
        ) -> Result<InputAgentQueueReader>;

        [[nodiscard]]
        auto readAvailable() -> Result<std::vector<Entry>>;
    };

    // The composition root of one agent run: it validates the arguments and the
    // three IPC paths, resolves the window, and hands a WindowInputAgentDrive to
    // an AnnotationSession for runInputAgentQueueLoop to serve. It decides
    // nothing about a command; every such rule lives in one of those three.
    [[nodiscard]]
    auto runInputAgent(
        std::span<std::string const> raw
    ) -> Status;
}
