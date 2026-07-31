#pragma once

#include "drive.hpp"
#include "loop.hpp"
#include "protocol.hpp"
#include "text-reader.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace uf::input_agent
{
    struct InputAgentFramePaths final
    {
        std::filesystem::path before{};
        std::filesystem::path after{};

        auto operator==(InputAgentFramePaths const&) const -> bool = default;
    };

    // One action op's outcome exactly as its results line reports it: the two
    // frames that bracket the action, whether the input actually reached the
    // target, and one error field. delivered is the distinction every caller
    // depends on, so a rejected op must never report it true.
    struct InputAgentActionResult final
    {
        std::optional<uint64> beforeFrame{};
        std::optional<uint64> afterFrame{};
        bool                  delivered{};
        std::optional<Error>  error{};
    };

    // Resolves the before-frame and after-frame outputs of one action op. Both
    // must stay inside the canonical output directory, must not alias each other
    // or either IPC file, and must not exist yet: an input-agent output is
    // always a fresh file, so a caller can never mistake a stale frame on disk
    // for the one this action produced.
    [[nodiscard]]
    auto resolveInputAgentFramePaths(
        std::filesystem::path const& outputBefore,
        std::filesystem::path const& outputAfter,
        std::filesystem::path const& canonicalOutputDirectory,
        std::filesystem::path const& canonicalQueue,
        std::filesystem::path const& canonicalResults,
        std::string_view operation
    ) -> Result<InputAgentFramePaths>;

    // click, key, and scroll share this line shape, because each frames one
    // delivered action between a before-frame and an after-frame.
    [[nodiscard]]
    auto serializeInputAgentActionResult(
        std::string_view operation,
        InputAgentActionResult const& result
    ) -> std::string;

    // One read op's outcome exactly as its results line reports it: which
    // observation the rectangle was read from, and either the lines that were
    // read or why they were not.
    struct InputAgentReadResult final
    {
        // Absent exactly when the command failed before an observation existed,
        // which is every failure that is not about the text.
        std::optional<uint64> frame{};

        // No in-class initializer: TextReadOutcome deliberately has no default
        // that stands for both an empty success and a failure, so a read result
        // has to say which one it is.
        TextReadOutcome outcome;
    };

    // The read line, which is its own shape rather than the action shape: a read
    // brackets nothing, delivers nothing, and writes no frame, so `delivered`
    // and the two frame ids would each be a permanent null.
    //
    // The three answers an operator must tell apart are structural here rather
    // than buried in the error text: `reader_ready` says whether reading is
    // possible in this run at all, `lines` is null exactly when the read did not
    // run, and an empty `lines` array with `ok` true is a region that was read
    // and holds no text.
    [[nodiscard]]
    auto serializeInputAgentReadResult(
        InputAgentReadResult const& result
    ) -> std::string;

    // What an authoring session does with the frames a drive returns: it names
    // the files they land in, brackets one input between a before-frame and an
    // after-frame, reads text out of a rectangle of one of them, and answers
    // each queue command with exactly one results line.
    //
    // This is the annotation half of the agent, and the drive below it is the
    // other. The line between them is that a drive knows only a window, an input
    // and a frame, while everything here is about what an author is measuring --
    // which is why `read` was added here and adds nothing to IInputAgentDrive,
    // and why the fences a delivery passes are not visible here at all.
    //
    // It owns the drive and the reader rather than borrowing them, so neither
    // the window nor the loaded model can outlive the session that uses it, and
    // closing the session closes the capture session below.
    class AnnotationSession final : public IInputAgentSession
    {
        std::unique_ptr<IInputAgentDrive>      m_drive;
        std::unique_ptr<IInputAgentTextReader> m_reader;

        std::filesystem::path m_outputDirectory;
        std::filesystem::path m_queue;
        std::filesystem::path m_results;

        // One overload per command a session can answer, so adding an
        // alternative to InputAgentCommand fails to compile here instead of
        // slipping through a chain of tests nobody extended.
        [[nodiscard]]
        auto run(
            InputAgentCaptureCommand const& command
        ) -> InputAgentCommandOutcome;
        [[nodiscard]]
        auto run(
            InputAgentClickCommand const& command
        ) -> InputAgentCommandOutcome;
        [[nodiscard]]
        auto run(
            InputAgentKeyCommand const& command
        ) -> InputAgentCommandOutcome;
        [[nodiscard]]
        auto run(
            InputAgentScrollCommand const& command
        ) -> InputAgentCommandOutcome;
        [[nodiscard]]
        auto run(
            InputAgentReadCommand const& command
        ) -> InputAgentCommandOutcome;
        [[nodiscard]]
        auto run(InputAgentQuitCommand const&) -> InputAgentCommandOutcome;

    public:
        // The three paths are the canonical forms the caller has already
        // resolved: the directory every output must stay inside, and the two IPC
        // files no output may alias.
        AnnotationSession(
            std::unique_ptr<IInputAgentDrive> p_drive,
            std::unique_ptr<IInputAgentTextReader> p_reader,
            std::filesystem::path canonicalOutputDirectory,
            std::filesystem::path canonicalQueue,
            std::filesystem::path canonicalResults
        );

        [[nodiscard]]
        auto execute(
            InputAgentCommand const& command
        ) -> InputAgentCommandOutcome override;

        auto clearCommandAudit() noexcept -> void override;

        [[nodiscard]] auto close() -> Status override;
    };
}
