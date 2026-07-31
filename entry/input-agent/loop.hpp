#pragma once

#include "platform/windows-file-writer.hpp"
#include "protocol.hpp"

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>

#include <trace/event.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace uf::input_agent
{
    class InputAgentQueueCursor;
    class InputAgentQueueReader;

    // How long the agent waits before looking at its queue again. It is named
    // here rather than buried in the loop because an idle timeout is only ever
    // noticed one poll at a time: the agent cannot detect silence finer than
    // this step, whatever timeout an operator asks for.
    inline constexpr auto k_inputAgentPollInterval = (
        std::chrono::duration_cast<MonotonicInstant::Duration>(
            std::chrono::milliseconds{100}
        )
    );

    // One answered queue command: the exact results line the operator reads,
    // and whether the agent may go on serving the queue afterwards.
    struct InputAgentCommandOutcome final
    {
        std::string resultLine{};
        bool        stopAgent{};

        auto operator==(InputAgentCommandOutcome const&) const -> bool = default;
    };

    // The answer to a queue line that never became a command. It names no op,
    // because there is no op to name.
    [[nodiscard]]
    auto serializeInvalidInputAgentCommand(Error const& error) -> std::string;

    [[nodiscard]] auto serializeInputAgentQuitResult() -> std::string;

    // What answers one queue command, and the loop's whole view of the run below
    // it. The agent's unit of work is a whole command -- capture and its two
    // framing outputs included -- answered as exactly one results line, which is
    // why nothing finer appears here.
    //
    // It sits behind a port because a test process cannot reach what the real
    // implementation stands on: AnnotationSession over an IInputAgentDrive over
    // a resolved window, a per-monitor DPI context and a Windows Graphics
    // Capture session. Every decision the loop makes around a command can be
    // exercised without any of them.
    class IInputAgentSession
    {
    public:
        IInputAgentSession() = default;

        IInputAgentSession(IInputAgentSession const&) = delete;
        IInputAgentSession(IInputAgentSession&&) = delete;
        auto operator=(IInputAgentSession const&) -> IInputAgentSession& = delete;
        auto operator=(IInputAgentSession&&) -> IInputAgentSession& = delete;

        virtual ~IInputAgentSession() = default;

        // Runs one command and answers with the results line it earned. A quit
        // never arrives here: ending the run is the loop's own decision, not the
        // session's.
        [[nodiscard]]
        virtual auto execute(
            InputAgentCommand const& command
        ) -> InputAgentCommandOutcome = 0;

        // Drops the audit records the command just answered produced. The loop
        // calls this once per answered command, which is what keeps a session
        // of ten thousand commands from accumulating all of their records.
        virtual auto clearCommandAudit() noexcept -> void = 0;

        // Ends the session, and with it the capture session below. Every exit
        // the loop takes runs this, so a finished agent never leaves one
        // attached to a live window.
        [[nodiscard]] virtual auto close() -> Status = 0;
    };

    // The loop's entire view of time: what time it is, and how it waits for the
    // queue to grow. Both sit behind a port because the rule that ends an idle
    // run is a statement about elapsed time, so a test that could not replace
    // the clock would have to spend the wall-clock seconds it is asserting
    // about -- and would still be at the mercy of the scheduler that granted
    // them.
    class IInputAgentPollClock
    {
    public:
        IInputAgentPollClock() = default;

        IInputAgentPollClock(IInputAgentPollClock const&) = delete;
        IInputAgentPollClock(IInputAgentPollClock&&) = delete;
        auto operator=(IInputAgentPollClock const&)
            -> IInputAgentPollClock& = delete;
        auto operator=(IInputAgentPollClock&&)
            -> IInputAgentPollClock& = delete;

        virtual ~IInputAgentPollClock() = default;

        [[nodiscard]] virtual auto now() const noexcept -> MonotonicInstant = 0;

        virtual auto waitForNextPoll() -> void = 0;
    };

    // The agent's real clock: the steady clock, and a sleeping thread between
    // polls.
    class SystemInputAgentPollClock final : public IInputAgentPollClock
    {
    public:
        [[nodiscard]] auto now() const noexcept -> MonotonicInstant override;

        auto waitForNextPoll() -> void override;
    };

    // The member every results line opens with, naming the front-end that
    // produced it.
    inline constexpr auto k_inputAgentFrontEndMember = (
        std::string_view{"front_end"}
    );

    // The results file: one line per answered command, flushed durably before
    // the queue cursor is allowed past the command that line answers.
    //
    // It is also where the front-end stamp goes on, for the reason
    // trace::TraceRecorder rather than each emitter owns that stamp: this is the
    // one place every answer passes through, including the two the loop itself
    // authors, so no answer can reach the file without saying who produced it.
    // Until an annotation session drives the host it has no run and no
    // generation, so it writes no umbraflow-trace/v1 line and this file is its
    // whole evidence stream; the value stamped here is nevertheless
    // trace::FrontEnd's, so the day it does join the host the attribution a
    // reader already knows does not change.
    class InputAgentResultWriter final
    {
        platform::FileWriter m_writer;
        std::string          m_stamp;

        InputAgentResultWriter(
            platform::FileWriter writer,
            std::string stamp
        ) noexcept;

    public:
        InputAgentResultWriter(InputAgentResultWriter const&) = delete;
        auto operator=(InputAgentResultWriter const&)
            -> InputAgentResultWriter& = delete;
        InputAgentResultWriter(InputAgentResultWriter&&) noexcept = default;
        auto operator=(InputAgentResultWriter&&) noexcept
            -> InputAgentResultWriter& = default;
        ~InputAgentResultWriter() = default;

        // `frontEnd` has no default, on trace::TraceRecorder's reasoning: which
        // front-end produced a stream is not something a construction site may
        // leave unsaid, and a defaulted attribution would silently name one of
        // them on every stream that forgot to choose.
        [[nodiscard]]
        static auto create(
            std::filesystem::path const& path,
            trace::FrontEnd frontEnd
        ) -> Result<InputAgentResultWriter>;

        // `line` must be a JSON object carrying at least one member, which every
        // answer this agent produces is. The stamp is inserted as its first
        // member, so a reader meets the attribution before the answer.
        [[nodiscard]] auto write(std::string_view line) -> Status;

        [[nodiscard]] auto flush() -> Status;
    };

    // The agent's serving loop, and the only place the ways a run ends are
    // decided: the operator's quit, a command whose failure means the window is
    // no longer the one that was observed, a queue that has gone silent for the
    // idle timeout, and an I/O failure it cannot answer.
    //
    // Every borrow here is a collaborator the loop drives, never an output
    // channel: reading a queue advances it, recording a position rewrites the
    // cursor file, answering appends to the results file, and both ports exist
    // to be called. Nothing is returned through any of them.
    [[nodiscard]]
    auto runInputAgentQueueLoop(
        InputAgentQueueReader& reader,
        InputAgentQueueCursor& cursor,
        InputAgentResultWriter& results,
        IInputAgentSession& session,
        IInputAgentPollClock& clock,
        MonotonicInstant::Duration idleTimeout
    ) -> Status;
}
