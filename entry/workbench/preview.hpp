#pragma once

#include <annotation/authoring-compiler.hpp>
#include <annotation/authoring-document.hpp>
#include <annotation/recognition-runtime.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <vision/sad.hpp>

#include <optional>
#include <span>
#include <vector>

namespace uf::workbench
{
    enum class PreviewPageKind : uint8
    {
        Resolved,
        Unknown,
        Ambiguous,
    };

    // One recognizer's evidence as surfaced to the property/preview panels. It
    // flattens the fields the GUI reads from an AnchorEvidence so the panel layer
    // never depends on the recognition module's evidence type.
    struct PreviewAnchorRow final
    {
        annotation::RecognizerId m_recognizerId;

        bool                     m_hit{};
        std::optional<uint64>    m_sadScore{};
        uint64                   m_maximumSad{};
        std::optional<PixelRect> m_matchedRect{};
    };

    // A recognizer search that a policy limit interrupted before it produced
    // evidence, naming the recognizer that was running and why it stopped.
    struct PreviewStop final
    {
        annotation::RecognizerId m_recognizerId;
        SadSearchStopReason      m_reason{};
    };

    // The outcome of previewing the current document against one source image.
    // The page evaluation always runs: it either classifies the page
    // (m_pageKind, with m_resolvedPageId set only when resolved) or stops
    // (m_pageStop). The action fields are populated only when the selected
    // recognizer is an action target that was evaluated.
    struct PreviewResult final
    {
        std::optional<PreviewPageKind>    m_pageKind{};
        std::optional<annotation::PageId> m_resolvedPageId{};
        std::vector<PreviewAnchorRow>     m_anchorRows{};
        std::optional<PreviewStop>        m_pageStop{};

        std::optional<PreviewAnchorRow> m_actionEvidence{};
        std::optional<PreviewStop>      m_actionStop{};
    };

    // Compiles the document with its in-memory sources, builds a recognition
    // runtime, and evaluates the page against the selected source's image. When
    // the selected recognizer is an action target, its evidence is evaluated too.
    // The policy carries the comparison budget and any deadline or cancellation,
    // so a caller can preview under a real limit or a zero budget.
    [[nodiscard]]
    auto runPreview(
        annotation::AuthoringDocument const& document,
        std::span<annotation::AuthoringSourceAsset const> sourceAssets,
        annotation::SourceId selectedSourceId,
        std::optional<annotation::RecognizerId> selectedRecognizerId,
        annotation::RecognitionPolicy const& policy
    ) -> Result<PreviewResult>;
}
