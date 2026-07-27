#pragma once

#include "authoring-edit.hpp"
#include "page-view.hpp"

#include <annotation/authoring-document.hpp>
#include <annotation/catalog.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <optional>
#include <string>

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
        // EditPage::MemberId. Same key the view snapshot uses; kept as one name
        // so both sides rebase together when the placement key arrives.
        using MemberId = PageView::MemberId;

        // The screen a new anchor is authored against. Everything else -- the
        // starting rectangles and the default threshold -- follows from the
        // project, exactly as drawing a "+ Identifying mark" does today.
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
        // kind the author picked, the template rectangle they dragged out, and,
        // derived from it, the initial per-page search region. Draw-to-create
        // cannot be a create-then-retemplate pair -- the one-commit-per-frame
        // queue rejects the second edit -- so the drawn geometry has to ride in
        // with the creation.
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

        // The outcome of placing a shared element onto a page. It names the new
        // per-page member so the caller can select it. In this phase it carries
        // no pixel score: scoring needs the source assets, which live in
        // AppState and which this layer deliberately does not borrow (the
        // editing layer owns a draft copy and nothing else). The panels keep
        // scoring at draw time through the existing id-keyed lookups, where the
        // live document and its assets are in reach.
        struct SharedRegionScore final
        {
            MemberId    id;
            std::string name{};
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

        // Signature edits.
        [[nodiscard]] auto placeAnchor(NewAnchorSpec const& spec) -> Result<AddedAnchor>;
        [[nodiscard]] auto requireAnchor(MemberId member) -> Status;
        [[nodiscard]] auto forbidAnchor(MemberId member) -> Status;
        [[nodiscard]] auto claimScreen(annotation::SourceId source) -> Status;

        [[nodiscard]]
        auto classifyScreen(
            annotation::SourceId source,
            annotation::RegressionClassification classification
        ) -> Status;

        // Region membership. placeInfo mints an info region rather than an
        // interactive one -- same page placement, no click offset -- so an info
        // element can be authored directly instead of only reached by retype.
        [[nodiscard]] auto placeRegion(NewRegionSpec const& spec) -> Result<AddedRegion>;
        [[nodiscard]] auto placeInfo(NewRegionSpec const& spec) -> Result<AddedRegion>;

        // Adds a member drawn on the canvas: the anchor, interactive region, or
        // info region the author dragged out, with a search region seeded from the
        // drawn template. Refuses a template smaller than a minimum extent, since a
        // box too small to see is one the author did not mean to draw.
        [[nodiscard]] auto placeDrawn(NewDrawnMemberSpec const& spec) -> Result<AddedMember>;

        [[nodiscard]] auto placeExisting(MemberId member) -> Status;

        [[nodiscard]]
        auto acceptSharedRegion(MemberId from) -> Result<SharedRegionScore>;

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

    // A live borrow onto one interactive region of the page being edited. Holds
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

        // Data, read from the owning draft.
        [[nodiscard]] auto name() const -> std::string;
        [[nodiscard]] auto templateRect() const -> PixelRect;
        [[nodiscard]] auto searchRoiOnThisPage() const -> PixelRect;
        [[nodiscard]] auto threshold() const -> uint32;
        [[nodiscard]] auto clickOffset() const -> std::optional<EditableTemplateOffset>;
        [[nodiscard]] auto isShared() const -> bool;
        [[nodiscard]] auto pagesPlacedOn() const -> std::vector<annotation::PageId>;

        // Operations, mutating the owning EditPage's draft.
        [[nodiscard]] auto rename(std::string name) -> Status;
        [[nodiscard]] auto setThreshold(uint32 basisPoints) -> Status;
        [[nodiscard]] auto setClickOffset(std::optional<EditableTemplateOffset> click) -> Status;
        [[nodiscard]] auto setSearchRoi(PixelRect roi) -> Status;

        // Carries every member of the shared element, exactly as retemplating
        // does today: the element is drawn once, so correcting its pixels
        // corrects it on every page it appears on. Each member keeps its own
        // search region.
        [[nodiscard]] auto setTemplateRect(PixelRect templateRect) -> Status;

        [[nodiscard]] auto setShared(bool shared) -> Status;

        [[nodiscard]]
        auto shareToPage(annotation::PageId page) -> Result<EditPage::SharedRegionScore>;

        // Withdraws this page's placement. When the page was this region's only
        // placement, the region's per-page copy has no reason to remain and is
        // removed entirely.
        [[nodiscard]] auto removeFromThisPage() -> Status;

        [[nodiscard]] auto deleteEverywhere() -> Result<DeletedEntity>;
    };

    // The symmetric handle for a signature member: an anchor that identifies (or
    // must not identify) the page. Same borrow contract as InteractiveRegion --
    // non-copyable, non-movable, id-resolved every call, mintable only by
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
        [[nodiscard]] auto templateRect() const -> PixelRect;
        [[nodiscard]] auto searchRoi() const -> PixelRect;
        [[nodiscard]] auto threshold() const -> uint32;
        [[nodiscard]] auto isShared() const -> bool;

        // Operations, mutating the owning EditPage's draft.
        [[nodiscard]] auto rename(std::string name) -> Status;
        [[nodiscard]] auto setThreshold(uint32 basisPoints) -> Status;
        [[nodiscard]] auto setSearchRoi(PixelRect roi) -> Status;
        [[nodiscard]] auto setTemplateRect(PixelRect templateRect) -> Status;

        // Moves this anchor between the page's required and forbidden sets. An
        // anchor identifies the page (required) or must not match on it
        // (forbidden); it is always exactly one of the two here.
        [[nodiscard]] auto require() -> Status;
        [[nodiscard]] auto forbid() -> Status;

        [[nodiscard]] auto deleteEverywhere() -> Result<DeletedEntity>;
    };
}
