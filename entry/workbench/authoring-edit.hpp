#pragma once

#include <annotation/authoring-document.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <cstddef>
#include <optional>
#include <string>
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

    // Removes one page and withdraws it from every recognizer that authorizes it.
    // Refuses when an action target would be left authorizing no page, or when a
    // regression expects the page to resolve: silently rewriting a recorded
    // expectation would destroy the intent the case was written to pin.
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
