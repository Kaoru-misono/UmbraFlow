#pragma once

#include "input-agent-protocol.hpp"
#include "platform/windows-file-writer.hpp"

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace uf::m0_demo
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

    // The live half of one agent run: a resolved window, its capture session,
    // and the OS input path. It sits behind a port because a test process has
    // none of those -- no window to resolve, no per-monitor DPI context, no
    // Windows Graphics Capture session -- while every decision the loop makes
    // around it can be exercised without them.
    //
    // Deliberately not engine::IActionSink. That port speaks the engine's
    // action vocabulary, one already-authorized click or keystroke at a time,
    // and it belongs to a module this executable does not link. The agent's
    // unit of work is a whole queue command, capture and its two framing
    // outputs included, answered as exactly one results line.
    class IInputAgentTarget
    {
    public:
        IInputAgentTarget() = default;

        IInputAgentTarget(IInputAgentTarget const&) = delete;
        IInputAgentTarget(IInputAgentTarget&&) = delete;
        auto operator=(IInputAgentTarget const&) -> IInputAgentTarget& = delete;
        auto operator=(IInputAgentTarget&&) -> IInputAgentTarget& = delete;

        virtual ~IInputAgentTarget() = default;

        // Runs one command against the live target and answers with the results
        // line it earned. A quit never arrives here: ending the run is the
        // loop's own decision, not the target's.
        [[nodiscard]]
        virtual auto execute(
            InputAgentCommand const& command
        ) -> InputAgentCommandOutcome = 0;

        // Drops the audit records the command just answered produced. The loop
        // calls this once per answered command, which is what keeps a session
        // of ten thousand commands from accumulating all of their records.
        virtual auto clearCommandAudit() noexcept -> void = 0;

        // Ends the capture session. Every exit the loop takes runs this, so a
        // finished agent never leaves a session attached to a live window.
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

    // The results file: one line per answered command, flushed durably before
    // the queue cursor is allowed past the command that line answers.
    class InputAgentResultWriter final
    {
        platform::FileWriter m_writer;

        explicit InputAgentResultWriter(
            platform::FileWriter writer
        ) noexcept;

    public:
        InputAgentResultWriter(InputAgentResultWriter const&) = delete;
        auto operator=(InputAgentResultWriter const&)
            -> InputAgentResultWriter& = delete;
        InputAgentResultWriter(InputAgentResultWriter&&) noexcept = default;
        auto operator=(InputAgentResultWriter&&) noexcept
            -> InputAgentResultWriter& = default;
        ~InputAgentResultWriter() = default;

        [[nodiscard]]
        static auto create(
            std::filesystem::path const& path
        ) -> Result<InputAgentResultWriter>;

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
        IInputAgentTarget& target,
        IInputAgentPollClock& clock,
        MonotonicInstant::Duration idleTimeout
    ) -> Status;
}
