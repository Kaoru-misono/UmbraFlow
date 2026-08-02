#pragma once

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace uf::cli
{
    // How far into its queue an exploration session has already gone. A chunk
    // counts as consumed only once it has RUN and been answered in the results
    // file, so a restart re-runs nothing the agent has been told about -- replaying
    // an agent's queue would re-deliver every click in it against a live target.
    struct ExploreQueuePosition final
    {
        uintmax consumedBytes{};
        uintmax consumedChunks{};

        auto operator==(ExploreQueuePosition const&) const -> bool = default;
    };

    // The queue path travels with the position so a cursor sitting beside a
    // different queue can be refused: its byte offset would seek into a file it
    // never described and silently skip whatever precedes it.
    struct ExploreQueueCursorRecord final
    {
        std::filesystem::path queue{};
        ExploreQueuePosition  position{};

        auto operator==(ExploreQueueCursorRecord const&) const -> bool = default;
    };

    // `framedBytes` ends just past the last newline, so it is the only position a
    // reader may resume from: starting mid-line would splice half a chunk onto the
    // next append.
    struct ExploreQueueExtent final
    {
        uintmax framedBytes{};
        uintmax framedChunks{};
        uintmax totalBytes{};

        auto operator==(ExploreQueueExtent const&) const -> bool = default;
    };

    // The cursor file for a queue, named after it so the pairing is legible in a
    // directory an operator reads by hand.
    [[nodiscard]]
    auto exploreQueueCursorPath(
        std::filesystem::path const& canonicalQueue
    ) -> std::filesystem::path;

    [[nodiscard]]
    auto serializeExploreQueueCursor(
        ExploreQueueCursorRecord const& record
    ) -> std::string;

    // Rejects every partial file: a cursor is rewritten whole, so a text missing a
    // field or its closing newline was cut short by a dying session.
    [[nodiscard]]
    auto parseExploreQueueCursor(
        std::string_view text,
        std::filesystem::path const& path
    ) -> Result<ExploreQueueCursorRecord>;

    // Absent means no session has recorded a position for this queue yet, the only
    // case a caller may treat as a fresh start. A cursor that names another queue,
    // or that does not parse, is a failure rather than an absence.
    [[nodiscard]]
    auto readExploreQueueCursor(
        std::filesystem::path const& path,
        std::filesystem::path const& canonicalQueue
    ) -> Result<std::optional<ExploreQueuePosition>>;

    [[nodiscard]]
    auto measureExploreQueue(
        std::filesystem::path const& queue
    ) -> Result<ExploreQueueExtent>;

    // The one decision that keeps a restart from replaying. A recorded position
    // always wins; with no cursor an empty queue starts at zero. With no cursor and
    // a queue that already holds chunks, guessing would replay history against a
    // live target or silently drop a batch, so it is refused.
    [[nodiscard]]
    auto resolveExploreQueueStart(
        std::optional<ExploreQueuePosition> const& recorded,
        ExploreQueueExtent const& extent,
        std::filesystem::path const& queue
    ) -> Result<ExploreQueuePosition>;

    // Owns the cursor file for one session. Rewriting the whole file on every
    // advance leaves it unlocked between chunks, so the directory stays
    // inspectable.
    class ExploreQueueCursor final
    {
        std::filesystem::path m_path;
        std::filesystem::path m_queue;
        ExploreQueuePosition  m_position;

        ExploreQueueCursor(
            std::filesystem::path path,
            std::filesystem::path queue,
            ExploreQueuePosition position
        ) noexcept;

    public:
        ExploreQueueCursor(ExploreQueueCursor const&) = delete;
        auto operator=(ExploreQueueCursor const&) -> ExploreQueueCursor& = delete;
        ExploreQueueCursor(ExploreQueueCursor&&) noexcept = default;
        auto operator=(ExploreQueueCursor&&) noexcept -> ExploreQueueCursor& = default;
        ~ExploreQueueCursor() = default;

        // Publishes the starting position before the first chunk runs, so a session
        // that dies having done nothing leaves a file rather than the absence that
        // forces the next start to refuse.
        [[nodiscard]]
        static auto open(
            std::filesystem::path path,
            std::filesystem::path canonicalQueue,
            ExploreQueuePosition start
        ) -> Result<ExploreQueueCursor>;

        [[nodiscard]] auto position() const noexcept -> ExploreQueuePosition;

        // Records that the chunk ending at `consumedBytes` has run and been
        // answered. Callers advance AFTER writing the result line, so a hard kill
        // in between costs a replay of that one chunk instead of the whole queue.
        [[nodiscard]] auto advance(uintmax consumedBytes) -> Status;
    };
}
