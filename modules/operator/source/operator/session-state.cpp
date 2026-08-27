#include "session-state.hpp"

#include <core/error/contracts.hpp>

#include <domain/error.hpp>

#include <format>
#include <string_view>
#include <utility>

namespace uf::operator_runtime
{
    auto SessionStateStore::storedBytes() const noexcept -> std::size_t
    {
        return m_storedBytes;
    }

    auto SessionStateStore::names() const -> std::vector<std::string>
    {
        auto names = std::vector<std::string>{};
        names.reserve(m_entries.size());
        for (auto const& entry : m_entries)
        {
            names.emplace_back(entry.first);
        }
        return names;
    }

    auto SessionStateStore::find(std::string_view name) const
        -> CanonicalJson const*
    {
        auto const found = m_entries.find(name);
        return found == m_entries.end() ? nullptr : &found->second;
    }

    auto SessionStateStore::store(std::string name, CanonicalJson value)
        -> Status
    {
        auto const found = m_entries.find(std::string_view{name});
        auto const present = found != m_entries.end();
        if (!present && m_entries.size() == k_maximumEntries)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "this session already holds {} state entries, which is its "
                    "ceiling, so {} cannot be stored",
                    k_maximumEntries,
                    name
                )
            );
        }

        // What the store would weigh AFTER the write, so an overwrite is
        // charged for its new bytes alone and a caller that replaces one large
        // entry with a small one is not refused for bytes it is giving back.
        auto const replaced = present
            ? found->first.size() + found->second.bytes().size()
            : std::size_t{0U};
        UF_ASSERT(m_storedBytes >= replaced);
        auto const stored = m_storedBytes - replaced + name.size()
            + value.bytes().size();
        if (stored > k_maximumBytes)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "storing {} would take this session's state to {} bytes, "
                    "past its ceiling of {}",
                    name,
                    stored,
                    k_maximumBytes
                )
            );
        }

        m_entries.insert_or_assign(std::move(name), std::move(value));
        m_storedBytes = stored;
        return ok();
    }
}
