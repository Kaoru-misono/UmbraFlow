#include "workbench-app.hpp"

#include <annotation/resource.hpp>

#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <random>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
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

    AppState::Selection::Selection(Screen screen) noexcept
        : m_value{screen}
    {
    }

    AppState::Selection::Selection(Page page) noexcept
        : m_value{page}
    {
    }

    AppState::Selection::Selection(Element element) noexcept
        : m_value{std::move(element)}
    {
    }

    auto AppState::Selection::asScreen() const noexcept -> std::optional<Screen>
    {
        if (auto const* screen = std::get_if<Screen>(&m_value))
        {
            return *screen;
        }
        return std::nullopt;
    }

    auto AppState::Selection::asPage() const noexcept -> std::optional<Page>
    {
        if (auto const* page = std::get_if<Page>(&m_value))
        {
            return *page;
        }
        return std::nullopt;
    }

    auto AppState::Selection::asElement() const noexcept -> std::optional<Element>
    {
        if (auto const* element = std::get_if<Element>(&m_value))
        {
            return *element;
        }
        return std::nullopt;
    }

    auto AppState::Selection::shownScreen() const noexcept
        -> std::optional<annotation::SourceId>
    {
        if (auto const* screen = std::get_if<Screen>(&m_value))
        {
            return screen->sourceId;
        }
        if (auto const* element = std::get_if<Element>(&m_value))
        {
            return element->shownScreen;
        }
        return std::nullopt;
    }

    auto AppState::Selection::recognizer() const noexcept
        -> std::optional<annotation::ElementId>
    {
        if (auto const* element = std::get_if<Element>(&m_value))
        {
            return element->recognizerId;
        }
        return std::nullopt;
    }

    auto AppState::Selection::pageContext() const noexcept
        -> std::optional<annotation::PageId>
    {
        if (auto const* element = std::get_if<Element>(&m_value))
        {
            return element->pageContext;
        }
        return std::nullopt;
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

    auto AppState::revision() const noexcept -> uint64
    {
        return m_history.revision();
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
                &annotation::AuthoringSourceAsset::id
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

    auto AppState::selection() const noexcept -> Selection const&
    {
        return m_selection;
    }

    auto AppState::selectedSourceId() const noexcept
        -> std::optional<annotation::SourceId>
    {
        return m_selection.shownScreen();
    }

    auto AppState::selectedRecognizerId() const noexcept
        -> std::optional<annotation::ElementId>
    {
        return m_selection.recognizer();
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

    auto AppState::previewInvalidated() const noexcept -> bool
    {
        return m_previewInvalidated;
    }

    auto AppState::modelCheckInvalidated() const noexcept -> bool
    {
        return m_modelCheckInvalidated;
    }

    auto AppState::invalidatePreview() -> void
    {
        if (m_lastPreview.has_value())
        {
            m_previewInvalidated = true;
        }
        m_lastPreview.reset();
    }

    auto AppState::invalidateModelCheck() -> void
    {
        if (m_lastModelCheck.has_value())
        {
            m_modelCheckInvalidated = true;
        }
        m_lastModelCheck.reset();
    }

    auto AppState::dirty() const noexcept -> bool
    {
        // The document is unsaved when its edit-history position differs from the
        // one recorded at the last save or load. Undo and redo restore a
        // position, so undoing back to the saved state reads clean and redoing
        // past it reads dirty again.
        return m_history.position() != m_savedPosition;
    }

    auto AppState::applyEdit(AuthoringDraft const& draft) -> Result<bool>
    {
        UF_TRY_VALUE(changed, m_history.apply(draft));
        if (changed)
        {
            // The stored preview and model check describe the pre-edit document,
            // so a committed mutation invalidates both and marks them stale.
            invalidatePreview();
            invalidateModelCheck();
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
        if (edited.sources.empty())
        {
            edited.fingerprint = source.spec.fingerprint;
        }
        edited.sources.emplace_back(
            EditableSource{
                .id          = source.spec.id,
                .contentHash = source.spec.contentHash,
                .fingerprint = source.spec.fingerprint,
                .provenance  = source.spec.provenance,
            }
        );

        UF_TRY_VALUE(changed, applyEdit(edited));
        if (changed)
        {
            select(Selection::Screen{source.spec.id});
            // The cache is keyed by SourceId; a fresh mint never collides, but
            // guard the invariant so a repeat id replaces rather than duplicates.
            auto const cached = std::ranges::find(
                m_sources,
                source.spec.id,
                &annotation::AuthoringSourceAsset::id
            );
            if (cached == m_sources.end())
            {
                m_sources.emplace_back(std::move(source.asset));
            }
            else
            {
                *cached = std::move(source.asset);
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
        // The document moved, so the stored preview and model check no longer
        // describe it, and the selection may now point at an entity this revision
        // does not hold.
        invalidatePreview();
        invalidateModelCheck();
        reconcileSelectionToDocument();
        return true;
    }

    auto AppState::redo() -> bool
    {
        if (!m_history.redo())
        {
            return false;
        }
        invalidatePreview();
        invalidateModelCheck();
        reconcileSelectionToDocument();
        return true;
    }

    auto AppState::reconcileSelectionToDocument() -> void
    {
        auto const& document = m_history.document();

        auto const sourcePresent = [&document](annotation::SourceId id) -> bool
        {
            return std::ranges::any_of(
                document.sources(),
                [id](annotation::AuthoringSource const& source)
                {
                    return source.id() == id;
                }
            );
        };
        auto const recognizerPresent =
            [&document](annotation::ElementId id) -> bool
        {
            return std::ranges::any_of(
                document.catalog().recognizers(),
                [id](annotation::RecognizerDefinition const& recognizer)
                {
                    return recognizer.id() == id;
                }
            );
        };
        auto const pagePresent = [&document](annotation::PageId id) -> bool
        {
            return document.catalog().findPage(id) != nullptr;
        };

        if (auto const screen = m_selection.asScreen())
        {
            if (!sourcePresent(screen->sourceId))
            {
                m_selection = Selection{};
            }
            return;
        }
        if (auto const page = m_selection.asPage())
        {
            if (!pagePresent(page->pageId))
            {
                m_selection = Selection{};
            }
            return;
        }
        if (auto const element = m_selection.asElement())
        {
            // A shown screen that vanished cannot be drawn, so drop it first; a
            // deleted element then degrades to whatever screen remains, and to
            // nothing when none does.
            auto shown = element->shownScreen;
            if (shown.has_value() && !sourcePresent(*shown))
            {
                shown.reset();
            }
            if (!recognizerPresent(element->recognizerId))
            {
                m_selection = shown.has_value()
                    ? Selection{Selection::Screen{*shown}}
                    : Selection{};
                return;
            }
            auto next        = *element;
            next.shownScreen = shown;
            if (next.pageContext.has_value() && !pagePresent(*next.pageContext))
            {
                next.pageContext.reset();
            }
            m_selection = Selection{std::move(next)};
        }
    }

    auto AppState::select(Selection selection) noexcept -> void
    {
        // Inherit the currently shown screen into an element that names none, so
        // following a freshly created entity leaves the shown image untouched --
        // the behaviour the old setSelectedRecognizerId-without-a-source had.
        if (
            auto const element = selection.asElement();
            element.has_value() && !element->shownScreen.has_value()
        )
        {
            auto inherited        = *element;
            inherited.shownScreen = m_selection.shownScreen();
            selection             = Selection{std::move(inherited)};
        }

        auto const previousScreen = m_selection.shownScreen();
        m_selection               = std::move(selection);
        // A preview is evaluated against the shown screen, so it goes stale only
        // when that screen changes; reselecting the same screen keeps it. The
        // model check spans every screen and so is left alone by a selection move.
        if (m_selection.shownScreen() != previousScreen)
        {
            invalidatePreview();
        }
    }

    auto AppState::setCanvasView(CanvasView view) noexcept -> void
    {
        m_canvasView = view;
    }

    auto AppState::setLastPreview(PreviewResult preview) -> void
    {
        m_lastPreview        = std::move(preview);
        m_previewInvalidated = false;
    }

    auto AppState::setLastModelCheck(ModelCheck check) -> void
    {
        m_lastModelCheck        = std::move(check);
        m_modelCheckInvalidated = false;
    }

    auto AppState::markSaved() -> void
    {
        m_savedPosition = m_history.position();
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
                        return source.id() == asset.id;
                    }
                );
            }
        );
    }
}
