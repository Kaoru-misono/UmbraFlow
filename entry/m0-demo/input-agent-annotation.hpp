#pragma once

#include "input-agent-drive.hpp"
#include "input-agent-loop.hpp"
#include "input-agent-protocol.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace uf::m0_demo
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

    // What an authoring session does with the frames a drive returns: it names
    // the files they land in, brackets one input between a before-frame and an
    // after-frame, and answers each queue command with exactly one results line.
    //
    // This is the annotation half of the agent, and the drive below it is the
    // other. The line between them is that a drive knows only a window, an input
    // and a frame, while everything here is about what an author is measuring --
    // which is why the verbs this layer will grow (reading a region, proposing an
    // element from what was read) add nothing to IInputAgentDrive, and why the
    // fences a delivery passes are not visible here at all.
    //
    // It owns the drive rather than borrowing it, so the window cannot outlive
    // the session that acts on it, and closing the session closes the capture
    // session below.
    class AnnotationSession final : public IInputAgentSession
    {
        std::unique_ptr<IInputAgentDrive> m_drive;

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
        auto run(InputAgentQuitCommand const&) -> InputAgentCommandOutcome;

    public:
        // The three paths are the canonical forms the caller has already
        // resolved: the directory every output must stay inside, and the two IPC
        // files no output may alias.
        AnnotationSession(
            std::unique_ptr<IInputAgentDrive> p_drive,
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
