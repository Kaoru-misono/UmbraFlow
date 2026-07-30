#include "variant.hpp"

#include "resource.hpp"

#include <domain/error.hpp>

#include <optional>
#include <utility>

namespace uf::annotation
{
    Variant::Variant(Spec spec) noexcept
        : m_name{std::move(spec.name)}
        , m_sourceId{spec.sourceId}
        , m_templateRect{spec.templateRect}
        , m_threshold{spec.threshold}
        , m_colourKey{spec.colourKey}
    {
    }

    auto Variant::create(Spec const& spec) -> Result<Variant>
    {
        // The one guard a variant's own fields can support, and the same one
        // Element::create applies to the same (template rect, threshold) pair.
        // The element keeps the other two: fitting the project resolution needs
        // the fingerprint and fitting inside the search region needs the search
        // ROI, and a variant owns neither.
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

        return Variant{spec};
    }

    auto Variant::name() const -> ResourceName { return m_name; }

    auto Variant::sourceId() const -> SourceId { return m_sourceId; }

    auto Variant::templateRect() const noexcept -> PixelRect { return m_templateRect; }

    auto Variant::threshold() const noexcept -> SimilarityThreshold { return m_threshold; }

    auto Variant::colourKey() const noexcept -> std::optional<ColourKey>
    {
        return m_colourKey;
    }
}
