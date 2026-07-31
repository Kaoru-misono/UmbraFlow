#pragma once

#include "resource.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>

#include <domain/space.hpp>

#include <optional>

namespace uf::annotation
{
    // One appearance of one element. An appearance changes what a patch of pixels
    // LOOKS like; it does not change where that patch is or what it means. A
    // form that moves or resizes is not an appearance, and a form that would carry
    // different capabilities is not one either.
    //
    // Appearances are named because for a 1x/2x/3x speed button the matched form
    // IS the state: the script reads which one matched and thereby learns the
    // current speed. That is why the name is a ResourceName -- it has to reach
    // the script surface as a member key -- and not an index.
    class Appearance final
    {
    public:
        struct Spec final
        {
            ResourceName             name;
            SourceId                 sourceId;
            PixelRect                templateRect;
            SimilarityThreshold      threshold;
            std::optional<ColourKey> colourKey{};
        };

    private:
        ResourceName             m_name;
        SourceId                 m_sourceId;
        PixelRect                m_templateRect;
        SimilarityThreshold      m_threshold;
        std::optional<ColourKey> m_colourKey;

        explicit Appearance(Spec spec) noexcept;

    public:
        auto operator==(Appearance const&) const -> bool = default;

        [[nodiscard]]
        static auto create(Spec const& spec) -> Result<Appearance>;

        [[nodiscard]] auto name() const -> ResourceName;
        [[nodiscard]] auto sourceId() const -> SourceId;
        [[nodiscard]] auto templateRect() const noexcept -> PixelRect;
        [[nodiscard]] auto threshold() const noexcept -> SimilarityThreshold;

        // Which of the template's pixels count, on the same terms the element
        // states it: absent means all of them.
        [[nodiscard]] auto colourKey() const noexcept -> std::optional<ColourKey>;
    };
}
