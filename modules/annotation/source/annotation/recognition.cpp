#include "recognition.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-arithmetic.hpp>

#include <domain/error.hpp>

#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace uf::annotation
{
    namespace
    {
        [[nodiscard]]
        auto stopFailure(
            RecognizerId recognizerId,
            SadSearchStopReason reason
        ) -> std::unexpected<Error>
        {
            auto const id = recognizerId.value().toString();
            switch (reason)
            {
            case SadSearchStopReason::Cancelled:
                return fail(
                    AutomationErrorKind::Cancelled,
                    std::format("page recognition cancelled at anchor {}", id)
                );
            case SadSearchStopReason::TimedOut:
                return fail(
                    AutomationErrorKind::Timeout,
                    std::format("page recognition timed out at anchor {}", id)
                );
            case SadSearchStopReason::ComparisonBudgetExhausted:
                return fail(
                    AutomationErrorKind::RecognitionFailed,
                    std::format("page recognition budget exhausted at anchor {}", id)
                );
            }

            UF_UNREACHABLE_MSG("Unknown SadSearchStopReason value");
        }

        [[nodiscard]]
        auto findEvidence(
            std::span<AnchorEvidence const> evidence,
            RecognizerId id
        ) noexcept -> AnchorEvidence const*
        {
            auto const found = std::ranges::find(
                evidence,
                id,
                &AnchorEvidence::recognizerId
            );
            return found == evidence.end() ? nullptr : &*found;
        }
    }

    auto FrameIdentity::fromFrame(Frame const& frame) noexcept -> FrameIdentity
    {
        return FrameIdentity{
            frame.sessionId(),
            frame.targetGeneration(),
            frame.id()
        };
    }

    auto FrameIdentity::sessionId() const noexcept -> SessionId { return m_sessionId; }
    auto FrameIdentity::targetGeneration() const noexcept -> TargetGeneration
    {
        return m_targetGeneration;
    }
    auto FrameIdentity::frameId() const noexcept -> FrameId { return m_frameId; }

    AnchorEvidence::AnchorEvidence(
        RecognizerId recognizerId,
        bool hit,
        std::optional<uint64> sadScore,
        uint64 maximumSad,
        std::optional<PixelRect> matchedRect,
        std::optional<float> displayConfidence
    ) noexcept
        : m_recognizerId{std::move(recognizerId)}
        , m_hit{hit}
        , m_sadScore{sadScore}
        , m_maximumSad{maximumSad}
        , m_matchedRect{matchedRect}
        , m_displayConfidence{displayConfidence}
    {
    }

    auto AnchorEvidence::recognizerId() const -> RecognizerId { return m_recognizerId; }
    auto AnchorEvidence::hit() const noexcept -> bool { return m_hit; }
    auto AnchorEvidence::sadScore() const noexcept -> std::optional<uint64> { return m_sadScore; }
    auto AnchorEvidence::maximumSad() const noexcept -> uint64 { return m_maximumSad; }
    auto AnchorEvidence::matchedRect() const noexcept -> std::optional<PixelRect>
    {
        return m_matchedRect;
    }
    auto AnchorEvidence::displayConfidence() const noexcept -> std::optional<float>
    {
        return m_displayConfidence;
    }

    AnchorEvaluation::AnchorEvaluation(
        RecognizerId recognizerId,
        Evaluation evaluation
    ) noexcept
        : m_recognizerId{std::move(recognizerId)}
        , m_evaluation{std::move(evaluation)}
    {
    }

    auto AnchorEvaluation::fromSadOutcome(
        RecognizerDefinition const& recognizer,
        SadSearchOutcome const& outcome
    ) -> Result<AnchorEvaluation>
    {
        if (recognizer.annotationType() != AnnotationType::PageAnchor)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "page resolution may evaluate only page_anchor recognizers"
            );
        }

        if (auto const* p_stop = std::get_if<SadSearchStopReason>(&outcome))
        {
            return AnchorEvaluation{
                recognizer.id(),
                *p_stop
            };
        }

        auto const& match = std::get<std::optional<SadMatch>>(outcome);
        auto const templateRect = recognizer.templateRect();
        UF_TRY_VALUE(
            maximumSad,
            recognizer.threshold().maximumSad(
                templateRect.width(),
                templateRect.height()
            )
        );

        if (!match)
        {
            return AnchorEvaluation{
                recognizer.id(),
                AnchorEvidence{
                    recognizer.id(),
                    false,
                    std::nullopt,
                    maximumSad,
                    std::nullopt,
                    std::nullopt
                }
            };
        }

        auto const searchRoi = recognizer.searchRoi();
        auto const matchedRight = checkedAdd(match->x(), templateRect.width());
        auto const matchedBottom = checkedAdd(match->y(), templateRect.height());
        if (
            !matchedRight
            || !matchedBottom
            || match->x() < searchRoi.x()
            || match->y() < searchRoi.y()
            || *matchedRight > searchRoi.right()
            || *matchedBottom > searchRoi.bottom()
        )
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "SAD matcher returned a rectangle outside the recognizer search_roi"
            );
        }

        UF_TRY_VALUE(
            matchedRect,
            PixelRect::create(
                match->x(),
                match->y(),
                templateRect.width(),
                templateRect.height()
            )
        );
        auto const pixels = checkedMultiply(
            static_cast<uint64>(templateRect.width()),
            static_cast<uint64>(templateRect.height())
        );
        if (!pixels)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "template dimensions overflow the SAD evidence calculation"
            );
        }

        auto const maximumPossibleSad = checkedMultiply(*pixels, uint64{255});
        if (!maximumPossibleSad || match->score() > *maximumPossibleSad)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "SAD matcher returned a score outside the template distance range"
            );
        }

        auto const displayConfidence = 1.0F - (
            static_cast<float>(match->score())
            / static_cast<float>(*maximumPossibleSad)
        );
        return AnchorEvaluation{
            recognizer.id(),
            AnchorEvidence{
                recognizer.id(),
                match->score() <= maximumSad,
                match->score(),
                maximumSad,
                matchedRect,
                displayConfidence
            }
        };
    }

    auto AnchorEvaluation::recognizerId() const -> RecognizerId { return m_recognizerId; }
    auto AnchorEvaluation::evaluation() const noexcept -> Evaluation const&
    {
        return m_evaluation;
    }

    PageEvaluation::PageEvaluation(
        PageId pageId,
        std::vector<AnchorEvidence> required,
        std::vector<AnchorEvidence> forbidden,
        bool candidate
    ) noexcept
        : m_pageId{std::move(pageId)}
        , m_required{std::move(required)}
        , m_forbidden{std::move(forbidden)}
        , m_candidate{candidate}
    {
    }

    auto PageEvaluation::pageId() const -> PageId { return m_pageId; }
    auto PageEvaluation::required() const noexcept -> std::span<AnchorEvidence const>
    {
        return m_required;
    }
    auto PageEvaluation::forbidden() const noexcept -> std::span<AnchorEvidence const>
    {
        return m_forbidden;
    }
    auto PageEvaluation::candidate() const noexcept -> bool { return m_candidate; }

    PageResolutionEvidence::PageResolutionEvidence(
        ProjectId projectId,
        FrameIdentity frameIdentity,
        std::vector<PageEvaluation> pages,
        std::vector<PageId> candidatePageIds
    ) noexcept
        : m_projectId{std::move(projectId)}
        , m_frameIdentity{frameIdentity}
        , m_pages{std::move(pages)}
        , m_candidatePageIds{std::move(candidatePageIds)}
    {
    }

    auto PageResolutionEvidence::projectId() const noexcept -> ProjectId const&
    {
        return m_projectId;
    }
    auto PageResolutionEvidence::frameIdentity() const noexcept -> FrameIdentity
    {
        return m_frameIdentity;
    }
    auto PageResolutionEvidence::pages() const noexcept -> std::span<PageEvaluation const>
    {
        return m_pages;
    }
    auto PageResolutionEvidence::candidatePageIds() const noexcept -> std::span<PageId const>
    {
        return m_candidatePageIds;
    }

    ResolvedPage::ResolvedPage(PageId pageId, PageResolutionEvidence evidence) noexcept
        : m_pageId{std::move(pageId)}
        , m_evidence{std::move(evidence)}
    {
    }

    auto ResolvedPage::pageId() const -> PageId { return m_pageId; }
    auto ResolvedPage::evidence() const noexcept -> PageResolutionEvidence const&
    {
        return m_evidence;
    }

    UnknownPage::UnknownPage(PageResolutionEvidence evidence) noexcept
        : m_evidence{std::move(evidence)}
    {
    }

    auto UnknownPage::evidence() const noexcept -> PageResolutionEvidence const&
    {
        return m_evidence;
    }

    AmbiguousPages::AmbiguousPages(PageResolutionEvidence evidence) noexcept
        : m_evidence{std::move(evidence)}
    {
    }

    auto AmbiguousPages::evidence() const noexcept -> PageResolutionEvidence const&
    {
        return m_evidence;
    }

    auto PageResolver::resolve(
        RecognitionCatalog const& catalog,
        FrameIdentity frameIdentity,
        std::span<AnchorEvaluation const> evaluations
    ) -> Result<PageOutcome>
    {
        auto const expectedOrder = catalog.pageAnchorOrder();
        auto completed = std::vector<AnchorEvidence>{};
        completed.reserve(expectedOrder.size());

        for (auto index = std::size_t{0}; index < evaluations.size(); ++index)
        {
            if (
                index >= expectedOrder.size()
                || evaluations[index].recognizerId() != expectedOrder[index]
            )
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "anchor evaluations do not follow the catalog's stable UUID order"
                );
            }

            auto const& evaluation = evaluations[index].evaluation();
            if (auto const* p_stop = std::get_if<SadSearchStopReason>(&evaluation))
            {
                return stopFailure(evaluations[index].recognizerId(), *p_stop);
            }

            completed.emplace_back(std::get<AnchorEvidence>(evaluation));
        }

        if (completed.size() != expectedOrder.size())
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "page resolution requires one completed evaluation for every referenced anchor"
            );
        }

        auto pageEvaluations = std::vector<PageEvaluation>{};
        auto candidatePageIds = std::vector<PageId>{};
        pageEvaluations.reserve(catalog.pages().size());
        candidatePageIds.reserve(catalog.pages().size());
        for (auto const& page : catalog.pages())
        {
            auto required = std::vector<AnchorEvidence>{};
            auto forbidden = std::vector<AnchorEvidence>{};
            required.reserve(page.required().size());
            forbidden.reserve(page.forbidden().size());
            auto candidate = true;

            for (auto const id : page.required())
            {
                auto const* p_evidence = findEvidence(completed, id);
                UF_CHECK(p_evidence != nullptr);
                required.emplace_back(*p_evidence);
                candidate = candidate && p_evidence->hit();
            }
            for (auto const id : page.forbidden())
            {
                auto const* p_evidence = findEvidence(completed, id);
                UF_CHECK(p_evidence != nullptr);
                forbidden.emplace_back(*p_evidence);
                candidate = candidate && !p_evidence->hit();
            }

            pageEvaluations.emplace_back(
                page.id(),
                std::move(required),
                std::move(forbidden),
                candidate
            );
            if (candidate)
            {
                candidatePageIds.emplace_back(page.id());
            }
        }

        auto evidence = PageResolutionEvidence{
            catalog.projectId(),
            frameIdentity,
            std::move(pageEvaluations),
            std::move(candidatePageIds)
        };
        auto const candidates = evidence.candidatePageIds();
        if (candidates.empty())
        {
            return PageOutcome{
                std::in_place_type<UnknownPage>,
                UnknownPage{std::move(evidence)}
            };
        }

        if (candidates.size() == 1)
        {
            auto const pageId = candidates.front();
            return PageOutcome{
                std::in_place_type<ResolvedPage>,
                ResolvedPage{pageId, std::move(evidence)}
            };
        }

        return PageOutcome{
            std::in_place_type<AmbiguousPages>,
            AmbiguousPages{std::move(evidence)}
        };
    }
}
