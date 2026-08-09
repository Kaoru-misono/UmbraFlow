#pragma once

#include <core/error/result.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uf::script
{
    // Immutable Luau bytecode for one pure, data-only module. Compilation and
    // exact export validation happen once in compile(); invoke() loads those
    // bytes into a fresh quota-bound VM and passes one immutable string in and
    // one immutable string out. A frozen artifact.read(name) reader lazily copies
    // only registered immutable blobs into that VM. No host installer or native
    // capability seam is part of this API.
    class PureDataProgram final
    {
        class State;

        std::shared_ptr<State const> m_state;

        explicit PureDataProgram(std::shared_ptr<State const> p_state) noexcept;

    public:
        struct Artifact final
        {
            std::string name{};
            std::string bytes{};
        };

        PureDataProgram(PureDataProgram const&) noexcept = default;
        PureDataProgram(PureDataProgram&&) noexcept = default;
        auto operator=(PureDataProgram const&) noexcept -> PureDataProgram& = default;
        auto operator=(PureDataProgram&&) noexcept -> PureDataProgram& = default;
        ~PureDataProgram() = default;

        [[nodiscard]]
        static auto compile(std::string_view moduleId,
                            std::string_view source,
                            std::span<std::string_view const> entryPoints,
                            std::vector<Artifact> artifacts) -> Result<PureDataProgram>;

        [[nodiscard]]
        auto invoke(std::string_view entryPoint, std::string_view immutableInput) const
            -> Result<std::string>;
    };
} // namespace uf::script
