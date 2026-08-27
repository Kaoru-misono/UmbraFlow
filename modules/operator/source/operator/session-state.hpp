#pragma once

#include "project-plugin.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace uf::operator_runtime
{
    // What one session remembers between the chunks that run inside it: a
    // bounded map from a caller-chosen name to the exact canonical bytes that
    // caller stored under it.
    //
    // IT INTERPRETS NOTHING. A name is an opaque string and a value is a
    // CanonicalJson this store never reads into, so the last write under a name
    // is returned byte for byte and nothing here folds, compares or versions
    // it. That is the same line
    // docs/decisions/2026-08-23-the-framework-stops-interpreting-project-state.md
    // draws: how a Project's state evolves is the Project's decision, and a
    // map that overwrites is the whole of what the framework offers it.
    //
    // LIFETIME. One store belongs to one Operator session and is held by the
    // object that mints that session's identity, so it is born and dies with
    // the session id its entries were written under. It is memory and never a
    // file: nothing here outlives the process, and a later session starts empty
    // whatever the earlier one stored.
    //
    // THE TWO CEILINGS ARE THE FRAMEWORK'S OWN. A session store is memory a
    // session holds for as long as it holds a target, so an unbounded one is a
    // leak with a Tool in front of it. store() refuses past either ceiling and
    // names what was exceeded and what the limit was, which is what
    // docs/decisions/2026-08-23-the-framework-enforces-limits-it-was-given.md
    // requires of every limit the framework does enforce.
    //
    // NOT thread-safe: every verb runs on the session's own thread.
    class SessionStateStore final
    {
    public:
        // Named entries, and total stored bytes across them. Both are counted
        // against the caller's own material -- a name and the canonical bytes
        // stored under it -- rather than against an allocator reading, because
        // a ceiling a caller cannot compute for itself is a ceiling it cannot
        // stay under.
        static constexpr auto k_maximumEntries = std::size_t{256U};
        static constexpr auto k_maximumBytes   = std::size_t{1'048'576U};

    private:
        // Transparent comparison so a lookup spells its name as the view the
        // caller already holds, rather than building a std::string to throw
        // away.
        std::map<std::string, CanonicalJson, std::less<>> m_entries{};

        std::size_t m_storedBytes{};

    public:
        [[nodiscard]] auto storedBytes() const noexcept -> std::size_t;

        // Every name this session has stored, in UTF-8 order.
        [[nodiscard]] auto names() const -> std::vector<std::string>;

        // The bytes stored under `name`, or nullptr when this session stored
        // none. The borrow is this store's and lives until the next store()
        // over the same name; a caller that keeps it must copy.
        [[nodiscard]]
        auto find(std::string_view name) const UF_LIFETIME_BOUND
            -> CanonicalJson const*;

        // Last write wins. A refusal leaves the store exactly as it was, so a
        // caller that met a ceiling still holds everything it stored before.
        [[nodiscard]] auto store(std::string name, CanonicalJson value) -> Status;
    };
}
