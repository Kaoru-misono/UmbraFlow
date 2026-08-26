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
    // The four scoped modules are Luau sources in the Framework bundle, exactly
    // as the pure Framework modules are. compile() refuses a bundle that does
    // not carry all four under their exact names, and hands each of them, and
    // nothing else, the private capability table as its chunk argument.
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
    // Each evaluate() owns one fresh VM and one elapsed-time window. The
    // release-owned Framework module views must outlive this object; the task
    // bundle satisfies that with generated string literals. Framework resources
    // and the Tool Runtime are owned here.
    class ScopedToolSession final
    {
        std::vector<FrameworkModule>           m_frameworkModules;
        std::vector<PureDataProgram::Resource> m_frameworkResources;
        ToolRuntimeInvoke                      m_invokeTool;
        std::stop_token                        m_cancellation;
        MonotonicInstant::Duration             m_maximumRuntime;
        std::size_t                            m_memoryQuotaBytes;
        HeapUsage                              m_outcomeHeapUsage{};
        HeapUsage                              m_heapUsage{};
        bool                                   m_generationSpent{};

        ScopedToolSession(
            std::vector<FrameworkModule> frameworkModules,
            std::vector<PureDataProgram::Resource> frameworkResources,
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

        // Validates the scoped Framework closure and pinned catalog before a
        // session is handed back. Zero memoryQuotaBytes has the same explicit
        // meaning EngineConfig gives it: no allocator ceiling.
        [[nodiscard]]
        static auto create(
            std::vector<FrameworkModule> frameworkModules,
            std::vector<PureDataProgram::Resource> frameworkResources,
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
