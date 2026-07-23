#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>
#include <core/types/strong-value.hpp>

#include <domain/space.hpp>

#include <array>
#include <compare>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
        static constexpr auto s_basisPointMaximum = uint32{10'000};

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

    struct RecognizerSpec final
    {
        RecognizerId m_id;
        ResourceName m_name;
        AnnotationType m_annotationType;
        PixelRect m_templateRect;
        PixelRect m_searchRoi;
        SimilarityThreshold m_threshold;
        std::optional<TemplateOffset> m_defaultClick;
        std::vector<PageId> m_allowedPageIds;
    };

    class RecognizerDefinition final
    {
        RecognizerId m_id;
        ResourceName m_name;
        AnnotationType m_annotationType;
        PixelRect m_templateRect;
        PixelRect m_searchRoi;
        SimilarityThreshold m_threshold;
        std::optional<TemplateOffset> m_defaultClick;
        std::vector<PageId> m_allowedPageIds;

        explicit RecognizerDefinition(RecognizerSpec spec) noexcept;

    public:
        [[nodiscard]]
        static auto create(
            ProjectFingerprint fingerprint,
            RecognizerSpec const& spec
        ) -> Result<RecognizerDefinition>;

        [[nodiscard]] auto id() const -> RecognizerId;
        [[nodiscard]] auto name() const -> ResourceName;
        [[nodiscard]] auto annotationType() const noexcept -> AnnotationType;
        [[nodiscard]] auto templateRect() const noexcept -> PixelRect;
        [[nodiscard]] auto searchRoi() const noexcept -> PixelRect;
        [[nodiscard]] auto threshold() const noexcept -> SimilarityThreshold;
        [[nodiscard]] auto defaultClick() const noexcept -> std::optional<TemplateOffset>;

        [[nodiscard]]
        auto allowedPageIds() const noexcept UF_LIFETIME_BOUND -> std::span<PageId const>;
    };

    struct PageSpec final
    {
        PageId m_id;
        ResourceName m_name;
        std::vector<RecognizerId> m_required;
        std::vector<RecognizerId> m_forbidden;
    };

    class PageSignature final
    {
        PageId m_id;
        ResourceName m_name;
        std::vector<RecognizerId> m_required;
        std::vector<RecognizerId> m_forbidden;

        explicit PageSignature(PageSpec spec) noexcept;

    public:
        [[nodiscard]]
        static auto create(PageSpec const& spec) -> Result<PageSignature>;

        [[nodiscard]] auto id() const -> PageId;
        [[nodiscard]] auto name() const -> ResourceName;

        [[nodiscard]]
        auto required() const noexcept UF_LIFETIME_BOUND -> std::span<RecognizerId const>;

        [[nodiscard]]
        auto forbidden() const noexcept UF_LIFETIME_BOUND -> std::span<RecognizerId const>;
    };

    class RecognitionCatalog final
    {
        ProjectId m_projectId;
        ProjectFingerprint m_fingerprint;
        std::vector<RecognizerDefinition> m_recognizers;
        std::vector<PageSignature> m_pages;
        std::vector<RecognizerId> m_pageAnchorOrder;

        RecognitionCatalog(
            ProjectId projectId,
            ProjectFingerprint fingerprint,
            std::vector<RecognizerDefinition> recognizers,
            std::vector<PageSignature> pages,
            std::vector<RecognizerId> pageAnchorOrder
        ) noexcept;

    public:
        [[nodiscard]]
        static auto create(
            ProjectId projectId,
            ProjectFingerprint fingerprint,
            std::vector<RecognizerDefinition> recognizers,
            std::vector<PageSignature> pages
        ) -> Result<RecognitionCatalog>;

        [[nodiscard]]
        auto projectId() const noexcept UF_LIFETIME_BOUND -> ProjectId const&;

        [[nodiscard]] auto fingerprint() const noexcept -> ProjectFingerprint;

        [[nodiscard]]
        auto recognizers() const noexcept UF_LIFETIME_BOUND -> std::span<RecognizerDefinition const>;

        [[nodiscard]]
        auto pages() const noexcept UF_LIFETIME_BOUND -> std::span<PageSignature const>;

        // Returned observations remain valid only while this catalog is alive.
        [[nodiscard]]
        auto findRecognizer(
            RecognizerId id
        ) const noexcept UF_LIFETIME_BOUND -> RecognizerDefinition const*;

        [[nodiscard]]
        auto findPage(PageId id) const noexcept UF_LIFETIME_BOUND -> PageSignature const*;

        [[nodiscard]]
        auto pageAnchorOrder() const noexcept UF_LIFETIME_BOUND -> std::span<RecognizerId const>;
    };
}
