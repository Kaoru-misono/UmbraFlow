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
                    .m_id          = source.id(),
                    .m_contentHash = source.contentHash(),
                    .m_fingerprint = source.fingerprint(),
                    .m_provenance  = source.provenance(),
                }
            );
        }

        auto const definitions   = document.catalog().recognizers();
        auto const relationships = document.recognizerSources();
        UF_CHECK(definitions.size() == relationships.size());
        auto recognizers = std::vector<EditableRecognizer>{};
        recognizers.reserve(definitions.size());
        for (auto index = std::size_t{0}; index < definitions.size(); ++index)
        {
            auto const& definition   = checkedAt(definitions, index);
            auto const& relationship = checkedAt(relationships, index);
            UF_CHECK(definition.id() == relationship.m_recognizerId);

            auto defaultClick = std::optional<EditableTemplateOffset>{};
            if (auto const offset = definition.defaultClick())
            {
                defaultClick = EditableTemplateOffset{
                    .m_x = offset->x(),
                    .m_y = offset->y(),
                };
            }
            recognizers.emplace_back(
                EditableRecognizer{
                    .m_id                    = definition.id(),
                    .m_name                  = definition.name().value(),
                    .m_annotationType        = definition.annotationType(),
                    .m_sourceId              = relationship.m_sourceId,
                    .m_templateRect          = definition.templateRect(),
                    .m_searchRoi             = definition.searchRoi(),
                    .m_similarityBasisPoints = definition.threshold().basisPoints(),
                    .m_defaultClick          = defaultClick,
                    .m_allowedPageIds = {
                        definition.allowedPageIds().begin(),
                        definition.allowedPageIds().end(),
                    },
                    .m_shared = relationship.m_shared,
                }
            );
        }

        auto pages = std::vector<EditablePage>{};
        pages.reserve(document.catalog().pages().size());
        for (auto const& page : document.catalog().pages())
        {
            pages.emplace_back(
                EditablePage{
                    .m_id        = page.id(),
                    .m_name      = page.name().value(),
                    .m_required  = {page.required().begin(), page.required().end()},
                    .m_forbidden = {page.forbidden().begin(), page.forbidden().end()},
                }
            );
        }

        auto regressions = std::vector<EditableRegression>{};
        regressions.reserve(document.regressions().size());
        for (auto const& regression : document.regressions())
        {
            regressions.emplace_back(
                EditableRegression{
                    .m_id             = regression.id(),
                    .m_sourceId       = regression.sourceId(),
                    .m_classification = regression.classification(),
                    .m_expectation    = regression.expectation(),
                }
            );
        }

        return AuthoringDraft{
            .m_projectId   = document.catalog().projectId(),
            .m_fingerprint = document.catalog().fingerprint(),
            .m_sources     = std::move(sources),
            .m_recognizers = std::move(recognizers),
            .m_pages       = std::move(pages),
            .m_regressions = std::move(regressions),
        };
    }

    auto buildAuthoringDocument(
        AuthoringDraft const& draft
    ) -> Result<annotation::AuthoringDocument>
    {
        auto sources = std::vector<annotation::AuthoringSource>{};
        sources.reserve(draft.m_sources.size());
        for (auto const& source : draft.m_sources)
        {
            UF_TRY_VALUE(
                validated,
                annotation::AuthoringSource::create(
                    annotation::AuthoringSourceSpec{
                        .m_id          = source.m_id,
                        .m_contentHash = source.m_contentHash,
                        .m_fingerprint = source.m_fingerprint,
                        .m_provenance  = source.m_provenance,
                    }
                )
            );
            sources.emplace_back(std::move(validated));
        }

        auto recognizers = std::vector<annotation::AuthoringRecognizerSpec>{};
        recognizers.reserve(draft.m_recognizers.size());
        for (auto const& recognizer : draft.m_recognizers)
        {
            UF_TRY_VALUE(
                name,
                annotation::ResourceName::create(recognizer.m_name)
            );
            UF_TRY_VALUE(
                threshold,
                annotation::SimilarityThreshold::create(
                    recognizer.m_similarityBasisPoints
                )
            );

            auto defaultClick = std::optional<annotation::TemplateOffset>{};
            if (recognizer.m_defaultClick)
            {
                UF_TRY_VALUE(
                    offset,
                    annotation::TemplateOffset::create(
                        recognizer.m_defaultClick->m_x,
                        recognizer.m_defaultClick->m_y,
                        recognizer.m_templateRect.width(),
                        recognizer.m_templateRect.height()
                    )
                );
                defaultClick = offset;
            }

            UF_TRY_VALUE(
                definition,
                annotation::RecognizerDefinition::create(
                    draft.m_fingerprint,
                    annotation::RecognizerSpec{
                        .m_id             = recognizer.m_id,
                        .m_name           = std::move(name),
                        .m_annotationType = recognizer.m_annotationType,
                        .m_templateRect   = recognizer.m_templateRect,
                        .m_searchRoi      = recognizer.m_searchRoi,
                        .m_threshold      = threshold,
                        .m_defaultClick   = defaultClick,
                        .m_allowedPageIds = recognizer.m_allowedPageIds,
                    }
                )
            );
            recognizers.emplace_back(
                annotation::AuthoringRecognizerSpec{
                    .m_definition = std::move(definition),
                    .m_sourceId   = recognizer.m_sourceId,
                    .m_shared     = recognizer.m_shared,
                }
            );
        }

        auto pages = std::vector<annotation::PageSignature>{};
        pages.reserve(draft.m_pages.size());
        for (auto const& page : draft.m_pages)
        {
            UF_TRY_VALUE(name, annotation::ResourceName::create(page.m_name));
            UF_TRY_VALUE(
                validated,
                annotation::PageSignature::create(
                    annotation::PageSpec{
                        .m_id        = page.m_id,
                        .m_name      = std::move(name),
                        .m_required  = page.m_required,
                        .m_forbidden = page.m_forbidden,
                    }
                )
            );
            pages.emplace_back(std::move(validated));
        }

        auto regressions = std::vector<annotation::RegressionCase>{};
        regressions.reserve(draft.m_regressions.size());
        for (auto const& regression : draft.m_regressions)
        {
            regressions.emplace_back(
                annotation::RegressionSpec{
                    .m_id             = regression.m_id,
                    .m_sourceId       = regression.m_sourceId,
                    .m_classification = regression.m_classification,
                    .m_expectation    = regression.m_expectation,
                }
            );
        }

        return annotation::AuthoringDocument::create(
            draft.m_projectId,
            draft.m_fingerprint,
            std::move(sources),
            std::move(recognizers),
            std::move(pages),
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
                       draft.m_recognizers,
                       candidate,
                       &EditableRecognizer::m_name
                   )
                || std::ranges::contains(
                       draft.m_pages,
                       candidate,
                       &EditablePage::m_name
                   );
        };

        auto const limit = draft.m_recognizers.size() + draft.m_pages.size() + 1U;
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
                draft.m_sources,
                spec.m_sourceId,
                &EditableSource::m_id
            )
        )
        {
            return missingSource(spec.m_sourceId);
        }

        auto anchorName = freshAuthoringName(draft, "anchor");
        draft.m_recognizers.emplace_back(
            EditableRecognizer{
                .m_id                    = spec.m_anchorId,
                .m_name                  = anchorName,
                .m_annotationType        = annotation::AnnotationType::PageAnchor,
                .m_sourceId              = spec.m_sourceId,
                .m_templateRect          = spec.m_templateRect,
                .m_searchRoi             = spec.m_searchRoi,
                .m_similarityBasisPoints = spec.m_similarityBasisPoints,
                .m_defaultClick          = {},
                .m_allowedPageIds        = {},
            }
        );

        auto pageName = freshAuthoringName(draft, "page");
        draft.m_pages.emplace_back(
            EditablePage{
                .m_id        = spec.m_pageId,
                .m_name      = pageName,
                .m_required  = {spec.m_anchorId},
                .m_forbidden = {},
            }
        );

        // The case is what states "this screen is that page". Nothing else in the
        // document records it: an anchor names the page it identifies, but the
        // image the anchor happens to be drawn on is not the same claim.
        draft.m_regressions.emplace_back(
            EditableRegression{
                .m_id             = spec.m_regressionId,
                .m_sourceId       = spec.m_sourceId,
                .m_classification = annotation::RegressionClassification::Positive,
                .m_expectation    = annotation::ResolvedRegression{spec.m_pageId},
            }
        );

        return CreatedPage{
            .m_draft      = std::move(draft),
            .m_pageName   = std::move(pageName),
            .m_anchorName = std::move(anchorName),
        };
    }

    auto addPageMember(
        AuthoringDraft draft,
        PageMemberSpec const& spec
    ) -> Result<AddedPageMember>
    {
        if (
            !std::ranges::contains(
                draft.m_sources,
                spec.m_sourceId,
                &EditableSource::m_id
            )
        )
        {
            return missingSource(spec.m_sourceId);
        }

        auto const page = std::ranges::find(
            draft.m_pages,
            spec.m_pageId,
            &EditablePage::m_id
        );
        if (page == draft.m_pages.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "page {} is not part of this draft",
                    spec.m_pageId.value().toString()
                )
            );
        }

        auto const isAnchor = spec.m_kind == PageMemberKind::Anchor;
        auto name = freshAuthoringName(draft, isAnchor ? "anchor" : "region");
        draft.m_recognizers.emplace_back(
            EditableRecognizer{
                .m_id   = spec.m_recognizerId,
                .m_name = name,
                .m_annotationType = isAnchor
                    ? annotation::AnnotationType::PageAnchor
                    : annotation::AnnotationType::ActionTarget,
                .m_sourceId              = spec.m_sourceId,
                .m_templateRect          = spec.m_templateRect,
                .m_searchRoi             = spec.m_searchRoi,
                .m_similarityBasisPoints = spec.m_similarityBasisPoints,
                .m_defaultClick          = {},
                .m_allowedPageIds        = isAnchor
                    ? std::vector<annotation::PageId>{}
                    : std::vector<annotation::PageId>{spec.m_pageId},
            }
        );
        if (isAnchor)
        {
            page->m_required.emplace_back(spec.m_recognizerId);
        }

        return AddedPageMember{
            .m_draft = std::move(draft),
            .m_name  = std::move(name),
        };
    }

    auto sharedRegionMembers(
        AuthoringDraft const& draft,
        annotation::RecognizerId id
    ) -> std::vector<annotation::RecognizerId>
    {
        auto members = std::vector<annotation::RecognizerId>{};
        auto const origin = std::ranges::find(
            draft.m_recognizers,
            id,
            &EditableRecognizer::m_id
        );
        if (origin == draft.m_recognizers.end())
        {
            return members;
        }
        // An unmarked region is a group of one. Nothing else can be reusing its
        // pixels, because reuse only happens through a marked element, and
        // returning it keeps a plain template drag on the same path as a shared
        // one.
        if (!origin->m_shared)
        {
            members.emplace_back(origin->m_id);
            return members;
        }
        for (auto const& recognizer : draft.m_recognizers)
        {
            if (
                recognizer.m_shared
                && recognizer.m_sourceId == origin->m_sourceId
                && recognizer.m_templateRect == origin->m_templateRect
                && recognizer.m_annotationType == origin->m_annotationType
            )
            {
                members.emplace_back(recognizer.m_id);
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
            draft.m_recognizers,
            id,
            &EditableRecognizer::m_id
        );
        if (target == draft.m_recognizers.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "recognizer {} is not part of this draft",
                    id.value().toString()
                )
            );
        }
        if (target->m_annotationType != annotation::AnnotationType::ActionTarget)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "\"{}\" is not an interactive region; only those are reused "
                    "across pages",
                    target->m_name
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
                    target->m_name,
                    members.size()
                )
            );
        }

        for (auto const& memberId : members)
        {
            auto const member = std::ranges::find(
                draft.m_recognizers,
                memberId,
                &EditableRecognizer::m_id
            );
            UF_CHECK(member != draft.m_recognizers.end());
            member->m_shared = shared;
        }
        return draft;
    }

    auto shareRegionOnPage(
        AuthoringDraft draft,
        SharedRegionSpec const& spec
    ) -> Result<AddedPageMember>
    {
        auto const origin = std::ranges::find(
            draft.m_recognizers,
            spec.m_shareFrom,
            &EditableRecognizer::m_id
        );
        if (origin == draft.m_recognizers.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "recognizer {} is not part of this draft",
                    spec.m_shareFrom.value().toString()
                )
            );
        }
        if (origin->m_annotationType != annotation::AnnotationType::ActionTarget)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "\"{}\" is not an interactive region; only those are shared "
                    "across pages",
                    origin->m_name
                )
            );
        }
        if (
            !std::ranges::contains(draft.m_pages, spec.m_pageId, &EditablePage::m_id)
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "page {} is not part of this draft",
                    spec.m_pageId.value().toString()
                )
            );
        }

        auto const alreadyThere = std::ranges::any_of(
            draft.m_recognizers,
            [&origin, &spec](EditableRecognizer const& recognizer)
            {
                return recognizer.m_sourceId == origin->m_sourceId
                    && recognizer.m_templateRect == origin->m_templateRect
                    && recognizer.m_annotationType == origin->m_annotationType
                    && std::ranges::contains(
                           recognizer.m_allowedPageIds,
                           spec.m_pageId
                       );
            }
        );
        if (alreadyThere)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "\"{}\" is already on that page",
                    origin->m_name
                )
            );
        }

        // Everything origin is needed for happens here, before the append: a
        // vector append may reallocate, and every iterator into it dies with the
        // old buffer. Reuse is what being shared means, so an element that
        // reaches a second page is marked whether or not the author ticked the
        // box first.
        origin->m_shared        = true;
        auto const templateRect = origin->m_templateRect;
        auto const sourceId     = origin->m_sourceId;
        auto const threshold    = origin->m_similarityBasisPoints;
        auto const defaultClick = origin->m_defaultClick;
        auto const originName   = origin->m_name;

        // Named after the element and the page it lands on, rather than the next
        // free "region_N". The copy is that element on that page, and a generated
        // number says neither -- which is what left a renamed element paired with
        // a stale one the first time this existed.
        auto const page = std::ranges::find(
            draft.m_pages,
            spec.m_pageId,
            &EditablePage::m_id
        );
        UF_CHECK(page != draft.m_pages.end());
        auto name = freshAuthoringName(
            draft,
            std::format("{}_{}", originName, page->m_name)
        );
        draft.m_recognizers.emplace_back(
            EditableRecognizer{
                .m_id                    = spec.m_recognizerId,
                .m_name                  = name,
                .m_annotationType        = annotation::AnnotationType::ActionTarget,
                .m_sourceId              = sourceId,
                .m_templateRect          = templateRect,
                .m_searchRoi             = spec.m_searchRoi,
                .m_similarityBasisPoints = threshold,
                .m_defaultClick          = defaultClick,
                .m_allowedPageIds        = {spec.m_pageId},
                .m_shared                = true,
            }
        );

        return AddedPageMember{
            .m_draft = std::move(draft),
            .m_name  = std::move(name),
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
                draft.m_recognizers,
                memberId,
                &EditableRecognizer::m_id
            );
            UF_CHECK(member != draft.m_recognizers.end());
            if (
                templateRect.width() > member->m_searchRoi.width()
                || templateRect.height() > member->m_searchRoi.height()
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "the new template does not fit the range \"{}\" searches; "
                        "widen that range first",
                        member->m_name
                    )
                );
            }
        }

        for (auto const& memberId : members)
        {
            auto const member = std::ranges::find(
                draft.m_recognizers,
                memberId,
                &EditableRecognizer::m_id
            );
            UF_CHECK(member != draft.m_recognizers.end());
            member->m_templateRect = templateRect;
        }

        return RetemplatedRegion{
            .m_draft = std::move(draft),
            // The recognizer being dragged is not something the author needs
            // reporting back to them; the others are.
            .m_movedMembers = members.size() - 1U,
        };
    }

    auto claimScreenForPage(
        AuthoringDraft draft,
        ScreenClaimSpec const& spec
    ) -> Result<AuthoringDraft>
    {
        if (
            !std::ranges::contains(
                draft.m_sources,
                spec.m_sourceId,
                &EditableSource::m_id
            )
        )
        {
            return missingSource(spec.m_sourceId);
        }
        if (
            !std::ranges::contains(draft.m_pages, spec.m_pageId, &EditablePage::m_id)
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "page {} is not part of this draft",
                    spec.m_pageId.value().toString()
                )
            );
        }

        auto const expectation = annotation::RegressionExpectation{
            annotation::ResolvedRegression{spec.m_pageId},
        };
        auto const existing = std::ranges::find(
            draft.m_regressions,
            spec.m_sourceId,
            &EditableRegression::m_sourceId
        );
        if (existing != draft.m_regressions.end())
        {
            existing->m_expectation = expectation;
            return draft;
        }

        draft.m_regressions.emplace_back(
            EditableRegression{
                .m_id             = spec.m_regressionId,
                .m_sourceId       = spec.m_sourceId,
                .m_classification = annotation::RegressionClassification::Positive,
                .m_expectation    = expectation,
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
            draft.m_recognizers,
            id,
            &EditableRecognizer::m_id
        );
        if (target == draft.m_recognizers.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "recognizer {} is not part of this draft",
                    id.value().toString()
                )
            );
        }
        if (target->m_annotationType == type)
        {
            return RetypedRecognizer{.m_draft = std::move(draft)};
        }

        // Only a page anchor may fill a required or forbidden role, so leaving
        // the page anchor type withdraws this recognizer from every signature. A
        // signature naming nothing else cannot survive that withdrawal, and the
        // author has to resolve which anchor takes over.
        auto withdrawnRoles = std::size_t{0};
        auto anchoredPages  = std::vector<annotation::PageId>{};
        if (target->m_annotationType == annotation::AnnotationType::PageAnchor)
        {
            for (auto const& page : draft.m_pages)
            {
                auto const named = (
                    std::ranges::contains(page.m_required, id)
                    || std::ranges::contains(page.m_forbidden, id)
                );
                auto const alone = (
                    page.m_required.size() + page.m_forbidden.size() == 1U
                );
                if (named && alone)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "\"{}\" is the only recognizer page \"{}\" names; "
                            "give that page another page anchor before changing "
                            "this recognizer's type",
                            target->m_name,
                            page.m_name
                        )
                    );
                }
                // Remembered before the withdrawal: a page this recognizer was
                // required on is where it demonstrably appears, which is a far
                // better authorization to offer than an arbitrary page. A page
                // it was forbidden on is the opposite and is never offered.
                if (std::ranges::contains(page.m_required, id))
                {
                    anchoredPages.emplace_back(page.m_id);
                }
            }
            for (auto& page : draft.m_pages)
            {
                auto const before = page.m_required.size() + page.m_forbidden.size();
                std::erase(page.m_required, id);
                std::erase(page.m_forbidden, id);
                withdrawnRoles += (
                    before - (page.m_required.size() + page.m_forbidden.size())
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
            if (target->m_allowedPageIds.empty())
            {
                if (draft.m_pages.empty())
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "an action target must authorize at least one page and "
                        "this project has none; add a page first"
                    );
                }
                authorizedPage = anchoredPages.empty()
                    ? draft.m_pages.front().m_id
                    : anchoredPages.front();
                target->m_allowedPageIds.emplace_back(*authorizedPage);
            }
        }
        else
        {
            // Only an action target may carry a default click.
            clearedClick = target->m_defaultClick.has_value();
            target->m_defaultClick.reset();
            if (type == annotation::AnnotationType::PageAnchor)
            {
                // A page anchor's membership is its signature role, never an
                // authorization it holds itself.
                clearedAuthorizations = target->m_allowedPageIds.size();
                target->m_allowedPageIds.clear();
            }
        }
        target->m_annotationType = type;

        return RetypedRecognizer{
            .m_draft                 = std::move(draft),
            .m_authorizedPage        = authorizedPage,
            .m_withdrawnRoles        = withdrawnRoles,
            .m_clearedAuthorizations = clearedAuthorizations,
            .m_clearedClick          = clearedClick,
        };
    }

    auto deleteRecognizer(
        AuthoringDraft draft,
        annotation::RecognizerId id
    ) -> Result<DeletedEntity>
    {
        auto const target = std::ranges::find(
            draft.m_recognizers,
            id,
            &EditableRecognizer::m_id
        );
        if (target == draft.m_recognizers.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "recognizer {} is not part of this draft",
                    id.value().toString()
                )
            );
        }

        for (auto const& page : draft.m_pages)
        {
            auto const named = (
                std::ranges::contains(page.m_required, id)
                || std::ranges::contains(page.m_forbidden, id)
            );
            auto const alone = (
                page.m_required.size() + page.m_forbidden.size() == 1U
            );
            if (named && alone)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "\"{}\" is the only recognizer page \"{}\" names; "
                        "delete that page first, or give it another page anchor",
                        target->m_name,
                        page.m_name
                    )
                );
            }
        }

        auto withdrawnRoles = std::size_t{0};
        for (auto& page : draft.m_pages)
        {
            auto const before = page.m_required.size() + page.m_forbidden.size();
            std::erase(page.m_required, id);
            std::erase(page.m_forbidden, id);
            withdrawnRoles += (
                before - (page.m_required.size() + page.m_forbidden.size())
            );
        }
        draft.m_recognizers.erase(target);

        return DeletedEntity{
            .m_draft          = std::move(draft),
            .m_withdrawnRoles = withdrawnRoles,
        };
    }

    auto deletePage(
        AuthoringDraft draft,
        annotation::PageId id
    ) -> Result<DeletedEntity>
    {
        auto const target = std::ranges::find(
            draft.m_pages,
            id,
            &EditablePage::m_id
        );
        if (target == draft.m_pages.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "page {} is not part of this draft",
                    id.value().toString()
                )
            );
        }

        for (auto const& recognizer : draft.m_recognizers)
        {
            auto const onlyAuthorization = (
                recognizer.m_annotationType == annotation::AnnotationType::ActionTarget
                && recognizer.m_allowedPageIds.size() == 1U
                && std::ranges::contains(recognizer.m_allowedPageIds, id)
            );
            if (onlyAuthorization)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "page \"{}\" is the only page action target \"{}\" is "
                        "authorized on; authorize it elsewhere or delete it first",
                        target->m_name,
                        recognizer.m_name
                    )
                );
            }
        }

        auto clearedAuthorizations = std::size_t{0};
        for (auto& recognizer : draft.m_recognizers)
        {
            auto const before = recognizer.m_allowedPageIds.size();
            std::erase(recognizer.m_allowedPageIds, id);
            clearedAuthorizations += before - recognizer.m_allowedPageIds.size();
        }

        // A case expecting this page to resolve cannot be reclassified into
        // anything the author meant, so it goes with the page rather than
        // blocking the deletion. Every page created from a captured screen owns
        // one, so refusing here would make such a page undeletable.
        auto const removedRegressions = std::erase_if(
            draft.m_regressions,
            [id](EditableRegression const& regression)
            {
                auto const* p_resolved = std::get_if<annotation::ResolvedRegression>(
                    &regression.m_expectation
                );
                return p_resolved != nullptr && p_resolved->m_pageId == id;
            }
        );
        draft.m_pages.erase(target);

        return DeletedEntity{
            .m_draft                 = std::move(draft),
            .m_clearedAuthorizations = clearedAuthorizations,
            .m_removedRegressions    = removedRegressions,
        };
    }

    auto deleteSource(
        AuthoringDraft draft,
        annotation::SourceId id
    ) -> Result<DeletedEntity>
    {
        auto const target = std::ranges::find(
            draft.m_sources,
            id,
            &EditableSource::m_id
        );
        if (target == draft.m_sources.end())
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
            draft.m_recognizers,
            id,
            &EditableRecognizer::m_sourceId
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

        auto const before = draft.m_regressions.size();
        std::erase_if(
            draft.m_regressions,
            [id](EditableRegression const& regression) -> bool
            {
                return regression.m_sourceId == id;
            }
        );
        auto const removedRegressions = before - draft.m_regressions.size();
        draft.m_sources.erase(target);

        return DeletedEntity{
            .m_draft              = std::move(draft),
            .m_removedRegressions = removedRegressions,
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
        return true;
    }

    auto AuthoringEditHistory::undo() -> bool
    {
        if (m_undo.empty()) return false;

        m_redo.emplace_back(std::move(m_current));
        m_current = std::move(m_undo.back());
        m_undo.pop_back();
        return true;
    }

    auto AuthoringEditHistory::redo() -> bool
    {
        if (m_redo.empty()) return false;

        m_undo.emplace_back(std::move(m_current));
        m_current = std::move(m_redo.back());
        m_redo.pop_back();
        return true;
    }
}
