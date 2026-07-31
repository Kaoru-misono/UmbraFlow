#pragma once

#include "catalog.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>

#include <vision/sad.hpp>

#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace uf::annotation
{
    // The single classification of a stopped anchor search. Recognition
    // failures and regression control flow both read it instead of deciding
    // per stop reason at each site.
    //
    // No reason maps to a kind whose failureResponse is StepFailed, and each
    // reason has a kind of its own. That is the load-bearing property, not an
    // accident of the three current reasons: a stop means the search never
    // decided, so a caller must not read it as the step having been ruled out.
    // A completed search that matched nothing is not routed through here at all
    // -- it is an AnchorEvidence whose hit() is false, and page resolution turns
    // that into UnknownPage with no error.
    [[nodiscard]]
    auto searchStopKind(SadSearchStopReason reason) noexcept -> AutomationErrorKind;

    // The returned view is backed by static string literals, so it outlives
    // every caller and needs no owner to be kept alive.
    [[nodiscard]]
    auto searchStopDescription(SadSearchStopReason reason) noexcept -> std::string_view;

    class FrameIdentity final
    {
        CaptureSessionId m_sessionId;
        TargetGeneration m_targetGeneration;
        FrameId          m_frameId;

    public:
        constexpr FrameIdentity(
            CaptureSessionId sessionId,
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

        [[nodiscard]] auto sessionId() const noexcept -> CaptureSessionId;
        [[nodiscard]] auto targetGeneration() const noexcept -> TargetGeneration;
        [[nodiscard]] auto frameId() const noexcept -> FrameId;
    };

    class AnchorEvidence final
    {
        friend class AnchorEvaluation;

        ElementId                   m_elementId;
        bool                        m_hit;
        std::optional<ResourceName> m_appearanceName;
        std::optional<uint64>       m_sadScore;
        uint64                      m_maximumSad;
        std::optional<PixelRect>    m_matchedRect;
        std::optional<float>        m_displayConfidence;

        AnchorEvidence(
            ElementId elementId,
            bool hit,
            std::optional<ResourceName> appearanceName,
            std::optional<uint64> sadScore,
            uint64 maximumSad,
            std::optional<PixelRect> matchedRect,
            std::optional<float> displayConfidence
        ) noexcept;

    public:
        auto operator==(AnchorEvidence const&) const -> bool = default;

        // The evidence for an element that declares no appearances: the page's own
        // resolution located it, so the rectangle is where it was annotated and
        // there are no pixels of its own to score. It always hits, and its
        // maximum SAD is zero because no comparison was made. This is section
        // 2.1's accepted cost for reading, applied to interaction by section
        // 4.2.2, and it is why nothing can re-verify such a rectangle before a
        // click.
        [[nodiscard]]
        static auto locatedByPage(
            ElementId elementId,
            PixelRect rect
        ) -> AnchorEvidence;

        [[nodiscard]] auto elementId() const -> ElementId;
        [[nodiscard]] auto hit() const noexcept -> bool;

        // Which appearance produced this evidence, absent only when the element
        // declares none. "Why did this match" cannot be answered from the
        // evidence stream without it.
        [[nodiscard]]
        auto appearanceName() const noexcept UF_LIFETIME_BOUND
            -> std::optional<ResourceName> const&;

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
        ElementId  m_elementId;
        Evaluation m_evaluation;

        AnchorEvaluation(
            ElementId elementId,
            Evaluation evaluation
        ) noexcept;

    public:
        // One appearance's search, turned into evidence. Every threshold and every
        // template rectangle here belongs to that appearance, not to the element,
        // because maximumSad is a function of the template's own size and
        // threshold. The search region is passed rather than read off the
        // element, because a page reference may have refined it and the
        // matched rectangle has to be checked against the region actually
        // searched.
        [[nodiscard]]
        static auto fromSadOutcome(
            CompiledElement const& element,
            CompiledAppearance const& appearance,
            PixelRect searchRoi,
            SadSearchOutcome const& outcome
        ) -> Result<AnchorEvaluation>;

        // One element's settled answer. An appearance set is folded into a single
        // piece of evidence before the resolver sees it, so this is how the
        // fold's result re-enters the pipeline.
        [[nodiscard]]
        static auto fromEvidence(AnchorEvidence evidence) -> AnchorEvaluation;

        [[nodiscard]] auto elementId() const -> ElementId;

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
        ProjectId     m_projectId;
        FrameIdentity m_frameIdentity;

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
