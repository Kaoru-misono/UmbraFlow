#pragma once

#include <task/template-store.hpp>

#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <engine/session.hpp>

#include <optional>
#include <vector>

namespace uf::task
{
    // What the engine has already answered about the frame ONE observation cycle
    // retains, so the same question asked twice on one frame is asked of the
    // engine once.
    //
    // The question is the verb and the region, plus the template for a search;
    // the frame is the cycle. Nothing here judges or reshapes an answer -- it
    // comes back exactly as the engine gave it, which is what lets a caller be
    // unable to tell a served answer from a fresh one.
    //
    // WHY A REPEAT CANNOT DIFFER, AND WHEN THIS WOULD HAVE TO GO: a cycle retains
    // one frame for its whole life, and matching and reading are deterministic
    // functions of that frame and the region. If either capability ever gains a
    // per-call input the key below does not carry -- a threshold, a language, a
    // model choice -- this stops being sound and must gain that input or be
    // removed.
    //
    // WHAT MAKES IT PAY is that a page model shares elements between pages: five
    // pages of the reference project identify by one page_title element, so
    // resolving every declared page against one screen asks for that one
    // rectangle once per page. Measured over that project's 85 screens, the
    // matrix asked for 6903 reads at 5843 distinct (cycle, rect) questions and
    // 3495 searches at 2295 distinct ones.
    //
    // ONLY A COMPLETED ANSWER IS REMEMBERED. A refusal -- an exhausted budget, a
    // search stopped by its comparison ceiling, an adapter that would not read --
    // established nothing about the screen, so there is nothing to serve back and
    // the next caller reaches the engine exactly as it would have.
    //
    // The cycle ordinal is carried on every call rather than latched by a
    // separate step, so freshness is not a discipline any caller can forget: an
    // answer remembered under another ordinal is never served, and remembering
    // under a new one drops everything from the old. Ordinals are minted
    // monotonically by one CycleLedger and never reused, and a ticket from
    // another generation is refused before this is reached, so "same ordinal" is
    // exactly "same frame".
    //
    // NOT thread-safe: every method runs on the VM's owning thread.
    class CycleAnswers final
    {
        // No in-class initializer for the rect: PixelRect has no default state.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        struct TextEntry final
        {
            PixelRect rect;

            std::optional<engine::TextReading> reading{};
        };

        struct LinesEntry final
        {
            PixelRect rect;

            std::vector<engine::TextReading> lines{};
        };

        // No in-class initializer for the ROI: PixelRect has no default state.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        struct MatchEntry final
        {
            TemplateTicket templateTicket{};

            PixelRect searchRoi;

            std::optional<engine::MatchFound> found{};
        };

        // Which cycle every entry below belongs to. Zero names no cycle: a ledger
        // mints its first ordinal as one.
        uint64 m_cycleOrdinal{};

        // Linear lookup, and it stays that way while the bound is one screen's
        // distinct regions -- 76 at the widest over the reference project, against
        // an OCR read that costs 2-13 ms.
        std::vector<TextEntry>  m_texts{};
        std::vector<LinesEntry> m_lineBlocks{};
        std::vector<MatchEntry> m_matches{};

        // Drops every answer from an earlier cycle. Called by each remember, which
        // is the only operation that can be the first of a new cycle.
        auto retainOnly(uint64 cycleOrdinal) -> void;

    public:
        // The answer this cycle already has, or null when it has none. A non-null
        // result may still hold an empty optional or an empty list: "this frame
        // reads no text there" is an answer and is served like any other.
        //
        // The borrow lasts until the next remember on this cache.
        [[nodiscard]]
        auto findText(
            uint64 cycleOrdinal,
            PixelRect rect
        ) const noexcept UF_LIFETIME_BOUND
            -> std::optional<engine::TextReading> const*;

        // cycle_read_lines' own table. A block read and a single-line read over
        // one rectangle are two questions with two answer shapes, and the engine
        // runs a different pipeline for each, so they never share an entry.
        [[nodiscard]]
        auto findLines(
            uint64 cycleOrdinal,
            PixelRect rect
        ) const noexcept UF_LIFETIME_BOUND
            -> std::vector<engine::TextReading> const*;

        [[nodiscard]]
        auto findMatch(
            uint64 cycleOrdinal,
            TemplateTicket templateTicket,
            PixelRect searchRoi
        ) const noexcept UF_LIFETIME_BOUND
            -> std::optional<engine::MatchFound> const*;

        // Records what the engine answered. The answer is copied rather than moved
        // in because the caller returns the same value it just received, and one
        // reading is a short string beside the inference that produced it.
        auto rememberText(
            uint64 cycleOrdinal,
            PixelRect rect,
            std::optional<engine::TextReading> const& reading
        ) -> void;

        auto rememberLines(
            uint64 cycleOrdinal,
            PixelRect rect,
            std::vector<engine::TextReading> const& lines
        ) -> void;

        auto rememberMatch(
            uint64 cycleOrdinal,
            TemplateTicket templateTicket,
            PixelRect searchRoi,
            std::optional<engine::MatchFound> const& found
        ) -> void;
    };
}
