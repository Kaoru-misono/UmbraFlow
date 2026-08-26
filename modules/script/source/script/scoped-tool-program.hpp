#pragma once

#include <script/pure-data-program.hpp>
#include <script/tool-runtime.hpp>

#include <json/value.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace uf::script
{
    // What one scoped run is started under.
    //
    // No in-class initializer for the call identity: ContentHash has no absent
    // value, and the identity is what attributes an execution-limit failure.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct ScopedRunRequest final
    {
        ContentHash callIdentity;

        // The outer Project Tool whose fresh VM owns every allocation and
        // elapsed millisecond in this run.
        std::string budgetOwner{};

        // The wall-clock ceiling that Tool's registration declared. Zero is
        // invalid rather than a spelling of an unlimited or default budget.
        uint64 maximumElapsedMillis{};

        // Hard cancellation. Armed on the Luau interrupt as well as handed to
        // every Tool call, because a script can spin in pure computation without
        // ever reaching one. A stop is teardown rather than unwind: the VM dies
        // without resuming the script, deliberately indistinguishable from a
        // crash, and the durable machinery covers it.
        std::stop_token cancellation{};
    };

    // The third program type. It shares PureDataProgram's closed-graph
    // compilation, host-owned require resolver, per-invoke fresh quota-bound VM,
    // resource closure and deep-freeze discipline, and adds exactly one native
    // seam: the Tool Runtime call.
    //
    // The scoped module catalog is a property of THIS TYPE and not of a VM boot
    // or a constructor flag, because a pure program's inability to load a scoped
    // module has to fail in the RESOLVER, naming the module it asked for. Two
    // genuinely different contracts get two types; a capability-set parameter
    // would make purity a value, and a program in a signature would stop telling
    // the reader whether the thing in hand can reach the world.
    //
    // The scoped modules are Luau sources in the Framework bundle, exactly as
    // the pure Framework modules are. compile() refuses a bundle that does not
    // carry every one of them under its exact name, and hands the private
    // capability table to the generated Tool face alone -- the module that
    // renders the pinned catalog into one callable per Tool -- and to nothing
    // else.
    class ScopedToolProgram final
    {
    public:
        // Bounded like every other name this boundary admits. The call ceiling
        // belongs only to an interactive session; a Project handler is a leaf.
        static constexpr auto k_maximumToolNameBytes        = std::size_t{128U};
        static constexpr auto k_maximumToolNameSegments     = std::size_t{8U};
        static constexpr auto k_maximumToolNameSegmentBytes = std::size_t{64U};
        static constexpr auto k_maximumInteractiveToolCalls = uint64{1024U};

    private:
        class State;

        std::shared_ptr<State const> m_state;

        explicit ScopedToolProgram(std::shared_ptr<State const> p_state) noexcept;

    public:
        ScopedToolProgram(ScopedToolProgram const&) noexcept = default;
        ScopedToolProgram(ScopedToolProgram&&) noexcept      = default;
        auto operator=(ScopedToolProgram const&) noexcept -> ScopedToolProgram& = default;
        auto operator=(ScopedToolProgram&&) noexcept -> ScopedToolProgram&      = default;
        ~ScopedToolProgram() = default;

        // The exact Framework modules this program type admits beyond the pure
        // ones, in the order its environment identity states them. Published as
        // data for the same reason the pure global whitelist is: the set is
        // otherwise unobservable from outside a VM, so a test can bind it only
        // by comparing it here.
        [[nodiscard]]
        static auto scopedModuleNames() -> std::span<std::string_view const>;

        // The subset of those a Project-authored module may resolve. The Tool
        // face renderer is deliberately absent: it holds the private capability
        // table, so the only route from Project source to the Tool Runtime is a
        // module this type GENERATED from the pinned catalog.
        [[nodiscard]]
        static auto projectVisibleScopedModuleNames()
            -> std::span<std::string_view const>;

        // `frameworkModules` must carry every name scopedModuleNames() states,
        // and may carry pure Framework modules besides. The Tool primitive is
        // installed as a terminal named refusal: a Project handler is a leaf.
        //
        // The pinned Tool catalog the scoped facades read is a Framework
        // resource and belongs in `frameworkResources`, whose names must all sit
        // inside the reserved `umbraflow.` namespace that `resources` is refused
        // for. The catalog's own name is the Framework bundle's to state; this
        // module reserves the class and never names one member of it.
        [[nodiscard]]
        static auto compile(
            std::string_view pluginId,
            std::string_view entryModule,
            std::vector<PureDataProgram::Module> modules,
            std::span<std::string_view const> entryPoints,
            std::vector<PureDataProgram::Resource> resources,
            std::span<FrameworkModule const> frameworkModules,
            std::vector<PureDataProgram::Resource> frameworkResources
        ) -> Result<ScopedToolProgram>;

        // One leaf handler run in one fresh VM.
        [[nodiscard]]
        auto invoke(
            std::string_view entryPoint,
            json::Value const& immutableInput,
            ScopedRunRequest const& request
        ) const -> Result<json::Value>;
    };

    // The chunk-at-a-time front end to the same scoped environment. It is a
    // transport shape, not another capability set: every chunk resolves the
    // modules scopedModuleNames() states and reaches the same one
    // ToolRuntimeInvoke primitive a registered Project handler reaches.
    //
    // A chunk requires three things and no fourth: the Framework modules, the
    // Tool face this run's pinned catalog renders, and THE VERIFIED PROJECT
    // CLOSURE OF THE GENERATION ITS SESSION PINNED. The third is the same
    // module graph ScopedToolProgram::compile is given for a registered
    // handler, so a Project's own decision logic is written once and required
    // from either side. What separates the two sides is the RUN, not the
    // module: ScopedSessionRun admits a Tool call through the session's root
    // and counts it, ScopedToolRun refuses every Tool name because a handler is
    // a leaf, and one module required from both places is subject to whichever
    // run it is executing inside.
    //
    // Each evaluate() owns one fresh VM and one elapsed-time window. The
    // release-owned Framework module views must outlive this object; the task
    // bundle satisfies that with generated string literals. Framework
    // resources, the Project closure and the Tool Runtime are owned here.
    class ScopedToolSession final
    {
        std::vector<FrameworkModule>           m_frameworkModules;

        // The Tool face this session's pinned catalog renders to, as owned
        // sources. They are OWNED rather than borrowed because they exist only
        // for this run: they are derived from the catalog resource below, not
        // from the release, so no static literal backs them. Each evaluate()
        // builds its module views from this vector, which is never mutated
        // after construction.
        std::vector<PureDataProgram::Module>   m_generatedModules;

        std::vector<PureDataProgram::Resource> m_frameworkResources;

        // The generation's own verified module and resource closures, held as
        // the exact bytes create() was handed. They are OWNED and never re-read
        // from anywhere: a session that went back to the project directory
        // could run code the ledger's module_manifest_hash does not name, and
        // the run would stop being attributable to the generation it pinned.
        std::vector<PureDataProgram::Module>   m_projectModules;
        std::vector<PureDataProgram::Resource> m_projectResources;

        ToolRuntimeInvoke          m_invokeTool;
        std::stop_token            m_cancellation;
        MonotonicInstant::Duration m_maximumRuntime;
        std::size_t                m_memoryQuotaBytes;
        HeapUsage                  m_outcomeHeapUsage{};
        HeapUsage                  m_heapUsage{};
        bool                       m_generationSpent{};

        ScopedToolSession(
            std::vector<FrameworkModule> frameworkModules,
            std::vector<PureDataProgram::Module> generatedModules,
            std::vector<PureDataProgram::Resource> frameworkResources,
            std::vector<PureDataProgram::Module> projectModules,
            std::vector<PureDataProgram::Resource> projectResources,
            ToolRuntimeInvoke invokeTool,
            std::stop_token cancellation,
            MonotonicInstant::Duration maximumRuntime,
            std::size_t memoryQuotaBytes
        ) noexcept;

    public:
        ScopedToolSession(ScopedToolSession const&) = delete;
        auto operator=(ScopedToolSession const&) -> ScopedToolSession& = delete;
        ScopedToolSession(ScopedToolSession&&) noexcept                = default;
        auto operator=(ScopedToolSession&&) noexcept
            -> ScopedToolSession& = default;
        ~ScopedToolSession() = default;

        // Validates the scoped Framework closure, the pinned catalog and the
        // Project closure before a session is handed back. Zero
        // memoryQuotaBytes has the same explicit meaning EngineConfig gives it:
        // no allocator ceiling.
        //
        // `projectModules` is the generation's verified module closure and
        // `projectResources` its verified resource closure, both as the exact
        // bytes their registration was held against. A module named as the
        // session's own root is refused by name here rather than left to
        // collide inside the compiler, because the two names mean different
        // things and only the caller can tell which one it meant.
        [[nodiscard]]
        static auto create(
            std::vector<FrameworkModule> frameworkModules,
            std::vector<PureDataProgram::Resource> frameworkResources,
            std::vector<PureDataProgram::Module> projectModules,
            std::vector<PureDataProgram::Resource> projectResources,
            ToolRuntimeInvoke invokeTool,
            std::stop_token cancellation,
            MonotonicInstant::Duration maximumRuntime,
            uint64 memoryQuotaBytes
        ) -> Result<ScopedToolSession>;

        // Compiles and runs one source chunk as the body of a generated scoped
        // entry point. A chunk returns the same scalar transport values Engine
        // carried: absent, boolean, number or string. Tables are refused rather
        // than silently erased.
        [[nodiscard]]
        auto evaluate(
            std::string_view source,
            std::string_view chunkName
        ) -> Result<ScriptValue>;

        // The pre-reclamation heap reading for the last chunk. It exists so a
        // caller can identify a failure that met the configured ceiling before
        // the chunk VM is collected and destroyed.
        [[nodiscard]] auto outcomeHeapUsage() const noexcept -> HeapUsage;

        // The last chunk VM's reading after full collection. Module and global
        // state never cross into the next chunk; the high-water mark remains a
        // useful diagnostic for the result line.
        [[nodiscard]] auto heapUsage() const noexcept -> HeapUsage;

        // A deadline, instruction budget or stop token spends the interactive
        // generation. An ordinary compile/runtime failure spends only its
        // chunk, whose fresh VM is already gone.
        [[nodiscard]] auto generationSpent() const noexcept -> bool;
    };

    // The read-only JSON resource name the host bakes a run's pinned Tool
    // catalog into, and the scoped program type reads to render its Tool face.
    // It is owned here rather than by the Framework bundle because the module
    // set a scoped closure carries is DERIVED from these bytes: the type that
    // builds the closure has to be able to find them.
    [[nodiscard]]
    auto scopedToolCatalogResourceName() noexcept -> std::string_view;

    // The exact scoped environment bytes scopedToolEnvironmentHash is taken
    // over: everything the pure environment attests to, plus the scoped module
    // catalog, the Tool Runtime facade contract and the ceilings only this
    // program type has. Published for the same reason the pure material is, and
    // distinct from pluginEnvironmentMaterial() by construction: a build that
    // moved one scoped module name or changed what invoke answers with must move
    // this digest and must not move that one.
    [[nodiscard]]
    auto scopedToolEnvironmentMaterial() -> std::string;

    [[nodiscard]]
    auto scopedToolEnvironmentHash() -> Result<ContentHash>;
} // namespace uf::script
