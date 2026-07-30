#include "edit-page.hpp"

#include "authoring-actions.hpp"
#include "authoring-edit.hpp"
#include "page-view.hpp"
#include "panel-state.hpp"
#include "canvas-math.hpp"
#include "workbench-app.hpp"

#include <annotation/authoring-document.hpp>
#include <annotation/capabilities.hpp>
#include <annotation/resource.hpp>

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
        // What a newly drawn element starts as before the author drags it,
        // mirroring authoring-actions: a box small enough to be legal at any
        // project resolution, and the annotation design's default similarity.
        constexpr auto k_startingTemplateExtent        = uint32{16};
        constexpr auto k_startingSimilarityBasisPoints = uint32{9'000};

        // The smallest template a draw-to-create gesture accepts, in source
        // pixels. A box below this in either axis is a stray click-drag rather
        // than a mark the author meant to draw, and is refused with a message.
        constexpr auto k_minimumDrawnTemplateExtent = uint32{2};

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

        // Moves one page's reference to one element between the two identify
        // roles, minting the reference when the page has none yet. The element
        // has to declare identify: pixels that cannot be evidence cannot be
        // put in a signature.
        [[nodiscard]]
        auto setSignatureRole(
            AuthoringDraft& draft,
            annotation::PageId pageId,
            annotation::ElementId member,
            annotation::SignatureRole role
        ) -> Status
        {
            auto const* p_target = findEditableRecognizer(draft, member);
            if (p_target == nullptr)
            {
                return missingMember(member);
            }
            if (!p_target->capabilities.identify.has_value())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "\"{}\" is not an identifying mark",
                        p_target->name
                    )
                );
            }
            if (!std::ranges::contains(draft.pages, pageId, &EditablePage::id))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "this page is no longer in the project"
                );
            }

            auto const existing = std::ranges::find_if(
                draft.references,
                [pageId, member](EditableReference const& reference)
                {
                    return reference.pageId == pageId
                        && reference.elementId == member;
                }
            );
            auto const exercised = annotation::ExercisedIdentify{.role = role};
            if (existing != draft.references.end())
            {
                existing->exercised.identify = exercised;
                // The anchor pass reads the element-level region, so a reference
                // that starts exercising identify gives up any refinement it had.
                existing->searchRoi.reset();
                return ok();
            }

            draft.references.emplace_back(
                EditableReference{
                    .pageId    = pageId,
                    .elementId = member,
                    .holding   = annotation::Holding::Owned,
                    .exercised = EditableExercised{.identify = exercised},
                }
            );
            return ok();
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
                    .anchorId              = annotation::ElementId{
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

        auto const recognizerId = annotation::ElementId{mintResourceId()};
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
        auto draft = m_draft;
        UF_TRY(
            setSignatureRole(
                draft,
                m_id,
                member,
                annotation::SignatureRole::Required
            )
        );
        m_draft = std::move(draft);
        return ok();
    }

    auto EditPage::forbidAnchor(MemberId member) -> Status
    {
        auto draft = m_draft;
        UF_TRY(
            setSignatureRole(
                draft,
                m_id,
                member,
                annotation::SignatureRole::Forbidden
            )
        );
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

        auto const recognizerId = annotation::ElementId{mintResourceId()};
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

    auto EditPage::placeInfo(NewRegionSpec const& spec) -> Result<AddedRegion>
    {
        UF_TRY_VALUE(rects, startingRects(m_draft.fingerprint));

        auto const recognizerId = annotation::ElementId{mintResourceId()};
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
                    .kind                  = PageMemberKind::InfoRegion,
                }
            )
        );
        m_draft = std::move(added.draft);
        return AddedRegion{
            .id   = recognizerId,
            .name = std::move(added.name),
        };
    }

    auto EditPage::placeDrawn(NewDrawnMemberSpec const& spec) -> Result<AddedMember>
    {
        if (
            spec.templateRect.width() < k_minimumDrawnTemplateExtent
            || spec.templateRect.height() < k_minimumDrawnTemplateExtent
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "the drawn box is too small; drag out a larger rectangle"
            );
        }

        // The search region is derived from the drawn template rather than
        // seeded to the whole frame: the author drew the mark where it sits, so
        // looking for it near there is both the better first guess and a cheaper
        // search. It always contains the template, so addPageMember accepts it.
        UF_TRY_VALUE(
            searchRoi,
            searchRoiForDrawnTemplate(
                spec.templateRect,
                m_draft.fingerprint.width(),
                m_draft.fingerprint.height()
            )
        );

        auto const recognizerId = annotation::ElementId{mintResourceId()};
        UF_TRY_VALUE(
            added,
            addPageMember(
                m_draft,
                PageMemberSpec{
                    .recognizerId          = recognizerId,
                    .pageId                = m_id,
                    .sourceId              = spec.sourceId,
                    .templateRect          = spec.templateRect,
                    .searchRoi             = searchRoi,
                    .similarityBasisPoints = k_startingSimilarityBasisPoints,
                    .kind                  = spec.kind,
                }
            )
        );
        m_draft = std::move(added.draft);
        return AddedMember{
            .id   = recognizerId,
            .name = std::move(added.name),
            .kind = spec.kind,
        };
    }

    auto EditPage::placeExisting(MemberId member) -> Status
    {
        auto const* p_target = findRecognizer(member);
        if (p_target == nullptr)
        {
            return missingMember(member);
        }
        auto const already = std::ranges::any_of(
            m_draft.references,
            [member, this](EditableReference const& reference)
            {
                return reference.elementId == member
                    && reference.pageId == m_id;
            }
        );
        if (already)
        {
            return ok();
        }

        // The reference seeds its per-page region from the element's own, the
        // same region a fresh reference would inherit.
        auto const seedRoi = p_target->searchRoi;
        UF_TRY_VALUE(
            referenced,
            referenceElementOnPage(
                m_draft,
                ReferenceElementSpec{
                    .elementId = member,
                    .pageId    = m_id,
                    .searchRoi = seedRoi,
                }
            )
        );
        m_draft = std::move(referenced.draft);
        return ok();
    }

    auto EditPage::anchor(MemberId member) -> Result<PageAnchor>
    {
        auto const* p_target = findRecognizer(member);
        if (p_target == nullptr)
        {
            return missingMember(member);
        }
        if (!p_target->capabilities.identify.has_value())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "\"{}\" is not an identifying mark",
                    p_target->name
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
        auto const* p_target = findRecognizer(member);
        if (p_target == nullptr)
        {
            return missingMember(member);
        }
        if (!p_target->capabilities.interact.has_value())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "\"{}\" is not an interactive region",
                    p_target->name
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

    auto InteractiveRegion::templateRect() const -> std::optional<PixelRect>
    {
        auto const* target = m_page.findRecognizer(m_id);
        UF_CHECK(target != nullptr);
        auto const* p_variant = primaryVariant(*target);
        return p_variant == nullptr
            ? std::nullopt
            : std::optional<PixelRect>{p_variant->templateRect};
    }

    auto InteractiveRegion::searchRoiOnThisPage() const -> PixelRect
    {
        // The per-page range is the reference's refinement when it made one:
        // an element referenced by several pages may search a different
        // rectangle on each.
        auto const& draft    = m_page.draftView();
        auto const reference = std::ranges::find_if(
            draft.references,
            [this](EditableReference const& candidate)
            {
                return candidate.pageId == m_page.pageId()
                    && candidate.elementId == m_id;
            }
        );
        auto const* target = m_page.findRecognizer(m_id);
        UF_CHECK(target != nullptr);
        if (reference != draft.references.end())
        {
            return reference->searchRoi.value_or(target->searchRoi);
        }
        // Not referenced here yet: the element's own region is the seed a
        // reference would inherit.
        return target->searchRoi;
    }

    auto InteractiveRegion::threshold() const -> std::optional<uint32>
    {
        auto const* target = m_page.findRecognizer(m_id);
        UF_CHECK(target != nullptr);
        auto const* p_variant = primaryVariant(*target);
        return p_variant == nullptr
            ? std::nullopt
            : std::optional<uint32>{p_variant->similarityBasisPoints};
    }

    auto InteractiveRegion::clickOffset() const
        -> std::optional<EditableTemplateOffset>
    {
        auto const* target = m_page.findRecognizer(m_id);
        UF_CHECK(target != nullptr);
        auto const& interact = target->capabilities.interact;
        return interact.has_value() ? interact->clickOffset : std::nullopt;
    }

    auto InteractiveRegion::pagesReferencing() const -> std::vector<annotation::PageId>
    {
        return uf::workbench::pagesReferencing(m_page.draftView(), m_id);
    }

    auto InteractiveRegion::colourKey() const -> std::optional<annotation::ColourKey>
    {
        auto const* target = m_page.findRecognizer(m_id);
        UF_CHECK(target != nullptr);
        auto const* p_variant = primaryVariant(*target);
        return p_variant == nullptr ? std::nullopt : p_variant->colourKey;
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
        if (target->variants.empty())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "\"{}\" is located by its page and matches no pixels, so it "
                    "has no threshold to set",
                    target->name
                )
            );
        }
        target->variants.front().similarityBasisPoints = basisPoints;
        m_page.replaceDraft(std::move(draft));
        return ok();
    }

    auto InteractiveRegion::setColourKey(
        std::optional<annotation::ColourKey> key
    ) -> Status
    {
        UF_TRY_VALUE(
            updated,
            setElementColourKey(m_page.draftCopy(), m_id, key)
        );
        m_page.replaceDraft(std::move(updated));
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
        // The offset lives inside the interact capability, so an element that
        // cannot be clicked has nowhere to put one. region() already refused
        // such an element, which is why this is a check rather than a failure.
        UF_CHECK(target->capabilities.interact.has_value());
        target->capabilities.interact->clickOffset = click;
        m_page.replaceDraft(std::move(draft));
        return ok();
    }

    auto InteractiveRegion::setSearchRoi(PixelRect roi) -> Status
    {
        auto draft = m_page.draftCopy();
        // Write this page's reference, so refining the range here leaves every
        // other page's range untouched -- the defect this replaces mutated the
        // element's shared default and moved the region on every page at once.
        auto const reference = std::ranges::find_if(
            draft.references,
            [this](EditableReference const& candidate)
            {
                return candidate.pageId == m_page.pageId()
                    && candidate.elementId == m_id;
            }
        );
        if (reference != draft.references.end())
        {
            if (reference->exercised.identify.has_value())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "this page identifies by these pixels, and the anchor pass "
                    "reads the element's own range, so it cannot be refined here"
                );
            }
            reference->searchRoi = roi;
            m_page.replaceDraft(std::move(draft));
            return ok();
        }
        // Not referenced here: fall back to the element's default so an
        // unreferenced element stays editable rather than silently dropping the
        // edit.
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

    auto InteractiveRegion::referenceOnPage(annotation::PageId page) -> Status
    {
        auto const* origin = m_page.findRecognizer(m_id);
        if (origin == nullptr)
        {
            return missingMember(m_id);
        }

        auto const roi = origin->searchRoi;
        UF_TRY_VALUE(
            referenced,
            referenceElementOnPage(
                m_page.draftCopy(),
                ReferenceElementSpec{
                    .elementId = m_id,
                    .pageId    = page,
                    .searchRoi = roi,
                }
            )
        );
        m_page.replaceDraft(std::move(referenced.draft));
        return ok();
    }

    auto InteractiveRegion::removeFromThisPage() -> Status
    {
        UF_TRY_VALUE(
            draft,
            removeReferenceFromPage(
                m_page.draftCopy(),
                m_id,
                m_page.pageId()
            )
        );
        m_page.replaceDraft(std::move(draft));
        return ok();
    }

    auto InteractiveRegion::deleteEverywhere() -> Result<DeletedEntity>
    {
        // One element, deleted once: deleteRecognizer withdraws every page's
        // reference to it along with the element itself.
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

    auto PageAnchor::templateRect() const -> std::optional<PixelRect>
    {
        auto const* target = m_page.findRecognizer(m_id);
        UF_CHECK(target != nullptr);
        auto const* p_variant = primaryVariant(*target);
        return p_variant == nullptr
            ? std::nullopt
            : std::optional<PixelRect>{p_variant->templateRect};
    }

    auto PageAnchor::searchRoi() const -> PixelRect
    {
        auto const* target = m_page.findRecognizer(m_id);
        UF_CHECK(target != nullptr);
        return target->searchRoi;
    }

    auto PageAnchor::threshold() const -> std::optional<uint32>
    {
        auto const* target = m_page.findRecognizer(m_id);
        UF_CHECK(target != nullptr);
        auto const* p_variant = primaryVariant(*target);
        return p_variant == nullptr
            ? std::nullopt
            : std::optional<uint32>{p_variant->similarityBasisPoints};
    }

    auto PageAnchor::colourKey() const -> std::optional<annotation::ColourKey>
    {
        auto const* target = m_page.findRecognizer(m_id);
        UF_CHECK(target != nullptr);
        auto const* p_variant = primaryVariant(*target);
        return p_variant == nullptr ? std::nullopt : p_variant->colourKey;
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
        // An element that identifies always declares an appearance: pixels are
        // what it is evidence with, and the model refuses one without them.
        UF_CHECK(!target->variants.empty());
        target->variants.front().similarityBasisPoints = basisPoints;
        m_page.replaceDraft(std::move(draft));
        return ok();
    }

    auto PageAnchor::setColourKey(
        std::optional<annotation::ColourKey> key
    ) -> Status
    {
        UF_TRY_VALUE(
            updated,
            setElementColourKey(m_page.draftCopy(), m_id, key)
        );
        m_page.replaceDraft(std::move(updated));
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
        // One element, deleted once: deleteRecognizer withdraws every page's
        // reference to it along with the element itself.
        UF_TRY_VALUE(deleted, deleteRecognizer(m_page.draftCopy(), m_id));
        m_page.replaceDraft(deleted.draft);
        return deleted;
    }
}
