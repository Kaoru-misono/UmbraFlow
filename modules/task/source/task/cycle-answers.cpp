#include <task/cycle-answers.hpp>

#include <task/template-store.hpp>

#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <engine/session.hpp>

#include <algorithm>
#include <optional>
#include <vector>

namespace uf::task
{
    auto CycleAnswers::retainOnly(uint64 cycleOrdinal) -> void
    {
        if (m_cycleOrdinal == cycleOrdinal)
        {
            return;
        }
        m_texts.clear();
        m_lineBlocks.clear();
        m_matches.clear();
        m_cycleOrdinal = cycleOrdinal;
    }

    auto CycleAnswers::findText(
        uint64 cycleOrdinal,
        PixelRect rect
    ) const noexcept -> std::optional<engine::TextReading> const*
    {
        if (cycleOrdinal != m_cycleOrdinal)
        {
            return nullptr;
        }
        auto const found = std::ranges::find_if(
            m_texts,
            [rect](TextEntry const& entry) noexcept
            {
                return entry.rect == rect;
            }
        );
        return found == m_texts.end() ? nullptr : &found->reading;
    }

    auto CycleAnswers::findLines(
        uint64 cycleOrdinal,
        PixelRect rect
    ) const noexcept -> std::vector<engine::TextReading> const*
    {
        if (cycleOrdinal != m_cycleOrdinal)
        {
            return nullptr;
        }
        auto const found = std::ranges::find_if(
            m_lineBlocks,
            [rect](LinesEntry const& entry) noexcept
            {
                return entry.rect == rect;
            }
        );
        return found == m_lineBlocks.end() ? nullptr : &found->lines;
    }

    auto CycleAnswers::findMatch(
        uint64 cycleOrdinal,
        TemplateTicket templateTicket,
        PixelRect searchRoi
    ) const noexcept -> std::optional<engine::MatchFound> const*
    {
        if (cycleOrdinal != m_cycleOrdinal)
        {
            return nullptr;
        }
        auto const found = std::ranges::find_if(
            m_matches,
            [templateTicket, searchRoi](MatchEntry const& entry) noexcept
            {
                return (
                    entry.templateTicket == templateTicket
                    && entry.searchRoi == searchRoi
                );
            }
        );
        return found == m_matches.end() ? nullptr : &found->found;
    }

    auto CycleAnswers::rememberText(
        uint64 cycleOrdinal,
        PixelRect rect,
        std::optional<engine::TextReading> const& reading
    ) -> void
    {
        retainOnly(cycleOrdinal);
        m_texts.emplace_back(
            TextEntry{
                .rect    = rect,
                .reading = reading,
            }
        );
    }

    auto CycleAnswers::rememberLines(
        uint64 cycleOrdinal,
        PixelRect rect,
        std::vector<engine::TextReading> const& lines
    ) -> void
    {
        retainOnly(cycleOrdinal);
        m_lineBlocks.emplace_back(
            LinesEntry{
                .rect  = rect,
                .lines = lines,
            }
        );
    }

    auto CycleAnswers::rememberMatch(
        uint64 cycleOrdinal,
        TemplateTicket templateTicket,
        PixelRect searchRoi,
        std::optional<engine::MatchFound> const& found
    ) -> void
    {
        retainOnly(cycleOrdinal);
        m_matches.emplace_back(
            MatchEntry{
                .templateTicket = templateTicket,
                .searchRoi      = searchRoi,
                .found          = found,
            }
        );
    }
}
