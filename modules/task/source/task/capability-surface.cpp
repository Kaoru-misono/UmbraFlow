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
        std::vector<RecognizerHandleSpec> recognizers,
        std::vector<PageHandleSpec> pages
    ) noexcept
        : m_recognizers{std::move(recognizers)}
        , m_pages{std::move(pages)}
    {
    }

    auto CapabilitySurface::create(
        annotation::RecognitionCatalog const& catalog
    ) -> Result<CapabilitySurface>
    {
        auto recognizers = std::vector<RecognizerHandleSpec>{};
        auto seenNames   = std::unordered_set<std::string>{};

        for (auto const& definition : catalog.recognizers())
        {
            // Only action targets are findable from a script; page anchors are
            // page-internal evidence and info regions have no verb yet.
            if (definition.annotationType() != annotation::AnnotationType::ActionTarget)
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
                    "duplicate action-target name in uf.recognizers: " + name
                );
            }

            recognizers.push_back(
                RecognizerHandleSpec{.name = std::move(name), .id = definition.id()}
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

        return CapabilitySurface{std::move(recognizers), std::move(pages)};
    }

    auto CapabilitySurface::recognizerCount() const noexcept -> std::size_t
    {
        return m_recognizers.size();
    }

    auto CapabilitySurface::pageCount() const noexcept -> std::size_t
    {
        return m_pages.size();
    }

    auto CapabilitySurface::recognizers() const noexcept
        -> std::span<RecognizerHandleSpec const>
    {
        return m_recognizers;
    }

    auto CapabilitySurface::pages() const noexcept -> std::span<PageHandleSpec const>
    {
        return m_pages;
    }
}
