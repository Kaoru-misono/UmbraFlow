#include "authoring-edit.hpp"

#include <core/error/contracts.hpp>
#include <core/safety/checked-access.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace uf::workbench
{
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
