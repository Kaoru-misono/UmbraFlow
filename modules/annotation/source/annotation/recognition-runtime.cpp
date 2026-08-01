#include "recognition-runtime.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/space.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace uf::annotation
{
    namespace
    {
        [[nodiscard]]
        auto invalidRuntime(std::string message) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::move(message)
            );
        }

        [[nodiscard]]
        auto pageRecognitionFailure(
            PageRecognitionStop const& stop
        ) -> std::unexpected<Error>
        {
            return fail(
                searchStopKind(stop.reason),
                std::format(
                    "page recognition {} at anchor {}",
                    searchStopDescription(stop.reason),
                    stop.elementId.value().toString()
                )
            );
        }

        // Appearance scores are not comparable as they stand: maximumSad is a
        // function of each appearance's own template size and threshold, so a raw
        // SAD from a 40x12 template says nothing beside one from a 90x20. The
        // exact integer comparison of two normalized margins is their cross
        // product, which needs no division and so loses nothing to truncation.
        //
        // Preference order: a hit beats a non-hit; between two of a kind the
        // lower normalized distance wins; a search that matched nothing at all
        // is last. A tie keeps the incumbent, which is the earlier declaration.
        // Declaration order decides ties and nothing else, because "first past
        // the threshold wins" would let a wide early appearance answer for a
        // narrow later one and move the click to its own rectangle, with
        // nothing downstream able to notice.
        [[nodiscard]]
        auto strictlyBetter(
            AnchorEvidence const& candidate,
            AnchorEvidence const& incumbent
        ) -> Result<bool>
        {
            if (candidate.hit() != incumbent.hit())
            {
                return candidate.hit();
            }

            auto const candidateScore = candidate.sadScore();
            auto const incumbentScore = incumbent.sadScore();
            if (!candidateScore)
            {
                return false;
            }
            if (!incumbentScore)
            {
                return true;
            }

            auto const left  = checkedMultiply(*candidateScore, incumbent.maximumSad());
            auto const right = checkedMultiply(*incumbentScore, candidate.maximumSad());
            if (!left || !right)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "comparing two appearance margins overflowed"
                );
            }
            return *left < *right;
        }
    }

    RecognitionRuntime::RecognitionRuntime(
        RuntimeManifest manifest,
        std::vector<StoredTemplate> templates
    ) noexcept
        : m_manifest{std::move(manifest)}
        , m_templates{std::move(templates)}
    {
    }

    auto RecognitionRuntime::create(
        RuntimeManifest manifest,
        std::vector<EncodedRuntimeTemplate> encodedTemplates
    ) -> Result<RecognitionRuntime>
    {
        std::ranges::sort(encodedTemplates, {}, &EncodedRuntimeTemplate::hash);
        for (auto index = std::size_t{1}; index < encodedTemplates.size(); ++index)
        {
            if (encodedTemplates[index - 1U].hash == encodedTemplates[index].hash)
            {
                return invalidRuntime(
                    std::format(
                        "duplicate encoded runtime template {}",
                        encodedTemplates[index].hash.toString()
                    )
                );
            }
        }

        auto expectedHashes = std::vector<ContentHash>{};
        expectedHashes.reserve(manifest.assets().size());
        for (auto const& asset : manifest.assets())
        {
            expectedHashes.emplace_back(asset.templateHash);
        }
        std::ranges::sort(expectedHashes);
        auto const uniqueEnd = std::ranges::unique(expectedHashes).begin();
        expectedHashes.erase(uniqueEnd, expectedHashes.end());
        if (expectedHashes.size() != encodedTemplates.size())
        {
            return invalidRuntime(
                std::format(
                    "runtime template closure requires {} unique assets but received {}",
                    expectedHashes.size(),
                    encodedTemplates.size()
                )
            );
        }

        auto templates = std::vector<StoredTemplate>{};
        templates.reserve(encodedTemplates.size());
        for (auto index = std::size_t{0}; index < encodedTemplates.size(); ++index)
        {
            auto& encoded       = encodedTemplates[index];
            auto const expected = expectedHashes[index];
            if (encoded.hash != expected)
            {
                return invalidRuntime(
                    std::format(
                        "runtime template closure expected {} but received {}",
                        expected.toString(),
                        encoded.hash.toString()
                    )
                );
            }

            UF_TRY_VALUE(actualHash, sha256(encoded.pngBytes));
            if (actualHash != encoded.hash)
            {
                return invalidRuntime(
                    std::format(
                        "runtime template {} does not match its content hash",
                        encoded.hash.toString()
                    )
                );
            }

            UF_TRY_VALUE(
                decodedTemplate,
                decodeTemplateImage(encoded.pngBytes, encoded.hash.toString())
            );
            templates.emplace_back(
                StoredTemplate{
                    .hash  = encoded.hash,
                    .image = std::move(decodedTemplate),
                }
            );
        }

        auto runtime = RecognitionRuntime{
            std::move(manifest),
            std::move(templates)
        };
        for (auto const& element : runtime.m_manifest.catalog().elements())
        {
            for (auto const& appearance : element.appearances())
            {
                auto const* p_asset = runtime.m_manifest.findAsset(
                    element.id(),
                    appearance.name
                );
                UF_CHECK(p_asset != nullptr);
                auto const* p_template = runtime.findTemplate(p_asset->templateHash);
                UF_CHECK(p_template != nullptr);
                if (
                    p_template->width != appearance.templateRect.width()
                    || p_template->height != appearance.templateRect.height()
                )
                {
                    return invalidRuntime(
                        std::format(
                            "runtime template {} dimensions {}x{} do not match appearance {} geometry {}x{}",
                            p_asset->templateHash.toString(),
                            p_template->width,
                            p_template->height,
                            appearance.name.value(),
                            appearance.templateRect.width(),
                            appearance.templateRect.height()
                        )
                    );
                }
            }
        }
        return runtime;
    }

    auto RecognitionRuntime::findTemplate(
        ContentHash const& hash
    ) const noexcept -> GrayTemplateImage const*
    {
        auto const found = std::ranges::lower_bound(
            m_templates,
            hash,
            {},
            &StoredTemplate::hash
        );
        if (found == m_templates.end() || found->hash != hash)
        {
            return nullptr;
        }
        return &found->image;
    }

    auto RecognitionRuntime::manifest() const noexcept -> RuntimeManifest const&
    {
        return m_manifest;
    }

    auto RecognitionRuntime::evaluateGrayPage(
        Frame const& frame,
        GrayImage const& grayFrame,
        RecognitionPolicy const& policy,
        SadSearchPoll const& poll
    ) const -> Result<PageRecognitionAttempt>
    {
        auto evaluations               = std::vector<AnchorEvaluation>{};
        auto completedEvidence         = std::vector<AnchorEvidence>{};
        auto completedPixelComparisons = uint64{0};
        auto const& catalog            = m_manifest.catalog();
        auto const anchorOrder         = catalog.pageAnchorOrder();
        evaluations.reserve(anchorOrder.size());
        completedEvidence.reserve(anchorOrder.size());

        for (auto const id : anchorOrder)
        {
            auto const* p_element = catalog.findElement(id);
            UF_CHECK(p_element != nullptr);

            auto const remainingBudget = checkedSubtract(
                policy.maximumPixelComparisons,
                completedPixelComparisons
            );
            UF_CHECK(remainingBudget.has_value());
            // No page is known yet -- that is exactly what lets one search
            // serve every page -- so identify never honours a pinned appearance
            // and always folds across all of them.
            UF_TRY_VALUE(
                attempt,
                matchElement(
                    grayFrame,
                    *p_element,
                    p_element->searchRoi(),
                    std::nullopt,
                    *remainingBudget,
                    poll
                )
            );
            auto const newCompleted = checkedAdd(
                completedPixelComparisons,
                attempt.completedPixelComparisons
            );
            UF_CHECK(newCompleted.has_value());
            completedPixelComparisons = *newCompleted;

            if (
                auto const* p_stop = std::get_if<SadSearchStopReason>(
                    &attempt.result
                )
            )
            {
                return PageRecognitionAttempt{
                    .result = PageRecognitionStop{
                        .elementId = id,
                        .reason    = *p_stop,
                    },
                    .completedAnchorEvidence   = std::move(completedEvidence),
                    .completedPixelComparisons = completedPixelComparisons,
                };
            }

            auto const& evidence = std::get<AnchorEvidence>(attempt.result);
            completedEvidence.emplace_back(evidence);
            evaluations.emplace_back(
                AnchorEvaluation::fromEvidence(evidence)
            );
        }

        UF_TRY_VALUE(
            pageOutcome,
            PageResolver::resolve(
                catalog,
                FrameIdentity::fromFrame(frame),
                evaluations
            )
        );
        return PageRecognitionAttempt{
            .result                    = std::move(pageOutcome),
            .completedAnchorEvidence   = std::move(completedEvidence),
            .completedPixelComparisons = completedPixelComparisons,
        };
    }

    auto RecognitionRuntime::ensureCompatibleFrame(
        Frame const& frame,
        ProjectFingerprint liveFingerprint
    ) const -> Status
    {
        return uf::ensureCompatibleFrame(
            frame,
            liveFingerprint,
            m_manifest.catalog().fingerprint()
        );
    }

    auto RecognitionRuntime::evaluatePage(
        Frame const& frame,
        ProjectFingerprint liveFingerprint,
        RecognitionPolicy const& policy
    ) const -> Result<PageRecognitionAttempt>
    {
        UF_TRY(ensureCompatibleFrame(frame, liveFingerprint));

        auto const poll = makeSadSearchPoll(policy);
        return withGrayFrame(
            frame,
            [this, &frame, &policy, &poll](
                GrayImage const& grayFrame
            ) -> Result<PageRecognitionAttempt>
            {
                return evaluateGrayPage(frame, grayFrame, policy, poll);
            }
        );
    }

    auto RecognitionRuntime::recognizePage(
        Frame const& frame,
        ProjectFingerprint liveFingerprint,
        RecognitionPolicy const& policy
    ) const -> Result<PageOutcome>
    {
        UF_TRY_VALUE(attempt, evaluatePage(frame, liveFingerprint, policy));
        if (
            auto const* p_stop = std::get_if<PageRecognitionStop>(
                &attempt.result
            )
        )
        {
            return pageRecognitionFailure(*p_stop);
        }
        return std::get<PageOutcome>(std::move(attempt.result));
    }

    auto RecognitionRuntime::matchElement(
        GrayImage const& grayFrame,
        CompiledElement const& element,
        PixelRect searchRoi,
        std::optional<ResourceName> const& pinnedAppearance,
        uint64 maximumPixelComparisons,
        SadSearchPoll const& poll
    ) const -> Result<ElementMatchAttempt>
    {
        auto completedPixelComparisons = uint64{0};
        auto best                      = std::optional<AnchorEvidence>{};
        for (auto const& appearance : element.appearances())
        {
            if (pinnedAppearance.has_value() && appearance.name != *pinnedAppearance)
            {
                continue;
            }

            auto const* p_asset = m_manifest.findAsset(element.id(), appearance.name);
            UF_CHECK(p_asset != nullptr);
            auto const* p_template = findTemplate(p_asset->templateHash);
            UF_CHECK(p_template != nullptr);

            auto const remainingBudget = checkedSubtract(
                maximumPixelComparisons,
                completedPixelComparisons
            );
            UF_CHECK(remainingBudget.has_value());
            UF_TRY_VALUE(
                sadReport,
                matchGrayTemplateImage(
                    grayFrame,
                    *p_template,
                    searchRoi,
                    *remainingBudget,
                    poll
                )
            );
            auto const newCompleted = checkedAdd(
                completedPixelComparisons,
                sadReport.completedPixelComparisons
            );
            UF_CHECK(newCompleted.has_value());
            completedPixelComparisons = *newCompleted;

            if (
                auto const* p_stop = std::get_if<SadSearchStopReason>(
                    &sadReport.outcome
                )
            )
            {
                return ElementMatchAttempt{
                    .result                    = *p_stop,
                    .completedPixelComparisons = completedPixelComparisons,
                };
            }

            UF_TRY_VALUE(
                evaluation,
                AnchorEvaluation::fromSadOutcome(
                    element,
                    appearance,
                    searchRoi,
                    sadReport.outcome
                )
            );
            auto const* p_evidence = std::get_if<AnchorEvidence>(
                &evaluation.evaluation()
            );
            UF_CHECK(p_evidence != nullptr);
            if (!best)
            {
                best = *p_evidence;
                continue;
            }
            UF_TRY_VALUE(better, strictlyBetter(*p_evidence, *best));
            if (better)
            {
                best = *p_evidence;
            }
        }

        UF_CHECK_MSG(
            best.has_value(),
            "matching an element requires at least one searchable appearance"
        );
        return ElementMatchAttempt{
            .result                    = *std::move(best),
            .completedPixelComparisons = completedPixelComparisons,
        };
    }

    auto RecognitionRuntime::evaluateActionTarget(
        Frame const& frame,
        ProjectFingerprint liveFingerprint,
        PageId pageId,
        ElementId elementId,
        RecognitionPolicy const& policy
    ) const -> Result<ActionTargetAttempt>
    {
        UF_TRY(ensureCompatibleFrame(frame, liveFingerprint));

        auto const& catalog      = m_manifest.catalog();
        auto const* p_element = catalog.findElement(elementId);
        if (p_element == nullptr)
        {
            return invalidRuntime(
                std::format(
                    "element {} is not present in the runtime catalog",
                    elementId.value().toString()
                )
            );
        }

        // Authorisation IS the reference, so a page that does not exercise
        // interact on this element has no action here to locate.
        auto const* p_reference = catalog.findReference(pageId, elementId);
        if (p_reference == nullptr || !p_reference->exercised.hasInteract())
        {
            return invalidRuntime(
                "the resolved page does not exercise interact on this element"
            );
        }

        auto const searchRoi = p_reference->searchRoi.value_or(
            p_element->searchRoi()
        );

        // No appearances means the page's own resolution located it: the
        // rectangle is where it was annotated, and there is nothing to match.
        if (p_element->appearances().empty())
        {
            return ActionTargetAttempt{
                .result                    = AnchorEvidence::locatedByPage(elementId, searchRoi),
                .completedPixelComparisons = uint64{0},
            };
        }

        auto const poll = makeSadSearchPoll(policy);
        return withGrayFrame(
            frame,
            [this, p_element, p_reference, searchRoi, &policy, &poll](
                GrayImage const& grayFrame
            ) -> Result<ActionTargetAttempt>
            {
                UF_TRY_VALUE(
                    attempt,
                    matchElement(
                        grayFrame,
                        *p_element,
                        searchRoi,
                        p_reference->appearance,
                        policy.maximumPixelComparisons,
                        poll
                    )
                );
                if (
                    auto const* p_stop = std::get_if<SadSearchStopReason>(
                        &attempt.result
                    )
                )
                {
                    return ActionTargetAttempt{
                        .result = PageRecognitionStop{
                            .elementId = p_element->id(),
                            .reason       = *p_stop,
                        },
                        .completedPixelComparisons = attempt.completedPixelComparisons,
                    };
                }
                return ActionTargetAttempt{
                    .result                    = std::get<AnchorEvidence>(std::move(attempt.result)),
                    .completedPixelComparisons = attempt.completedPixelComparisons,
                };
            }
        );
    }

    auto RecognitionRuntime::evaluateAppearance(
        Frame const& frame,
        ProjectFingerprint liveFingerprint,
        ElementId elementId,
        ResourceName const& appearance,
        PixelRect searchRoi,
        RecognitionPolicy const& policy
    ) const -> Result<ActionTargetAttempt>
    {
        UF_TRY(ensureCompatibleFrame(frame, liveFingerprint));

        auto const* p_element = m_manifest.catalog().findElement(elementId);
        if (p_element == nullptr)
        {
            return invalidRuntime(
                std::format(
                    "element {} is not present in the runtime catalog",
                    elementId.value().toString()
                )
            );
        }
        if (p_element->findAppearance(appearance) == nullptr)
        {
            return invalidRuntime(
                std::format(
                    "element {} declares no appearance \"{}\"",
                    elementId.value().toString(),
                    appearance.value()
                )
            );
        }

        auto const pinned = std::optional<ResourceName>{appearance};
        auto const poll   = makeSadSearchPoll(policy);
        return withGrayFrame(
            frame,
            [this, p_element, searchRoi, &pinned, &policy, &poll](
                GrayImage const& grayFrame
            ) -> Result<ActionTargetAttempt>
            {
                UF_TRY_VALUE(
                    attempt,
                    matchElement(
                        grayFrame,
                        *p_element,
                        searchRoi,
                        pinned,
                        policy.maximumPixelComparisons,
                        poll
                    )
                );
                if (
                    auto const* p_stop = std::get_if<SadSearchStopReason>(
                        &attempt.result
                    )
                )
                {
                    return ActionTargetAttempt{
                        .result = PageRecognitionStop{
                            .elementId = p_element->id(),
                            .reason       = *p_stop,
                        },
                        .completedPixelComparisons = attempt.completedPixelComparisons,
                    };
                }
                return ActionTargetAttempt{
                    .result                    = std::get<AnchorEvidence>(std::move(attempt.result)),
                    .completedPixelComparisons = attempt.completedPixelComparisons,
                };
            }
        );
    }

    auto resolveClickPixel(
        CompiledElement const& element,
        PixelRect const& matchedRect
    ) -> Result<PixelPoint>
    {
        auto const& interact = element.capabilities().interact();
        if (!interact.has_value())
        {
            return invalidRuntime(
                "only an element that interacts defines a click point"
            );
        }

        if (auto const offset = interact->clickOffset)
        {
            auto const clickX = checkedAdd(matchedRect.x(), offset->x());
            auto const clickY = checkedAdd(matchedRect.y(), offset->y());
            if (!clickX || !clickY)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "action_target click offset overflowed the matched rectangle"
                );
            }
            return PixelPoint{*clickX, *clickY};
        }

        // Integer division truncates toward zero, so the center resolves to one
        // reproducible pixel for both even and odd rectangle extents. The origin
        // plus half the extent cannot exceed the validated right/bottom edge, so
        // the sum stays inside uint32 without a checked add.
        auto const centerX = matchedRect.x() + matchedRect.width() / 2U;
        auto const centerY = matchedRect.y() + matchedRect.height() / 2U;
        return PixelPoint{centerX, centerY};
    }
}
