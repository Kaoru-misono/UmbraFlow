#include <script/pure-data-program.hpp>

#include "program-runtime.hpp"

#include <json/value.hpp>

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>

#include <domain/content-hash.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::script
{
    namespace
    {
        // The wall-clock ceiling on one pure call. A pure entry point computes
        // and returns without waiting for anything, so seconds are the whole of
        // what it can legitimately need; a program type whose calls block in a
        // Tool sets its own ceiling and publishes it in its own identity.
        constexpr auto k_pureRuntimeCeiling =
            MonotonicInstant::Duration{std::chrono::seconds{2}};
    } // namespace

    class PureDataProgram::State final
    {
    public:
        detail::ProgramClosure closure{};
    };

    PureDataProgram::PureDataProgram(std::shared_ptr<State const> p_state) noexcept
        : m_state{std::move(p_state)}
    {
    }

    auto PureDataProgram::validateModuleClosure(
        std::string_view entryModule,
        std::span<Module const> modules
    ) -> Status
    {
        return detail::validateModuleClosure(entryModule, modules);
    }

    auto PureDataProgram::validateResourceClosure(
        std::span<Resource const> resources
    ) -> Status
    {
        return detail::validateResourceClosure(resources);
    }

    auto PureDataProgram::compile(
        std::string_view pluginId,
        std::string_view entryModule,
        std::vector<Module> modules,
        std::span<std::string_view const> entryPoints,
        std::vector<Resource> resources,
        std::span<FrameworkModule const> frameworkModules,
        std::vector<Resource> frameworkResources
    ) -> Result<PureDataProgram>
    {
        // No capability-bound modules, so this program type's resolver carries
        // no scoped module name at all and its Framework modules are handed no
        // chunk argument. That is what makes a PureDataProgram in a signature
        // tell the reader the thing in hand cannot reach the world.
        auto const spec = detail::ClosureSpec{
            .pluginId         = pluginId,
            .entryModule      = entryModule,
            .entryPoints      = entryPoints,
            .frameworkModules = frameworkModules,
        };
        UF_TRY_VALUE(
            closure,
            detail::compileClosure(
                spec,
                std::move(modules),
                std::move(resources),
                std::move(frameworkResources)
            )
        );

        auto vm = detail::QuotaBoundVm{k_pureRuntimeCeiling, std::stop_token{}};
        UF_TRY(detail::admitClosure(vm, closure, {}));

        auto state = std::make_shared<State>(State{.closure = std::move(closure)});
        return PureDataProgram{std::shared_ptr<State const>{std::move(state)}};
    }

    auto PureDataProgram::invoke(
        std::string_view entryPoint,
        json::Value const& immutableInput
    ) const -> Result<json::Value>
    {
        if (!std::ranges::binary_search(m_state->closure.entryPoints, entryPoint))
        {
            return detail::refuse("pure data entry point is not registered");
        }

        auto vm = detail::QuotaBoundVm{k_pureRuntimeCeiling, std::stop_token{}};
        return detail::invokeClosure(
            vm,
            m_state->closure,
            {},
            entryPoint,
            immutableInput
        );
    }

    auto pureEnvironmentGlobals() -> std::span<std::string_view const>
    {
        return detail::k_pureGlobals;
    }

    auto pluginEnvironmentMaterial() -> std::string
    {
        auto material = detail::sharedEnvironmentMaterial(k_pureRuntimeCeiling);
        material += '}';
        return material;
    }

    auto pluginEnvironmentHash() -> Result<ContentHash>
    {
        auto const material = pluginEnvironmentMaterial();
        return sha256(std::as_bytes(std::span{material}));
    }
} // namespace uf::script
