#include "edit-page.hpp"

#include "authoring-actions.hpp"
#include "authoring-edit.hpp"
#include "page-view.hpp"
#include "panel-state.hpp"
#include "app/workbench-app.hpp"

#include <annotation/authoring-document.hpp>
#include <annotation/catalog.hpp>

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/space.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        // What a newly drawn recognizer starts as before the author drags it,
        // mirroring authoring-actions: a box small enough to be legal at any
        // project resolution, and the annotation design's default similarity.
        constexpr auto k_startingTemplateExtent        = uint32{16};
        constexpr auto k_startingSimilarityBasisPoints = uint32{9'000};

        struct StartingRects final
        {
            PixelRect templateRect;
            PixelRect searchRoi;
        };

        [[nodiscard]]
        auto startingRects(
            annotation::ProjectFingerprint fingerprint
        ) -> Result<StartingRects>
        {
            auto const width  = fingerprint.width();
            auto const height = fingerprint.height();
            UF_TRY_VALUE(
                templateRect,
                PixelRect::create(
                    0U,
                    0U,
                    std::min<uint32>(k_startingTemplateExtent, width),
                    std::min<uint32>(k_startingTemplateExtent, height)
                )
            );
            UF_TRY_VALUE(searchRoi, PixelRect::create(0U, 0U, width, height));
            return StartingRects{
                .templateRect = templateRect,
                .searchRoi    = searchRoi,
            };
        }

        [[nodiscard]]
        auto missingMember(PageView::MemberId member) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "member {} is not on this page",
                    member.value().toString()
                )
            );
        }
    }

    EditPage::EditPage(
        AuthoringDraft draft,
        annotation::PageId id,
        uint64 baseRevision
    )
        : m_draft{std::move(draft)}
        , m_id{id}
        , m_baseRevision{baseRevision}
    {
    }

    auto EditPage::open(
        AppState const& state,
        annotation::PageId id
    ) -> Result<EditPage>
    {
        auto draft = state.draft();
        if (
            !std::ranges::contains(draft.pages, id, &EditablePage::id)
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "page {} is not part of this project",
                    id.value().toString()
                )
            );
        }
        return EditPage{std::move(draft), id, state.revision()};
    }

    auto EditPage::createFrom(
        AppState const& state,
        annotation::SourceId source
    ) -> Result<EditPage>
    {
        auto draft = state.draft();
        UF_TRY_VALUE(rects, startingRects(draft.fingerprint));

        auto const pageId = annotation::PageId{mintResourceId()};
        UF_TRY_VALUE(
            created,
            createPageFromSource(
                std::move(draft),
                NewPageSpec{
                    .pageId                = pageId,
                    .anchorId              = annotation::RecognizerId{
                        mintResourceId()
                    },
                    .regressionId          = annotation::RegressionId{
                        mintResourceId()
                    },
                    .sourceId              = source,
                    .templateRect          = rects.templateRect,
                    .searchRoi             = rects.searchRoi,
                    .similarityBasisPoints = k_startingSimilarityBasisPoints,
                }
            )
        );
        return EditPage{std::move(created.draft), pageId, state.revision()};
    }

    auto EditPage::id() const noexcept -> annotation::PageId
    {
        return m_id;
    }

    auto EditPage::baseRevision() const noexcept -> uint64
    {
        return m_baseRevision;
    }

    auto EditPage::view() const -> PageView
    {
        auto view = PageView::of(m_draft, m_id);
        // The target page is an invariant of a live EditPage: open() rejects an
        // absent page and no operation removes the page being edited.
        UF_CHECK(view.has_value());
        return *std::move(view);
    }

    auto EditPage::placeAnchor(NewAnchorSpec const& spec) -> Result<AddedAnchor>
    {
        UF_TRY_VALUE(rects, startingRects(m_draft.fingerprint));

        auto const recognizerId = annotation::RecognizerId{mintResourceId()};
        UF_TRY_VALUE(
            added,
            addPageMember(
                m_draft,
                PageMemberSpec{
                    .recognizerId          = recognizerId,
                    .pageId                = m_id,
                    .sourceId              = spec.sourceId,
                    .templateRect          = rects.templateRect,
                    .searchRoi             = rects.searchRoi,
                    .similarityBasisPoints = k_startingSimilarityBasisPoints,
                    .kind                  = PageMemberKind::Anchor,
                }
            )
        );
        m_draft = std::move(added.draft);
        return AddedAnchor{
            .id   = recognizerId,
            .name = std::move(added.name),
        };
    }

    auto EditPage::requireAnchor(MemberId member) -> Status
    {
        auto draft         = m_draft;
        auto const* target = findEditableRecognizer(draft, member);
        if (target == nullptr)
        {
            return missingMember(member);
        }
        if (target->annotationType != annotation::AnnotationType::PageAnchor)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "\"{}\" is not an identifying mark",
                    target->name
                )
            );
        }

        auto const page = std::ranges::find(
            draft.pages,
            m_id,
            &EditablePage::id
        );
        if (page == draft.pages.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "this page is no longer in the project"
            );
        }
        std::erase(page->forbidden, member);
        if (!std::ranges::contains(page->required, member))
        {
            page->required.emplace_back(member);
        }
        m_draft = std::move(draft);
        return ok();
    }

    auto EditPage::forbidAnchor(MemberId member) -> Status
    {
        auto draft         = m_draft;
        auto const* target = findEditableRecognizer(draft, member);
        if (target == nullptr)
        {
            return missingMember(member);
        }
        if (target->annotationType != annotation::AnnotationType::PageAnchor)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "\"{}\" is not an identifying mark",
                    target->name
                )
            );
        }

        auto const page = std::ranges::find(
            draft.pages,
            m_id,
            &EditablePage::id
        );
        if (page == draft.pages.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "this page is no longer in the project"
            );
        }
        std::erase(page->required, member);
        if (!std::ranges::contains(page->forbidden, member))
        {
            page->forbidden.emplace_back(member);
        }
        m_draft = std::move(draft);
        return ok();
    }

    auto EditPage::claimScreen(annotation::SourceId source) -> Status
    {
        UF_TRY_VALUE(
            next,
            claimScreenForPage(
                m_draft,
                ScreenClaimSpec{
                    .regressionId = annotation::RegressionId{mintResourceId()},
                    .sourceId     = source,
                    .pageId       = m_id,
                }
            )
        );
        m_draft = std::move(next);
        return ok();
    }

    auto EditPage::classifyScreen(
        annotation::SourceId source,
        annotation::RegressionClassification classification
    ) -> Status
    {
        if (
            !std::ranges::contains(
                m_draft.sources,
                source,
                &EditableSource::id
            )
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "source {} is not part of this project",
                    source.value().toString()
                )
            );
        }

        auto draft          = m_draft;
        auto const existing = std::ranges::find(
            draft.regressions,
            source,
            &EditableRegression::sourceId
        );
        if (existing != draft.regressions.end())
        {
            existing->classification = classification;
        }
        else
        {
            // No case yet ties this screen to the page. Record one classified as
            // asked, resolving to this page, mirroring how a claim is created.
            draft.regressions.emplace_back(
                EditableRegression{
                    .id             = annotation::RegressionId{mintResourceId()},
                    .sourceId       = source,
                    .classification = classification,
                    .expectation    = annotation::ResolvedRegression{m_id},
                }
            );
        }
        m_draft = std::move(draft);
        return ok();
    }

    auto EditPage::placeRegion(NewRegionSpec const& spec) -> Result<AddedRegion>
    {
        UF_TRY_VALUE(rects, startingRects(m_draft.fingerprint));

        auto const recognizerId = annotation::RecognizerId{mintResourceId()};
        UF_TRY_VALUE(
            added,
            addPageMember(
                m_draft,
                PageMemberSpec{
                    .recognizerId          = recognizerId,
                    .pageId                = m_id,
                    .sourceId              = spec.sourceId,
                    .templateRect          = rects.templateRect,
                    .searchRoi             = rects.searchRoi,
                    .similarityBasisPoints = k_startingSimilarityBasisPoints,
                    .kind                  = PageMemberKind::ActionTarget,
                }
            )
        );
        m_draft = std::move(added.draft);
        return AddedRegion{
            .id   = recognizerId,
            .name = std::move(added.name),
        };
    }

    auto EditPage::placeExisting(MemberId member) -> Status
    {
        auto draft         = m_draft;
        auto const* target = findEditableRecognizer(draft, member);
        if (target == nullptr)
        {
            return missingMember(member);
        }
        if (target->annotationType == annotation::AnnotationType::PageAnchor)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "\"{}\" is a page anchor and joins a page through its "
                    "signature, not a placement",
                    target->name
                )
            );
        }
        // The placement seeds its per-page search region from the element's own,
        // the same region a fresh placement of it would start with.
        auto const seedRoi = target->searchRoi;
        auto const already = std::ranges::any_of(
            draft.placements,
            [member, this](EditablePlacement const& placement)
            {
                return placement.elementId == member && placement.pageId == m_id;
            }
        );
        if (!already)
        {
            draft.placements.emplace_back(
                EditablePlacement{
                    .pageId    = m_id,
                    .elementId = member,
                    .searchRoi = seedRoi,
                }
            );
        }
        m_draft = std::move(draft);
        return ok();
    }

    auto EditPage::acceptSharedRegion(MemberId from) -> Result<EditPage::SharedRegionScore>
    {
        auto const* origin = findRecognizer(from);
        if (origin == nullptr)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "that region is no longer in the project"
            );
        }

        auto const roi = origin->searchRoi;
        UF_TRY_VALUE(
            shared,
            shareRegionOnPage(
                m_draft,
                SharedRegionSpec{
                    .elementId = from,
                    .pageId    = m_id,
                    .searchRoi = roi,
                }
            )
        );
        m_draft = std::move(shared.draft);
        // The same element is now placed here; there is no copy to select, so the
        // element itself is what the caller selects.
        return EditPage::SharedRegionScore{
            .id   = from,
            .name = std::move(shared.name),
        };
    }

    auto EditPage::anchor(MemberId member) -> Result<PageAnchor>
    {
        auto const* target = findRecognizer(member);
        if (target == nullptr)
        {
            return missingMember(member);
        }
        if (target->annotationType != annotation::AnnotationType::PageAnchor)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "\"{}\" is not an identifying mark",
                    target->name
                )
            );
        }
        return Result<PageAnchor>{
            std::in_place,
            detail::HandleKey{},
            *this,
            member,
        };
    }

    auto EditPage::region(MemberId member) -> Result<InteractiveRegion>
    {
        auto const* target = findRecognizer(member);
        if (target == nullptr)
        {
            return missingMember(member);
        }
        if (target->annotationType != annotation::AnnotationType::ActionTarget)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "\"{}\" is not an interactive region",
                    target->name
                )
            );
        }
        return Result<InteractiveRegion>{
            std::in_place,
            detail::HandleKey{},
            *this,
            member,
        };
    }

    auto EditPage::commit(PanelUiState& ui, std::string description) && -> void
    {
        // Only stamp the base revision onto the request this call actually
        // parked. A frame already carrying an edit keeps it -- the
        // one-commit-per-frame guard lives in requestEdit -- and touching that
        // other request's revision would corrupt a commit this one did not make.
        auto const parkedBefore = ui.pendingEdit.has_value();
        requestEdit(ui, std::move(m_draft), std::move(description));
        if (!parkedBefore && ui.pendingEdit.has_value())
        {
            ui.pendingEdit->baseRevision = m_baseRevision;
        }
    }

    auto EditPage::commitSelecting(
        PanelUiState& ui,
        std::string description,
        MemberId select,
        std::optional<annotation::SourceId> selectSource
    ) && -> void
    {
        auto const parkedBefore = ui.pendingEdit.has_value();
        requestEditSelecting(
            ui,
            std::move(m_draft),
            std::move(description),
            select,
            selectSource
        );
        if (!parkedBefore && ui.pendingEdit.has_value())
        {
            ui.pendingEdit->baseRevision = m_baseRevision;
        }
    }

    auto EditPage::draftCopy() const -> AuthoringDraft
    {
        return m_draft;
    }

    auto EditPage::replaceDraft(AuthoringDraft draft) -> void
    {
        m_draft = std::move(draft);
    }

    auto EditPage::draftView() const noexcept -> AuthoringDraft const&
    {
        return m_draft;
    }

    auto EditPage::findRecognizer(MemberId member) const -> EditableRecognizer const*
    {
        auto const found = std::ranges::find(
            m_draft.recognizers,
            member,
            &EditableRecognizer::id
        );
        if (found == m_draft.recognizers.end())
        {
            return nullptr;
        }
        return &*found;
    }

    auto EditPage::pageId() const noexcept -> annotation::PageId
    {
        return m_id;
    }

    InteractiveRegion::InteractiveRegion(
        detail::HandleKey /*key*/,
        EditPage& page,
        PageView::MemberId id
    )
        : m_page{page}
        , m_id{id}
    {
    }

    auto InteractiveRegion::name() const -> std::string
    {
        auto const* target = m_page.findRecognizer(m_id);
        UF_CHECK(target != nullptr);
        return target->name;
    }

    auto InteractiveRegion::templateRect() const -> PixelRect
    {
        auto const* target = m_page.findRecognizer(m_id);
        UF_CHECK(target != nullptr);
        return target->templateRect;
    }

    auto InteractiveRegion::searchRoiOnThisPage() const -> PixelRect
    {
        auto const* target = m_page.findRecognizer(m_id);
        UF_CHECK(target != nullptr);
        return target->searchRoi;
    }

    auto InteractiveRegion::threshold() const -> uint32
    {
        auto const* target = m_page.findRecognizer(m_id);
        UF_CHECK(target != nullptr);
        return target->similarityBasisPoints;
    }

    auto InteractiveRegion::clickOffset() const
        -> std::optional<EditableTemplateOffset>
    {
        auto const* target = m_page.findRecognizer(m_id);
        UF_CHECK(target != nullptr);
        return target->defaultClick;
    }

    auto InteractiveRegion::isShared() const -> bool
    {
        auto const* target = m_page.findRecognizer(m_id);
        UF_CHECK(target != nullptr);
        return target->shared;
    }

    auto InteractiveRegion::pagesPlacedOn() const -> std::vector<annotation::PageId>
    {
        return uf::workbench::pagesPlacedOn(m_page.draftView(), m_id);
    }

    auto InteractiveRegion::rename(std::string name) -> Status
    {
        auto draft   = m_page.draftCopy();
        auto* target = findEditableRecognizer(draft, m_id);
        if (target == nullptr)
        {
            return missingMember(m_id);
        }
        target->name = std::move(name);
        m_page.replaceDraft(std::move(draft));
        return ok();
    }

    auto InteractiveRegion::setThreshold(uint32 basisPoints) -> Status
    {
        auto draft   = m_page.draftCopy();
        auto* target = findEditableRecognizer(draft, m_id);
        if (target == nullptr)
        {
            return missingMember(m_id);
        }
        target->similarityBasisPoints = basisPoints;
        m_page.replaceDraft(std::move(draft));
        return ok();
    }

    auto InteractiveRegion::setClickOffset(
        std::optional<EditableTemplateOffset> click
    ) -> Status
    {
        auto draft   = m_page.draftCopy();
        auto* target = findEditableRecognizer(draft, m_id);
        if (target == nullptr)
        {
            return missingMember(m_id);
        }
        target->defaultClick = click;
        m_page.replaceDraft(std::move(draft));
        return ok();
    }

    auto InteractiveRegion::setSearchRoi(PixelRect roi) -> Status
    {
        auto draft   = m_page.draftCopy();
        auto* target = findEditableRecognizer(draft, m_id);
        if (target == nullptr)
        {
            return missingMember(m_id);
        }
        target->searchRoi = roi;
        m_page.replaceDraft(std::move(draft));
        return ok();
    }

    auto InteractiveRegion::setTemplateRect(PixelRect templateRect) -> Status
    {
        UF_TRY_VALUE(
            retemplated,
            setElementTemplateRect(m_page.draftCopy(), m_id, templateRect)
        );
        m_page.replaceDraft(std::move(retemplated.draft));
        return ok();
    }

    auto InteractiveRegion::setShared(bool shared) -> Status
    {
        UF_TRY_VALUE(
            marked,
            setRegionShared(m_page.draftCopy(), m_id, shared)
        );
        m_page.replaceDraft(std::move(marked));
        return ok();
    }

    auto InteractiveRegion::shareToPage(
        annotation::PageId page
    ) -> Result<EditPage::SharedRegionScore>
    {
        auto const* origin = m_page.findRecognizer(m_id);
        if (origin == nullptr)
        {
            return missingMember(m_id);
        }

        auto const roi = origin->searchRoi;
        UF_TRY_VALUE(
            shared,
            shareRegionOnPage(
                m_page.draftCopy(),
                SharedRegionSpec{
                    .elementId = m_id,
                    .pageId    = page,
                    .searchRoi = roi,
                }
            )
        );
        m_page.replaceDraft(std::move(shared.draft));
        return EditPage::SharedRegionScore{
            .id   = m_id,
            .name = std::move(shared.name),
        };
    }

    auto InteractiveRegion::removeFromThisPage() -> Status
    {
        auto draft         = m_page.draftCopy();
        auto const* target = findEditableRecognizer(draft, m_id);
        if (target == nullptr)
        {
            return missingMember(m_id);
        }

        auto const pages  = uf::workbench::pagesPlacedOn(draft, m_id);
        auto const onThis = std::ranges::contains(pages, m_page.pageId());
        if (!onThis)
        {
            m_page.replaceDraft(std::move(draft));
            return ok();
        }
        // Withdrawing an interactive element's last placement would leave it
        // placed nowhere, which the closure rule forbids. This is the point the
        // v1 copy model silently deleted the recognizer; under v2 the element is
        // one thing on N pages, so the author is told to delete it instead.
        if (pages.size() <= 1U)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "\"{}\" is only on this page; an interactive region must stay "
                    "on at least one, so delete it instead of removing it here",
                    target->name
                )
            );
        }
        std::erase_if(
            draft.placements,
            [this](EditablePlacement const& placement)
            {
                return placement.elementId == m_id
                    && placement.pageId == m_page.pageId();
            }
        );
        m_page.replaceDraft(std::move(draft));
        return ok();
    }

    auto InteractiveRegion::deleteEverywhere() -> Result<DeletedEntity>
    {
        // One element, deleted once: deleteRecognizer withdraws it from every
        // signature and removes all its placements.
        UF_TRY_VALUE(deleted, deleteRecognizer(m_page.draftCopy(), m_id));
        m_page.replaceDraft(deleted.draft);
        return deleted;
    }

    PageAnchor::PageAnchor(
        detail::HandleKey /*key*/,
        EditPage& page,
        PageView::MemberId id
    )
        : m_page{page}
        , m_id{id}
    {
    }

    auto PageAnchor::name() const -> std::string
    {
        auto const* target = m_page.findRecognizer(m_id);
        UF_CHECK(target != nullptr);
        return target->name;
    }

    auto PageAnchor::templateRect() const -> PixelRect
    {
        auto const* target = m_page.findRecognizer(m_id);
        UF_CHECK(target != nullptr);
        return target->templateRect;
    }

    auto PageAnchor::searchRoi() const -> PixelRect
    {
        auto const* target = m_page.findRecognizer(m_id);
        UF_CHECK(target != nullptr);
        return target->searchRoi;
    }

    auto PageAnchor::threshold() const -> uint32
    {
        auto const* target = m_page.findRecognizer(m_id);
        UF_CHECK(target != nullptr);
        return target->similarityBasisPoints;
    }

    auto PageAnchor::isShared() const -> bool
    {
        auto const* target = m_page.findRecognizer(m_id);
        UF_CHECK(target != nullptr);
        return target->shared;
    }

    auto PageAnchor::rename(std::string name) -> Status
    {
        auto draft   = m_page.draftCopy();
        auto* target = findEditableRecognizer(draft, m_id);
        if (target == nullptr)
        {
            return missingMember(m_id);
        }
        target->name = std::move(name);
        m_page.replaceDraft(std::move(draft));
        return ok();
    }

    auto PageAnchor::setThreshold(uint32 basisPoints) -> Status
    {
        auto draft   = m_page.draftCopy();
        auto* target = findEditableRecognizer(draft, m_id);
        if (target == nullptr)
        {
            return missingMember(m_id);
        }
        target->similarityBasisPoints = basisPoints;
        m_page.replaceDraft(std::move(draft));
        return ok();
    }

    auto PageAnchor::setSearchRoi(PixelRect roi) -> Status
    {
        auto draft   = m_page.draftCopy();
        auto* target = findEditableRecognizer(draft, m_id);
        if (target == nullptr)
        {
            return missingMember(m_id);
        }
        target->searchRoi = roi;
        m_page.replaceDraft(std::move(draft));
        return ok();
    }

    auto PageAnchor::setTemplateRect(PixelRect templateRect) -> Status
    {
        UF_TRY_VALUE(
            retemplated,
            setElementTemplateRect(m_page.draftCopy(), m_id, templateRect)
        );
        m_page.replaceDraft(std::move(retemplated.draft));
        return ok();
    }

    auto PageAnchor::require() -> Status
    {
        return m_page.requireAnchor(m_id);
    }

    auto PageAnchor::forbid() -> Status
    {
        return m_page.forbidAnchor(m_id);
    }

    auto PageAnchor::deleteEverywhere() -> Result<DeletedEntity>
    {
        // One anchor, deleted once: deleteRecognizer withdraws it from every
        // signature it names.
        UF_TRY_VALUE(deleted, deleteRecognizer(m_page.draftCopy(), m_id));
        m_page.replaceDraft(deleted.draft);
        return deleted;
    }
}
