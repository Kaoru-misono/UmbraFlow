#pragma once

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace uf::cli
{
    // How far into its queue an exploration session has already gone.
    //
    // A chunk counts as consumed only once it has RUN and been answered in the
    // results file, so this position never runs ahead of the answers. A restart
    // that resumes from it re-runs nothing the agent has already been told about
    // -- which is the property that matters here far more than it does for a task
    // run: replaying an agent's queue would re-deliver every click in it against
    // a live target.
    struct ExploreQueuePosition final
    {
        uintmax consumedBytes{};
        uintmax consumedChunks{};

        auto operator==(ExploreQueuePosition const&) const -> bool = default;
    };

    // A cursor file exactly as it reads on disk.
    //
    // The queue path travels with the position because a cursor sitting beside a
    // different queue has to be refused rather than believed: its byte offset
    // would seek into a file it never described and silently skip whatever
    // precedes it. This is the m0-demo input agent's rule, carried over
    // deliberately -- it is the reason that agent could be restarted at all.
    struct ExploreQueueCursorRecord final
    {
        std::filesystem::path queue{};
        ExploreQueuePosition  position{};

        auto operator==(ExploreQueueCursorRecord const&) const -> bool = default;
    };

    // What a queue file already holds. `framedBytes` ends just past the last
    // newline, so it is the only position a reader may resume from: starting
    // mid-line would splice half a chunk onto the next append.
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

    // Rejects every partial file: a cursor is rewritten whole, so a text that
    // lacks a field or its closing newline was cut short by a dying session and
    // says nothing trustworthy about what already ran.
    [[nodiscard]]
    auto parseExploreQueueCursor(
        std::string_view text,
        std::filesystem::path const& path
    ) -> Result<ExploreQueueCursorRecord>;

    // Absent means no session has recorded a position for this queue yet, which
    // is the only case a caller may treat as a fresh start. A cursor that names
    // another queue, or that does not parse, is a failure rather than an absence.
    [[nodiscard]]
    auto readExploreQueueCursor(
        std::filesystem::path const& path,
        std::filesystem::path const& canonicalQueue
    ) -> Result<std::optional<ExploreQueuePosition>>;

    [[nodiscard]]
    auto measureExploreQueue(
        std::filesystem::path const& queue
    ) -> Result<ExploreQueueExtent>;

    // The one decision that keeps a restart from replaying.
    //
    // A recorded position always wins. With no cursor, an EMPTY queue starts at
    // zero -- that is the ordinary first session, where the agent creates the
    // file and then appends to it. With no cursor and a queue that already holds
    // chunks there is no safe reading: those lines were either written for a
    // session that died or for this one, and guessing either way replays history
    // against a live target or silently drops a batch. So it is refused, and the
    // refusal says what to do about it. The input agent solved this with a
    // three-way policy flag; refusing outright needs no flag and is the same
    // fail-closed answer its default already gave.
    [[nodiscard]]
    auto resolveExploreQueueStart(
        std::optional<ExploreQueuePosition> const& recorded,
        ExploreQueueExtent const& extent,
        std::filesystem::path const& queue
    ) -> Result<ExploreQueuePosition>;

    // Owns the cursor file for one session. Rewriting the whole file on every
    // advance keeps the durable state a single self-describing document and
    // leaves it unlocked between chunks, so the directory stays inspectable.
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

        // Publishes the starting position before the first chunk runs, so a
        // session that dies having done nothing still leaves an unambiguous file
        // behind instead of the absence that forces the next start to refuse.
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
