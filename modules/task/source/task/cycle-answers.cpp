#include <task/cycle-answers.hpp>

#include <task/template-store.hpp>

#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <engine/session.hpp>

#include <ocr/engine.hpp>

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
        m_reads.clear();
        m_matches.clear();
        m_cycleOrdinal = cycleOrdinal;
    }

    auto CycleAnswers::findRead(
        uint64 cycleOrdinal,
        PixelRect rect,
        ocr::TextLayout layout
    ) const noexcept -> std::vector<engine::TextReading> const*
    {
        if (cycleOrdinal != m_cycleOrdinal)
        {
            return nullptr;
        }
        auto const found = std::ranges::find_if(
            m_reads,
            [rect, layout](ReadEntry const& entry) noexcept
            {
                return entry.rect == rect && entry.layout == layout;
            }
        );
        return found == m_reads.end() ? nullptr : &found->lines;
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

    auto CycleAnswers::rememberRead(
        uint64 cycleOrdinal,
        PixelRect rect,
        ocr::TextLayout layout,
        std::vector<engine::TextReading> const& lines
    ) -> void
    {
        retainOnly(cycleOrdinal);
        m_reads.emplace_back(
            ReadEntry{
                .rect   = rect,
                .layout = layout,
                .lines  = lines,
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
