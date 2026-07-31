#pragma once

#include <annotation/authoring-document.hpp>
#include <annotation/capabilities.hpp>
#include <annotation/catalog.hpp>
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

    // The name every appearance the authoring layer mints carries. One element
    // gets one appearance here, because a rectangle is drawn once; the CLI and
    // the model are what a second one arrives through.
    inline constexpr auto k_defaultVariantName = std::string_view{"default"};

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

    // One appearance of one element, held the way the draft holds everything
    // else: unvalidated. The threshold stays basis points and the rectangle
    // stays a bare rect, because an author drags both through states the
    // document types refuse, and the rebuild is where they are judged.
    struct EditableVariant final
    {
        std::string                          name{};
        annotation::SourceId                 sourceId;
        PixelRect                            templateRect;
        uint32                               similarityBasisPoints{};
        std::optional<annotation::ColourKey> colourKey{};
    };

    // The draft twin of annotation::Interact. The click offset stays in raw
    // coordinates rather than a TemplateOffset because TemplateOffset is
    // validated against a template's extent, and the author moves the template
    // and the offset in separate edits.
    struct EditableInteract final
    {
        std::optional<EditableTemplateOffset> clickOffset{};
    };

    // The draft twin of annotation::ElementCapabilities: the same three
    // optionals, held unvalidated so an author can take one capability off and
    // put another on across two edits. The all-empty set is what
    // ElementCapabilities::create refuses when the draft is rebuilt.
    //
    // The nesting is the point rather than the shape: an element with no
    // interact has nowhere to put a click offset, and no discipline is needed
    // to keep it that way.
    struct EditableCapabilities final
    {
        std::optional<annotation::Identify> identify{};
        std::optional<EditableInteract>     interact{};
        std::optional<annotation::Read>     read{};
    };

    struct EditableRecognizer final
    {
        annotation::ElementId id;
        std::string           name{};
        EditableCapabilities  capabilities{};

        // The element's own search region: the one the anchor pass uses, and
        // the seed a page reference copies when it refines its own.
        PixelRect searchRoi;

        // Ordered, and empty is a legal state: it says this rectangle is
        // located by the page being recognised rather than by pixels of its
        // own, which is what a readable cell needs and what makes such an
        // element unable to be identity evidence.
        std::vector<EditableVariant> variants{};
    };

    // The draft twin of annotation::ExercisedCapabilities. A second type rather
    // than a second use of EditableCapabilities, mirroring the model's own
    // split: what a page adds to identify (which way the evidence points) has
    // no meaning on an element, and the OCR parameters have none on a page.
    struct EditableExercised final
    {
        std::optional<annotation::ExercisedIdentify> identify{};
        std::optional<annotation::ExercisedInteract> interact{};
        std::optional<annotation::ExercisedRead>     read{};
    };

    // One page's use of one element: the draft-side twin of
    // annotation::PageReference, which makeAuthoringDraft and
    // buildAuthoringDocument map across with no inversion. A page's signature
    // is derived from the references exercising identify, so there is nothing
    // else on the page side to hold it.
    struct EditableReference final
    {
        annotation::PageId    pageId;
        annotation::ElementId elementId;
        annotation::Holding   holding{annotation::Holding::Owned};
        EditableExercised     exercised{};

        // Absent means "use the element's own region". A reference exercising
        // identify may not set it: the anchor pass reads the element-level
        // region, and refining it would search the same pixels twice a cycle.
        std::optional<PixelRect> searchRoi{};

        // Which appearance applies on this page, when the page decides it.
        // Carried so a document authored with one survives an edit here.
        std::optional<std::string> variant{};
    };

    struct EditablePage final
    {
        annotation::PageId id;
        std::string        name{};
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
        std::vector<EditableReference>  references{};
        std::vector<EditablePage>       pages{};
        std::vector<EditableRegression> regressions{};
    };

    // The appearance the single-appearance editing verbs read and write. This
    // layer draws one rectangle per element, so it is the first declared
    // variant; an element located by its page declares none and yields nothing.
    // The observation is valid only until the draft is mutated.
    [[nodiscard]]
    auto primaryVariant(
        EditableRecognizer const& recognizer UF_LIFETIME_BOUND
    ) noexcept -> EditableVariant const*;

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

    // Creates a page from one captured screen in a single edit: the first
    // element that identifies it, the page, the reference that exercises
    // identify and thereby derives the page's signature, and the regression
    // case recording that this source is expected to resolve to it. The four
    // cannot be authored one at a time -- a page no reference identifies is
    // rejected -- so a page-per-screen workflow needs them to commit together.
    // Fails when the source is not part of the draft.
    [[nodiscard]]
    auto createPageFromSource(
        AuthoringDraft draft,
        NewPageSpec const& spec
    ) -> Result<CreatedPage>;

    // What capability a newly drawn rectangle is given. The author picks one
    // when they draw, which is the only decision the gesture carries; the model
    // allows a set, and an element gains its second capability by being
    // referenced again rather than by being drawn again.
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

    // Adds one element to a page, its capability and the page's use of it
    // committed together: an element that identifies enters the page's derived
    // signature, one that interacts is authorised there, one that is read joins
    // the same way. Neither half can be made afterwards on its own, because a
    // page with no identify reference and an interacting element no page
    // exercises are both rejected. Fails when the page or the source is not
    // part of the draft.
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
    // derived from the original that stays unique, and the same search region,
    // capabilities, and appearances. The copy inherits the original's page
    // references, retargeted to the new id, so it lands on exactly the pages the
    // original sits on. That is what keeps it valid -- an element declaring
    // interact must be exercised somewhere -- and it matches a model where an
    // element is one thing referenced by N pages. Fails when the source element
    // is not part of the draft.
    [[nodiscard]]
    auto duplicateElement(
        AuthoringDraft draft,
        DuplicateElementSpec const& spec
    ) -> Result<DuplicatedElement>;

    // The pages that reference one element, in reference order, without
    // repeats. Every page-side use is a reference now, so this covers a page
    // that identifies by the element as well as one that clicks or reads it.
    [[nodiscard]]
    auto pagesReferencing(
        AuthoringDraft const& draft,
        annotation::ElementId id
    ) -> std::vector<annotation::PageId>;

    // Withdraws one page's reference to one element. Refused when the page
    // would be left with nothing exercising identify -- it could then recognise
    // no screen at all -- and when the element declares interact and this is the
    // only page exercising it, since the document closure rule forbids the
    // result. Removing an absent reference is idempotent.
    [[nodiscard]]
    auto removeReferenceFromPage(
        AuthoringDraft draft,
        annotation::ElementId id,
        annotation::PageId pageId
    ) -> Result<AuthoringDraft>;

    // Sets or clears the colour key on every appearance of one element. Which
    // pixels count is a fact about the pixels, so it belongs to the appearance
    // they were cut into; this layer authors one appearance per element, and
    // applying the key to all of them is what keeps that true for a document
    // that arrived with more. Fails when the element is not part of the draft.
    [[nodiscard]]
    auto setElementColourKey(
        AuthoringDraft draft,
        annotation::ElementId id,
        std::optional<annotation::ColourKey> colourKey
    ) -> Result<AuthoringDraft>;

    struct ReferenceElementSpec final
    {
        annotation::ElementId elementId;
        annotation::PageId    pageId;

        // What this page will do with the element, and the whole of it.
        // Absent means every use a placement carries on its own -- interact
        // and read, whichever the element declares -- which is what dragging
        // pixels onto a second page asks for.
        //
        // Identify is never among those. Its page-side payload is the role,
        // and whether a mark is evidence FOR this page or AGAINST it is a
        // question the element has no answer to, so a page joins a signature
        // only by asking for it. Read the other way, that is what keeps
        // borrowing honest: exercising interact IS the authorisation to click
        // here, and a page taking up a mark as evidence did not ask for that.
        std::optional<EditableExercised> exercised{};

        // Absent means the element's own region. That is what "this page did
        // not refine it" says, and it is what a caller gets by not asking:
        // pinning a copy of the element's rectangle here would go stale the
        // moment the element's own moved.
        std::optional<PixelRect> searchRoi{};
    };

    struct ReferencedElement final
    {
        AuthoringDraft draft;
        std::string    name{};
    };

    // Puts one existing element on a second page: a new reference to the SAME
    // element, held as Referenced and exercising what the spec asks for. No
    // copy is minted -- one element is referenced by N pages, so a later
    // appearance edit touches it once and every page sees it.
    //
    // One verb for all three capabilities, because a page's use of an element
    // is one set rather than three kinds. It is what makes "sortie clicks this
    // mark, battle clicks it AND is identified by it" one element with two
    // references instead of two rectangles over one patch -- two ids, two
    // templates, two searches a cycle -- which is the duplication the
    // capability model exists to delete.
    //
    // Referenced is the whole of what the old reuse flag tried to say, and it
    // cannot contradict the references the way a flag could: a borrowed element
    // is one whose home page is another.
    //
    // Fails when the element or the page is not in the draft, when that page
    // already references it, when the requested set is empty or exercises
    // something the element does not declare, when nothing is asked for and the
    // element only identifies (such an element joins a page through its
    // signature, not by being placed on it), and when the request exercises
    // identify and refines a search region at once.
    [[nodiscard]]
    auto referenceElementOnPage(
        AuthoringDraft draft,
        ReferenceElementSpec const& spec
    ) -> Result<ReferencedElement>;

    // Points one page's identify evidence at one element: the reference gains
    // the role when the page already has one, and is minted through
    // referenceElementOnPage when it does not -- so a page taking up a mark
    // borrows it rather than claiming to own pixels whose home is elsewhere.
    //
    // Fails for every reason referenceElementOnPage does, and additionally when
    // the page's existing reference refines a search region. The anchor pass
    // reads the element-level region, so dropping that refinement to make room
    // for the role would discard a measurement the author made without saying
    // so; which of the two goes is theirs to decide.
    [[nodiscard]]
    auto setReferenceIdentifyRole(
        AuthoringDraft draft,
        annotation::ElementId id,
        annotation::PageId pageId,
        annotation::SignatureRole role
    ) -> Result<AuthoringDraft>;

    struct RetemplatedElement final
    {
        AuthoringDraft draft;
        std::size_t    referencingPages{};
    };

    // Moves one element's sole appearance. Because an element is a single thing
    // referenced by N pages, correcting its pixels corrects it on every page at
    // once; there are no copies to carry. Each reference may keep its own search
    // region, so the moved template must still fit them all.
    //
    // Fails when the element declares no appearance to move, when it declares
    // more than one (this layer draws one rectangle and cannot choose between
    // them), or when the element's own search region or any reference's could
    // not contain the moved template: silently widening a range the author drew
    // would enlarge both the search cost and the surface for a false match.
    [[nodiscard]]
    auto setElementTemplateRect(
        AuthoringDraft draft,
        annotation::ElementId id,
        PixelRect templateRect
    ) -> Result<RetemplatedElement>;

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

    // A deletion together with what it took with it. Every entity in a document
    // is referenced by others, so removing one always edits its neighbours; the
    // counts let the caller state what else moved. Which counts apply depends on
    // what was deleted, and the rest stay zero.
    struct DeletedEntity final
    {
        AuthoringDraft draft;

        // References that put the deleted element in a page's signature, and
        // page references withdrawn wholesale by a page deletion. Two counts
        // rather than one because they answer different questions: what a page
        // lost from its signature, and what an element lost of its reach.
        std::size_t withdrawnRoles{};
        std::size_t withdrawnReferences{};
        std::size_t removedRegressions{};
    };

    // Removes one element and every page reference to it. Refuses when a page
    // identifies by it and by nothing else, because that page would be left
    // recognising no screen at all; the author decides whether the page goes too
    // or another element takes over.
    [[nodiscard]]
    auto deleteRecognizer(
        AuthoringDraft draft,
        annotation::ElementId id
    ) -> Result<DeletedEntity>;

    // Removes one page, every reference it made, and the regression cases
    // expecting it to resolve, which have no meaning once the page they name is
    // gone. Refuses only when an element declaring interact would be left with
    // no page exercising it, since that is a choice between deleting it and
    // re-pointing it that only the author can make.
    [[nodiscard]]
    auto deletePage(
        AuthoringDraft draft,
        annotation::PageId id
    ) -> Result<DeletedEntity>;

    // Removes one source and the regression cases recorded against it, which
    // cannot outlive the image they classify. Refuses while any appearance is
    // still cut from the source, since a rectangle is only meaningful against
    // the image it was drawn on.
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
