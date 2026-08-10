#pragma once

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace uf::cli
{
    // How far into its queue a session has already gone. A line counts as
    // consumed only once it has RUN and been answered in the results file, so a
    // restart re-runs nothing the other side has been told about -- replaying a
    // queue would re-deliver every click and keystroke in it against a live
    // target.
    struct QueuePosition final
    {
        uintmax consumedBytes{};
        uintmax consumedLines{};

        auto operator==(QueuePosition const&) const -> bool = default;
    };

    // The queue path travels with the position so a cursor sitting beside a
    // different queue can be refused: its byte offset would seek into a file it
    // never described and silently skip whatever precedes it.
    struct QueueCursorRecord final
    {
        std::filesystem::path queue{};
        QueuePosition         position{};

        auto operator==(QueueCursorRecord const&) const -> bool = default;
    };

    // `framedBytes` ends just past the last newline, so it is the only position a
    // reader may resume from: starting mid-line would splice half a line onto the
    // next append.
    struct QueueExtent final
    {
        uintmax framedBytes{};
        uintmax framedLines{};
        uintmax totalBytes{};

        auto operator==(QueueExtent const&) const -> bool = default;
    };

    // The cursor file for a queue, named after it so the pairing is legible in a
    // directory an operator reads by hand.
    [[nodiscard]]
    auto queueCursorPath(
        std::filesystem::path const& canonicalQueue
    ) -> std::filesystem::path;

    [[nodiscard]]
    auto serializeQueueCursor(QueueCursorRecord const& record) -> std::string;

    // Rejects every partial file: a cursor is rewritten whole, so a text missing a
    // field or its closing newline was cut short by a dying session.
    [[nodiscard]]
    auto parseQueueCursor(
        std::string_view text,
        std::filesystem::path const& path
    ) -> Result<QueueCursorRecord>;

    // Absent means no session has recorded a position for this queue yet, the only
    // case a caller may treat as a fresh start. A cursor that names another queue,
    // or that does not parse, is a failure rather than an absence.
    [[nodiscard]]
    auto readQueueCursor(
        std::filesystem::path const& path,
        std::filesystem::path const& canonicalQueue
    ) -> Result<std::optional<QueuePosition>>;

    [[nodiscard]]
    auto measureQueueExtent(
        std::filesystem::path const& queue
    ) -> Result<QueueExtent>;

    // The one decision that keeps a restart from replaying. A recorded position
    // always wins; with no cursor an empty queue starts at zero. With no cursor and
    // a queue that already holds lines, guessing would replay history against a
    // live target or silently drop a batch, so it is refused.
    [[nodiscard]]
    auto resolveQueueStart(
        std::optional<QueuePosition> const& recorded,
        QueueExtent const& extent,
        std::filesystem::path const& queue
    ) -> Result<QueuePosition>;

    // Owns the cursor file for one session. Rewriting the whole file on every
    // advance leaves it unlocked between lines, so the directory stays
    // inspectable.
    class QueueCursor final
    {
        std::filesystem::path m_path;
        std::filesystem::path m_queue;
        QueuePosition         m_position;

        QueueCursor(
            std::filesystem::path path,
            std::filesystem::path queue,
            QueuePosition position
        ) noexcept;

    public:
        QueueCursor(QueueCursor const&) = delete;
        auto operator=(QueueCursor const&) -> QueueCursor& = delete;
        QueueCursor(QueueCursor&&) noexcept = default;
        auto operator=(QueueCursor&&) noexcept -> QueueCursor& = default;
        ~QueueCursor() = default;

        // Publishes the starting position before the first line runs, so a session
        // that dies having done nothing leaves a file rather than the absence that
        // forces the next start to refuse.
        [[nodiscard]]
        static auto open(
            std::filesystem::path path,
            std::filesystem::path canonicalQueue,
            QueuePosition start
        ) -> Result<QueueCursor>;

        // Records that the line ending at `consumedBytes` has run and been
        // answered. Callers advance AFTER writing the result line, so a hard kill
        // in between costs a replay of that one line instead of the whole queue.
        [[nodiscard]] auto advance(uintmax consumedBytes) -> Status;
    };
}
