#include <script/engine.hpp>

#include "allocator.hpp"
#include "cancellation.hpp"
#include "environment.hpp"
#include "sandbox.hpp"

#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <chrono>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>

// Luau's C headers do not build clean under the project's /W4 /WX profile, and
// a manifest-driven module has no CMakeLists to mark them external; wrap them as
// image/ffi/png-decoder.cpp does.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
#include <lua.h>
#include <lualib.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace uf::script
{
    namespace
    {
        // Convert the config's 64-bit ceiling to the allocator's size_t ledger
        // unit, clamping past the addressable range to the maximum (effectively
        // unlimited here). Zero passes through and disables the ceiling.
        [[nodiscard]]
        auto quotaLimit(uint64 bytes) -> std::size_t
        {
            return checkedCast<std::size_t>(bytes)
                .value_or(std::numeric_limits<std::size_t>::max());
        }

        // The refusal every call gets once a generation has been spent, naming
        // what spent it. Shared by runNumber and runValue so an agent reading
        // one never learns less than an agent reading the other.
        [[nodiscard]]
        auto terminalRefusal(InterruptState const& control) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::Cancelled,
                "engine is terminal: a prior unit of script was hard-cancelled ["
                    + describeBreak(control) + "]"
            );
        }

        struct LuaStateDeleter final
        {
            auto operator()(lua_State* p_state) const noexcept -> void
            {
                if (p_state == nullptr)
                {
                    return;
                }

                // SAFETY: p_state is the owning lua_State handle returned by
                // createStateWithQuota. Closing it releases the whole VM.
                lua_close(p_state);
            }
        };
    }

    class Engine::Impl final
    {
        friend class Engine;

        // Memory ledger backing the accounting allocator, read on every
        // allocation. Declared before m_state so it is destroyed last: closing
        // the VM runs frees that must still reach a live ledger.
        MemoryQuota m_quota;

        // The owned VM handle, null until create() allocates it through the
        // accounting allocator, so a construction that fails before then closes
        // nothing.
        std::unique_ptr<lua_State, LuaStateDeleter> m_state;

        // Live cancellation/budget state the interrupt callback reads. The
        // deadline is anchored at construction, so the framework boot runs under
        // it too; address-stable because Impl is heap-pinned behind unique_ptr.
        InterruptState m_control;

        // The configured ceiling on ONE unit of script, kept so every run can
        // re-anchor the deadline against it.
        std::chrono::steady_clock::duration m_maxRuntime;

        // The host's decoder for a raised value nobody caught. Copied rather
        // than borrowed: create() takes the config by const reference and the
        // caller may destroy it, while this is read for the Engine's whole life.
        RaisedErrorClassifier m_classifyRaisedError;

        // Set once the interrupt has hard-cancelled a task thread: the VM
        // generation is spent, so every later run refuses with Cancelled instead
        // of resuming an abandoned VM.
        bool m_terminal{false};

    public:
        explicit Impl(EngineConfig const& config)
            : m_quota{.limitBytes = quotaLimit(config.memoryQuotaBytes)}
            , m_control{
                  .cancellation = config.cancellation,
                  .budgetTicks  = config.interruptBudgetTicks,
                  .deadline     = std::chrono::steady_clock::now() + config.maxRuntime,
              }
            , m_maxRuntime{config.maxRuntime}
            , m_classifyRaisedError{config.classifyRaisedError}
        {
        }

        Impl(Impl const&) = delete;
        Impl(Impl&&) = delete;
        auto operator=(Impl const&) -> Impl& = delete;
        auto operator=(Impl&&) -> Impl& = delete;

        ~Impl() = default;

        // Anchor the wall clock at the unit of script about to run: the ceiling
        // bounds ONE chunk, not the VM's life (see EngineConfig::maxRuntime).
        // The framework boot keeps the construction anchor above, so nothing is
        // left unguarded between create() and the first run.
        auto beginUnitOfScript() noexcept -> void
        {
            m_control.runStartedAt = std::chrono::steady_clock::now();
            m_control.deadline     = m_control.runStartedAt + m_maxRuntime;
        }
    };

    auto HeapUsage::headroomBytes() const noexcept -> uint64
    {
        if (ceilingBytes == 0)
        {
            return std::numeric_limits<uint64>::max();
        }
        return ceilingBytes > usedBytes ? ceilingBytes - usedBytes : uint64{0};
    }

    auto heapUsage(lua_State* state) noexcept -> HeapUsage
    {
        auto const* p_quota = quotaFor(state);
        if (p_quota == nullptr)
        {
            return HeapUsage{};
        }

        // Widening the ledger's size_t to the report's uint64 loses nothing on
        // any host this project builds for, so it needs no checked cast; the
        // narrowing direction is quotaLimit's, above.
        return HeapUsage{
            .usedBytes    = static_cast<uint64>(p_quota->used),
            .ceilingBytes = static_cast<uint64>(p_quota->limitBytes),
            .peakBytes    = static_cast<uint64>(p_quota->peak),
        };
    }

    auto collectGarbage(lua_State* state) -> HeapUsage
    {
        if (state != nullptr)
        {
            // Runs no Lua: Luau has no __gc metamethod and a sweep can only call
            // a C userdata destructor, so this is safe from inside a
            // lua_CFunction, whose own frame values are stack roots.
            lua_gc(state, LUA_GCCOLLECT, 0);
        }
        return heapUsage(state);
    }

    ScriptValue::ScriptValue(bool value) noexcept
        : m_value{value}
    {
    }

    ScriptValue::ScriptValue(double value) noexcept
        : m_value{value}
    {
    }

    ScriptValue::ScriptValue(std::string value) noexcept
        : m_value{std::move(value)}
    {
    }

    auto ScriptValue::absent() const noexcept -> bool
    {
        return std::holds_alternative<std::monostate>(m_value);
    }

    auto ScriptValue::boolean() const noexcept -> std::optional<bool>
    {
        auto const* p_value = std::get_if<bool>(&m_value);
        return p_value != nullptr ? std::optional<bool>{*p_value} : std::nullopt;
    }

    auto ScriptValue::number() const noexcept -> std::optional<double>
    {
        auto const* p_value = std::get_if<double>(&m_value);
        return p_value != nullptr ? std::optional<double>{*p_value} : std::nullopt;
    }

    auto ScriptValue::text() const noexcept -> std::string const*
    {
        return std::get_if<std::string>(&m_value);
    }

    Engine::Engine(std::unique_ptr<Impl> p_impl) noexcept
        : m_impl{std::move(p_impl)}
    {
    }

    Engine::Engine(Engine&&) noexcept = default;
    auto Engine::operator=(Engine&&) noexcept -> Engine& = default;
    Engine::~Engine() = default;

    auto Engine::create(EngineConfig const& config) -> Result<Engine>
    {
        // Build Impl first so its address-stable m_quota exists before the VM:
        // the accounting allocator needs that ledger pointer at creation time.
        auto impl = std::make_unique<Impl>(config);

        // Allocate the VM through the accounting allocator, which charges every
        // byte against impl->m_quota and refuses growth past the ceiling. Null
        // on host allocation failure, as luaL_newstate would be.
        impl->m_state.reset(createStateWithQuota(&impl->m_quota));
        auto* state = impl->m_state.get();
        if (state == nullptr)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "createStateWithQuota returned null"
            );
        }
        luaL_openlibs(state);

        // Arm hard cancellation before ANY Lua code runs, the framework bundle
        // included: arming it here rather than after the sandbox is what puts
        // the framework's own boot under the same stop token, instruction budget
        // and deadline a task run answers to. m_control lives in the heap-pinned
        // Impl, so the userdata pointer stays valid.
        installInterrupt(state, &impl->m_control);

        // Boot the two environments. installSandbox owns the whole ordered
        // sequence, because an ordering this security-relevant belongs in one
        // place rather than spread across its callers. A failure returns the
        // reason its own step gave, and `impl` closes the VM already allocated.
        UF_TRY(installSandbox(state, config, &impl->m_control));

        return Engine{std::move(impl)};
    }

    auto Engine::runNumber(
        std::string_view source,
        std::string_view chunkName
    ) -> Result<double>
    {
        // A hard cancel spends the whole VM generation: once a task thread has
        // been broken this Engine is terminal and never resumes another.
        if (m_impl->m_terminal)
        {
            return terminalRefusal(m_impl->m_control);
        }

        m_impl->beginUnitOfScript();

        auto result = runNumberInProjectEnvironment(
            m_impl->m_state.get(),
            source,
            chunkName,
            &m_impl->m_control,
            &m_impl->m_classifyRaisedError
        );

        // The runner already reported the cancel; this only spends the
        // generation, so every later call refuses without touching the VM.
        if (m_impl->m_control.broken())
        {
            m_impl->m_terminal = true;
        }

        return result;
    }

    auto Engine::runValue(
        std::string_view source,
        std::string_view chunkName
    ) -> Result<ScriptValue>
    {
        if (m_impl->m_terminal)
        {
            return terminalRefusal(m_impl->m_control);
        }

        m_impl->beginUnitOfScript();

        auto result = runValueInProjectEnvironment(
            m_impl->m_state.get(),
            source,
            chunkName,
            &m_impl->m_control,
            &m_impl->m_classifyRaisedError
        );

        if (m_impl->m_control.broken())
        {
            m_impl->m_terminal = true;
        }

        return result;
    }

    auto Engine::collectGarbage() -> HeapUsage
    {
        return uf::script::collectGarbage(m_impl->m_state.get());
    }

    auto Engine::generationSpent() const noexcept -> bool
    {
        return m_impl->m_terminal;
    }

    auto Engine::heapUsage() const noexcept -> HeapUsage
    {
        return uf::script::heapUsage(m_impl->m_state.get());
    }
}
