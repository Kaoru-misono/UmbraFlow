#pragma once

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace uf::cli
{
    // The queue-and-results plumbing every line-driven front-end runs on:
    // `drive` reads operator commands, `explore` reads agent chunks, and a
    // difference between the two readers is a difference in which lines reach a
    // live target.

    [[nodiscard]]
    auto invalid(std::string message) -> std::unexpected<Error>;

    [[nodiscard]]
    auto pathFailure(
        std::string_view operation,
        std::filesystem::path const& path,
        std::error_code error
    ) -> std::unexpected<Error>;

    [[nodiscard]]
    auto canonicalize(
        std::filesystem::path const& path,
        std::string_view role
    ) -> Result<std::filesystem::path>;

    // What a front-end calls its queue file and itself, as a reader's refusals
    // spell them: `{.queue = "command queue", .session = "a drive session"}`.
    // They are supplied rather than inferred because they are the whole
    // difference between the operator's queue and the agent's.
    struct QueueNaming final
    {
        std::string_view queue{};
        std::string_view session{};
    };

    // `endOffset` is just past the terminator. It travels with the text because
    // that is what a cursor advances to; deriving it again in the loop is where
    // an off-by-one would replay a line.
    struct FramedLine final
    {
        std::string line{};
        uintmax     endOffset{};
    };

    // Reads whole lines appended since the last call, starting from the offset it
    // was built with. A partial trailing line is held back until its terminator
    // arrives -- a line appended in two writes must not have half of it executed
    // -- and a line is handed out exactly once however often the queue is polled.
    class QueueReader final
    {
        std::filesystem::path m_path;
        std::string           m_queueNoun;
        std::string           m_sessionNoun;

        uintmax     m_offset;
        std::string m_pending{};

    public:
        // `startOffset` carries no default: it is where the durable cursor puts
        // the session, and a caller that omitted it would replay every line
        // already in the queue against a live target.
        QueueReader(
            std::filesystem::path path,
            QueueNaming naming,
            uintmax startOffset
        );

        [[nodiscard]] auto readAvailable() -> Result<std::vector<FramedLine>>;
    };

    // Appends one result line per queue line and flushes after each, so whoever
    // reads the file sees a line's outcome before the next one runs.
    class ResultWriter final
    {
        std::ofstream m_stream;
        std::string   m_label;

        ResultWriter(std::ofstream stream, std::string label) noexcept;

    public:
        // `label` names the front-end in an append failure ("drive", "explore").
        [[nodiscard]]
        static auto create(
            std::filesystem::path const& path,
            std::string_view label
        ) -> Result<ResultWriter>;

        [[nodiscard]] auto write(std::string_view line) -> Status;
    };
}
