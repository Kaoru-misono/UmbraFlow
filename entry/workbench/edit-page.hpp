#pragma once

#include "authoring-edit.hpp"
#include "page-view.hpp"

#include <annotation/authoring-document.hpp>
#include <annotation/resource.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <optional>
#include <string>
#include <vector>

namespace uf::workbench
{
    // Forward declarations: EditPage and the two handles reference one another
    // (EditPage mints handles; handles name EditPage's nested result types, so
    // their definitions follow it), and the commit path borrows the panels'
    // between-frame state, which no part of this header needs the layout of.
    class EditPage;
    class InteractiveRegion;
    class PageAnchor;
    struct PanelUiState;
    class AppState;

    namespace detail
    {
        // Passkey. The handles' constructors must be public so a Result<Handle>
        // can construct one in place -- std::expected is not a friend and cannot
        // reach a private constructor -- but a handle must still be mintable only
        // by EditPage. Only EditPage can create a HandleKey, so only EditPage can
        // call those constructors.
        class HandleKey final
        {
            HandleKey() = default;

            friend class ::uf::workbench::EditPage;
        };
    }


    // One page, opened for editing. Owns a copy of the whole project draft: the
    // draft is the unit of rebuild-and-validate, so there is no such thing as an
    // edited half-page. Every operation mutates the owned copy; the live document
    // is untouched until commit.
    //
    // Frame-scoped, and enforced rather than asked for. The editor records the
    // history revision it was opened against, and a commit whose base is no
    // longer current is refused. An EditPage stored across frames cannot
    // resurrect state the author undid; its commit fails visibly instead.
    class EditPage final
    {
    public:
        // The page-local key this editor speaks in, re-exposed so its API reads
        // EditPage::MemberId. Same key the view snapshot uses.
        using MemberId = PageView::MemberId;

        // The screen a new element is authored against. Everything else -- the
        // starting rectangles and the default threshold -- follows from the
        // project.
        struct NewAnchorSpec final
        {
            annotation::SourceId sourceId;
        };

        struct NewRegionSpec final
        {
            annotation::SourceId sourceId;
        };

        struct AddedAnchor final
        {
            MemberId    id;
            std::string name{};
        };

        struct AddedRegion final
        {
            MemberId    id;
            std::string name{};
        };

        // A member drawn straight onto the canvas: one transaction carrying the
        // capability the author picked and the template rectangle they dragged
        // out. Draw-to-create cannot be a create-then-retemplate pair -- the
        // one-commit-per-frame queue rejects the second edit -- so the drawn
        // geometry has to ride in with the creation.
        struct NewDrawnMemberSpec final
        {
            annotation::SourceId sourceId;
            PageMemberKind       kind{};
            PixelRect            templateRect;
        };

        // The outcome of a draw-to-create, naming the new element so the caller
        // selects it once the commit lands, and echoing the kind it was made as.
        struct AddedMember final
        {
            MemberId       id;
            std::string    name{};
            PageMemberKind kind{};
        };

    private:
        friend class InteractiveRegion;
        friend class PageAnchor;

        AuthoringDraft     m_draft;
        annotation::PageId m_id;
        uint64             m_baseRevision;

        EditPage(
            AuthoringDraft draft,
            annotation::PageId id,
            uint64 baseRevision
        );

    public:
        [[nodiscard]]
        static auto open(
            AppState const& state,
            annotation::PageId id
        ) -> Result<EditPage>;

        [[nodiscard]]
        static auto createFrom(
            AppState const& state,
            annotation::SourceId source
        ) -> Result<EditPage>;

        // The page this editor targets, and the revision it was opened against.
        [[nodiscard]] auto id() const noexcept -> annotation::PageId;
        [[nodiscard]] auto baseRevision() const noexcept -> uint64;

        // The drawing snapshot for this page, built from the owned draft.
        [[nodiscard]] auto view() const -> PageView;

        // Signature edits. Both move this page's reference to the element
        // between the two identify roles, minting the reference when the page
        // does not have one yet.
        [[nodiscard]] auto placeAnchor(NewAnchorSpec const& spec) -> Result<AddedAnchor>;
        [[nodiscard]] auto requireAnchor(MemberId member) -> Status;
        [[nodiscard]] auto forbidAnchor(MemberId member) -> Status;
        [[nodiscard]] auto claimScreen(annotation::SourceId source) -> Status;

        [[nodiscard]]
        auto classifyScreen(
            annotation::SourceId source,
            annotation::RegressionClassification classification
        ) -> Status;

        // Region membership. placeInfo mints an element that is read rather than
        // clicked -- same page reference, no click offset -- so a readable cell
        // can be authored directly.
        [[nodiscard]] auto placeRegion(NewRegionSpec const& spec) -> Result<AddedRegion>;
        [[nodiscard]] auto placeInfo(NewRegionSpec const& spec) -> Result<AddedRegion>;

        // Adds a member drawn on the canvas: the element the author dragged out,
        // with the capability they picked. Refuses a template smaller than a
        // minimum extent, since a box too small to see is one the author did not
        // mean to draw.
        [[nodiscard]] auto placeDrawn(NewDrawnMemberSpec const& spec) -> Result<AddedMember>;

        // References an element this page does not have yet, borrowing pixels
        // whose home is another page. Idempotent: a page that already references
        // the element is left alone.
        [[nodiscard]] auto placeExisting(MemberId member) -> Status;

        // Handles onto members of this page: local-scope only, resolved by id on
        // every call.
        [[nodiscard]] auto anchor(MemberId member) UF_LIFETIME_BOUND -> Result<PageAnchor>;
        [[nodiscard]] auto region(MemberId member) UF_LIFETIME_BOUND -> Result<InteractiveRegion>;

        // The only exits. Both route through the existing requestEdit queue,
        // preserving the one-commit-per-frame guard and, for the selecting form,
        // select-only-after-landing, and both stamp the base revision so a stale
        // commit is refused. Consuming the editor is what makes "one EditPage,
        // one commit" structural.
        auto commit(PanelUiState& ui, std::string description) && -> void;

        auto commitSelecting(
            PanelUiState& ui,
            std::string description,
            MemberId select,
            std::optional<annotation::SourceId> selectSource
        ) && -> void;

    private:
        // Borrowed by the handles, which are friends. draftCopy hands out a copy
        // to feed the value-taking edit transactions; replaceDraft installs the
        // result; findRecognizer resolves a member for a read, valid only until
        // the next mutation.
        [[nodiscard]] auto draftCopy() const -> AuthoringDraft;
        auto replaceDraft(AuthoringDraft draft) -> void;

        [[nodiscard]]
        auto draftView() const noexcept UF_LIFETIME_BOUND -> AuthoringDraft const&;

        [[nodiscard]]
        auto findRecognizer(MemberId member) const UF_LIFETIME_BOUND
            -> EditableRecognizer const*;

        [[nodiscard]] auto pageId() const noexcept -> annotation::PageId;
    };

    // A live borrow onto one clickable element of the page being edited. Holds
    // its owning EditPage and the member's id, and re-resolves the id on every
    // call, so a structural edit made through another handle cannot leave this
    // one pointing at freed storage. Non-copyable and non-movable: it exists
    // only as a local inside the draw that asked for it, and a PanelUiState
    // member of this type does not compile.
    class InteractiveRegion final
    {
        EditPage&          m_page;
        PageView::MemberId m_id;

    public:
        InteractiveRegion(detail::HandleKey key, EditPage& page, PageView::MemberId id);

        InteractiveRegion(InteractiveRegion const&)                        = delete;
        InteractiveRegion(InteractiveRegion&&)                             = delete;
        auto operator=(InteractiveRegion const&) -> InteractiveRegion&     = delete;
        auto operator=(InteractiveRegion&&) -> InteractiveRegion&          = delete;
        ~InteractiveRegion()                                               = default;

        // Data, read from the owning draft. The appearance-derived reads are
        // absent for an element located by its page, which declares none.
        [[nodiscard]] auto name() const -> std::string;
        [[nodiscard]] auto templateRect() const -> std::optional<PixelRect>;
        [[nodiscard]] auto searchRoiOnThisPage() const -> PixelRect;
        [[nodiscard]] auto threshold() const -> std::optional<uint32>;
        [[nodiscard]] auto clickOffset() const -> std::optional<EditableTemplateOffset>;
        [[nodiscard]] auto pagesReferencing() const -> std::vector<annotation::PageId>;

        [[nodiscard]]
        auto colourKey() const -> std::optional<annotation::ColourKey>;

        // Operations, mutating the owning EditPage's draft.
        [[nodiscard]] auto rename(std::string name) -> Status;
        [[nodiscard]] auto setThreshold(uint32 basisPoints) -> Status;

        // Carries every page the element is referenced by, like setTemplateRect:
        // which of its pixels count is a fact about the pixels, drawn once.
        [[nodiscard]]
        auto setColourKey(std::optional<annotation::ColourKey> key) -> Status;
        [[nodiscard]] auto setClickOffset(std::optional<EditableTemplateOffset> click) -> Status;
        [[nodiscard]] auto setSearchRoi(PixelRect roi) -> Status;

        // Carries every page the element is on, exactly as retemplating always
        // did: the element is drawn once, so correcting its pixels corrects it
        // wherever it appears. Each reference keeps its own search region.
        [[nodiscard]] auto setTemplateRect(PixelRect templateRect) -> Status;

        // Puts these pixels on a second page as a borrowed reference.
        [[nodiscard]] auto referenceOnPage(annotation::PageId page) -> Status;

        // Withdraws this page's reference to the element. Refused when it would
        // leave the element clickable nowhere.
        [[nodiscard]] auto removeFromThisPage() -> Status;

        [[nodiscard]] auto deleteEverywhere() -> Result<DeletedEntity>;
    };

    // The symmetric handle for a signature member: an element that identifies
    // (or must not identify) the page. Same borrow contract as InteractiveRegion
    // -- non-copyable, non-movable, id-resolved every call, mintable only by
    // EditPage.
    class PageAnchor final
    {
        EditPage&          m_page;
        PageView::MemberId m_id;

    public:
        PageAnchor(detail::HandleKey key, EditPage& page, PageView::MemberId id);

        PageAnchor(PageAnchor const&)                        = delete;
        PageAnchor(PageAnchor&&)                             = delete;
        auto operator=(PageAnchor const&) -> PageAnchor&     = delete;
        auto operator=(PageAnchor&&) -> PageAnchor&          = delete;
        ~PageAnchor()                                        = default;

        // Data, read from the owning draft.
        [[nodiscard]] auto name() const -> std::string;
        [[nodiscard]] auto templateRect() const -> std::optional<PixelRect>;
        [[nodiscard]] auto searchRoi() const -> PixelRect;
        [[nodiscard]] auto threshold() const -> std::optional<uint32>;

        [[nodiscard]]
        auto colourKey() const -> std::optional<annotation::ColourKey>;

        // Operations, mutating the owning EditPage's draft.
        [[nodiscard]] auto rename(std::string name) -> Status;
        [[nodiscard]] auto setThreshold(uint32 basisPoints) -> Status;

        // The element's own region, never a per-page refinement: the anchor pass
        // runs before any page is known, so a reference exercising identify is
        // refused a region of its own.
        [[nodiscard]] auto setSearchRoi(PixelRect roi) -> Status;
        [[nodiscard]] auto setTemplateRect(PixelRect templateRect) -> Status;

        // An identifying mark is the commonest thing to key: the menu entry a
        // page is identified by is white text over artwork that changes under it.
        [[nodiscard]]
        auto setColourKey(std::optional<annotation::ColourKey> key) -> Status;

        // Moves this element between the page's required and forbidden sets. It
        // is evidence for the page or evidence against it; here it is always
        // exactly one of the two.
        [[nodiscard]] auto require() -> Status;
        [[nodiscard]] auto forbid() -> Status;

        [[nodiscard]] auto deleteEverywhere() -> Result<DeletedEntity>;
    };
}
