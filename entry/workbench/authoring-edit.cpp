#include "authoring-edit.hpp"

#include <core/error/contracts.hpp>
#include <core/safety/checked-access.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        [[nodiscard]]
        auto missingSource(annotation::SourceId id) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "source {} is not part of this draft",
                    id.value().toString()
                )
            );
        }
    }

    auto makeAuthoringDraft(
        annotation::AuthoringDocument const& document
    ) -> AuthoringDraft
    {
        auto sources = std::vector<EditableSource>{};
        sources.reserve(document.sources().size());
        for (auto const& source : document.sources())
        {
            sources.emplace_back(
                EditableSource{
                    .id          = source.id(),
                    .contentHash = source.contentHash(),
                    .fingerprint = source.fingerprint(),
                    .provenance  = source.provenance(),
                }
            );
        }

        // v2 native: elements and placements map straight across, one to one, with
        // no inversion. Element search regions stay on the element; page
        // membership stays on the placements below.
        auto recognizers = std::vector<EditableRecognizer>{};
        recognizers.reserve(document.elements().size());
        for (auto const& element : document.elements())
        {
            auto defaultClick = std::optional<EditableTemplateOffset>{};
            if (
                auto const* p_interactive = std::get_if<annotation::InteractiveElement>(
                    &element.kind()
                )
            )
            {
                if (auto const offset = p_interactive->clickOffset)
                {
                    defaultClick = EditableTemplateOffset{
                        .x = offset->x(),
                        .y = offset->y(),
                    };
                }
            }

            recognizers.emplace_back(
                EditableRecognizer{
                    .id                    = element.id(),
                    .name                  = element.name().value(),
                    .annotationType        = element.annotationType(),
                    .sourceId              = element.sourceId(),
                    .templateRect          = element.templateRect(),
                    .searchRoi             = element.searchRoi(),
                    .similarityBasisPoints = element.threshold().basisPoints(),
                    .defaultClick          = defaultClick,
                    .shared                = element.shared(),
                }
            );
        }

        auto placements = std::vector<EditablePlacement>{};
        placements.reserve(document.placements().size());
        for (auto const& placement : document.placements())
        {
            placements.emplace_back(
                EditablePlacement{
                    .pageId    = placement.pageId,
                    .elementId = placement.elementId,
                    .searchRoi = placement.searchRoi,
                }
            );
        }

        auto pages = std::vector<EditablePage>{};
        pages.reserve(document.catalog().pages().size());
        for (auto const& page : document.catalog().pages())
        {
            pages.emplace_back(
                EditablePage{
                    .id        = page.id(),
                    .name      = page.name().value(),
                    .required  = {page.required().begin(), page.required().end()},
                    .forbidden = {page.forbidden().begin(), page.forbidden().end()},
                }
            );
        }

        auto regressions = std::vector<EditableRegression>{};
        regressions.reserve(document.regressions().size());
        for (auto const& regression : document.regressions())
        {
            regressions.emplace_back(
                EditableRegression{
                    .id             = regression.id(),
                    .sourceId       = regression.sourceId(),
                    .classification = regression.classification(),
                    .expectation    = regression.expectation(),
                }
            );
        }

        return AuthoringDraft{
            .projectId   = document.catalog().projectId(),
            .fingerprint = document.catalog().fingerprint(),
            .sources     = std::move(sources),
            .recognizers = std::move(recognizers),
            .placements  = std::move(placements),
            .pages       = std::move(pages),
            .regressions = std::move(regressions),
        };
    }

    auto buildAuthoringDocument(
        AuthoringDraft const& draft
    ) -> Result<annotation::AuthoringDocument>
    {
        auto sources = std::vector<annotation::AuthoringSource>{};
        sources.reserve(draft.sources.size());
        for (auto const& source : draft.sources)
        {
            UF_TRY_VALUE(
                validated,
                annotation::AuthoringSource::create(
                    annotation::AuthoringSourceSpec{
                        .id          = source.id,
                        .contentHash = source.contentHash,
                        .fingerprint = source.fingerprint,
                        .provenance  = source.provenance,
                    }
                )
            );
            sources.emplace_back(std::move(validated));
        }

        // v2 native: elements and placements map straight across with no
        // inversion. A placement that references an anchor, or an interactive
        // element with none, is rejected by the document's own closure checks.
        auto elements = std::vector<annotation::Element>{};
        elements.reserve(draft.recognizers.size());
        for (auto const& recognizer : draft.recognizers)
        {
            UF_TRY_VALUE(
                name,
                annotation::ResourceName::create(recognizer.name)
            );
            UF_TRY_VALUE(
                threshold,
                annotation::SimilarityThreshold::create(
                    recognizer.similarityBasisPoints
                )
            );

            auto clickOffset = std::optional<annotation::TemplateOffset>{};
            if (recognizer.defaultClick)
            {
                UF_TRY_VALUE(
                    offset,
                    annotation::TemplateOffset::create(
                        recognizer.defaultClick->x,
                        recognizer.defaultClick->y,
                        recognizer.templateRect.width(),
                        recognizer.templateRect.height()
                    )
                );
                clickOffset = offset;
            }

            auto kind = annotation::ElementKind{annotation::AnchorElement{}};
            switch (recognizer.annotationType)
            {
            case annotation::AnnotationType::PageAnchor:
                kind = annotation::AnchorElement{};
                break;
            case annotation::AnnotationType::ActionTarget:
                kind = annotation::InteractiveElement{.clickOffset = clickOffset};
                break;
            case annotation::AnnotationType::InfoRegion:
                kind = annotation::InfoElement{};
                break;
            }

            UF_TRY_VALUE(
                element,
                annotation::Element::create(
                    draft.fingerprint,
                    annotation::Element::Spec{
                        .id           = recognizer.id,
                        .name         = std::move(name),
                        .sourceId     = recognizer.sourceId,
                        .templateRect = recognizer.templateRect,
                        .searchRoi    = recognizer.searchRoi,
                        .threshold    = threshold,
                        .kind         = std::move(kind),
                        .shared       = recognizer.shared,
                    }
                )
            );
            elements.emplace_back(std::move(element));
        }

        auto placements = std::vector<annotation::AuthoringPlacement>{};
        placements.reserve(draft.placements.size());
        for (auto const& placement : draft.placements)
        {
            placements.emplace_back(
                annotation::AuthoringPlacement{
                    .pageId    = placement.pageId,
                    .elementId = placement.elementId,
                    .searchRoi = placement.searchRoi,
                }
            );
        }

        auto pages = std::vector<annotation::PageSignature>{};
        pages.reserve(draft.pages.size());
        for (auto const& page : draft.pages)
        {
            UF_TRY_VALUE(name, annotation::ResourceName::create(page.name));
            UF_TRY_VALUE(
                validated,
                annotation::PageSignature::create(
                    annotation::PageSpec{
                        .id        = page.id,
                        .name      = std::move(name),
                        .required  = page.required,
                        .forbidden = page.forbidden,
                    }
                )
            );
            pages.emplace_back(std::move(validated));
        }

        auto regressions = std::vector<annotation::RegressionCase>{};
        regressions.reserve(draft.regressions.size());
        for (auto const& regression : draft.regressions)
        {
            regressions.emplace_back(
                annotation::RegressionSpec{
                    .id             = regression.id,
                    .sourceId       = regression.sourceId,
                    .classification = regression.classification,
                    .expectation    = regression.expectation,
                }
            );
        }

        return annotation::AuthoringDocument::create(
            draft.projectId,
            draft.fingerprint,
            std::move(sources),
            std::move(elements),
            std::move(pages),
            std::move(placements),
            std::move(regressions)
        );
    }

    auto freshAuthoringName(
        AuthoringDraft const& draft,
        std::string_view stem
    ) -> std::string
    {
        auto const taken = [&draft](std::string_view candidate)
        {
            return std::ranges::contains(
                       draft.recognizers,
                       candidate,
                       &EditableRecognizer::name
                   )
                || std::ranges::contains(
                       draft.pages,
                       candidate,
                       &EditablePage::name
                   );
        };

        auto const limit = draft.recognizers.size() + draft.pages.size() + 1U;
        auto candidate   = std::string{};
        for (auto index = std::size_t{1}; index <= limit; ++index)
        {
            candidate = std::format("{}_{}", stem, index);
            if (!taken(candidate))
            {
                break;
            }
        }
        return candidate;
    }

    auto createPageFromSource(
        AuthoringDraft draft,
        NewPageSpec const& spec
    ) -> Result<CreatedPage>
    {
        if (
            !std::ranges::contains(
                draft.sources,
                spec.sourceId,
                &EditableSource::id
            )
        )
        {
            return missingSource(spec.sourceId);
        }

        auto anchorName = freshAuthoringName(draft, "anchor");
        draft.recognizers.emplace_back(
            EditableRecognizer{
                .id                    = spec.anchorId,
                .name                  = anchorName,
                .annotationType        = annotation::AnnotationType::PageAnchor,
                .sourceId              = spec.sourceId,
                .templateRect          = spec.templateRect,
                .searchRoi             = spec.searchRoi,
                .similarityBasisPoints = spec.similarityBasisPoints,
                .defaultClick          = {},
            }
        );

        auto pageName = freshAuthoringName(draft, "page");
        draft.pages.emplace_back(
            EditablePage{
                .id        = spec.pageId,
                .name      = pageName,
                .required  = {spec.anchorId},
                .forbidden = {},
            }
        );

        // The case is what states "this screen is that page". Nothing else in the
        // document records it: an anchor names the page it identifies, but the
        // image the anchor happens to be drawn on is not the same claim.
        draft.regressions.emplace_back(
            EditableRegression{
                .id             = spec.regressionId,
                .sourceId       = spec.sourceId,
                .classification = annotation::RegressionClassification::Positive,
                .expectation    = annotation::ResolvedRegression{spec.pageId},
            }
        );

        return CreatedPage{
            .draft      = std::move(draft),
            .pageName   = std::move(pageName),
            .anchorName = std::move(anchorName),
        };
    }

    auto addPageMember(
        AuthoringDraft draft,
        PageMemberSpec const& spec
    ) -> Result<AddedPageMember>
    {
        if (
            !std::ranges::contains(
                draft.sources,
                spec.sourceId,
                &EditableSource::id
            )
        )
        {
            return missingSource(spec.sourceId);
        }

        auto const page = std::ranges::find(
            draft.pages,
            spec.pageId,
            &EditablePage::id
        );
        if (page == draft.pages.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "page {} is not part of this draft",
                    spec.pageId.value().toString()
                )
            );
        }

        auto const isAnchor = spec.kind == PageMemberKind::Anchor;
        auto const type     = [kind = spec.kind]
        {
            switch (kind)
            {
            case PageMemberKind::Anchor:
                return annotation::AnnotationType::PageAnchor;
            case PageMemberKind::ActionTarget:
                return annotation::AnnotationType::ActionTarget;
            case PageMemberKind::InfoRegion:
                return annotation::AnnotationType::InfoRegion;
            }
            UF_UNREACHABLE_MSG("unknown PageMemberKind value");
        }();
        auto const stem = [kind = spec.kind]() -> std::string_view
        {
            switch (kind)
            {
            case PageMemberKind::Anchor:
                return "anchor";
            case PageMemberKind::ActionTarget:
                return "region";
            case PageMemberKind::InfoRegion:
                return "info";
            }
            UF_UNREACHABLE_MSG("unknown PageMemberKind value");
        }();
        auto name = freshAuthoringName(draft, stem);
        draft.recognizers.emplace_back(
            EditableRecognizer{
                .id                    = spec.recognizerId,
                .name                  = name,
                .annotationType        = type,
                .sourceId              = spec.sourceId,
                .templateRect          = spec.templateRect,
                .searchRoi             = spec.searchRoi,
                .similarityBasisPoints = spec.similarityBasisPoints,
                .defaultClick          = {},
            }
        );
        // An anchor joins the page's signature; an interactive or info element
        // joins it through a placement carrying this page's own search region.
        if (isAnchor)
        {
            page->required.emplace_back(spec.recognizerId);
        }
        else
        {
            draft.placements.emplace_back(
                EditablePlacement{
                    .pageId    = spec.pageId,
                    .elementId = spec.recognizerId,
                    .searchRoi = spec.searchRoi,
                }
            );
        }

        return AddedPageMember{
            .draft = std::move(draft),
            .name  = std::move(name),
        };
    }

    auto pagesPlacedOn(
        AuthoringDraft const& draft,
        annotation::RecognizerId id
    ) -> std::vector<annotation::PageId>
    {
        auto pages = std::vector<annotation::PageId>{};
        for (auto const& placement : draft.placements)
        {
            if (
                placement.elementId == id
                && !std::ranges::contains(pages, placement.pageId)
            )
            {
                pages.emplace_back(placement.pageId);
            }
        }
        return pages;
    }

    auto duplicateElement(
        AuthoringDraft draft,
        DuplicateElementSpec const& spec
    ) -> Result<DuplicatedElement>
    {
        auto const origin = std::ranges::find(
            draft.recognizers,
            spec.sourceElementId,
            &EditableRecognizer::id
        );
        if (origin == draft.recognizers.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "recognizer {} is not part of this draft",
                    spec.sourceElementId.value().toString()
                )
            );
        }

        // Derived before the copy is inserted so the new name is measured
        // against the names already taken, the original's among them.
        auto name = freshAuthoringName(draft, origin->name);
        auto copy = *origin;
        copy.id   = spec.newElementId;
        copy.name = name;

        // Mirror the original's placements onto the copy, each retargeted to the
        // new id and keeping its own per-page search region. Collected before the
        // recognizer is appended so a reallocation cannot invalidate the read.
        auto placements = std::vector<EditablePlacement>{};
        for (auto const& placement : draft.placements)
        {
            if (placement.elementId == spec.sourceElementId)
            {
                placements.emplace_back(
                    EditablePlacement{
                        .pageId    = placement.pageId,
                        .elementId = spec.newElementId,
                        .searchRoi = placement.searchRoi,
                    }
                );
            }
        }

        draft.recognizers.emplace_back(std::move(copy));
        draft.placements.insert(
            draft.placements.end(),
            placements.begin(),
            placements.end()
        );

        return DuplicatedElement{
            .draft = std::move(draft),
            .name  = std::move(name),
        };
    }

    auto setRegionShared(
        AuthoringDraft draft,
        annotation::RecognizerId id,
        bool shared
    ) -> Result<AuthoringDraft>
    {
        auto const target = std::ranges::find(
            draft.recognizers,
            id,
            &EditableRecognizer::id
        );
        if (target == draft.recognizers.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "recognizer {} is not part of this draft",
                    id.value().toString()
                )
            );
        }
        if (target->annotationType != annotation::AnnotationType::ActionTarget)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "\"{}\" is not an interactive region; only those are reused "
                    "across pages",
                    target->name
                )
            );
        }

        // v2: the mark is pure intent and never touches page membership, so a
        // region placed on several pages can be unmarked freely -- there are no
        // copies to keep linked, only one element placed N times.
        target->shared = shared;
        return draft;
    }

    auto shareRegionOnPage(
        AuthoringDraft draft,
        SharedRegionSpec const& spec
    ) -> Result<SharedRegionPlacement>
    {
        auto const origin = std::ranges::find(
            draft.recognizers,
            spec.elementId,
            &EditableRecognizer::id
        );
        if (origin == draft.recognizers.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "recognizer {} is not part of this draft",
                    spec.elementId.value().toString()
                )
            );
        }
        if (origin->annotationType != annotation::AnnotationType::ActionTarget)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "\"{}\" is not an interactive region; only those are shared "
                    "across pages",
                    origin->name
                )
            );
        }
        if (
            !std::ranges::contains(draft.pages, spec.pageId, &EditablePage::id)
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "page {} is not part of this draft",
                    spec.pageId.value().toString()
                )
            );
        }

        auto const alreadyThere = std::ranges::any_of(
            draft.placements,
            [&spec](EditablePlacement const& placement)
            {
                return placement.elementId == spec.elementId
                    && placement.pageId == spec.pageId;
            }
        );
        if (alreadyThere)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "\"{}\" is already on that page",
                    origin->name
                )
            );
        }

        // One element, placed again. No recognizer copy is minted -- a later
        // template edit touches this element once and every placement sees it.
        // Reaching a second page is what being shared means, so the element is
        // marked whether or not the author ticked the box first.
        origin->shared = true;
        auto name      = origin->name;
        draft.placements.emplace_back(
            EditablePlacement{
                .pageId    = spec.pageId,
                .elementId = spec.elementId,
                .searchRoi = spec.searchRoi,
            }
        );

        return SharedRegionPlacement{
            .draft = std::move(draft),
            .name  = std::move(name),
        };
    }

    auto setElementTemplateRect(
        AuthoringDraft draft,
        annotation::RecognizerId id,
        PixelRect templateRect
    ) -> Result<RetemplatedRegion>
    {
        auto const target = std::ranges::find(
            draft.recognizers,
            id,
            &EditableRecognizer::id
        );
        if (target == draft.recognizers.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "recognizer {} is not part of this draft",
                    id.value().toString()
                )
            );
        }
        if (
            templateRect.width() > target->searchRoi.width()
            || templateRect.height() > target->searchRoi.height()
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "the new template does not fit the range \"{}\" searches; "
                    "widen that range first",
                    target->name
                )
            );
        }

        // Every placement of this element keeps its own per-page search region,
        // so the moved template must still fit each one.
        auto placedOn = std::size_t{0};
        for (auto const& placement : draft.placements)
        {
            if (placement.elementId != id)
            {
                continue;
            }
            ++placedOn;
            if (
                templateRect.width() > placement.searchRoi.width()
                || templateRect.height() > placement.searchRoi.height()
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "the new template does not fit the range \"{}\" searches "
                        "on one of its pages; widen that range first",
                        target->name
                    )
                );
            }
        }

        target->templateRect = templateRect;
        return RetemplatedRegion{
            .draft           = std::move(draft),
            .otherPlacements = placedOn,
        };
    }

    auto claimScreenForPage(
        AuthoringDraft draft,
        ScreenClaimSpec const& spec
    ) -> Result<AuthoringDraft>
    {
        if (
            !std::ranges::contains(
                draft.sources,
                spec.sourceId,
                &EditableSource::id
            )
        )
        {
            return missingSource(spec.sourceId);
        }
        if (
            !std::ranges::contains(draft.pages, spec.pageId, &EditablePage::id)
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "page {} is not part of this draft",
                    spec.pageId.value().toString()
                )
            );
        }

        auto const expectation = annotation::RegressionExpectation{
            annotation::ResolvedRegression{spec.pageId},
        };
        auto const existing = std::ranges::find(
            draft.regressions,
            spec.sourceId,
            &EditableRegression::sourceId
        );
        if (existing != draft.regressions.end())
        {
            existing->expectation = expectation;
            return draft;
        }

        draft.regressions.emplace_back(
            EditableRegression{
                .id             = spec.regressionId,
                .sourceId       = spec.sourceId,
                .classification = annotation::RegressionClassification::Positive,
                .expectation    = expectation,
            }
        );
        return draft;
    }

    auto recordScreenExpectation(
        AuthoringDraft draft,
        ScreenExpectationSpec const& spec
    ) -> Result<AuthoringDraft>
    {
        if (
            !std::ranges::contains(
                draft.sources,
                spec.sourceId,
                &EditableSource::id
            )
        )
        {
            return missingSource(spec.sourceId);
        }

        struct Recorded final
        {
            annotation::RegressionExpectation    expectation;
            annotation::RegressionClassification classification{};
        };
        auto const recorded = [&]() -> Recorded
        {
            switch (spec.expectation)
            {
            case PagelessExpectation::Unknown:
                return Recorded{
                    .expectation    = annotation::UnknownRegression{},
                    .classification = annotation::RegressionClassification::Negative,
                };
            case PagelessExpectation::Ambiguous:
                return Recorded{
                    .expectation    = annotation::AmbiguousRegression{},
                    .classification = annotation::RegressionClassification::Confusable,
                };
            }
            UF_UNREACHABLE_MSG("Unknown PagelessExpectation value");
        }();

        auto const existing = std::ranges::find(
            draft.regressions,
            spec.sourceId,
            &EditableRegression::sourceId
        );
        if (existing != draft.regressions.end())
        {
            existing->classification = recorded.classification;
            existing->expectation    = recorded.expectation;
            return draft;
        }

        draft.regressions.emplace_back(
            EditableRegression{
                .id             = spec.regressionId,
                .sourceId       = spec.sourceId,
                .classification = recorded.classification,
                .expectation    = recorded.expectation,
            }
        );
        return draft;
    }

    auto retypeRecognizer(
        AuthoringDraft draft,
        annotation::RecognizerId id,
        annotation::AnnotationType type
    ) -> Result<RetypedRecognizer>
    {
        auto const target = std::ranges::find(
            draft.recognizers,
            id,
            &EditableRecognizer::id
        );
        if (target == draft.recognizers.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "recognizer {} is not part of this draft",
                    id.value().toString()
                )
            );
        }
        if (target->annotationType == type)
        {
            return RetypedRecognizer{.draft = std::move(draft)};
        }

        // Only a page anchor may fill a required or forbidden role, so leaving
        // the page anchor type withdraws this recognizer from every signature. A
        // signature naming nothing else cannot survive that withdrawal, and the
        // author has to resolve which anchor takes over.
        auto withdrawnRoles = std::size_t{0};
        auto anchoredPages  = std::vector<annotation::PageId>{};
        if (target->annotationType == annotation::AnnotationType::PageAnchor)
        {
            for (auto const& page : draft.pages)
            {
                auto const named = (
                    std::ranges::contains(page.required, id)
                    || std::ranges::contains(page.forbidden, id)
                );
                auto const alone = (
                    page.required.size() + page.forbidden.size() == 1U
                );
                if (named && alone)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "\"{}\" is the only recognizer page \"{}\" names; "
                            "give that page another page anchor before changing "
                            "this recognizer's type",
                            target->name,
                            page.name
                        )
                    );
                }
                // Remembered before the withdrawal: a page this recognizer was
                // required on is where it demonstrably appears, which is a far
                // better authorization to offer than an arbitrary page. A page
                // it was forbidden on is the opposite and is never offered.
                if (std::ranges::contains(page.required, id))
                {
                    anchoredPages.emplace_back(page.id);
                }
            }
            for (auto& page : draft.pages)
            {
                auto const before = page.required.size() + page.forbidden.size();
                std::erase(page.required, id);
                std::erase(page.forbidden, id);
                withdrawnRoles += (
                    before - (page.required.size() + page.forbidden.size())
                );
            }
        }

        auto const wasAnchor = (
            target->annotationType == annotation::AnnotationType::PageAnchor
        );
        auto authorizedPage        = std::optional<annotation::PageId>{};
        auto clearedAuthorizations = std::size_t{0};
        auto clearedClick          = false;
        if (type == annotation::AnnotationType::ActionTarget)
        {
            // The closure rule: an interactive element must be placed on at least
            // one page. An element already placed (an info region keeps its
            // placements across the change) needs nothing; otherwise it is placed
            // on the page it is already on -- a page it anchored, or the project's
            // first page when it was an anchor with only a forbidden role.
            auto const alreadyPlaced = std::ranges::any_of(
                draft.placements,
                [id](EditablePlacement const& placement)
                {
                    return placement.elementId == id;
                }
            );
            if (!alreadyPlaced)
            {
                auto candidate = std::optional<annotation::PageId>{};
                if (!anchoredPages.empty())
                {
                    candidate = anchoredPages.front();
                }
                else if (wasAnchor && !draft.pages.empty())
                {
                    candidate = draft.pages.front().id;
                }
                if (!candidate.has_value())
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "\"{}\" is on no page; an interactive region must be "
                            "placed on at least one, so add it to a page before "
                            "making it interactive",
                            target->name
                        )
                    );
                }
                draft.placements.emplace_back(
                    EditablePlacement{
                        .pageId    = *candidate,
                        .elementId = id,
                        .searchRoi = target->searchRoi,
                    }
                );
                authorizedPage = candidate;
            }
        }
        else
        {
            // Only an interactive element may carry a default click.
            clearedClick = target->defaultClick.has_value();
            target->defaultClick.reset();
            if (type == annotation::AnnotationType::PageAnchor)
            {
                // An anchor joins a page through its signature and cannot be
                // placed, so every placement of it is withdrawn. An info region
                // is placeable, so its placements are kept.
                auto const before = draft.placements.size();
                std::erase_if(
                    draft.placements,
                    [id](EditablePlacement const& placement)
                    {
                        return placement.elementId == id;
                    }
                );
                clearedAuthorizations = before - draft.placements.size();
            }
        }
        target->annotationType = type;

        return RetypedRecognizer{
            .draft                 = std::move(draft),
            .authorizedPage        = authorizedPage,
            .withdrawnRoles        = withdrawnRoles,
            .clearedAuthorizations = clearedAuthorizations,
            .clearedClick          = clearedClick,
        };
    }

    auto deleteRecognizer(
        AuthoringDraft draft,
        annotation::RecognizerId id
    ) -> Result<DeletedEntity>
    {
        auto const target = std::ranges::find(
            draft.recognizers,
            id,
            &EditableRecognizer::id
        );
        if (target == draft.recognizers.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "recognizer {} is not part of this draft",
                    id.value().toString()
                )
            );
        }

        for (auto const& page : draft.pages)
        {
            auto const named = (
                std::ranges::contains(page.required, id)
                || std::ranges::contains(page.forbidden, id)
            );
            auto const alone = (
                page.required.size() + page.forbidden.size() == 1U
            );
            if (named && alone)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "\"{}\" is the only recognizer page \"{}\" names; "
                        "delete that page first, or give it another page anchor",
                        target->name,
                        page.name
                    )
                );
            }
        }

        auto withdrawnRoles = std::size_t{0};
        for (auto& page : draft.pages)
        {
            auto const before = page.required.size() + page.forbidden.size();
            std::erase(page.required, id);
            std::erase(page.forbidden, id);
            withdrawnRoles += (
                before - (page.required.size() + page.forbidden.size())
            );
        }
        // The element's own placements go with it: they name a page for a
        // recognizer that no longer exists.
        std::erase_if(
            draft.placements,
            [id](EditablePlacement const& placement)
            {
                return placement.elementId == id;
            }
        );
        draft.recognizers.erase(target);

        return DeletedEntity{
            .draft          = std::move(draft),
            .withdrawnRoles = withdrawnRoles,
        };
    }

    auto deletePage(
        AuthoringDraft draft,
        annotation::PageId id
    ) -> Result<DeletedEntity>
    {
        auto const target = std::ranges::find(
            draft.pages,
            id,
            &EditablePage::id
        );
        if (target == draft.pages.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "page {} is not part of this draft",
                    id.value().toString()
                )
            );
        }

        // An interactive element placed only on this page would be left placed
        // nowhere, which the closure rule forbids; that is a choice between
        // deleting it and re-placing it that only the author can make. An info
        // region may be left unplaced, so its placement does not block deletion.
        for (auto const& recognizer : draft.recognizers)
        {
            if (
                recognizer.annotationType
                != annotation::AnnotationType::ActionTarget
            )
            {
                continue;
            }
            auto const onThisPage = std::ranges::any_of(
                draft.placements,
                [&recognizer, id](EditablePlacement const& placement)
                {
                    return placement.elementId == recognizer.id
                        && placement.pageId == id;
                }
            );
            auto const onOtherPage = std::ranges::any_of(
                draft.placements,
                [&recognizer, id](EditablePlacement const& placement)
                {
                    return placement.elementId == recognizer.id
                        && placement.pageId != id;
                }
            );
            if (onThisPage && !onOtherPage)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "page \"{}\" is the only page interactive region \"{}\" is "
                        "placed on; place it elsewhere or delete it first",
                        target->name,
                        recognizer.name
                    )
                );
            }
        }

        auto const placementsBefore = draft.placements.size();
        std::erase_if(
            draft.placements,
            [id](EditablePlacement const& placement)
            {
                return placement.pageId == id;
            }
        );
        auto const clearedAuthorizations = (
            placementsBefore - draft.placements.size()
        );

        // A case expecting this page to resolve cannot be reclassified into
        // anything the author meant, so it goes with the page rather than
        // blocking the deletion. Every page created from a captured screen owns
        // one, so refusing here would make such a page undeletable.
        auto const removedRegressions = std::erase_if(
            draft.regressions,
            [id](EditableRegression const& regression)
            {
                auto const* p_resolved = std::get_if<annotation::ResolvedRegression>(
                    &regression.expectation
                );
                return p_resolved != nullptr && p_resolved->pageId == id;
            }
        );
        draft.pages.erase(target);

        return DeletedEntity{
            .draft                 = std::move(draft),
            .clearedAuthorizations = clearedAuthorizations,
            .removedRegressions    = removedRegressions,
        };
    }

    auto deleteSource(
        AuthoringDraft draft,
        annotation::SourceId id
    ) -> Result<DeletedEntity>
    {
        auto const target = std::ranges::find(
            draft.sources,
            id,
            &EditableSource::id
        );
        if (target == draft.sources.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "source {} is not part of this draft",
                    id.value().toString()
                )
            );
        }

        auto const authored = std::ranges::count(
            draft.recognizers,
            id,
            &EditableRecognizer::sourceId
        );
        if (authored > 0)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "{} recognizer{} authored on this source; delete {} first",
                    authored,
                    authored == 1 ? " is" : "s are",
                    authored == 1 ? "it" : "them"
                )
            );
        }

        auto const before = draft.regressions.size();
        std::erase_if(
            draft.regressions,
            [id](EditableRegression const& regression) -> bool
            {
                return regression.sourceId == id;
            }
        );
        auto const removedRegressions = before - draft.regressions.size();
        draft.sources.erase(target);

        return DeletedEntity{
            .draft              = std::move(draft),
            .removedRegressions = removedRegressions,
        };
    }

    AuthoringEditHistory::AuthoringEditHistory(
        annotation::AuthoringDocument document
    )
        : m_current{std::move(document)}
    {
    }

    auto AuthoringEditHistory::document() const noexcept
        -> annotation::AuthoringDocument const&
    {
        return m_current;
    }

    auto AuthoringEditHistory::draft() const -> AuthoringDraft
    {
        return makeAuthoringDraft(m_current);
    }

    auto AuthoringEditHistory::canUndo() const noexcept -> bool
    {
        return !m_undo.empty();
    }

    auto AuthoringEditHistory::canRedo() const noexcept -> bool
    {
        return !m_redo.empty();
    }

    auto AuthoringEditHistory::revision() const noexcept -> uint64
    {
        return m_revision;
    }

    auto AuthoringEditHistory::apply(
        AuthoringDraft const& draft
    ) -> Result<bool>
    {
        UF_TRY_VALUE(next, buildAuthoringDocument(draft));
        if (
            annotation::serializeAuthoringDocument(next)
            == annotation::serializeAuthoringDocument(m_current)
        )
        {
            return false;
        }

        if (m_undo.size() == k_maximumAuthoringUndoEntries)
        {
            m_undo.erase(m_undo.begin());
        }
        m_undo.emplace_back(std::move(m_current));
        m_current = std::move(next);
        m_redo.clear();
        ++m_revision;
        return true;
    }

    auto AuthoringEditHistory::undo() -> bool
    {
        if (m_undo.empty()) return false;

        m_redo.emplace_back(std::move(m_current));
        m_current = std::move(m_undo.back());
        m_undo.pop_back();
        ++m_revision;
        return true;
    }

    auto AuthoringEditHistory::redo() -> bool
    {
        if (m_redo.empty()) return false;

        m_undo.emplace_back(std::move(m_current));
        m_current = std::move(m_redo.back());
        m_redo.pop_back();
        ++m_revision;
        return true;
    }
}
