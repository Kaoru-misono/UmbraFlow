#include "appearance.hpp"

#include "resource.hpp"

#include <domain/error.hpp>

#include <optional>
#include <utility>

namespace uf::annotation
{
    Appearance::Appearance(Spec spec) noexcept
        : m_name{std::move(spec.name)}
        , m_sourceId{spec.sourceId}
        , m_templateRect{spec.templateRect}
        , m_threshold{spec.threshold}
        , m_colourKey{spec.colourKey}
    {
    }

    auto Appearance::create(Spec const& spec) -> Result<Appearance>
    {
        // The one guard an appearance's own fields can support, and the same one
        // Element::create applies to the same (template rect, threshold) pair.
        // The element keeps the other two: fitting the project resolution needs
        // the fingerprint and fitting inside the search region needs the search
        // ROI, and an appearance owns neither.
        //
        // An empty template rect is deliberately not checked here. PixelRect
        // refuses a zero width or height at its own construction, so the check
        // could never fire and no test could turn it red.
        UF_TRY(
            spec.threshold.maximumSad(
                spec.templateRect.width(),
                spec.templateRect.height()
            )
        );

        return Appearance{spec};
    }

    auto Appearance::name() const -> ResourceName { return m_name; }

    auto Appearance::sourceId() const -> SourceId { return m_sourceId; }

    auto Appearance::templateRect() const noexcept -> PixelRect { return m_templateRect; }

    auto Appearance::threshold() const noexcept -> SimilarityThreshold { return m_threshold; }

    auto Appearance::colourKey() const noexcept -> std::optional<ColourKey>
    {
        return m_colourKey;
    }
}
