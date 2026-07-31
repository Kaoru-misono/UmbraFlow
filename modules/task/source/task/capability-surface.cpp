#include <task/capability-surface.hpp>

#include <core/error/result.hpp>

#include <annotation/catalog.hpp>

#include <domain/error.hpp>

#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace uf::task
{
    CapabilitySurface::CapabilitySurface(
        std::vector<ElementHandleSpec> elements,
        std::vector<PageHandleSpec> pages
    ) noexcept
        : m_elements{std::move(elements)}
        , m_pages{std::move(pages)}
    {
    }

    auto CapabilitySurface::create(
        annotation::RecognitionCatalog const& catalog
    ) -> Result<CapabilitySurface>
    {
        auto elements  = std::vector<ElementHandleSpec>{};
        auto seenNames = std::unordered_set<std::string>{};

        for (auto const& definition : catalog.elements())
        {
            // Findable means interactable: a handle a script holds is one it
            // could go on to click. Identify contributes page-internal
            // evidence and read has no verb yet, so neither earns a handle on
            // its own. Asking for the capability rather than for the absence of
            // the other two is what lets an element that both names its page
            // and can be clicked appear here exactly once -- which is the whole
            // point of capabilities being a set rather than a choice.
            if (!definition.capabilities().hasInteract())
            {
                continue;
            }

            // ResourceName's invariant already proves this is a direct Luau
            // member key; the surface only has to reject a collision that would
            // otherwise silently overwrite a handle.
            auto name = definition.name().value();
            if (!seenNames.insert(name).second)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "duplicate interactive element name in uf.elements: " + name
                );
            }

            elements.push_back(
                ElementHandleSpec{.name = std::move(name), .id = definition.id()}
            );
        }

        auto pages         = std::vector<PageHandleSpec>{};
        auto seenPageNames = std::unordered_set<std::string>{};

        for (auto const& signature : catalog.pages())
        {
            auto name = signature.name().value();
            if (!seenPageNames.insert(name).second)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "duplicate page name in uf.pages: " + name
                );
            }

            pages.push_back(
                PageHandleSpec{.name = std::move(name), .id = signature.id()}
            );
        }

        return CapabilitySurface{std::move(elements), std::move(pages)};
    }

    auto CapabilitySurface::elementCount() const noexcept -> std::size_t
    {
        return m_elements.size();
    }

    auto CapabilitySurface::pageCount() const noexcept -> std::size_t
    {
        return m_pages.size();
    }

    auto CapabilitySurface::elements() const noexcept
        -> std::span<ElementHandleSpec const>
    {
        return m_elements;
    }

    auto CapabilitySurface::pages() const noexcept -> std::span<PageHandleSpec const>
    {
        return m_pages;
    }
}
