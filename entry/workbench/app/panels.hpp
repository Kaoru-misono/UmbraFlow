#pragma once

#include "../canvas-math.hpp"
#include "../workbench-app.hpp"

#include "panel-state.hpp"
#include "platform/windows-texture-cache.hpp"
#include "source-ingestion.hpp"

#include <annotation/authoring-compiler.hpp>
#include <annotation/authoring-document.hpp>
#include <annotation/resource.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>

namespace uf::workbench
{
    // The platform-backed operations the panels invoke but cannot perform
    // themselves: uploading/pruning GPU textures, opening the OS file picker,
    // and capturing a frame from a bound target. Each is a call-scoped callback
    // the shell wires to a platform helper; the panels stay free of Win32,
    // Direct3D, and Windows Graphics Capture. The callbacks are borrowed for the
    // draw and never retained.
    struct WorkbenchServices final
    {
        std::function<
            Result<platform::GpuSourceTexture>(
                annotation::AuthoringSourceAsset const&
            )
        > textureFor{};

        std::function<
            void(std::span<annotation::AuthoringSource const>)
        > pruneTextures{};

        std::function<
            Result<std::optional<std::filesystem::path>>()
        > pickPngToImport{};

        std::function<
            Result<IngestedSource>(
                annotation::SourceId,
                std::string const&
            )
        > captureFromTarget{};

        // Appends one operation-log entry, given its severity, the timestamp the
        // panels already stamped the in-memory event with, and the message. The
        // shell composes the "{timestamp}  {SEVERITY}  {message}" line via
        // formatLogLine, so the severity word is never pre-baked into the
        // message. Optional: when unset the panels simply do not log to disk. The
        // entry point wires it to a file so a session's actions and errors
        // survive being overwritten on the transient status line.
        std::function<
            void(LogSeverity, std::string_view, std::string_view)
        > appendLog{};
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
