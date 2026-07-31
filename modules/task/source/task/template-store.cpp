#include <task/template-store.hpp>

#include <task/cycle-ledger.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <annotation/content-hash.hpp>
#include <annotation/recognition-runtime.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <span>
#include <utility>

namespace uf::task
{
    TemplateStore::TemplateStore() noexcept
        : m_generation{mintHandleGeneration()}
    {
    }

    auto TemplateStore::size() const noexcept -> std::size_t
    {
        return m_entries.size();
    }

    auto TemplateStore::load(
        std::span<std::byte const> pngBytes
    ) -> Result<TemplateTicket>
    {
        if (pngBytes.empty() || pngBytes.size() > k_maximumTemplateBytes)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "a template blob of {} bytes is empty or beyond the host's "
                    "{} byte ceiling",
                    pngBytes.size(),
                    k_maximumTemplateBytes
                )
            );
        }

        UF_TRY_VALUE(hash, annotation::sha256(pngBytes));
        auto const existing = std::ranges::find_if(
            m_entries,
            [&hash](Entry const& entry) noexcept -> bool
            {
                return entry.image.hash == hash;
            }
        );
        if (existing != m_entries.end())
        {
            return TemplateTicket{
                .generation = m_generation,
                .ordinal    = existing->ordinal,
            };
        }

        if (m_entries.size() >= k_maximumLoadedTemplates)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "this generation already holds the host's maximum of {} "
                    "decoded templates",
                    k_maximumLoadedTemplates
                )
            );
        }

        UF_TRY_VALUE(image, annotation::decodeTemplateImage(pngBytes, hash));
        auto const ordinal = m_nextOrdinal;
        ++m_nextOrdinal;
        m_entries.emplace_back(
            Entry{
                .ordinal = ordinal,
                .image   = std::move(image),
            }
        );
        return TemplateTicket{
            .generation = m_generation,
            .ordinal    = ordinal,
        };
    }

    auto TemplateStore::find(
        TemplateTicket ticket
    ) const noexcept -> annotation::GrayTemplateImage const*
    {
        if (ticket.generation != m_generation)
        {
            return nullptr;
        }
        auto const found = std::ranges::find(
            m_entries,
            ticket.ordinal,
            &Entry::ordinal
        );
        if (found == m_entries.end())
        {
            return nullptr;
        }
        return &found->image;
    }
}
