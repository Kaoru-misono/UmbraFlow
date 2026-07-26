#include "workbench-app.hpp"

#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        constexpr auto k_defaultProjectId  = std::string_view{"personal.workbench"};
        constexpr auto k_defaultWidth      = uint32{1280};
        constexpr auto k_defaultHeight     = uint32{720};
        constexpr auto k_defaultDpi        = uint32{96};
        constexpr auto k_uuidVersionByte   = std::size_t{6};
        constexpr auto k_uuidVariantByte   = std::size_t{8};
        constexpr auto k_randomWordBytes   = std::size_t{4};
    }

    auto mintResourceId() -> annotation::ResourceId
    {
        auto device = std::random_device{};
        auto bytes  = std::array<std::byte, 16>{};

        // std::random_device yields 32-bit words, so fill four bytes per draw.
        for (
            auto offset = std::size_t{0};
            offset < bytes.size();
            offset += k_randomWordBytes
        )
        {
            auto const word = static_cast<uint32>(device());
            bytes.at(offset + 0U) = static_cast<std::byte>(word & 0xFFU);
            bytes.at(offset + 1U) = static_cast<std::byte>((word >> 8U) & 0xFFU);
            bytes.at(offset + 2U) = static_cast<std::byte>((word >> 16U) & 0xFFU);
            bytes.at(offset + 3U) = static_cast<std::byte>((word >> 24U) & 0xFFU);
        }

        // Version 4: clear the high nibble of byte 6 and set it to 0100.
        bytes.at(k_uuidVersionByte) = (
            (bytes.at(k_uuidVersionByte) & std::byte{0x0F}) | std::byte{0x40}
        );
        // Variant 1 (RFC 4122): clear the top two bits of byte 8 and set 10.
        bytes.at(k_uuidVariantByte) = (
            (bytes.at(k_uuidVariantByte) & std::byte{0x3F}) | std::byte{0x80}
        );

        return annotation::ResourceId::fromBytes(bytes);
    }

    AppState::AppState(
        std::filesystem::path projectRoot,
        annotation::AuthoringDocument document,
        std::vector<annotation::AuthoringSourceAsset> sources
    )
        : m_projectRoot{std::move(projectRoot)}
        , m_history{std::move(document)}
        , m_sources{std::move(sources)}
    {
    }

    auto AppState::createEmpty(
        std::filesystem::path projectRoot
    ) -> Result<AppState>
    {
        UF_TRY_VALUE(
            projectId,
            annotation::ProjectId::create(std::string{k_defaultProjectId})
        );
        UF_TRY_VALUE(
            fingerprint,
            annotation::ProjectFingerprint::create(
                k_defaultWidth,
                k_defaultHeight,
                k_defaultDpi,
                k_defaultDpi
            )
        );
        UF_TRY_VALUE(
            document,
            annotation::AuthoringDocument::create(
                std::move(projectId),
                fingerprint,
                {},
                {},
                {},
                {}
            )
        );

        return AppState{
            std::move(projectRoot),
            std::move(document),
            {},
        };
    }

    auto AppState::projectRoot() const noexcept -> std::filesystem::path const&
    {
        return m_projectRoot;
    }

    auto AppState::document() const noexcept
        -> annotation::AuthoringDocument const&
    {
        return m_history.document();
    }

    auto AppState::draft() const -> AuthoringDraft
    {
        return m_history.draft();
    }

    auto AppState::canUndo() const noexcept -> bool
    {
        return m_history.canUndo();
    }

    auto AppState::canRedo() const noexcept -> bool
    {
        return m_history.canRedo();
    }

    auto AppState::sources() const noexcept
        -> std::span<annotation::AuthoringSourceAsset const>
    {
        return m_sources;
    }

    auto AppState::compilerSourceAssets() const
        -> Result<std::vector<annotation::AuthoringSourceAsset>>
    {
        auto const documentSources = document().sources();
        auto assets                = std::vector<annotation::AuthoringSourceAsset>{};
        assets.reserve(documentSources.size());
        for (auto const& source : documentSources)
        {
            auto const cached = std::ranges::find(
                m_sources,
                source.id(),
                &annotation::AuthoringSourceAsset::m_id
            );
            if (cached == m_sources.end())
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    std::format(
                        "workbench source {} has no cached asset; the source "
                        "cache and the document have diverged",
                        source.id().value().toString()
                    )
                );
            }
            assets.emplace_back(*cached);
        }
        return assets;
    }

    auto AppState::selectedSourceId() const noexcept
        -> std::optional<annotation::SourceId>
    {
        return m_selectedSourceId;
    }

    auto AppState::selectedRecognizerId() const noexcept
        -> std::optional<annotation::RecognizerId>
    {
        return m_selectedRecognizerId;
    }

    auto AppState::canvasView() const noexcept -> CanvasView
    {
        return m_canvasView;
    }

    auto AppState::lastPreview() const noexcept
        -> std::optional<PreviewResult> const&
    {
        return m_lastPreview;
    }

    auto AppState::lastModelCheck() const noexcept
        -> std::optional<ModelCheck> const&
    {
        return m_lastModelCheck;
    }

    auto AppState::dirty() const noexcept -> bool
    {
        // Conservatively stays true after undoing back to the last saved
        // document. AuthoringEditHistory exposes no stable position or revision
        // cursor, and its undo stack is truncated at a fixed size, so there is
        // no cheap value to compare a saved position against; over-reporting
        // only offers a redundant save.
        // TODO(cpp-debt): dirty() over-reports after undo-to-saved-state —
        // ceiling: an unnecessary save is offered; upgrade: add a monotonic
        // revision id to AuthoringEditHistory that undo and redo restore and
        // truncation preserves, record it on a successful save, and compute
        // dirty() as the current revision differing from the saved one.
        return m_dirty;
    }

    auto AppState::applyEdit(AuthoringDraft const& draft) -> Result<bool>
    {
        UF_TRY_VALUE(changed, m_history.apply(draft));
        if (changed)
        {
            m_dirty = true;
            // The stored preview and model check describe the pre-edit document,
            // so a committed mutation invalidates both.
            m_lastPreview.reset();
            m_lastModelCheck.reset();
            // An edit may remove the very entity the selection names -- a
            // deletion always does -- and a selection pointing at something this
            // revision does not hold would be edited into a rejected draft.
            reconcileSelectionToDocument();
        }
        return changed;
    }

    auto AppState::addIngestedSource(IngestedSource source) -> Result<bool>
    {
        auto edited = m_history.draft();
        if (edited.m_sources.empty())
        {
            edited.m_fingerprint = source.m_spec.m_fingerprint;
        }
        edited.m_sources.emplace_back(
            EditableSource{
                .m_id          = source.m_spec.m_id,
                .m_contentHash = source.m_spec.m_contentHash,
                .m_fingerprint = source.m_spec.m_fingerprint,
                .m_provenance  = source.m_spec.m_provenance,
            }
        );

        UF_TRY_VALUE(changed, applyEdit(edited));
        if (changed)
        {
            m_selectedSourceId = source.m_spec.m_id;
            // The cache is keyed by SourceId; a fresh mint never collides, but
            // guard the invariant so a repeat id replaces rather than duplicates.
            auto const cached = std::ranges::find(
                m_sources,
                source.m_spec.m_id,
                &annotation::AuthoringSourceAsset::m_id
            );
            if (cached == m_sources.end())
            {
                m_sources.emplace_back(std::move(source.m_asset));
            }
            else
            {
                *cached = std::move(source.m_asset);
            }
        }
        return changed;
    }

    auto AppState::undo() -> bool
    {
        if (!m_history.undo())
        {
            return false;
        }
        m_dirty = true;
        // The document moved, so the stored preview and model check no longer
        // describe it, and the selection may now point at an entity this revision
        // does not hold.
        m_lastPreview.reset();
        m_lastModelCheck.reset();
        reconcileSelectionToDocument();
        return true;
    }

    auto AppState::redo() -> bool
    {
        if (!m_history.redo())
        {
            return false;
        }
        m_dirty = true;
        m_lastPreview.reset();
        m_lastModelCheck.reset();
        reconcileSelectionToDocument();
        return true;
    }

    auto AppState::reconcileSelectionToDocument() -> void
    {
        auto const& document = m_history.document();
        if (m_selectedRecognizerId.has_value())
        {
            auto const target  = *m_selectedRecognizerId;
            auto const present = std::ranges::any_of(
                document.catalog().recognizers(),
                [target](annotation::RecognizerDefinition const& recognizer)
                {
                    return recognizer.id() == target;
                }
            );
            if (!present)
            {
                m_selectedRecognizerId.reset();
            }
        }
        if (m_selectedSourceId.has_value())
        {
            auto const target  = *m_selectedSourceId;
            auto const present = std::ranges::any_of(
                document.sources(),
                [target](annotation::AuthoringSource const& source)
                {
                    return source.id() == target;
                }
            );
            if (!present)
            {
                m_selectedSourceId.reset();
            }
        }
    }

    auto AppState::setSelectedSourceId(
        std::optional<annotation::SourceId> id
    ) noexcept -> void
    {
        if (m_selectedSourceId == id)
        {
            return;
        }
        m_selectedSourceId = id;
        // A preview is evaluated against the selected source, so changing the
        // selection makes the stored preview stale.
        m_lastPreview.reset();
    }

    auto AppState::setSelectedRecognizerId(
        std::optional<annotation::RecognizerId> id
    ) noexcept -> void
    {
        m_selectedRecognizerId = id;
    }

    auto AppState::setCanvasView(CanvasView view) noexcept -> void
    {
        m_canvasView = view;
    }

    auto AppState::setLastPreview(PreviewResult preview) -> void
    {
        m_lastPreview = std::move(preview);
    }

    auto AppState::setLastModelCheck(ModelCheck check) -> void
    {
        m_lastModelCheck = std::move(check);
    }

    auto AppState::markSaved() -> void
    {
        m_dirty = false;
        pruneSourceCacheToDocument();
    }

    auto AppState::pruneSourceCacheToDocument() -> void
    {
        auto const documentSources = document().sources();
        std::erase_if(
            m_sources,
            [documentSources](
                annotation::AuthoringSourceAsset const& asset
            ) -> bool
            {
                return std::ranges::none_of(
                    documentSources,
                    [&asset](annotation::AuthoringSource const& source) -> bool
                    {
                        return source.id() == asset.m_id;
                    }
                );
            }
        );
    }
}
