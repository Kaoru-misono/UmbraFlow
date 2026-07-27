#pragma once

#include "authoring-edit.hpp"
#include "model-check-job.hpp"
#include "app/canvas-math.hpp"
#include "app/workbench-app.hpp"

#include <annotation/catalog.hpp>

#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace uf::workbench
{
    // Severity of an operation-log entry, decided at the report site rather than
    // sniffed back out of the message text. It is threaded through
    // WorkbenchServices::appendLog and rendered by the Log window, so it lives at
    // namespace scope where both the panels and the shell can name it.
    enum class LogSeverity : uint8
    {
        Info,
        Warning,
        Error,
    };

    // The uppercase word the on-disk log line carries for a severity: INFO,
    // WARN, or ERROR. The shell composes the line, so keeping the mapping here
    // gives the writer and its tests one definition to share.
    [[nodiscard]]
    auto logSeverityWord(LogSeverity severity) -> std::string_view;

    // Composes one operation-log line as "{timestamp}  {SEVERITY}  {message}".
    // The shell writes the returned string; panels never pre-bake the severity
    // word into the message they report.
    [[nodiscard]]
    auto formatLogLine(
        LogSeverity severity,
        std::string_view timestamp,
        std::string_view message
    ) -> std::string;

    // The wall-clock timestamp string the operation log stamps each entry with,
    // formatted as the writer has always formatted it: UTC at second resolution.
    // Computed once per mirrored event so the in-memory buffer and the disk line
    // carry the identical stamp.
    [[nodiscard]]
    auto formatLogTimestamp(
        std::chrono::system_clock::time_point instant
    ) -> std::string;

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
        // A draft a panel wants committed, held until the panels that borrow
        // into the document have finished drawing. A recognizer definition, page
        // signature, or source asset a panel reads is owned by the document, and
        // committing replaces the document wholesale -- so committing mid-draw
        // would leave the rest of that panel reading freed storage. The request
        // is applied once per frame, after the borrowing panels and before the
        // actions panel, which re-reads everything it touches.
        struct PendingEdit final
        {
            AuthoringDraft draft;
            std::string    description{};

            // Severity the description is reported at once the edit lands.
            // Info for an ordinary success; a placement that does not match on
            // its target screen lands as Warning. A refused edit never reaches
            // here -- applyPendingEdit reports its own rejection as Error.
            LogSeverity severity{LogSeverity::Info};

            // Installed once the edit lands, so creating an entity can select it
            // without the selection dangling at an id a rejected edit never
            // added. Carries the whole typed selection: an element-selecting
            // request names no shown screen, and AppState::select then inherits
            // the current one, leaving the shown image where it was.
            std::optional<AppState::Selection> selection{};

            // The history revision the draft was built against, when it came
            // from an EditPage. applyPendingEdit refuses the commit when this no
            // longer matches the history: a page opened, then left behind by an
            // undo or redo, must not overwrite the version that replaced it.
            // Absent for the free-function requests, which are built and
            // committed within one frame and so carry no cross-frame staleness
            // to guard against.
            std::optional<uint64> baseRevision{};
        };

        // Which rectangle a canvas drag is editing, so a released gesture
        // commits the right recognizer field. None means no drag is in progress.
        enum class CanvasDragTarget : uint8
        {
            None,
            TemplateRect,
            SearchRoi,
        };

        // One entry in the bounded operation-log history the Log window renders.
        // The severity is decided at the report site; the timestamp is the same
        // string the disk writer stamps, so the two stay in step.
        struct LogEvent final
        {
            LogSeverity severity{LogSeverity::Info};
            std::string timestamp{};
            std::string message{};
        };

        // How many events the in-memory history keeps. Older entries drop off the
        // front once the buffer is full; the disk log is unbounded and untouched.
        static constexpr std::size_t k_logEventCapacity{256};

        std::array<char, 256> targetTitle{};
        std::array<char, 256> nameBuffer{};

        std::optional<annotation::RecognizerId> nameBufferFor{};
        std::string                             nameSeededValue{};
        bool                                    nameInputActive{};
        std::string                             statusLine{};
        LogSeverity                             statusSeverity{LogSeverity::Info};
        std::string                             lastLoggedStatus{};
        std::deque<LogEvent>                    logEvents{};
        std::optional<PendingEdit>              pendingEdit{};

        CanvasDragTarget            dragTarget{CanvasDragTarget::None};
        std::optional<RectGripKind> dragGrip{};
        std::optional<PixelRect>    dragStartRect{};

        // The shared region being dragged onto a page, remembered from the frame
        // the drag began. It cannot be a frame-local handed from the drag source
        // to the drop target: on the frame the mouse is released the source no
        // longer submits itself -- ImGui has already cleared the active id -- and
        // that is exactly the frame the drop is accepted on.
        std::optional<annotation::RecognizerId> draggedRegion{};

        // The whole-model check runs off the GUI thread, so it lives across
        // frames. Neither copyable nor movable, which is why PanelUiState is
        // built once and only ever passed by reference.
        ModelCheckJob modelCheck{};

        // The reporting seam every panel and action writes through, stating the
        // outcome's severity explicitly rather than leaving the message text to
        // be classified later. It sets the transient status line the actions
        // panel shows and the severity the next mirror will carry.
        auto report(LogSeverity severity, std::string message) -> void;

        // Mirrors the current status into the bounded event history, collapsing a
        // consecutive duplicate the same way the disk log does and dropping the
        // oldest entry once the buffer is full. Returns the recorded event so the
        // caller can write the same line to disk, or nothing when the status was
        // empty or an unchanged repeat. The caller supplies the timestamp so the
        // buffer entry and the disk line share one stamp.
        [[nodiscard]]
        auto captureLogEvent(
            std::string_view timestamp
        ) -> std::optional<LogEvent>;

        // Empties the in-memory history behind the Log window's clear button. The
        // disk log is a separate, unbounded record and is left untouched.
        auto clearLog() -> void;
    };
}
