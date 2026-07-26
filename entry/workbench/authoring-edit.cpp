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

        // The draft stays v1-shaped in this phase: each EditableRecognizer keeps
        // its own allowedPageIds so panels, EditPage, and page-view need no
        // change. Here that membership is derived from the document's placements
        // rather than from an inverted field on the element.
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

            auto allowedPageIds = std::vector<annotation::PageId>{};
            for (auto const& placement : document.placements())
            {
                if (placement.elementId == element.id())
                {
                    allowedPageIds.emplace_back(placement.pageId);
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
                    .allowedPageIds        = std::move(allowedPageIds),
                    .shared                = element.shared(),
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

        // Invert the draft's per-recognizer allowedPageIds back into page-side
        // placements. An anchor with a stray authorization becomes a placement
        // that references it, which the document rejects -- the same failure the
        // v1 catalog produced for a page anchor carrying allowed_page_ids.
        auto elements   = std::vector<annotation::Element>{};
        auto placements = std::vector<annotation::AuthoringPlacement>{};
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
            for (auto const pageId : recognizer.allowedPageIds)
            {
                placements.emplace_back(
                    annotation::AuthoringPlacement{
                        .pageId    = pageId,
                        .elementId = recognizer.id,
                        .searchRoi = recognizer.searchRoi,
                    }
                );
            }
            elements.emplace_back(std::move(element));
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
                .allowedPageIds        = {},
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
        auto name = freshAuthoringName(draft, isAnchor ? "anchor" : "region");
        draft.recognizers.emplace_back(
            EditableRecognizer{
                .id   = spec.recognizerId,
                .name = name,
                .annotationType = isAnchor
                    ? annotation::AnnotationType::PageAnchor
                    : annotation::AnnotationType::ActionTarget,
                .sourceId              = spec.sourceId,
                .templateRect          = spec.templateRect,
                .searchRoi             = spec.searchRoi,
                .similarityBasisPoints = spec.similarityBasisPoints,
                .defaultClick          = {},
                .allowedPageIds        = isAnchor
                    ? std::vector<annotation::PageId>{}
                    : std::vector<annotation::PageId>{spec.pageId},
            }
        );
        if (isAnchor)
        {
            page->required.emplace_back(spec.recognizerId);
        }

        return AddedPageMember{
            .draft = std::move(draft),
            .name  = std::move(name),
        };
    }

    auto sharedRegionMembers(
        AuthoringDraft const& draft,
        annotation::RecognizerId id
    ) -> std::vector<annotation::RecognizerId>
    {
        auto members = std::vector<annotation::RecognizerId>{};
        auto const origin = std::ranges::find(
            draft.recognizers,
            id,
            &EditableRecognizer::id
        );
        if (origin == draft.recognizers.end())
        {
            return members;
        }
        // An unmarked region is a group of one. Nothing else can be reusing its
        // pixels, because reuse only happens through a marked element, and
        // returning it keeps a plain template drag on the same path as a shared
        // one.
        if (!origin->shared)
        {
            members.emplace_back(origin->id);
            return members;
        }
        for (auto const& recognizer : draft.recognizers)
        {
            if (
                recognizer.shared
                && recognizer.sourceId == origin->sourceId
                && recognizer.templateRect == origin->templateRect
                && recognizer.annotationType == origin->annotationType
            )
            {
                members.emplace_back(recognizer.id);
            }
        }
        return members;
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

        auto const members = sharedRegionMembers(draft, id);
        if (!shared && members.size() > 1U)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "\"{}\" is still used on {} pages; remove it from the others "
                    "before making it page-local again",
                    target->name,
                    members.size()
                )
            );
        }

        for (auto const& memberId : members)
        {
            auto const member = std::ranges::find(
                draft.recognizers,
                memberId,
                &EditableRecognizer::id
            );
            UF_CHECK(member != draft.recognizers.end());
            member->shared = shared;
        }
        return draft;
    }

    auto shareRegionOnPage(
        AuthoringDraft draft,
        SharedRegionSpec const& spec
    ) -> Result<AddedPageMember>
    {
        auto const origin = std::ranges::find(
            draft.recognizers,
            spec.shareFrom,
            &EditableRecognizer::id
        );
        if (origin == draft.recognizers.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "recognizer {} is not part of this draft",
                    spec.shareFrom.value().toString()
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
            draft.recognizers,
            [&origin, &spec](EditableRecognizer const& recognizer)
            {
                return recognizer.sourceId == origin->sourceId
                    && recognizer.templateRect == origin->templateRect
                    && recognizer.annotationType == origin->annotationType
                    && std::ranges::contains(
                           recognizer.allowedPageIds,
                           spec.pageId
                       );
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

        // Everything origin is needed for happens here, before the append: a
        // vector append may reallocate, and every iterator into it dies with the
        // old buffer. Reuse is what being shared means, so an element that
        // reaches a second page is marked whether or not the author ticked the
        // box first.
        origin->shared        = true;
        auto const templateRect = origin->templateRect;
        auto const sourceId     = origin->sourceId;
        auto const threshold    = origin->similarityBasisPoints;
        auto const defaultClick = origin->defaultClick;
        auto const originName   = origin->name;

        // Named after the element and the page it lands on, rather than the next
        // free "region_N". The copy is that element on that page, and a generated
        // number says neither -- which is what left a renamed element paired with
        // a stale one the first time this existed.
        auto const page = std::ranges::find(
            draft.pages,
            spec.pageId,
            &EditablePage::id
        );
        UF_CHECK(page != draft.pages.end());
        auto name = freshAuthoringName(
            draft,
            std::format("{}_{}", originName, page->name)
        );
        draft.recognizers.emplace_back(
            EditableRecognizer{
                .id                    = spec.recognizerId,
                .name                  = name,
                .annotationType        = annotation::AnnotationType::ActionTarget,
                .sourceId              = sourceId,
                .templateRect          = templateRect,
                .searchRoi             = spec.searchRoi,
                .similarityBasisPoints = threshold,
                .defaultClick          = defaultClick,
                .allowedPageIds        = {spec.pageId},
                .shared                = true,
            }
        );

        return AddedPageMember{
            .draft = std::move(draft),
            .name  = std::move(name),
        };
    }

    auto retemplateSharedRegion(
        AuthoringDraft draft,
        annotation::RecognizerId id,
        PixelRect templateRect
    ) -> Result<RetemplatedRegion>
    {
        auto const members = sharedRegionMembers(draft, id);
        if (members.empty())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "recognizer {} is not part of this draft",
                    id.value().toString()
                )
            );
        }

        for (auto const& memberId : members)
        {
            auto const member = std::ranges::find(
                draft.recognizers,
                memberId,
                &EditableRecognizer::id
            );
            UF_CHECK(member != draft.recognizers.end());
            if (
                templateRect.width() > member->searchRoi.width()
                || templateRect.height() > member->searchRoi.height()
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "the new template does not fit the range \"{}\" searches; "
                        "widen that range first",
                        member->name
                    )
                );
            }
        }

        for (auto const& memberId : members)
        {
            auto const member = std::ranges::find(
                draft.recognizers,
                memberId,
                &EditableRecognizer::id
            );
            UF_CHECK(member != draft.recognizers.end());
            member->templateRect = templateRect;
        }

        return RetemplatedRegion{
            .draft = std::move(draft),
            // The recognizer being dragged is not something the author needs
            // reporting back to them; the others are.
            .movedMembers = members.size() - 1U,
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

        auto authorizedPage        = std::optional<annotation::PageId>{};
        auto clearedAuthorizations = std::size_t{0};
        auto clearedClick          = false;
        if (type == annotation::AnnotationType::ActionTarget)
        {
            // An action target must authorize at least one page. Authorize
            // exactly one rather than every page: an action target is acted on
            // only where it is authorized, so widening that reach stays the
            // author's explicit decision.
            if (target->allowedPageIds.empty())
            {
                if (draft.pages.empty())
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "an action target must authorize at least one page and "
                        "this project has none; add a page first"
                    );
                }
                authorizedPage = anchoredPages.empty()
                    ? draft.pages.front().id
                    : anchoredPages.front();
                target->allowedPageIds.emplace_back(*authorizedPage);
            }
        }
        else
        {
            // Only an action target may carry a default click.
            clearedClick = target->defaultClick.has_value();
            target->defaultClick.reset();
            if (type == annotation::AnnotationType::PageAnchor)
            {
                // A page anchor's membership is its signature role, never an
                // authorization it holds itself.
                clearedAuthorizations = target->allowedPageIds.size();
                target->allowedPageIds.clear();
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

        for (auto const& recognizer : draft.recognizers)
        {
            auto const onlyAuthorization = (
                recognizer.annotationType == annotation::AnnotationType::ActionTarget
                && recognizer.allowedPageIds.size() == 1U
                && std::ranges::contains(recognizer.allowedPageIds, id)
            );
            if (onlyAuthorization)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "page \"{}\" is the only page action target \"{}\" is "
                        "authorized on; authorize it elsewhere or delete it first",
                        target->name,
                        recognizer.name
                    )
                );
            }
        }

        auto clearedAuthorizations = std::size_t{0};
        for (auto& recognizer : draft.recognizers)
        {
            auto const before = recognizer.allowedPageIds.size();
            std::erase(recognizer.allowedPageIds, id);
            clearedAuthorizations += before - recognizer.allowedPageIds.size();
        }

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
