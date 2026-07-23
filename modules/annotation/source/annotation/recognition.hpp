#pragma once

#include "catalog.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/frame.hpp>

#include <vision/sad.hpp>

#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace uf::annotation
{
    class FrameIdentity final
    {
        SessionId        m_sessionId;
        TargetGeneration m_targetGeneration;
        FrameId          m_frameId;

    public:
        constexpr FrameIdentity(
            SessionId sessionId,
            TargetGeneration targetGeneration,
            FrameId frameId
        ) noexcept
            : m_sessionId{sessionId}
            , m_targetGeneration{targetGeneration}
            , m_frameId{frameId}
        {
        }

        auto operator<=>(FrameIdentity const&) const = default;

        [[nodiscard]] static auto fromFrame(Frame const& frame) noexcept -> FrameIdentity;

        [[nodiscard]] auto sessionId() const noexcept -> SessionId;
        [[nodiscard]] auto targetGeneration() const noexcept -> TargetGeneration;
        [[nodiscard]] auto frameId() const noexcept -> FrameId;
    };

    class AnchorEvidence final
    {
        friend class AnchorEvaluation;

        RecognizerId             m_recognizerId;
        bool                     m_hit;
        std::optional<uint64>    m_sadScore;
        uint64                   m_maximumSad;
        std::optional<PixelRect> m_matchedRect;
        std::optional<float>     m_displayConfidence;

        AnchorEvidence(
            RecognizerId recognizerId,
            bool hit,
            std::optional<uint64> sadScore,
            uint64 maximumSad,
            std::optional<PixelRect> matchedRect,
            std::optional<float> displayConfidence
        ) noexcept;

    public:
        auto operator==(AnchorEvidence const&) const -> bool = default;

        [[nodiscard]] auto recognizerId() const -> RecognizerId;
        [[nodiscard]] auto hit() const noexcept -> bool;
        [[nodiscard]] auto sadScore() const noexcept -> std::optional<uint64>;
        [[nodiscard]] auto maximumSad() const noexcept -> uint64;
        [[nodiscard]] auto matchedRect() const noexcept -> std::optional<PixelRect>;
        [[nodiscard]] auto displayConfidence() const noexcept -> std::optional<float>;
    };

    class AnchorEvaluation final
    {
    public:
        using Evaluation = std::variant<AnchorEvidence, SadSearchStopReason>;

    private:
        RecognizerId m_recognizerId;
        Evaluation   m_evaluation;

        AnchorEvaluation(
            RecognizerId recognizerId,
            Evaluation evaluation
        ) noexcept;

    public:
        [[nodiscard]]
        static auto fromSadOutcome(
            RecognizerDefinition const& recognizer,
            SadSearchOutcome const& outcome
        ) -> Result<AnchorEvaluation>;

        [[nodiscard]] auto recognizerId() const -> RecognizerId;

        [[nodiscard]]
        auto evaluation() const noexcept UF_LIFETIME_BOUND -> Evaluation const&;
    };

    class PageEvaluation final
    {
        PageId                      m_pageId;
        std::vector<AnchorEvidence> m_required;
        std::vector<AnchorEvidence> m_forbidden;
        bool                        m_candidate;

    public:
        PageEvaluation(
            PageId pageId,
            std::vector<AnchorEvidence> required,
            std::vector<AnchorEvidence> forbidden,
            bool candidate
        ) noexcept;

        [[nodiscard]] auto pageId() const -> PageId;

        [[nodiscard]]
        auto required() const noexcept UF_LIFETIME_BOUND -> std::span<AnchorEvidence const>;

        [[nodiscard]]
        auto forbidden() const noexcept UF_LIFETIME_BOUND -> std::span<AnchorEvidence const>;

        [[nodiscard]] auto candidate() const noexcept -> bool;
    };

    class PageResolutionEvidence final
    {
        ProjectId                   m_projectId;
        FrameIdentity               m_frameIdentity;
        std::vector<PageEvaluation> m_pages;
        std::vector<PageId>         m_candidatePageIds;

    public:
        PageResolutionEvidence(
            ProjectId projectId,
            FrameIdentity frameIdentity,
            std::vector<PageEvaluation> pages,
            std::vector<PageId> candidatePageIds
        ) noexcept;

        [[nodiscard]]
        auto projectId() const noexcept UF_LIFETIME_BOUND -> ProjectId const&;

        [[nodiscard]] auto frameIdentity() const noexcept -> FrameIdentity;

        [[nodiscard]]
        auto pages() const noexcept UF_LIFETIME_BOUND -> std::span<PageEvaluation const>;

        [[nodiscard]]
        auto candidatePageIds() const noexcept UF_LIFETIME_BOUND -> std::span<PageId const>;
    };

    class PageResolver;

    class ResolvedPage final
    {
        friend class PageResolver;

        PageId                 m_pageId;
        PageResolutionEvidence m_evidence;

        ResolvedPage(PageId pageId, PageResolutionEvidence evidence) noexcept;

    public:
        [[nodiscard]] auto pageId() const -> PageId;

        [[nodiscard]]
        auto evidence() const noexcept UF_LIFETIME_BOUND -> PageResolutionEvidence const&;
    };

    class UnknownPage final
    {
        friend class PageResolver;

        PageResolutionEvidence m_evidence;

        explicit UnknownPage(PageResolutionEvidence evidence) noexcept;

    public:
        [[nodiscard]]
        auto evidence() const noexcept UF_LIFETIME_BOUND -> PageResolutionEvidence const&;
    };

    class AmbiguousPages final
    {
        friend class PageResolver;

        PageResolutionEvidence m_evidence;

        explicit AmbiguousPages(PageResolutionEvidence evidence) noexcept;

    public:
        [[nodiscard]]
        auto evidence() const noexcept UF_LIFETIME_BOUND -> PageResolutionEvidence const&;
    };

    using PageOutcome = std::variant<ResolvedPage, UnknownPage, AmbiguousPages>;

    class PageResolver final
    {
    public:
        [[nodiscard]]
        static auto resolve(
            RecognitionCatalog const& catalog,
            FrameIdentity frameIdentity,
            std::span<AnchorEvaluation const> evaluations
        ) -> Result<PageOutcome>;
    };
}
