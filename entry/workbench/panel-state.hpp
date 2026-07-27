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

    // A state-changing command the top toolbar draws but must not run inline. The
    // toolbar is submitted at the top of the frame, before the panels' parked
    // edit is committed, so a click here is queued on PanelUiState and dispatched
    // after applyPendingEdit -- otherwise an undo would run before the same-frame
    // widget-deactivation edit it is meant to reverse had landed. Capture and
    // import are separate flags because they reach the platform through
    // WorkbenchServices, which this layer deliberately does not name.
    enum class ToolbarCommand : uint8
    {
        SaveAndGenerate,
        Undo,
        Redo,
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

        // The one canvas gesture in progress, the explicit state machine the
        // canvas arbitrates every left interaction through. Exactly one is active:
        //   Idle          -- hovering; a fresh left press is arbitrated from here.
        //   GripEditing   -- a grip of the selected element is held; the detail
        //                    lives in dragTarget/dragGrip/dragStartRect and the
        //                    commit is handleRectEditing's on release. This state
        //                    mirrors dragTarget != None.
        //   PressPending  -- left pressed on empty canvas; below the drag
        //                    threshold it is still undecided between a click that
        //                    selects nothing and a rubber-band.
        //   RubberBanding -- past the threshold on empty canvas, dragging out a
        //                    new rectangle; the type picker opens on release.
        // Transitions: Idle -> GripEditing on a grip hit; Idle -> PressPending on
        // an empty press where a page context exists; PressPending -> RubberBanding
        // past the threshold; any -> Idle on release, on Escape, or on a completed
        // create. A click that hits a drawn rectangle selects it and stays Idle.
        enum class CanvasGesture : uint8
        {
            Idle,
            GripEditing,
            PressPending,
            RubberBanding,
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

        // Which tree row an inline rename is editing: a recognizer
        // (element/member row) or a page (its header). Both commit through the
        // same draft-edit path the Inspector's element rename already uses, so a
        // duplicate name is refused by the build and surfaced through report.
        enum class RenameKind : uint8
        {
            Recognizer,
            Page,
        };

        // The one tree row being renamed inline, if any -- a page header or a
        // member/element row, never two at once. The buffer seeds from the
        // current name and Enter commits it; Esc or a click away cancels. Not an
        // ImGui type, so the commit logic stays outside the drawing.
        struct InlineRename final
        {
            RenameKind             kind{RenameKind::Recognizer};
            annotation::ResourceId id;
            std::array<char, 256>  buffer{};
            bool                   justOpened{true};
        };

        // A delete-everywhere awaiting confirmation, named for the modal that
        // guards it. detail is the "what will be removed" line, built at request
        // time from the same counts the deletion produces. Cleared on confirm,
        // cancel, or a selection change, so no confirmation outlives the element
        // it was raised for.
        struct PendingDelete final
        {
            annotation::RecognizerId id;
            std::string              name{};
            std::string              detail{};
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

        // A toolbar command clicked this frame, dispatched after the parked edit
        // is committed; see ToolbarCommand for the ordering it protects. Capture
        // and import reach the platform through WorkbenchServices, so they are
        // flagged here and run by the shell rather than by the testable dispatch.
        std::optional<ToolbarCommand> pendingToolbarCommand{};
        bool                          captureRequested{};
        bool                          importRequested{};

        // A preview requested by the F5 shortcut, drained by the shell after the
        // frame's edit commits so it runs against the same document the Evidence
        // tab's Preview button would rather than the pre-commit one.
        bool previewRequested{};

        // The single in-progress inline tree rename and the single pending
        // delete-everywhere confirmation. pendingDeleteJustRequested opens the
        // confirmation modal the frame an entry point sets it.
        std::optional<InlineRename>  inlineRename{};
        std::optional<PendingDelete> pendingDelete{};

        bool pendingDeleteJustRequested{};

        CanvasDragTarget            dragTarget{CanvasDragTarget::None};
        std::optional<RectGripKind> dragGrip{};
        std::optional<PixelRect>    dragStartRect{};

        // The canvas interaction state machine and its in-progress data. The
        // rubber-band records the source pixel its press anchored on, so the
        // rectangle is rebuilt from that anchor to the cursor every frame rather
        // than accumulated. pendingCreateRect is the drawn rectangle held between
        // a rubber-band release and the type picker's choice; contextMenuTarget /
        // contextMenuPage are the element and page a right-click opened a menu
        // over, so the menu's actions name them after the click that placed it.
        CanvasGesture              canvasGesture{CanvasGesture::Idle};
        std::optional<CanvasPoint> rubberBandStartSource{};
        std::optional<PixelRect>   pendingCreateRect{};

        std::optional<annotation::RecognizerId> contextMenuTarget{};
        std::optional<annotation::PageId>       contextMenuPage{};

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
