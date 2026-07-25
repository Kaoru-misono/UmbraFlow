#pragma once

#include "canvas-math.hpp"
#include "workbench-app.hpp"

#include "platform/windows-texture-cache.hpp"
#include "source-ingestion.hpp"

#include <annotation/authoring-compiler.hpp>
#include <annotation/authoring-document.hpp>
#include <annotation/catalog.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace uf::workbench
{
    // The platform-backed operations the panels invoke but cannot perform
    // themselves: uploading a source image to a GPU texture, opening the OS file
    // picker, and capturing a frame from a bound target. Each is a call-scoped
    // callback the shell wires to a platform helper; the panels stay free of
    // Win32, Direct3D, and Windows Graphics Capture. The callbacks are borrowed
    // for the draw and never retained.
    struct WorkbenchServices final
    {
        std::function<
            Result<platform::GpuSourceTexture>(
                annotation::AuthoringSourceAsset const&
            )
        > m_textureFor{};

        std::function<
            Result<std::optional<std::filesystem::path>>()
        > m_pickPngToImport{};

        std::function<
            Result<IngestedSource>(
                annotation::SourceId,
                std::string const&
            )
        > m_captureFromTarget{};

        // Appends one timestamped operation-log line. Optional: when unset the
        // panels simply do not log. The entry point wires it to a file so a
        // session's actions and errors survive being overwritten on the
        // transient status line.
        std::function<void(std::string_view)> m_appendLog{};
    };

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
    };

    // Draws the whole workbench for one frame: the sources, canvas, properties,
    // and actions panels. Reads and mutates the session through AppState and the
    // backend, reaches the platform only through services, and keeps its widget
    // state in ui.
    auto drawWorkbench(
        AppState& state,
        WorkbenchServices const& services,
        PanelUiState& ui
    ) -> void;
}
