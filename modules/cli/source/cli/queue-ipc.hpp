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
    // The queue-and-results plumbing every line-driven front-end runs on.
    // `explore` is the only one left: the second reader, `drive`, was retired on
    // 2026-08-03 in `eafc273`. The plumbing stays front-end-agnostic anyway,
    // because what a reader is allowed to reach with a line is the front-end's
    // property and not this file's.

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
    // spell them: `{.queue = "chunk queue", .session = "an exploration
    // session"}` is the one caller, in explore.cpp. They are supplied rather
    // than inferred because a refusal has to name the queue the reader was
    // pointed at, and this file does not know which front-end pointed it.
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

        // NOT noexcept: std::ofstream's move constructor is not noexcept -- it
        // moves a basic_filebuf, which carries a locale and a buffer -- so this
        // promised something the standard library does not.
        ResultWriter(std::ofstream stream, std::string label);

    public:
        // `label` names the front-end in an append failure ("explore").
        [[nodiscard]]
        static auto create(
            std::filesystem::path const& path,
            std::string_view label
        ) -> Result<ResultWriter>;

        [[nodiscard]] auto write(std::string_view line) -> Status;
    };
}
