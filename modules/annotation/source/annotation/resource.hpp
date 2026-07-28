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
        struct RecognizerIdTag;
        struct PageIdTag;
    }

    using RecognizerId = StrongValue<detail::RecognizerIdTag, ResourceId>;
    using PageId = StrongValue<detail::PageIdTag, ResourceId>;

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

    enum class AnnotationType : uint8
    {
        PageAnchor,
        ActionTarget,
        InfoRegion,
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
}
