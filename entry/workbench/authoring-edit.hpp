#pragma once

#include <annotation/authoring-document.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uf::workbench
{
    inline constexpr auto k_maximumAuthoringUndoEntries = std::size_t{100};

    struct EditableTemplateOffset final
    {
        uint32 m_x{};
        uint32 m_y{};
    };

    struct EditableSource final
    {
        annotation::SourceId           m_id;
        annotation::ContentHash        m_contentHash;
        annotation::ProjectFingerprint m_fingerprint;
        annotation::SourceProvenance   m_provenance{};
    };

    struct EditableRecognizer final
    {
        annotation::RecognizerId              m_id;
        std::string                           m_name{};
        annotation::AnnotationType            m_annotationType{};
        annotation::SourceId                  m_sourceId;
        PixelRect                             m_templateRect;
        PixelRect                             m_searchRoi;
        uint32                                m_similarityBasisPoints{};
        std::optional<EditableTemplateOffset> m_defaultClick{};
        std::vector<annotation::PageId>       m_allowedPageIds{};

        // The author marked these pixels as reusable on other pages. Intent
        // rather than fact: it is set before any second page exists, which is
        // the whole point -- it is what puts the element somewhere the author
        // can reach it from the page they want to put it on.
        bool m_shared{};
    };

    struct EditablePage final
    {
        annotation::PageId                    m_id;
        std::string                           m_name{};
        std::vector<annotation::RecognizerId> m_required{};
        std::vector<annotation::RecognizerId> m_forbidden{};
    };

    struct EditableRegression final
    {
        annotation::RegressionId             m_id;
        annotation::SourceId                 m_sourceId;
        annotation::RegressionClassification m_classification{};
        annotation::RegressionExpectation    m_expectation;
    };

    struct AuthoringDraft final
    {
        annotation::ProjectId           m_projectId;
        annotation::ProjectFingerprint  m_fingerprint;
        std::vector<EditableSource>     m_sources{};
        std::vector<EditableRecognizer> m_recognizers{};
        std::vector<EditablePage>       m_pages{};
        std::vector<EditableRegression> m_regressions{};
    };

    [[nodiscard]]
    auto makeAuthoringDraft(
        annotation::AuthoringDocument const& document
    ) -> AuthoringDraft;

    [[nodiscard]]
    auto buildAuthoringDocument(
        AuthoringDraft const& draft
    ) -> Result<annotation::AuthoringDocument>;

    // The first free "<stem>_N" name in a draft. The catalog requires names to be
    // unique across recognizers and pages rather than within a kind, so both are
    // checked. One of the first N + 1 candidates is always free, so this ends.
    [[nodiscard]]
    auto freshAuthoringName(
        AuthoringDraft const& draft,
        std::string_view stem
    ) -> std::string;

    // Everything creating a page needs from the caller. The three ids are minted
    // by the caller because minting is not pure and these functions have to stay
    // testable; the names are derived from the draft.
    struct NewPageSpec final
    {
        annotation::PageId       m_pageId;
        annotation::RecognizerId m_anchorId;
        annotation::RegressionId m_regressionId;
        annotation::SourceId     m_sourceId;
        PixelRect                m_templateRect;
        PixelRect                m_searchRoi;
        uint32                   m_similarityBasisPoints{};
    };

    struct CreatedPage final
    {
        AuthoringDraft m_draft;
        std::string    m_pageName{};
        std::string    m_anchorName{};
    };

    // Creates a page from one captured screen in a single edit: the first anchor
    // that identifies it, the page whose signature requires that anchor, and the
    // regression case recording that this source is expected to resolve to it.
    // The three cannot be authored one at a time -- a page with an empty
    // signature is rejected, and an anchor belongs to a page only through a
    // signature -- so a page-per-screen workflow needs them to commit together.
    // Fails when the source is not part of the draft.
    [[nodiscard]]
    auto createPageFromSource(
        AuthoringDraft draft,
        NewPageSpec const& spec
    ) -> Result<CreatedPage>;

    // What a new recognizer is to the page it is being added to. An anchor is
    // evidence that the screen is that page and joins its signature; an action
    // target is something the author may click there and is authorized on it.
    // The two relationships are unrelated, and which one applies is the only
    // decision the author makes when drawing a rectangle.
    enum class PageMemberKind : uint8
    {
        Anchor,
        ActionTarget,
    };

    struct PageMemberSpec final
    {
        annotation::RecognizerId m_recognizerId;
        annotation::PageId       m_pageId;
        annotation::SourceId     m_sourceId;
        PixelRect                m_templateRect;
        PixelRect                m_searchRoi;
        uint32                   m_similarityBasisPoints{};
        PageMemberKind           m_kind{};
    };

    struct AddedPageMember final
    {
        AuthoringDraft m_draft;
        std::string    m_name{};
    };

    // Adds one recognizer to a page, typed and linked in the same edit: an anchor
    // enters the page's required set, an action target is authorized on the page.
    // Neither link can be made afterwards on its own, because the catalog ties
    // both to the annotation type. Fails when the page or the source is not part
    // of the draft.
    [[nodiscard]]
    auto addPageMember(
        AuthoringDraft draft,
        PageMemberSpec const& spec
    ) -> Result<AddedPageMember>;

    // Every copy of one shared element. Two recognizers are the same element when
    // the author marked them reusable and they were cut from the same pixels for
    // the same purpose: same source image, same template rectangle, same
    // annotation type. The mark is what makes this a statement rather than a
    // coincidence -- without it a page anchor covering the same pixels as a
    // region would be dragged along by an edit to either.
    //
    // A region nobody marked is a group of one, so a plain template drag and a
    // shared one take the same path.
    //
    // The members stay ordinary recognizers: each is authorized on its own page
    // and carries its own search region, so the runtime and the generated
    // manifest never learn that a group exists.
    [[nodiscard]]
    auto sharedRegionMembers(
        AuthoringDraft const& draft,
        annotation::RecognizerId id
    ) -> std::vector<annotation::RecognizerId>;

    // Marks an interactive region as reusable on other pages, or takes the mark
    // off. Marking is what puts it somewhere the author can reach from the page
    // they want to put it on, before any second page exists -- so it is intent,
    // and intent has to be stored.
    //
    // Fails for anything that is not an interactive region, and refuses to unmark
    // an element still on more than one page: that is a choice between removing
    // the copies and keeping them linked, and only the author can make it.
    [[nodiscard]]
    auto setRegionShared(
        AuthoringDraft draft,
        annotation::RecognizerId id,
        bool shared
    ) -> Result<AuthoringDraft>;

    struct SharedRegionSpec final
    {
        annotation::RecognizerId m_recognizerId;
        annotation::RecognizerId m_shareFrom;
        annotation::PageId       m_pageId;
        PixelRect                m_searchRoi;
    };

    // Reuses one interactive region template on another page: a second
    // recognizer with the same source and template rectangle, its own search
    // region, and an authorization on that page alone. Sharing is restricted to
    // interactive regions; a page anchor joins a page through its signature,
    // where reuse means something different and is left to the advanced
    // controls.
    //
    // Fails when the region or the page is not in the draft, when the region is
    // not an interactive region, or when that page already has this element.
    [[nodiscard]]
    auto shareRegionOnPage(
        AuthoringDraft draft,
        SharedRegionSpec const& spec
    ) -> Result<AddedPageMember>;

    struct RetemplatedRegion final
    {
        AuthoringDraft m_draft;
        std::size_t    m_movedMembers{};
    };

    // Moves a recognizer's template rectangle, carrying every other member of its
    // shared element with it -- the whole point of drawing the element once is
    // that correcting it corrects it everywhere. Each member keeps its own search
    // region.
    //
    // Fails when a member search region could not contain the moved template,
    // naming that member: silently widening a range the author drew would enlarge
    // both the search cost and the surface for a false match.
    [[nodiscard]]
    auto retemplateSharedRegion(
        AuthoringDraft draft,
        annotation::RecognizerId id,
        PixelRect templateRect
    ) -> Result<RetemplatedRegion>;

    struct ScreenClaimSpec final
    {
        annotation::RegressionId m_regressionId;
        annotation::SourceId     m_sourceId;
        annotation::PageId       m_pageId;
    };

    // Records that one captured screen is expected to resolve to one page,
    // rewriting the screen's existing case rather than adding a second, since a
    // screen resolves to exactly one page. This is the document's only statement
    // of which screen a page stands for, and the only thing a whole-model check
    // can measure a resolution against. createPageFromSource writes one for every
    // page it creates; this exists for the pages that predate it and for
    // re-pointing a screen at a different page. The regression id is only read
    // when a new case is added. Fails when the screen or the page is not part of
    // the draft.
    [[nodiscard]]
    auto claimScreenForPage(
        AuthoringDraft draft,
        ScreenClaimSpec const& spec
    ) -> Result<AuthoringDraft>;

    // A recognizer type change together with every repair the change carried
    // with it. The caller reports all of them: the authorization is a permission
    // the author did not ask for, and the cleared fields make the conversion
    // lossy, so neither may happen silently.
    struct RetypedRecognizer final
    {
        AuthoringDraft                    m_draft;
        std::optional<annotation::PageId> m_authorizedPage{};
        std::size_t                       m_withdrawnRoles{};
        std::size_t                       m_clearedAuthorizations{};
        bool                              m_clearedClick{};
    };

    // Changes one recognizer's annotation type, repairing in the same draft every
    // field the catalog ties to the type, so the whole change commits as a single
    // valid edit. Editing the type on its own can never succeed: an action target
    // must authorize at least one page while a page anchor must authorize none,
    // only an action target may carry a default click, and only a page anchor may
    // appear in a page signature -- so no order of one-field-at-a-time edits
    // reaches the new type. Fails when no repair exists: becoming an action
    // target with no page to authorize, or leaving the page anchor type while
    // being the only recognizer some page names.
    [[nodiscard]]
    auto retypeRecognizer(
        AuthoringDraft draft,
        annotation::RecognizerId id,
        annotation::AnnotationType type
    ) -> Result<RetypedRecognizer>;

    // A deletion together with what it took with it. Every entity in a document
    // is referenced by others, so removing one always edits its neighbours; the
    // counts let the caller state what else moved. Which counts apply depends on
    // what was deleted, and the rest stay zero.
    struct DeletedEntity final
    {
        AuthoringDraft m_draft;
        std::size_t    m_withdrawnRoles{};
        std::size_t    m_clearedAuthorizations{};
        std::size_t    m_removedRegressions{};
    };

    // Removes one recognizer and withdraws it from every page signature that
    // names it. Refuses when a page names it and nothing else, because that page
    // would be left identifying no screen at all; the author decides whether the
    // page goes too or another anchor takes over.
    [[nodiscard]]
    auto deleteRecognizer(
        AuthoringDraft draft,
        annotation::RecognizerId id
    ) -> Result<DeletedEntity>;

    // Removes one page, withdraws it from every recognizer that authorizes it,
    // and removes the regression cases expecting it to resolve, which have no
    // meaning once the page they name is gone. Refuses only when an action target
    // would be left authorizing no page, since that is a choice between deleting
    // it and re-pointing it that only the author can make.
    [[nodiscard]]
    auto deletePage(
        AuthoringDraft draft,
        annotation::PageId id
    ) -> Result<DeletedEntity>;

    // Removes one source and the regression cases recorded against it, which
    // cannot outlive the image they classify. Refuses while any recognizer is
    // still authored on the source, since a recognizer's rectangles are only
    // meaningful against the image they were drawn on.
    [[nodiscard]]
    auto deleteSource(
        AuthoringDraft draft,
        annotation::SourceId id
    ) -> Result<DeletedEntity>;

    class AuthoringEditHistory final
    {
        annotation::AuthoringDocument              m_current;
        std::vector<annotation::AuthoringDocument> m_undo{};
        std::vector<annotation::AuthoringDocument> m_redo{};

    public:
        explicit AuthoringEditHistory(
            annotation::AuthoringDocument document
        );

        [[nodiscard]]
        auto document() const noexcept UF_LIFETIME_BOUND
            -> annotation::AuthoringDocument const&;

        [[nodiscard]] auto draft() const -> AuthoringDraft;
        [[nodiscard]] auto canUndo() const noexcept -> bool;
        [[nodiscard]] auto canRedo() const noexcept -> bool;

        [[nodiscard]]
        auto apply(
            AuthoringDraft const& draft
        ) -> Result<bool>;

        auto undo() -> bool;
        auto redo() -> bool;
    };
}
