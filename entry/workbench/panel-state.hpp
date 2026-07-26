#pragma once

#include "authoring-edit.hpp"
#include "model-check-job.hpp"
#include "app/canvas-math.hpp"

#include <annotation/catalog.hpp>

#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <array>
#include <optional>
#include <string>

namespace uf::workbench
{
    // A draft a panel wants committed, held until the panels that borrow into the
    // document have finished drawing. A recognizer definition, page signature, or
    // source asset a panel reads is owned by the document, and committing
    // replaces the document wholesale -- so committing mid-draw would leave the
    // rest of that panel reading freed storage. The request is applied once per
    // frame, after the borrowing panels and before the actions panel, which
    // re-reads everything it touches.
    struct PendingEdit final
    {
        AuthoringDraft m_draft;
        std::string    m_description{};

        // Selected once the edit lands, so creating an entity can select it
        // without the selection dangling at an id a rejected edit never added.
        std::optional<annotation::RecognizerId> m_selectRecognizer{};
        std::optional<annotation::SourceId>     m_selectSource{};
    };

    // Which rectangle a canvas drag is editing, so a released gesture commits the
    // right recognizer field. None means no drag is in progress.
    enum class CanvasDragTarget : uint8
    {
        None,
        TemplateRect,
        SearchRoi,
    };

    // The panels' own between-frame widget state, owned by the entry point and
    // passed back each frame. It holds text buffers the immediate-mode widgets
    // write into and the in-progress canvas drag, none of which belong in the
    // platform-free AppState. A drag records the rectangle it started from so the
    // edit commits exactly once, on release, as a single undo entry.
    //
    // Nothing here is an ImGui type. That is what lets the actions a panel
    // requests live in authoring-actions and model-check-view, which
    // test-workbench links directly; only the drawing needs the GUI.
    struct PanelUiState final
    {
        std::array<char, 256> m_targetTitle{};
        std::array<char, 256> m_nameBuffer{};

        std::optional<annotation::RecognizerId> m_nameBufferFor{};
        std::string                             m_nameSeededValue{};
        bool                                    m_nameInputActive{};
        std::string                             m_statusLine{};
        std::string                             m_lastLoggedStatus{};
        std::optional<PendingEdit>              m_pendingEdit{};

        CanvasDragTarget            m_dragTarget{CanvasDragTarget::None};
        std::optional<RectGripKind> m_dragGrip{};
        std::optional<PixelRect>    m_dragStartRect{};

        // The shared region being dragged onto a page, remembered from the frame
        // the drag began. It cannot be a frame-local handed from the drag source
        // to the drop target: on the frame the mouse is released the source no
        // longer submits itself -- ImGui has already cleared the active id -- and
        // that is exactly the frame the drop is accepted on.
        std::optional<annotation::RecognizerId> m_draggedRegion{};

        // The whole-model check runs off the GUI thread, so it lives across
        // frames. Neither copyable nor movable, which is why PanelUiState is
        // built once and only ever passed by reference.
        ModelCheckJob m_modelCheck{};
    };
}
