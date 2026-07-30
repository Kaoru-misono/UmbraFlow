#pragma once

#include <annotation/authoring-document.hpp>
#include <annotation/resource.hpp>

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
        uint32 x{};
        uint32 y{};
    };

    struct EditableSource final
    {
        annotation::SourceId           id;
        annotation::ContentHash        contentHash;
        annotation::ProjectFingerprint fingerprint;
        annotation::SourceProvenance   provenance{};
    };

    struct EditableRecognizer final
    {
        annotation::ElementId      id;
        std::string                name{};
        annotation::AnnotationType annotationType{};
        annotation::SourceId       sourceId;
        PixelRect                  templateRect;

        // The element's authoring-time default search region. Under the v2 model
        // page membership lives on placements, so this is the seed a fresh
        // placement copies; each placement then carries its own per-page ROI.
        PixelRect                             searchRoi;
        uint32                                similarityBasisPoints{};
        std::optional<EditableTemplateOffset> defaultClick{};

        // Which pixels of the template count, as the colour the author picked
        // plus a tolerance around it. Absent means all of them. The key is what
        // is stored, never the mask it bakes into: reopening a project has to
        // let the author move the tolerance and watch the selection move.
        std::optional<annotation::ColourKey> colourKey{};

        // The author marked these pixels as reusable on other pages. Intent
        // rather than fact: it is set before any second page exists, which is
        // the whole point -- it is what puts the element somewhere the author
        // can reach it from the page they want to put it on. Under v2 it no
        // longer groups copies; one element is placed on N pages natively.
        bool shared{};
    };

    // One interactive or info element placed on one page, with where the runtime
    // should look for it there. The direct draft-side twin of
    // annotation::AuthoringPlacement: makeAuthoringDraft and buildAuthoringDocument
    // map it across with no inversion.
    struct EditablePlacement final
    {
        annotation::PageId    pageId;
        annotation::ElementId elementId;
        PixelRect             searchRoi;
    };

    struct EditablePage final
    {
        annotation::PageId                 id;
        std::string                        name{};
        std::vector<annotation::ElementId> required{};
        std::vector<annotation::ElementId> forbidden{};
    };

    struct EditableRegression final
    {
        annotation::RegressionId             id;
        annotation::SourceId                 sourceId;
        annotation::RegressionClassification classification{};
        annotation::RegressionExpectation    expectation;
    };

    struct AuthoringDraft final
    {
        annotation::ProjectId           projectId;
        annotation::ProjectFingerprint  fingerprint;
        std::vector<EditableSource>     sources{};
        std::vector<EditableRecognizer> recognizers{};
        std::vector<EditablePlacement>  placements{};
        std::vector<EditablePage>       pages{};
        std::vector<EditableRegression> regressions{};
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
        annotation::PageId       pageId;
        annotation::ElementId    anchorId;
        annotation::RegressionId regressionId;
        annotation::SourceId     sourceId;
        PixelRect                templateRect;
        PixelRect                searchRoi;
        uint32                   similarityBasisPoints{};
    };

    struct CreatedPage final
    {
        AuthoringDraft draft;
        std::string    pageName{};
        std::string    anchorName{};
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
    // target is something the author may click there and is authorized on it
    // through a placement; an info region is a state region the task reads and
    // joins the page through a placement too, but carries no click. Which one
    // applies is the only decision the author makes when drawing a rectangle.
    enum class PageMemberKind : uint8
    {
        Anchor,
        ActionTarget,
        InfoRegion,
    };

    struct PageMemberSpec final
    {
        annotation::ElementId recognizerId;
        annotation::PageId    pageId;
        annotation::SourceId  sourceId;
        PixelRect             templateRect;
        PixelRect             searchRoi;
        uint32                similarityBasisPoints{};
        PageMemberKind        kind{};
    };

    struct AddedPageMember final
    {
        AuthoringDraft draft;
        std::string    name{};
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

    struct DuplicateElementSpec final
    {
        annotation::ElementId sourceElementId;
        annotation::ElementId newElementId;
    };

    struct DuplicatedElement final
    {
        AuthoringDraft draft;
        std::string    name{};
    };

    // Copies one element into an independent second element: a fresh id, a name
    // derived from the original that stays unique, and the same template rect,
    // search ROI, threshold, click offset, kind, and reuse mark. The copy
    // inherits the original's placements, retargeted to the new id, so it lands
    // on exactly the pages the original sits on. That is what keeps an action
    // target valid -- the closure rule requires an interactive element to be
    // placed on at least one page, so a placeless copy of one could never build
    // -- and it matches the v2 model where an element is one thing placed on N
    // pages: the copy is a new such thing with its own placement set. An anchor
    // carries no placements and joins pages through a signature, so its copy is
    // an unassigned spare the author then gives a role. Fails when the source
    // element is not part of the draft.
    [[nodiscard]]
    auto duplicateElement(
        AuthoringDraft draft,
        DuplicateElementSpec const& spec
    ) -> Result<DuplicatedElement>;

    // The pages one element is placed on, in placement order, without repeats.
    // Under v2 an element is one thing placed on N pages, so this is simply the
    // pages its placements name; an anchor, which joins pages through its
    // signature rather than a placement, yields none.
    [[nodiscard]]
    auto pagesPlacedOn(
        AuthoringDraft const& draft,
        annotation::ElementId id
    ) -> std::vector<annotation::PageId>;

    // Withdraws one info or interactive element from one page. Page anchors use
    // signature roles rather than placements and are refused. An interactive
    // element's last placement is also refused so the edit cannot violate the
    // document closure rule. Removing an absent placement is idempotent.
    [[nodiscard]]
    auto removePlacementFromPage(
        AuthoringDraft draft,
        annotation::ElementId id,
        annotation::PageId pageId
    ) -> Result<AuthoringDraft>;

    // Marks an interactive region as reusable on other pages, or takes the mark
    // off. Marking is what puts it in the Shared regions palette, where the
    // author can reach it from the page they want to put it on, before any second
    // page exists -- so it is intent, and intent has to be stored.
    //
    // Under v2 the mark no longer groups copies -- one element is placed on N
    // pages natively -- so it is pure intent and toggling it never touches page
    // membership. Fails only for anything that is not an interactive region.
    [[nodiscard]]
    auto setRegionShared(
        AuthoringDraft draft,
        annotation::ElementId id,
        bool shared
    ) -> Result<AuthoringDraft>;

    // Sets or clears the colour key on one element. Every kind may carry one: a
    // menu entry drawn as white text over changing artwork is as often the
    // anchor that identifies a page as it is something to click, and it is
    // exactly the case the key exists for. Passing no key restores the fully
    // opaque template. Fails when the element is not part of the draft.
    [[nodiscard]]
    auto setElementColourKey(
        AuthoringDraft draft,
        annotation::ElementId id,
        std::optional<annotation::ColourKey> colourKey
    ) -> Result<AuthoringDraft>;

    struct SharedRegionSpec final
    {
        annotation::ElementId elementId;
        annotation::PageId    pageId;
        PixelRect             searchRoi;
    };

    struct SharedRegionPlacement final
    {
        AuthoringDraft draft;
        std::string    name{};
    };

    // Places one interactive element on another page: a new placement of the SAME
    // element, carrying its own search region, seeded by the caller from the
    // origin placement. No recognizer copy is minted -- the element is one thing
    // placed on N pages, so a later template edit touches it once and every
    // placement sees it. The element is marked reusable, since reaching a second
    // page is what being shared means.
    //
    // Fails when the element or the page is not in the draft, when the element is
    // not an interactive region, or when that page already places it.
    [[nodiscard]]
    auto shareRegionOnPage(
        AuthoringDraft draft,
        SharedRegionSpec const& spec
    ) -> Result<SharedRegionPlacement>;

    struct RetemplatedRegion final
    {
        AuthoringDraft draft;
        std::size_t    otherPlacements{};
    };

    // Moves one element's template rectangle. Because an element is a single
    // thing placed on N pages, correcting its pixels corrects it on every page at
    // once; there are no copies to carry. Each placement keeps its own search
    // region, so the moved template must still fit them all.
    //
    // Fails when the element's own search region or any placement's search region
    // could not contain the moved template: silently widening a range the author
    // drew would enlarge both the search cost and the surface for a false match.
    [[nodiscard]]
    auto setElementTemplateRect(
        AuthoringDraft draft,
        annotation::ElementId id,
        PixelRect templateRect
    ) -> Result<RetemplatedRegion>;

    struct ScreenClaimSpec final
    {
        annotation::RegressionId regressionId;
        annotation::SourceId     sourceId;
        annotation::PageId       pageId;
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

    // The two regression expectations that name no page: a screen the model must
    // resolve to none of the project's pages, and one it is allowed to find
    // ambiguous. A claim resolves to one page and belongs on EditPage, which is
    // always opened against a page; these two are recorded against a screen
    // alone, so they cannot live there and stay in this free-function layer.
    enum class PagelessExpectation : uint8
    {
        Unknown,
        Ambiguous,
    };

    struct ScreenExpectationSpec final
    {
        annotation::RegressionId regressionId;
        annotation::SourceId     sourceId;
        PagelessExpectation      expectation{};
    };

    // Records that one captured screen is expected to resolve to no page, or is
    // allowed to be ambiguous, rewriting the screen's existing case rather than
    // adding a second, since a screen carries exactly one case. The
    // classification label is paired with the expectation -- Negative with
    // Unknown, Confusable with Ambiguous -- the same way claimScreenForPage pairs
    // Positive with a resolution. The regression id is only read when a new case
    // is added. Fails when the screen is not part of the draft.
    [[nodiscard]]
    auto recordScreenExpectation(
        AuthoringDraft draft,
        ScreenExpectationSpec const& spec
    ) -> Result<AuthoringDraft>;

    // A recognizer type change together with every repair the change carried
    // with it. The caller reports all of them: the authorization is a permission
    // the author did not ask for, and the cleared fields make the conversion
    // lossy, so neither may happen silently.
    struct RetypedRecognizer final
    {
        AuthoringDraft                    draft;
        std::optional<annotation::PageId> authorizedPage{};
        std::size_t                       withdrawnRoles{};
        std::size_t                       clearedAuthorizations{};
        bool                              clearedClick{};
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
        annotation::ElementId id,
        annotation::AnnotationType type
    ) -> Result<RetypedRecognizer>;

    // A deletion together with what it took with it. Every entity in a document
    // is referenced by others, so removing one always edits its neighbours; the
    // counts let the caller state what else moved. Which counts apply depends on
    // what was deleted, and the rest stay zero.
    struct DeletedEntity final
    {
        AuthoringDraft draft;
        std::size_t    withdrawnRoles{};
        std::size_t    clearedAuthorizations{};
        std::size_t    removedRegressions{};
    };

    // Removes one recognizer and withdraws it from every page signature that
    // names it. Refuses when a page names it and nothing else, because that page
    // would be left identifying no screen at all; the author decides whether the
    // page goes too or another anchor takes over.
    [[nodiscard]]
    auto deleteRecognizer(
        AuthoringDraft draft,
        annotation::ElementId id
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
        // One reachable document version paired with the identity of the
        // position it sits at. Undo and redo move a whole snapshot, so restoring
        // a version restores the position that names it -- which is what lets a
        // saved state be recognised again after an undo returns to it.
        struct Snapshot final
        {
            annotation::AuthoringDocument document;
            uint64                        position{};
        };

        annotation::AuthoringDocument m_current;
        std::vector<Snapshot>         m_undo{};
        std::vector<Snapshot>         m_redo{};

        // A monotonic count of the versions this history has moved through. It
        // advances on every applied change and on every undo and redo, so no
        // two document positions the author can reach share a value within a
        // session. An EditPage records it at open() and its commit is refused
        // when the count no longer matches -- a draft built against a version
        // the author has since undone can then never overwrite the one that
        // replaced it. Undo does not restore the prior count: the revision is
        // the history's position in time, not the document's identity.
        uint64 m_revision{};

        // The identity of the current document position, restored by undo and
        // redo rather than advanced by them. A fresh history sits at 0 and each
        // applied change mints the next value, so two positions name the same
        // document state exactly when their identities match. This is the
        // "document identity" m_revision deliberately is not: the dirty flag
        // records it on load and save and reads dirty as the current identity
        // differing from the saved one, so an undo back to a saved state reads
        // clean and a redo past it reads dirty again.
        uint64 m_position{};

        // The next position identity to mint, never reused within a session, so
        // a saved position pushed off the bounded undo stack is simply never
        // reached again and the state stays dirty, as it must.
        uint64 m_nextPosition{1};

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

        // The current position in the sequence of versions, for the stale-commit
        // guard. Advances on every applied change, undo, and redo.
        [[nodiscard]] auto revision() const noexcept -> uint64;

        // The identity of the current document position, for the dirty flag.
        // Restored by undo and redo, unlike revision(): two calls return the
        // same value exactly when the history holds the same document state.
        [[nodiscard]] auto position() const noexcept -> uint64;

        [[nodiscard]]
        auto apply(
            AuthoringDraft const& draft
        ) -> Result<bool>;

        auto undo() -> bool;
        auto redo() -> bool;
    };
}
