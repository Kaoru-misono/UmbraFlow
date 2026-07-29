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
#include <utility>

// Luau's C headers are third-party and do not build clean under the project's
// /W4 /WX profile; a manifest-driven module has no CMakeLists to mark them
// external, so wrap the includes exactly as the repo's other vendored FFI does
// (image/ffi/png-decoder.cpp).
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
        // unit, clamping a value beyond the addressable range to the maximum (an
        // effectively unlimited ceiling on this host). Zero passes through and
        // disables the ceiling.
        [[nodiscard]]
        auto quotaLimit(uint64 bytes) -> std::size_t
        {
            return checkedCast<std::size_t>(bytes)
                .value_or(std::numeric_limits<std::size_t>::max());
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

        // Memory ledger backing the accounting allocator, filled from the config
        // before the VM is created and read on every allocation. Declared first
        // so it is destroyed last: m_state closes the VM first, while frees from
        // Luau teardown can still reach this live ledger.
        MemoryQuota m_quota;

        // The owned VM handle. Left null until create() allocates it through the
        // accounting allocator, so a construction that fails before then closes
        // nothing.
        std::unique_ptr<lua_State, LuaStateDeleter> m_state;

        // Live cancellation/budget state the interrupt callback reads. Derived
        // from the config at construction (the deadline is anchored to now())
        // and address-stable because Impl is heap-pinned behind unique_ptr.
        InterruptState m_control;

        // Set once a task thread has been hard-cancelled (the interrupt issued
        // lua_break): the VM generation is spent, so every later runNumber
        // refuses with Cancelled instead of resuming an abandoned VM.
        bool m_terminal{false};

    public:
        explicit Impl(EngineConfig const& config)
            : m_quota{.limitBytes = quotaLimit(config.memoryQuotaBytes)}
            , m_control{
                  .cancellation = config.cancellation,
                  .budgetTicks  = config.interruptBudgetTicks,
                  .deadline     = std::chrono::steady_clock::now() + config.maxRuntime,
              }
        {
        }

        Impl(Impl const&) = delete;
        Impl(Impl&&) = delete;
        auto operator=(Impl const&) -> Impl& = delete;
        auto operator=(Impl&&) -> Impl& = delete;

        ~Impl() = default;
    };

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
        // byte against impl->m_quota and refuses growth past the configured
        // ceiling. Returns null on host allocation failure, as luaL_newstate
        // would.
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
        // included. m_control lives in the heap-pinned Impl, so the userdata
        // pointer stays valid. Arming it here rather than after the sandbox is
        // what puts the framework's own boot under the same stop token,
        // instruction budget and deadline a task run answers to; with no
        // framework modules configured no Lua code runs during the boot at all,
        // so nothing is charged against the budget.
        installInterrupt(state, &impl->m_control);

        // Boot the two environments. installSandbox owns the whole ordered
        // sequence -- framework environment and bundle, host tables, the global
        // strippings, luaL_sandbox, project environment prototype -- because an
        // ordering this security-relevant belongs in one place rather than
        // spread across its callers. A failure returns the reason its own step
        // gave, and `impl` closes the VM it had already allocated.
        UF_TRY(installSandbox(state, config, &impl->m_control));

        return Engine{std::move(impl)};
    }

    auto Engine::runNumber(
        std::string_view source,
        std::string_view chunkName
    ) -> Result<double>
    {
        // A hard cancel spends the whole VM generation: once a task thread has
        // been broken, this Engine is terminal and never resumes another thread.
        if (m_impl->m_terminal)
        {
            return fail(
                AutomationErrorKind::Cancelled,
                "engine is terminal: a prior task was hard-cancelled"
            );
        }

        auto result = runNumberInProjectEnvironment(
            m_impl->m_state.get(),
            source,
            chunkName,
            &m_impl->m_control
        );

        // runNumberOnThread already reported the cancel; this only spends the
        // generation, so every later call refuses without touching the VM.
        if (m_impl->m_control.broken)
        {
            m_impl->m_terminal = true;
        }

        return result;
    }
}
