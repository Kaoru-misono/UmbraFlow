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
    inline constexpr auto g_maximumAuthoringUndoEntries = std::size_t{100};

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
