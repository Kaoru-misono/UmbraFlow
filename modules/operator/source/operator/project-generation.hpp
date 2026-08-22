#pragma once

#include "manifest.hpp"
#include "project-plugin.hpp"
#include "tool-invocation.hpp"

#include <script/pure-data-program.hpp>
#include <script/scoped-tool-program.hpp>

#include <json/value.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>

#include <domain/content-hash.hpp>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    // One loaded two-closure Project registration generation: the reducer
    // compiled on the pure program type and the tool closure compiled on the
    // scoped program type, admitted together or not at all.
    //
    // Two types rather than two spellings. Reduction runs only on the pure
    // type, whose resolver refuses every scoped module by name, and dispatch
    // runs only on the scoped type, which is the only one holding a Tool
    // Runtime seam. Nothing here selects between them: there is no entry name
    // in both contracts and no value that decides where a name lives.
    //
    // Holding one is proof of both halves of the export join. The generation
    // STATED what each closure exports; the loader proved each statement
    // against what it should be -- exactly `reduce` for the reducer, exactly
    // the binding table's union for the tool closure -- and the Luau bridge
    // then proved each statement against the closure's actual exports when it
    // compiled it. Neither proof can stand in for the other: the first catches
    // a document whose declared contract and stated code disagree, the second
    // catches stated code the shipped bytes do not honour.
    //
    // It is also proof of the catalog join: every Tool the pinned catalog
    // declares is bound to an entry, and every binding names a Tool that
    // catalog declares.
    class ProjectGenerationHandle final
    {
        class State;

        friend class ProjectGenerationRegistrar;

        std::shared_ptr<State const> m_state;

        explicit ProjectGenerationHandle(
            std::shared_ptr<State const> p_state
        ) noexcept;

    public:
        ProjectGenerationHandle(ProjectGenerationHandle const&) noexcept = default;
        ProjectGenerationHandle(ProjectGenerationHandle&&) noexcept      = default;
        auto operator=(ProjectGenerationHandle const&) noexcept
            -> ProjectGenerationHandle& = default;
        auto operator=(ProjectGenerationHandle&&) noexcept
            -> ProjectGenerationHandle& = default;
        ~ProjectGenerationHandle() = default;

        [[nodiscard]] auto pluginId() const -> std::string;
        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;

        // Two closures, two manifest digests, answered apart. A generation
        // whose two closures were one digest would be a generation that could
        // not move one closure's code without moving the other's identity.
        [[nodiscard]] auto reducerModuleManifestHash() const -> ContentHash;
        [[nodiscard]] auto toolModuleManifestHash() const -> ContentHash;

        // The Framework catalog this generation's scoped facades were pinned
        // to, and the scoped environment the tool closure was compiled under.
        // Both are exposed because a durable row must record which code
        // answered a call, and neither is derivable from the generation alone.
        [[nodiscard]] auto frameworkToolCatalogHash() const -> ContentHash;
        [[nodiscard]] auto environmentIdentity() const -> ContentHash;

        // The join this generation was admitted on, and the declaration
        // authority behind it. The table is what the tool closure was compiled
        // over; the catalog is handed out whole rather than projected, because
        // the bounds one call is admitted under are read from it per call and
        // never from anything the compiler saw.
        [[nodiscard]]
        auto bindingTable() const noexcept UF_LIFETIME_BOUND
            -> ProjectToolBindingTable const&;

        [[nodiscard]]
        auto catalog() const noexcept UF_LIFETIME_BOUND
            -> ProjectToolCatalogSchemaOwner const&;

        // Mints canonical bytes through this generation's pinned schema owner.
        // Trusted Operator code needs it because it assembles the reduce
        // envelope itself rather than accepting one from a caller; it grants no
        // authority beyond proving the bytes are exact JCS.
        [[nodiscard]]
        auto canonicalize(std::string exactJcs) const -> Result<CanonicalJson>;

        // Run the generation's fold, in one fresh pure VM, and hand the answer
        // to the ProjectState authority this generation pinned. The stamped
        // document is the only reduced baseline a durable row may be written
        // from: a raw value would be a fold nothing judged, and a baseline is
        // the state every later revision is derived from.
        [[nodiscard]]
        auto reduce(CanonicalJson const& input) const -> Result<ValidatedDocument>;

        // Run the entry this generation bound to `toolName`, in one fresh
        // scoped VM, with the run's issuing coordinate carried in `request`. A
        // name this generation never bound is a refusal rather than an
        // absence.
        [[nodiscard]]
        auto invokeBoundTool(
            std::string_view toolName,
            json::Value const& canonicalArguments,
            script::ScopedRunRequest const& request
        ) const -> Result<json::Value>;

        // Whether the answer a bound entry produced is one this Tool's catalog
        // entry declared it could produce. It sits beside invokeBoundTool
        // rather than inside it because the dispatcher runs it between the
        // program's answer and the terminal durable row, and this seam writes
        // no row.
        [[nodiscard]]
        auto validateToolResult(
            std::string_view toolName,
            std::string_view exactResultJcs
        ) const -> Status;
    };

    // The fold a loaded Project registration performs, in the one spelling the
    // durable seams take it in.
    //
    // It exists because provisioning genuinely has to CALL the reducer and not
    // merely name it: a ProjectInstance row carries the reduction of its
    // complete Journal prefix, and that prefix is assembled by the Operator, so
    // no caller can be trusted to hand the answer in ready-made. What
    // provisioning must NOT take is the whole loaded generation: a seam that
    // named it would be a seam every later change to the generation handle had
    // to be walked through.
    //
    // So this is the fold and its provenance and nothing else: which
    // registration root answered, how a caller's exact envelope bytes become
    // canonical, and the fold itself.
    class ProjectBaselineReducer final
    {
        // The two calls together, rather than a fold that canonicalizes its own
        // input: the envelope is minted by the caller through the SAME schema
        // owner that judges the answer, so the bytes offered to the fold are
        // already proved exact by the authority that will stamp its result.
        using Canonicalizer = std::function<Result<CanonicalJson>(std::string)>;
        using Fold = std::function<Result<ValidatedDocument>(CanonicalJson const&)>;

        ContentHash   m_projectRegistrationHash;
        Canonicalizer m_canonicalize;
        Fold          m_fold;

    public:
        // The projection is implicit for the reason ProjectIdentity's is:
        // naming the narrowing would say only which loader the caller happens
        // to hold, which is the fact these seams must not depend on.
        ProjectBaselineReducer(ProjectGenerationHandle const& generation);

        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;

        [[nodiscard]]
        auto canonicalize(std::string exactJcs) const -> Result<CanonicalJson>;

        [[nodiscard]]
        auto reduce(CanonicalJson const& input) const -> Result<ValidatedDocument>;
    };

    // Startup-only exact registry, one entry per (plugin id, registration
    // root). A registration root is a generation, so re-registering the same
    // root is refused rather than recompiled.
    class ProjectGenerationRegistrar final
    {
    public:
        // One closure as it is offered to the registrar: the entry module its
        // manifest digest is taken over, and the exact module blobs of the
        // closed graph beneath it.
        //
        // The resource closure is deliberately not in here. A registration
        // pins ONE resource closure and both program types read it, so a
        // per-closure resource list would be two answers to which bytes this
        // project registered.
        struct ClosureModules final
        {
            std::string                    entryModule{};
            std::vector<ProjectModuleBlob> modules{};
        };

    private:
        std::map<
            std::pair<std::string, ContentHash>,
            ProjectGenerationHandle
        > m_generations{};

    public:
        // `catalog` must be the owner built over the exact Tool Catalog bytes
        // this generation pinned, `schemaOwner` the owner built over the exact
        // document schemas it pinned, and `invokeTool` is the one native seam
        // the compiled tool closure reaches the Tool Runtime through. The seam
        // is bound once, at compile time, and must therefore carry no run
        // state: every run-scoped value travels in the ScopedRunRequest of the
        // invoke that is executing.
        //
        // Neither closure's declared export set is derived here, and neither is
        // observed by running the module. The generation states both; this
        // function joins each statement against what the contract says it
        // should be, and hands the statement itself to the bridge as the set to
        // admit the closure against.
        //
        // `validateResults` judges what a bound entry answers with, and is
        // required for the reason the argument validator inside the catalog is:
        // a generation whose answers nothing judges is a generation whose
        // result schemas are decoration.
        [[nodiscard]]
        auto registerGeneration(
            VerifiedProjectGeneration const& generation,
            ProjectToolCatalogSchemaOwner catalog,
            ProjectSchemaOwner schemaOwner,
            ClosureModules reducerClosure,
            ClosureModules toolClosure,
            std::vector<ProjectResourceBlob> exactResources,
            ToolResultValidator validateResults,
            script::ToolRuntimeInvoke invokeTool
        ) -> Result<ProjectGenerationHandle>;

        [[nodiscard]]
        auto findExact(
            std::string const& pluginId,
            ContentHash projectRegistrationHash
        ) const -> Result<ProjectGenerationHandle>;
    };
} // namespace uf::operator_runtime
