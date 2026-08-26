#pragma once

#include "project-plugin.hpp"
#include "tool-invocation.hpp"

#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace uf::operator_runtime
{
    class OperatorCoordinator;

    // How a call reaches the world at all. A Project handler is a pure leaf: its
    // VM has immutable inputs and resources, and its Tool primitive terminates
    // the handler instead of issuing a call. A Framework provider is a direct
    // leaf whose implementation may reach the world.
    enum class ToolEffectComposition : uint8
    {
        PureLeaf,
        DirectLeaf,
    };

    // The closed set of answerers, and the only place the durable provider_kind
    // vocabulary is joined to what an answerer can do. A Project descriptor
    // with no binding is refused at load, so provider_kind='project' IS
    // "answered by a bound scoped entry".
    struct ToolAnswerer final
    {
        std::string_view      providerKind{};
        ToolEffectComposition composition{ToolEffectComposition::DirectLeaf};
    };

    inline constexpr auto k_toolAnswerers = std::array{
        ToolAnswerer{"framework", ToolEffectComposition::DirectLeaf},
        ToolAnswerer{"project", ToolEffectComposition::PureLeaf},
    };

    // The same lookup from the two spellings a call's answerer arrives in: the
    // provider identity a live coordinate carries, and the provider_kind a
    // durable row stores. They are two overloads of one name because they
    // answer one question off one table; the identity form is defined in terms
    // of the durable form, so the two cannot diverge.
    [[nodiscard]]
    auto toolEffectComposition(std::string_view providerKind)
        -> ToolEffectComposition;

    [[nodiscard]]
    auto toolEffectComposition(ToolProviderIdentity const& provider)
        -> ToolEffectComposition;

    // The one rule that decides whether a crash inside a dispatch can have left
    // an external effect no durable row records -- and therefore the one rule
    // that decides what a restart classifies uncertain, what a re-entry
    // refuses, whether a completion may report terminal failure, and whether an
    // executor may convert a provider's failure to uncertainty.
    //
    // A pure leaf can be re-entered because no external effect can escape its
    // handler. A read-only direct leaf declares no effect for a delivery to be
    // uncertain about. Only a mutating direct leaf in flight is the
    // non-replayable atom: the world may or may not have moved and no record can
    // say.
    //
    // It is declared here rather than beside any one of its four callers
    // because a second statement of it is the drift the re-entry ruling is most
    // exposed to: the callers are read months apart and only a crash exercises
    // them together.
    [[nodiscard]]
    auto toolCallEffectMayBeUnrecorded(
        ToolEffectComposition composition,
        ToolMutability mutability
    ) noexcept -> bool;

    enum class ToolCallState : uint8
    {
        Proposed,
        Admitted,
        Dispatching,
        Confirmed,
        ProvenAbsent,
        Possible,
        TerminalFailure,
        TerminallyUnresolved,
    };

    [[nodiscard]]
    auto toolCallStateWireName(ToolCallState state) noexcept -> std::string_view;

    [[nodiscard]]
    auto parseToolCallState(std::string_view value) -> Result<ToolCallState>;

    // The terminal split, and the whole of it: a state carries an outcome
    // exactly when a provider conclusion or a reconciliation wrote it, and the
    // three that do not are the three a call is still passing through.
    //
    // It is published rather than restated at each reader because three of them
    // exist -- the durable write path, the evidence rule and the effect rule --
    // and a fourth spelling is how a state joins the wrong half unnoticed.
    [[nodiscard]]
    auto toolCallStateHasOutcome(ToolCallState state) noexcept -> bool;

    enum class ToolCallCompletionKind : uint8
    {
        Confirmed,
        ProvenAbsent,
        Possible,
        TerminalFailure,
    };

    // One provider conclusion, expressed as exact canonical bytes. The named
    // constructors are the only way to form it, so an absent result/evidence
    // combination cannot be stored under a terminal classification.
    class ToolCallCompletion final
    {
        ToolCallCompletionKind       m_kind;
        CanonicalJson                m_payload;
        std::optional<CanonicalJson> m_evidence;

        ToolCallCompletion(
            ToolCallCompletionKind kind,
            CanonicalJson payload,
            std::optional<CanonicalJson> evidence
        );

    public:
        [[nodiscard]]
        static auto confirmed(
            CanonicalJson result,
            std::optional<CanonicalJson> evidence = std::nullopt
        ) -> ToolCallCompletion;

        [[nodiscard]]
        static auto provenAbsent(
            CanonicalJson explanation,
            CanonicalJson evidence
        ) -> ToolCallCompletion;

        [[nodiscard]]
        static auto possible(
            CanonicalJson explanation,
            std::optional<CanonicalJson> evidence = std::nullopt
        ) -> ToolCallCompletion;

        [[nodiscard]]
        static auto terminalFailure(
            CanonicalJson error,
            std::optional<CanonicalJson> evidence = std::nullopt
        ) -> ToolCallCompletion;

        [[nodiscard]] auto kind() const noexcept -> ToolCallCompletionKind;

        [[nodiscard]]
        auto payload() const noexcept UF_LIFETIME_BOUND -> CanonicalJson const&;

        [[nodiscard]]
        auto evidence() const noexcept UF_LIFETIME_BOUND
            -> std::optional<CanonicalJson> const&;
    };

    enum class ToolCallReconciliationKind : uint8
    {
        Confirmed,
        ProvenAbsent,
        TerminallyUnresolved,
    };

    // The durable state each provider conclusion writes. Published for the same
    // reason the terminal split is: the protocol material renders this mapping,
    // and a mapping rendered from a second table could describe a protocol the
    // ledger does not implement.
    [[nodiscard]]
    auto toolCallStateFor(ToolCallCompletionKind kind) noexcept -> ToolCallState;

    [[nodiscard]]
    auto toolCallStateFor(ToolCallReconciliationKind kind) noexcept -> ToolCallState;

    // The Tool call vocabulary rendered as exact canonical JSON: every durable
    // state wire name with the half of the split it belongs to, every provider
    // conclusion with the state it writes, and the two answerer kinds with what
    // each can reach the world through.
    //
    // It is a fragment of currentToolRuntimeProtocolMaterial() rather than a
    // digest of its own, so the identity taken over the whole moves when any
    // one of these moves. Every value in it is read from the tables above; none
    // of it is a second spelling that could describe a vocabulary the ledger
    // does not have.
    [[nodiscard]]
    auto toolCallVocabularyMaterial() -> std::string;

    // A Framework-owned conclusion over a previously possible mutating call,
    // and the answer a ToolReconciliationQuery returns.
    //
    // Evidence is mandatory for every classification: reconciliation is the
    // act of replacing uncertainty with a claim about the target, and without
    // fresh evidence no such claim has been earned. It is also what makes the
    // query durable, because the Coordinator stores it beside the outcome the
    // answer produced. TerminallyUnresolved ends this run but intentionally
    // leaves the target-wide mutation barrier set.
    class ToolCallReconciliation final
    {
        ToolCallReconciliationKind m_kind;
        CanonicalJson              m_payload;
        CanonicalJson              m_evidence;

        ToolCallReconciliation(
            ToolCallReconciliationKind kind,
            CanonicalJson payload,
            CanonicalJson evidence
        );

    public:
        [[nodiscard]]
        static auto confirmed(CanonicalJson result, CanonicalJson evidence)
            -> ToolCallReconciliation;

        [[nodiscard]]
        static auto provenAbsent(
            CanonicalJson explanation,
            CanonicalJson evidence
        ) -> ToolCallReconciliation;

        [[nodiscard]]
        static auto terminallyUnresolved(
            CanonicalJson explanation,
            CanonicalJson evidence
        ) -> ToolCallReconciliation;

        [[nodiscard]]
        auto kind() const noexcept -> ToolCallReconciliationKind;

        [[nodiscard]]
        auto payload() const noexcept UF_LIFETIME_BOUND -> CanonicalJson const&;

        [[nodiscard]]
        auto evidence() const noexcept UF_LIFETIME_BOUND -> CanonicalJson const&;
    };

    // An unforgeable handle to one durable admission row. Only the
    // Coordinator can mint it after re-reading live authority.
    //
    // It is move-only because it is the one-shot right to cross the dispatch
    // boundary, and a right two holders each believe they hold is two rights.
    // The ledger's compare-and-swap does refuse the second beginToolCallDispatch
    // over one admission, but that check exists for the crashed and fenced-out
    // incarnations it was written for; leaning on it to make a duplicated
    // capability harmless would make the capability's meaning a property of a
    // race rather than of the value. A single holder may still retry: what is
    // forbidden is handing a second holder an independent copy.
    class ToolCallAdmission final
    {
        friend class OperatorCoordinator;

        ContentHash m_callIdentity;
        uint64      m_attemptNumber;
        uint64      m_historyRevision;

        ToolCallAdmission(
            ContentHash callIdentity,
            uint64 attemptNumber,
            uint64 historyRevision
        );

    public:
        ToolCallAdmission(ToolCallAdmission&&) noexcept = default;
        auto operator=(ToolCallAdmission&&) noexcept -> ToolCallAdmission& = default;
        ToolCallAdmission(ToolCallAdmission const&) = delete;
        auto operator=(ToolCallAdmission const&) -> ToolCallAdmission& = delete;
        ~ToolCallAdmission() = default;

        [[nodiscard]] auto callIdentity() const -> ContentHash;
        [[nodiscard]] auto attemptNumber() const noexcept -> uint64;
        [[nodiscard]] auto historyRevision() const noexcept -> uint64;
    };

    // The token returned only after dispatching is durable. Provider code must
    // hold this token before it executes, then return it with its conclusion.
    //
    // This is the capability to execute that `caller independence is
    // structural` names, and it is move-only for the reason that ruling gives
    // it a name at all. Holding it is the licence to run provider code, and
    // provider effects are external: two holders means the provider runs twice,
    // and the ledger can refuse the second terminal write but can never unmake
    // the second effect. Completing is deliberately not a consuming operation,
    // because repeating the exact same completion is how a retry after a
    // crashed write rejoins; one holder retrying is the case that must work,
    // and two holders is the case that must not exist.
    class ToolCallDispatch final
    {
        friend class OperatorCoordinator;

        ContentHash m_callIdentity;
        uint64      m_attemptNumber;
        uint64      m_historyRevision;

        ToolCallDispatch(
            ContentHash callIdentity,
            uint64 attemptNumber,
            uint64 historyRevision
        );

    public:
        ToolCallDispatch(ToolCallDispatch&&) noexcept = default;
        auto operator=(ToolCallDispatch&&) noexcept -> ToolCallDispatch& = default;
        ToolCallDispatch(ToolCallDispatch const&) = delete;
        auto operator=(ToolCallDispatch const&) -> ToolCallDispatch& = delete;
        ~ToolCallDispatch() = default;

        [[nodiscard]] auto callIdentity() const -> ContentHash;
        [[nodiscard]] auto attemptNumber() const noexcept -> uint64;
        [[nodiscard]] auto historyRevision() const noexcept -> uint64;
    };

    enum class ToolOutcomeLookup : uint8
    {
        Created,
        Existing,
    };

    struct StoredToolCallOutcome final
    {
        ToolCallState     state{ToolCallState::TerminalFailure};
        ToolOutcomeLookup lookup{ToolOutcomeLookup::Created};
        uint64            revision{};
    };

    struct ToolCallReplay final
    {
        ToolCallState                state{ToolCallState::Proposed};
        uint64                       revision{};
        uint64                       activeAdmissionAttempt{};
        std::optional<CanonicalJson> payload{};
        std::optional<CanonicalJson> evidence{};
    };

    // The direct answer a synchronous caller reads. Only provider conclusions
    // and reconciliations reach this boundary; an internal ledger state is a
    // protocol violation and remains a raised Result failure.
    [[nodiscard]]
    auto toolCallAnswer(
        ContentHash callIdentity,
        ToolCallReplay const& replay
    ) -> Result<json::Value>;
}
