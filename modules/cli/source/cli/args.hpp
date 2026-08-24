#pragma once

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/space.hpp>

#include <ocr/engine.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uf::cli
{
    inline constexpr auto k_defaultRecognitionTimeout = (
        std::chrono::duration_cast<MonotonicInstant::Duration>(
            std::chrono::milliseconds{2000}
        )
    );
    inline constexpr auto k_defaultMaxFrameAge = (
        std::chrono::duration_cast<MonotonicInstant::Duration>(
            std::chrono::milliseconds{750}
        )
    );

    // A search's worst case is the number of candidate positions times the
    // template area. This ceiling covers ordinary authored regions while still
    // refusing an accidental full-frame search for a small template.
    inline constexpr auto k_defaultPixelComparisonBudget = uint64{1} << 31;

    inline constexpr auto k_defaultTracePath = std::string_view{
        "umbra-flow-trace.jsonl"
    };

    // A separate default from the one above, because both verbs bind a target
    // and FileTraceSink refuses a file that already carries evidence: one
    // shared default would make an observation refuse after an exploration
    // session had run in the same directory.
    inline constexpr auto k_defaultObserveTracePath = std::string_view{
        "umbra-flow-observe-trace.jsonl"
    };

    // A third default for the second reason: a Tool run binds the same target
    // an observation does, so sharing the observation's default would make a
    // run refuse after an observation had already written that file.
    inline constexpr auto k_defaultInvokeTracePath = std::string_view{
        "umbra-flow-invoke-trace.jsonl"
    };

    // How long an exploration session waits with an empty queue before it
    // releases capture and target resources after its agent disappears.
    inline constexpr auto k_defaultExploreIdleTimeout = (
        std::chrono::duration_cast<MonotonicInstant::Duration>(
            std::chrono::seconds{120}
        )
    );

    // The annotation front end. Queue and result paths are required because a
    // durable cursor, rather than a one-shot command, prevents an agent restart
    // from delivering the same input twice.
    //
    // The Operator production root is required for the reason ObserveArgs and
    // InvokeArgs require it, and there is no flagless spelling of it: an
    // exploration session goes through the one production admission door, so
    // it names the runtime that governs it exactly as every other verb does
    // (docs/decisions/2026-08-24-the-annotation-policy-is-the-operators.md).
    // The annotation policy is read from that root's own
    // `policy-artifact.json` and from nowhere else; absent, it resolves to the
    // Operator-owned deny-all artifact.
    struct ExploreArgs final
    {
        std::filesystem::path project{};
        intptr                windowHandle{};

        std::filesystem::path runtime{};

        std::filesystem::path queue{};
        std::filesystem::path results{};

        uint64                     budget{k_defaultPixelComparisonBudget};
        MonotonicInstant::Duration recognitionTimeout{k_defaultRecognitionTimeout};
        MonotonicInstant::Duration maxFrameAge{k_defaultMaxFrameAge};
        MonotonicInstant::Duration idleTimeout{k_defaultExploreIdleTimeout};

        std::filesystem::path trace{k_defaultTracePath};

        std::optional<std::filesystem::path> ocrModels{};

        auto operator==(ExploreArgs const&) const -> bool = default;
    };

    [[nodiscard]]
    auto parseExploreArguments(std::span<std::string const> raw) -> Result<ExploreArgs>;

    [[nodiscard]] auto exploreUsageText() noexcept -> std::string_view;

    // The project directory this binary loads a project out of and registers
    // its deployments' plugins from. One required path and nothing else:
    // everything else a project owns is named by its two root documents rather
    // than by a flag, which is what makes a project data rather than a command
    // line.
    struct OpenArgs final
    {
        std::filesystem::path project{};

        auto operator==(OpenArgs const&) const -> bool = default;
    };

    [[nodiscard]]
    auto parseOpenArguments(std::span<std::string const> raw) -> Result<OpenArgs>;

    [[nodiscard]] auto openUsageText() noexcept -> std::string_view;

    // One PNG already on disk, and how hard to look at it. There is no target
    // and no project: this verb measures a file, which is what makes it usable
    // by a caller that cannot reach a desktop at all.
    //
    // The model directory is required rather than optional as it is for
    // `explore`. platform::bindOcrEngine answers an absent directory with a
    // null engine, and a null engine here would read every image as holding no
    // text -- a fail-open answer indistinguishable from a correct one.
    struct OcrArgs final
    {
        std::filesystem::path image{};
        std::filesystem::path ocrModels{};

        // Absent reads the whole image. Present is the caller stating where the
        // text is, and it is refused when it leaves the image rather than
        // clamped.
        std::optional<PixelRect> rect{};

        ocr::TextLayout layout{ocr::TextLayout::Block};

        std::optional<uint32> maximumLines{};

        auto operator==(OcrArgs const&) const -> bool = default;
    };

    [[nodiscard]]
    auto parseOcrArguments(std::span<std::string const> raw) -> Result<OcrArgs>;

    [[nodiscard]] auto ocrUsageText() noexcept -> std::string_view;

    // The production read-only path: a project directory, a live target, and
    // the Operator production root that already holds this project's installed
    // RuntimeArtifact.
    //
    // The artifact is NAMED by the project directory and never by a flag. Its
    // root hash is this binary's own arithmetic over the manifest bytes the
    // project carries, so a caller cannot ask for a generation to be opened
    // under a digest it stated -- which is the same rule that keeps a project
    // author from typing a digest at all
    // (docs/archive/plans/2026-08-11-project-as-data.md 7.0 Q3).
    //
    // The model directory is required for OcrArgs' reason, one layer further
    // down: TaskContext::cycleRead answers a session with no OCR adapter
    // with UnsupportedCapability, and a Reader the model declared would
    // otherwise reach the plugin as a reading that failed rather than as a
    // refusal naming the flag nobody passed.
    struct ObserveArgs final
    {
        std::filesystem::path project{};
        intptr                windowHandle{};

        std::filesystem::path runtime{};

        std::filesystem::path ocrModels{};

        uint64                     budget{k_defaultPixelComparisonBudget};
        MonotonicInstant::Duration recognitionTimeout{k_defaultRecognitionTimeout};

        std::filesystem::path trace{k_defaultObserveTracePath};

        auto operator==(ObserveArgs const&) const -> bool = default;
    };

    [[nodiscard]]
    auto parseObserveArguments(
        std::span<std::string const> raw
    ) -> Result<ObserveArgs>;

    [[nodiscard]] auto observeUsageText() noexcept -> std::string_view;

    // What a model's transport delivers, as a command line can carry it: the
    // Tool it named, and the two documents its objective and its arguments
    // are.
    //
    // Documents rather than typed text, because AgentToolUse carries a parsed
    // value. The runtime that emitted the tool-use block already holds a
    // structure, and this front end stands in for that runtime by reading the
    // bytes it would have handed over
    // (modules/operator/source/operator/tool-actor-adapters.hpp).
    //
    // The profile is the fourth piece and the only one the transport does not
    // deliver: it is the budget document the session this call runs in is
    // pinned to. Every actor carries one, because every session spends the
    // operator's machine and the ledger pins none without a declared budget; a
    // ceiling the operator chose not to bind is written "unbounded" in that
    // document rather than left out of it. It sits inside each transport's own
    // material rather than beside the shared flags so that a transport added
    // later cannot be spelled without one.
    struct AgentToolRequest final
    {
        std::string           toolName{};
        std::filesystem::path objectiveDocument{};
        std::filesystem::path argumentsDocument{};
        std::filesystem::path agentProfileDocument{};

        auto operator==(AgentToolRequest const&) const -> bool = default;
    };

    // What a person's transport delivers: text, exactly as it was typed, and
    // deliberately NOT parsed here. HumanToolCommand carries bytes because a
    // person's spacing, member order and number spelling are all things exact
    // canonical form refuses, and the human adapter parses and re-renders
    // them; a front end that canonicalised first would be answering for the
    // person rather than carrying what they wrote.
    struct HumanToolRequest final
    {
        std::string           toolName{};
        std::string           objectiveText{};
        std::string           argumentsText{};
        std::filesystem::path agentProfileDocument{};

        auto operator==(HumanToolRequest const&) const -> bool = default;
    };

    // What a Project's transport delivers. The two documents are the model's,
    // for the model's reason. The name is not: ProjectAutomationStart carries
    // an `entryToolName` because the adapter asks the registration's own
    // binding table whose entry it is, so this front end names an entry and
    // cannot present another party's Tool as one of the Project's.
    struct ProjectAutomationRequest final
    {
        std::string           entryToolName{};
        std::filesystem::path objectiveDocument{};
        std::filesystem::path argumentsDocument{};
        std::filesystem::path agentProfileDocument{};

        auto operator==(ProjectAutomationRequest const&) const -> bool = default;
    };

    // Which of the three transports presents one call. A sum type rather than
    // one struct carrying all three halves: the transports deliver different
    // material, and a shape able to hold two at once would need something
    // downstream to choose between them after the parser already had to.
    using ActorToolRequest = std::variant<
        AgentToolRequest,
        HumanToolRequest,
        ProjectAutomationRequest>;

    // The production path that starts one Tool call at the top of a run.
    //
    // Everything above `request` is what a run needs whichever actor starts
    // it, and each for the reason ObserveArgs gives one layer down: the
    // project, the Operator production root that already holds its installed
    // RuntimeArtifact, the live target the run acts in, and the models a
    // Reader the project's model declares would otherwise fail open without.
    //
    // The request key is required and never derived. It is what the durable
    // root request is idempotent on, so a front end that minted one would make
    // every rerun of the same command a second durable root beside the first
    // rather than the same call resolved again.
    struct InvokeArgs final
    {
        std::filesystem::path project{};
        intptr                windowHandle{};

        std::filesystem::path runtime{};

        std::filesystem::path ocrModels{};

        std::string requestKey{};

        ActorToolRequest request{};

        // The capability set the session pins, spelled as `upgrade` spells it.
        // Empty pins a session that may call only the Tools requiring none; a
        // Tool requiring one this session does not hold is not offered to it
        // and is refused at admission.
        std::vector<std::string> capabilities{};

        uint64                     budget{k_defaultPixelComparisonBudget};
        MonotonicInstant::Duration recognitionTimeout{k_defaultRecognitionTimeout};

        std::filesystem::path trace{k_defaultInvokeTracePath};

        auto operator==(InvokeArgs const&) const -> bool = default;
    };

    [[nodiscard]]
    auto parseInvokeArguments(std::span<std::string const> raw) -> Result<InvokeArgs>;

    // The --actor word this request was named by. Answered beside the flag
    // table that reads it rather than restated by the verb that reports which
    // actor ran, so the word a caller typed and the word a report prints
    // cannot drift apart.
    [[nodiscard]]
    auto actorName(ActorToolRequest const& request) -> std::string_view;

    [[nodiscard]] auto invokeUsageText() noexcept -> std::string_view;

    // The Operator production root to sweep, and nothing else. There is no
    // project and no target: what the pass reads is the root's own reference
    // set, and a project named here could only narrow a decision that is only
    // correct when it is taken over the whole set at once.
    struct ReclaimArgs final
    {
        std::filesystem::path runtime{};

        auto operator==(ReclaimArgs const&) const -> bool = default;
    };

    [[nodiscard]]
    auto parseReclaimArguments(
        std::span<std::string const> raw
    ) -> Result<ReclaimArgs>;

    [[nodiscard]] auto reclaimUsageText() noexcept -> std::string_view;

    // The target listing takes no arguments because it discovers the handle
    // required by the privileged exploration entry point.
    [[nodiscard]] auto targetsUsageText() noexcept -> std::string_view;

    // The two verbs that publish a RuntimeArtifact release into an Operator
    // production root and record who authorised a capability expansion onto a
    // release. Both hashes are stated as the canonical spelling ContentHash
    // reads, `sha256:` followed by 64 lowercase hex digits.
    //
    // upgrade names the project the upgrade session registers against, the
    // Operator root that receives the release, the release handoff, and the
    // two hashes that make the handoff trustworthy: the digest of the
    // handoff's release.manifest.json, and the artifact root hash that
    // manifest declares. The ledger proves the second equals the first by
    // refusing to pin a session whose manifest names a root that was not
    // installed.
    struct UpgradeArgs final
    {
        std::filesystem::path project{};
        std::filesystem::path runtime{};
        std::filesystem::path handoff{};

        ContentHash releaseManifestHash;
        ContentHash artifactRootHash;

        // The capability set the upgrade session pins. Empty is the ordinary
        // first release; a later upgrade that expands the set is refused until
        // `approve` records the expansion.
        std::vector<std::string> capabilities{};

        auto operator==(UpgradeArgs const&) const -> bool = default;
    };

    [[nodiscard]]
    auto parseUpgradeArguments(
        std::span<std::string const> raw
    ) -> Result<UpgradeArgs>;

    [[nodiscard]] auto upgradeUsageText() noexcept -> std::string_view;

    // approve records the evidence that expanding onto artifactRootHash was
    // authorised. It names the same root and capability set the refused pin
    // named, plus the digest of the evidence itself -- the bytes the
    // authorisation was recorded against, whatever they are.
    struct ApproveArgs final
    {
        std::filesystem::path runtime{};

        ContentHash artifactRootHash;
        ContentHash evidenceHash;

        std::vector<std::string> capabilities{};

        auto operator==(ApproveArgs const&) const -> bool = default;
    };

    [[nodiscard]]
    auto parseApproveArguments(
        std::span<std::string const> raw
    ) -> Result<ApproveArgs>;

    [[nodiscard]] auto approveUsageText() noexcept -> std::string_view;

    [[nodiscard]] auto usageText() -> std::string;
}
