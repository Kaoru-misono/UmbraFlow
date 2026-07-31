#pragma once

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace uf::input_agent
{
    // How far into its queue an input-agent has already gone. A command counts
    // as consumed only once it has run against the target and been answered in
    // the results file, so this position never runs ahead of the results, and a
    // restart that resumes from it re-delivers nothing an operator already saw.
    struct InputAgentQueuePosition final
    {
        uintmax consumedBytes{};
        uintmax consumedCommands{};

        auto operator==(InputAgentQueuePosition const&) const -> bool = default;
    };

    // A cursor file exactly as it reads on disk. The queue path travels with
    // the position because a cursor sitting beside a different queue has to be
    // refused rather than believed: its byte offset would seek into a file it
    // never described and silently skip whatever precedes it.
    struct InputAgentQueueCursorRecord final
    {
        std::filesystem::path   queue{};
        InputAgentQueuePosition position{};

        auto operator==(InputAgentQueueCursorRecord const&) const
            -> bool = default;
    };

    // What a queue file already holds. framedBytes ends just past the last
    // newline, so it is the only position a reader may resume from: starting
    // mid-line would splice half a command onto the next append.
    struct InputAgentQueueExtent final
    {
        uintmax framedBytes{};
        uintmax framedCommands{};
        uintmax totalBytes{};

        auto operator==(InputAgentQueueExtent const&) const -> bool = default;
    };

    // Where an agent begins when no cursor exists yet and the queue is not
    // empty. There is no safe guess there: the commands already in the file
    // were either meant for the agent that died or written for this one, and
    // acting on the wrong reading either replays history against a live target
    // or silently drops a batch. Refuse is first so a default-initialized
    // policy asks instead of guessing.
    enum class InputAgentQueueStart : uint8
    {
        Refuse,
        Beginning,
        End,
    };

    // The cursor file for a queue, named after it so the pairing is legible in
    // a directory an operator reads by hand.
    [[nodiscard]]
    auto inputAgentQueueCursorPath(
        std::filesystem::path const& canonicalQueue
    ) -> std::filesystem::path;

    [[nodiscard]]
    auto serializeInputAgentQueueCursor(
        InputAgentQueueCursorRecord const& record
    ) -> std::string;

    // Rejects every partial file: a cursor is rewritten whole, so a text that
    // lacks a field or its closing newline was cut short by a dying agent and
    // says nothing trustworthy about what already ran.
    [[nodiscard]]
    auto parseInputAgentQueueCursor(
        std::string_view text,
        std::filesystem::path const& path
    ) -> Result<InputAgentQueueCursorRecord>;

    // Absent means no agent has recorded a position for this queue yet, which
    // is the only case a caller may treat as a fresh start. A cursor that names
    // another queue, or that does not parse, is a failure rather than an
    // absence.
    [[nodiscard]]
    auto readInputAgentQueueCursor(
        std::filesystem::path const& path,
        std::filesystem::path const& canonicalQueue
    ) -> Result<std::optional<InputAgentQueuePosition>>;

    [[nodiscard]]
    auto measureInputAgentQueue(
        std::filesystem::path const& queue
    ) -> Result<InputAgentQueueExtent>;

    // The one decision that keeps a restart from replaying: a recorded position
    // always wins, an empty queue always starts at zero, and anything else
    // needs the operator to say which reading is right.
    [[nodiscard]]
    auto resolveInputAgentQueueStart(
        std::optional<InputAgentQueuePosition> const& recorded,
        InputAgentQueueExtent const& extent,
        InputAgentQueueStart start
    ) -> Result<InputAgentQueuePosition>;

    // Owns the cursor file for one agent run. Rewriting the whole file on every
    // advance keeps the durable state a single self-describing document, and
    // leaves it unlocked between commands so the directory stays inspectable.
    class InputAgentQueueCursor final
    {
        std::filesystem::path   m_path;
        std::filesystem::path   m_queue;
        InputAgentQueuePosition m_position;

        InputAgentQueueCursor(
            std::filesystem::path path,
            std::filesystem::path queue,
            InputAgentQueuePosition position
        ) noexcept;

    public:
        InputAgentQueueCursor(InputAgentQueueCursor const&) = delete;
        auto operator=(InputAgentQueueCursor const&)
            -> InputAgentQueueCursor& = delete;
        InputAgentQueueCursor(InputAgentQueueCursor&&) noexcept = default;
        auto operator=(InputAgentQueueCursor&&) noexcept
            -> InputAgentQueueCursor& = default;
        ~InputAgentQueueCursor() = default;

        // Publishes the starting position before the first command runs, so an
        // agent that dies having done nothing still leaves an unambiguous file
        // behind instead of the absence that forces the next start to ask.
        [[nodiscard]]
        static auto open(
            std::filesystem::path path,
            std::filesystem::path canonicalQueue,
            InputAgentQueuePosition start
        ) -> Result<InputAgentQueueCursor>;

        [[nodiscard]]
        auto position() const noexcept -> InputAgentQueuePosition;

        // Records that the command ending at consumedBytes has run and been
        // answered. Callers advance after writing the results line, so a hard
        // kill in between costs a replay of that one command instead of the
        // whole queue.
        [[nodiscard]]
        auto advance(uintmax consumedBytes) -> Status;
    };
}
