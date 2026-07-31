#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>
#include <core/types/strong-value.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace uf::annotation
{
    class ResourceId final
    {
        std::array<uint8, 16> m_bytes;

        constexpr explicit ResourceId(std::array<uint8, 16> bytes) noexcept
            : m_bytes{bytes}
        {
        }

    public:
        auto operator<=>(ResourceId const&) const = default;

        [[nodiscard]]
        static auto parse(std::string_view value) -> Result<ResourceId>;

        // Builds an identifier from its 16 raw bytes verbatim. Callers own the
        // byte convention; this performs no validation of version or variant.
        [[nodiscard]]
        static auto fromBytes(std::span<std::byte const, 16> bytes) noexcept -> ResourceId;

        [[nodiscard]] auto toString() const -> std::string;
    };

    namespace detail
    {
        struct ElementIdTag;
        struct PageIdTag;
        struct SourceIdTag;
    }

    // Identifies one authored element: the rectangle an author drew, the uses
    // it may be put to, and the appearances it can take. It was spelled
    // RecognizerId until 2026-07-31, which said the thing recognizes; an
    // element the task only clicks or only reads recognizes nothing, and is
    // located by the page it sits on.
    //
    // One element compiles to exactly one CompiledElement under this id,
    // whatever pages reference it, so this is also the only id a page
    // signature, an authorisation, or a trace line ever has to resolve.
    using ElementId = StrongValue<detail::ElementIdTag, ResourceId>;
    using PageId = StrongValue<detail::PageIdTag, ResourceId>;
    using SourceId = StrongValue<detail::SourceIdTag, ResourceId>;

    class ProjectId final
    {
        std::string m_value;

        explicit ProjectId(std::string value) noexcept;

    public:
        auto operator<=>(ProjectId const&) const = default;

        [[nodiscard]] static auto create(std::string value) -> Result<ProjectId>;

        [[nodiscard]]
        auto value() const noexcept UF_LIFETIME_BOUND -> std::string const&;
    };

    class ResourceName final
    {
        std::string m_value;

        explicit ResourceName(std::string value) noexcept;

    public:
        auto operator<=>(ResourceName const&) const = default;

        [[nodiscard]] static auto create(std::string value) -> Result<ResourceName>;

        [[nodiscard]]
        auto value() const noexcept UF_LIFETIME_BOUND -> std::string const&;
    };

    class ProjectFingerprint final
    {
        uint32 m_width;
        uint32 m_height;
        uint32 m_dpiX;
        uint32 m_dpiY;

        constexpr ProjectFingerprint(
            uint32 width,
            uint32 height,
            uint32 dpiX,
            uint32 dpiY
        ) noexcept
            : m_width{width}
            , m_height{height}
            , m_dpiX{dpiX}
            , m_dpiY{dpiY}
        {
        }

    public:
        auto operator<=>(ProjectFingerprint const&) const = default;

        [[nodiscard]]
        static auto create(
            uint32 width,
            uint32 height,
            uint32 dpiX,
            uint32 dpiY
        ) -> Result<ProjectFingerprint>;

        [[nodiscard]] constexpr auto width() const noexcept -> uint32 { return m_width; }
        [[nodiscard]] constexpr auto height() const noexcept -> uint32 { return m_height; }
        [[nodiscard]] constexpr auto dpiX() const noexcept -> uint32 { return m_dpiX; }
        [[nodiscard]] constexpr auto dpiY() const noexcept -> uint32 { return m_dpiY; }
    };

    class SimilarityThreshold final
    {
        static constexpr auto k_basisPointMaximum = uint32{10'000};

        uint32 m_basisPoints;

        constexpr explicit SimilarityThreshold(uint32 basisPoints) noexcept
            : m_basisPoints{basisPoints}
        {
        }

    public:
        auto operator<=>(SimilarityThreshold const&) const = default;

        [[nodiscard]]
        static auto create(uint32 basisPoints) -> Result<SimilarityThreshold>;

        [[nodiscard]]
        constexpr auto basisPoints() const noexcept -> uint32
        {
            return m_basisPoints;
        }

        [[nodiscard]]
        auto maximumSad(uint32 templateWidth, uint32 templateHeight) const -> Result<uint64>;
    };

    class TemplateOffset final
    {
        uint32 m_x;
        uint32 m_y;

        constexpr TemplateOffset(uint32 x, uint32 y) noexcept
            : m_x{x}
            , m_y{y}
        {
        }

    public:
        auto operator<=>(TemplateOffset const&) const = default;

        [[nodiscard]]
        static auto create(
            uint32 x,
            uint32 y,
            uint32 templateWidth,
            uint32 templateHeight
        ) -> Result<TemplateOffset>;

        [[nodiscard]] constexpr auto x() const noexcept -> uint32 { return m_x; }
        [[nodiscard]] constexpr auto y() const noexcept -> uint32 { return m_y; }
    };

    // The colour an author picked out of the source image, and how far another
    // pixel may sit from it and still belong to the element's mask. Distance is
    // the sum of the three absolute channel differences, so it runs 0..765 and
    // one unit is one level on one channel.
    //
    // What is stored is the key, never the mask it produces. An author reopening
    // a project has to be able to move the tolerance and watch the selection
    // change; a baked mask cannot be moved back.
    class ColourKey final
    {
    public:
        // The full range of a channel, and of the distance across all three. A
        // key at the maximum tolerance admits every colour, which is legal and
        // useless -- the same mask as carrying no key at all.
        static constexpr auto k_maximumChannel   = uint32{255};
        static constexpr auto k_maximumTolerance = uint32{765};

    private:
        uint8  m_red;
        uint8  m_green;
        uint8  m_blue;
        uint32 m_tolerance;

        constexpr ColourKey(
            uint8 red,
            uint8 green,
            uint8 blue,
            uint32 tolerance
        ) noexcept
            : m_red{red}
            , m_green{green}
            , m_blue{blue}
            , m_tolerance{tolerance}
        {
        }

    public:
        auto operator==(ColourKey const&) const -> bool = default;

        // Channels are taken widened because both callers -- the canonical TOML
        // reader and the picker UI -- hold values that are only supposed to be
        // channel-sized, and this is where that is established.
        [[nodiscard]]
        static auto create(
            uint32 red,
            uint32 green,
            uint32 blue,
            uint32 tolerance
        ) -> Result<ColourKey>;

        [[nodiscard]] constexpr auto red() const noexcept -> uint8 { return m_red; }
        [[nodiscard]] constexpr auto green() const noexcept -> uint8 { return m_green; }
        [[nodiscard]] constexpr auto blue() const noexcept -> uint8 { return m_blue; }

        [[nodiscard]]
        constexpr auto tolerance() const noexcept -> uint32 { return m_tolerance; }

        // The mask weight one source pixel earns, which is exactly the alpha
        // byte the compiled template carries for it: 255 counts fully, 0 is
        // excluded, and between them the matcher weights the pixel partially.
        //
        // Full weight out to the tolerance, then a linear ramp to nothing at
        // twice it. The ramp is not decoration. A hard cut makes an author's
        // tolerance control jump in steps, and it cuts through the antialiased
        // skirt of a glyph, where the pixels just past the cut are still mostly
        // glyph -- on the measured menu entry a tolerance of 12 around the white
        // text takes 93.9% of the glyph and leaves a rim of edge pixels at
        // distance 13..24. Those are the pixels the ramp readmits, at the weight
        // they deserve. A tolerance of 0 has no ramp to speak of and stays an
        // exact-colour mask.
        [[nodiscard]]
        auto alphaFor(uint8 red, uint8 green, uint8 blue) const noexcept -> uint8;
    };
}
