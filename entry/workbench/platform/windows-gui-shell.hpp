#pragma once

#include "windows-texture-cache.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace uf::workbench::platform
{
    // Defined in the implementation. It owns every Win32, DXGI, Direct3D 11, and
    // Dear ImGui resource so none of those types cross this header boundary.
    struct GuiShellState;

    struct GuiShellConfig final
    {
        std::string           m_title{};
        uint32                m_width{1280};
        uint32                m_height{720};
        std::optional<uint32> m_smokeFrames{};
    };

    // The per-frame draw protocol: invoked once inside each pumped frame while an
    // ImGui frame is active. It is called synchronously from run() and never
    // retained, so it may borrow state that outlives the run() call.
    using GuiFrameCallback = std::function<void()>;

    // Owns the workbench's top-level window, its swap chain and device, and the
    // ImGui context wired to both backends. Construction performs all setup and
    // fails as a Result; the destructor tears every resource down in order.
    class GuiShell final
    {
        std::unique_ptr<GuiShellState> m_state;

        explicit GuiShell(std::unique_ptr<GuiShellState> state) noexcept;

    public:
        GuiShell(GuiShell&&) noexcept;
        auto operator=(GuiShell&&) noexcept -> GuiShell&;
        GuiShell(GuiShell const&) = delete;
        auto operator=(GuiShell const&) -> GuiShell& = delete;
        ~GuiShell();

        [[nodiscard]]
        static auto create(GuiShellConfig const& config) -> Result<GuiShell>;

        // The shell's source-image texture cache, backed by its Direct3D device.
        // The reference is valid for the shell's lifetime; callers use it to
        // upload source images for the canvas without touching Direct3D.
        [[nodiscard]]
        auto textures() noexcept UF_LIFETIME_BOUND -> TextureCache&;

        // Pumps the window message loop, rendering one frame per iteration and
        // invoking callback while its ImGui frame is active, until the window is
        // closed or the configured smoke-frame budget is reached.
        [[nodiscard]]
        auto run(GuiFrameCallback const& callback) -> Status;
    };
}
