#pragma once

#include "resource.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>

#include <domain/space.hpp>

#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace uf::annotation
{
    struct RecognizerSpec final
    {
        RecognizerId id;
        ResourceName name;

        AnnotationType      annotationType{};
        PixelRect           templateRect;
        PixelRect           searchRoi;
        SimilarityThreshold threshold;

        std::optional<TemplateOffset> defaultClick{};
        std::vector<PageId>           allowedPageIds{};
    };

    class RecognizerDefinition final
    {
        RecognizerId m_id;
        ResourceName m_name;

        AnnotationType      m_annotationType;
        PixelRect           m_templateRect;
        PixelRect           m_searchRoi;
        SimilarityThreshold m_threshold;

        std::optional<TemplateOffset> m_defaultClick;
        std::vector<PageId>           m_allowedPageIds;

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
        PageId                    id;
        ResourceName              name;
        std::vector<RecognizerId> required{};
        std::vector<RecognizerId> forbidden{};
    };

    class PageSignature final
    {
        PageId                    m_id;
        ResourceName              m_name;
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
        ProjectId                         m_projectId;
        ProjectFingerprint                m_fingerprint;
        std::vector<RecognizerDefinition> m_recognizers;
        std::vector<PageSignature>        m_pages;
        std::vector<RecognizerId>         m_pageAnchorOrder;

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
