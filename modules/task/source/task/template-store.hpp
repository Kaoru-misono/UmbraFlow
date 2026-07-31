#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <annotation/recognition-runtime.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace uf::task
{
    // The largest template blob one template_load may decode.
    //
    // Templates are crops of one screen, so the biggest one this project has
    // authored is a few hundred kilobytes of PNG. A megabyte leaves room for a
    // whole-panel crop and still refuses a script that hands the loader an
    // arbitrary file.
    inline constexpr auto k_maximumTemplateBytes = std::size_t{1} * 1024U * 1024U;

    // How many distinct templates one generation may hold decoded at once.
    //
    // The store never forgets a template, because a page model loads its
    // templates once and matches them for the life of the run; the ceiling is
    // what keeps "never forgets" from meaning "grows without bound". Sixty-four
    // elements with several appearances each is already an order of magnitude
    // above the largest project annotated so far.
    inline constexpr auto k_maximumLoadedTemplates = std::size_t{512};

    // All a script ever holds of a decoded template: a name, never the pixels.
    //
    // The same shape as a cycle ticket and for the same reason. The decoded
    // planes are megabytes that belong to the generation, so they are released
    // when the generation is torn down rather than whenever the Lua collector
    // gets to a handle.
    struct TemplateTicket final
    {
        uint64 generation{};
        uint64 ordinal{};
    };

    // The generation's decoded templates, addressed by ticket.
    //
    // A template is decoded ONCE. The alternative -- decode on every match --
    // was considered and rejected: a wait loop matching one template per poll
    // would pay a PNG decode per poll, and the decode is deterministic, so
    // repeating it can only cost time and never change an answer.
    //
    // Loading the same bytes twice returns the SAME ticket. That is what makes
    // the verb deterministic in the sense section 10 requires: a script that
    // loads its model file's templates in a different order still ends up with
    // the same handle for the same pixels, and the store holds one copy.
    //
    // NOT thread-safe: every method runs on the VM's owning thread.
    class TemplateStore final
    {
        struct Entry final
        {
            uint64                        ordinal{};
            annotation::GrayTemplateImage image;
        };

        uint64             m_generation;
        uint64             m_nextOrdinal{1};
        std::vector<Entry> m_entries{};

    public:
        TemplateStore() noexcept;

        [[nodiscard]] auto size() const noexcept -> std::size_t;

        // Decodes `pngBytes` and returns the ticket naming the result, or the
        // ticket an identical blob already has. A blob that is not a PNG this
        // project can decode, or one beyond the size ceiling, is refused as an
        // InvalidResource rather than becoming a template that matches nothing.
        [[nodiscard]]
        auto load(std::span<std::byte const> pngBytes) -> Result<TemplateTicket>;

        // The decoded template `ticket` names, or null when it names none --
        // a ticket from another generation, or one this store never minted. The
        // borrow lasts until the next load() on this store.
        [[nodiscard]]
        auto find(TemplateTicket ticket) const noexcept UF_LIFETIME_BOUND
            -> annotation::GrayTemplateImage const*;
    };
}
