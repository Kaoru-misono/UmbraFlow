#include "windows-gui-shell.hpp"

#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#pragma warning(push, 0)
#include <Windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <winrt/base.h>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#pragma warning(pop)

#include <array>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

// The Dear ImGui Win32 backend keeps this declaration in a #if 0 block to avoid
// pulling <windows.h> into its header, and documents that each application must
// copy it in. Both the return type and the parameter types are now in scope.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam
);

namespace uf::workbench::platform
{
    namespace
    {
        constexpr auto k_windowClassName = std::wstring_view{
            L"UmbraWorkbenchWindow"
        };
        constexpr auto k_swapChainBufferCount = uint32{2};

        // Where ImGui persists the docking layout. The arrangement of the panels
        // is a per-user preference of the application rather than a property of
        // any one annotation project, so it lives with the user's application
        // data instead of in a project directory or in whatever working directory
        // ImGui would otherwise default to.
        //
        // Yields nothing when the directory cannot be prepared. The layout is
        // then simply not persisted: an unwritable settings file must never stop
        // the workbench from opening.
        [[nodiscard]]
        auto layoutSettingsPath() -> std::optional<std::filesystem::path>
        {
            auto localAppData = std::array<wchar_t, MAX_PATH>{};
            // SAFETY: GetEnvironmentVariableW writes at most the passed capacity
            // and returns the character count; localAppData owns MAX_PATH
            // writable units for the duration of the call and no pointer is
            // retained.
            auto const length = GetEnvironmentVariableW(
                L"LOCALAPPDATA",
                localAppData.data(),
                static_cast<DWORD>(localAppData.size())
            );
            if (length == 0U || length >= localAppData.size())
            {
                return std::nullopt;
            }

            auto directory = std::filesystem::path{
                std::wstring_view{localAppData.data(), length}
            };
            directory /= L"UmbraFlow";
            directory /= L"workbench";

            auto createError = std::error_code{};
            std::filesystem::create_directories(directory, createError);
            if (createError)
            {
                return std::nullopt;
            }
            return directory / L"imgui-layout.ini";
        }
    }

    struct GuiShellState final
    {
        HWND      m_window{nullptr};
        HINSTANCE m_instance{nullptr};
        ATOM      m_windowClass{0};

        winrt::com_ptr<ID3D11Device>           m_device{};
        winrt::com_ptr<ID3D11DeviceContext>    m_context{};
        winrt::com_ptr<IDXGISwapChain>         m_swapChain{};
        winrt::com_ptr<ID3D11RenderTargetView> m_renderTarget{};

        std::optional<uint32> m_smokeFrames{};

        // Backing storage for ImGui's IniFilename, which ImGui stores as a bare
        // pointer rather than copying. It must outlive the context and must not
        // be written again once handed over, so it is set once during create and
        // destroyed with this object -- after the destructor body has torn the
        // context down. Empty when the layout is not being persisted.
        std::string m_layoutSettingsPath{};

        // A queued client-area size, set from WM_SIZE and applied before the next
        // frame. Zero in either field means no resize is pending.
        uint32 m_pendingWidth{0};
        uint32 m_pendingHeight{0};

        bool m_imguiContext{false};
        bool m_win32Backend{false};
        bool m_dx11Backend{false};

        std::optional<TextureCache> m_textures{};

        GuiShellState() noexcept = default;
        GuiShellState(GuiShellState const&) = delete;
        auto operator=(GuiShellState const&) -> GuiShellState& = delete;
        GuiShellState(GuiShellState&&) = delete;
        auto operator=(GuiShellState&&) -> GuiShellState& = delete;

        ~GuiShellState()
        {
            if (m_dx11Backend)
            {
                ImGui_ImplDX11_Shutdown();
            }
            if (m_win32Backend)
            {
                ImGui_ImplWin32_Shutdown();
            }
            if (m_imguiContext)
            {
                // ImGui flushes settings on a timer inside NewFrame and not at
                // shutdown, so a layout rearranged in the last few seconds before
                // closing would otherwise be lost.
                if (!m_layoutSettingsPath.empty())
                {
                    ImGui::SaveIniSettingsToDisk(m_layoutSettingsPath.c_str());
                }
                ImGui::DestroyContext();
            }
            if (m_window != nullptr)
            {
                // SAFETY: m_window is the single top-level window this object
                // created and still owns; DestroyWindow consumes it once.
                static_cast<void>(DestroyWindow(m_window));
            }
            if (m_windowClass != 0 && m_instance != nullptr)
            {
                // SAFETY: the class was registered against m_instance under this
                // name and no window of it survives the DestroyWindow above.
                static_cast<void>(
                    UnregisterClassW(k_windowClassName.data(), m_instance)
                );
            }
        }
    };

    namespace
    {
        [[nodiscard]]
        auto shellFailure(std::string message) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::UnsupportedCapability,
                std::move(message)
            );
        }

        [[nodiscard]]
        auto shellHresult(
            std::string_view operation,
            HRESULT hr
        ) -> std::unexpected<Error>
        {
            auto const nativeCode = systemErrorCode(static_cast<DWORD>(hr));
            return fail(
                AutomationErrorKind::UnsupportedCapability,
                std::format("workbench GUI shell failed to {}", operation),
                nativeCode
            );
        }

        [[nodiscard]]
        auto createRenderTarget(
            ID3D11Device& device,
            IDXGISwapChain& swapChain
        ) -> Result<winrt::com_ptr<ID3D11RenderTargetView>>
        {
            auto backBuffer = winrt::com_ptr<ID3D11Texture2D>{};
            // SAFETY: backBuffer is empty, so put_void receives ownership of the
            // buffer interface the swap chain returns for the call's duration.
            auto const bufferResult = swapChain.GetBuffer(
                0U,
                winrt::guid_of<ID3D11Texture2D>(),
                backBuffer.put_void()
            );
            if (FAILED(bufferResult) || !backBuffer)
            {
                return shellHresult("acquire the swap-chain back buffer", bufferResult);
            }

            auto renderTarget = winrt::com_ptr<ID3D11RenderTargetView>{};
            // SAFETY: backBuffer is a live texture and renderTarget is empty; the
            // device writes the new view into renderTarget.put().
            auto const viewResult = device.CreateRenderTargetView(
                backBuffer.get(),
                nullptr,
                renderTarget.put()
            );
            if (FAILED(viewResult) || !renderTarget)
            {
                return shellHresult("create the render target view", viewResult);
            }

            return renderTarget;
        }

        LRESULT CALLBACK guiShellWndProc(
            HWND window,
            UINT message,
            WPARAM wParam,
            LPARAM lParam
        )
        {
            if (ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam))
            {
                return 1;
            }

            // SAFETY: GWLP_USERDATA holds the GuiShellState that owns this window;
            // it outlives the window because the owner destroys the window before
            // freeing the state, and is null only before create() publishes it.
            auto* p_state = reinterpret_cast<GuiShellState*>(
                GetWindowLongPtrW(window, GWLP_USERDATA)
            );

            switch (message)
            {
            case WM_SIZE:
                if (p_state != nullptr && wParam != SIZE_MINIMIZED)
                {
                    p_state->m_pendingWidth  = static_cast<uint32>(LOWORD(lParam));
                    p_state->m_pendingHeight = static_cast<uint32>(HIWORD(lParam));
                }
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            default:
                break;
            }
            return DefWindowProcW(window, message, wParam, lParam);
        }

        [[nodiscard]]
        auto widenAscii(std::string const& text) -> std::wstring
        {
            auto wide = std::wstring{};
            wide.reserve(text.size());
            for (auto const character : text)
            {
                wide.push_back(
                    static_cast<wchar_t>(static_cast<unsigned char>(character))
                );
            }
            return wide;
        }
    }

    GuiShell::GuiShell(std::unique_ptr<GuiShellState> state) noexcept
        : m_state{std::move(state)}
    {
    }

    GuiShell::GuiShell(GuiShell&&) noexcept = default;
    auto GuiShell::operator=(GuiShell&&) noexcept -> GuiShell& = default;
    GuiShell::~GuiShell() = default;

    auto GuiShell::create(GuiShellConfig const& config) -> Result<GuiShell>
    {
        auto state = std::make_unique<GuiShellState>();
        state->m_smokeFrames = config.m_smokeFrames;

        // SAFETY: GetModuleHandleW(nullptr) returns the process image base; it
        // owns no resource and needs no release.
        state->m_instance = GetModuleHandleW(nullptr);

        auto windowClass   = WNDCLASSEXW{};
        windowClass.cbSize = sizeof(WNDCLASSEXW);
        windowClass.style         = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc   = guiShellWndProc;
        windowClass.hInstance     = state->m_instance;
        windowClass.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = k_windowClassName.data();

        // SAFETY: windowClass is fully initialized and lives for this call; the
        // atom it returns is recorded so the destructor can unregister the class.
        state->m_windowClass = RegisterClassExW(&windowClass);
        if (state->m_windowClass == 0)
        {
            return shellHresult(
                "register the window class",
                static_cast<HRESULT>(GetLastError())
            );
        }

        auto const title  = widenAscii(config.m_title);
        auto const width  = checkedCast<int>(config.m_width).value_or(1280);
        auto const height = checkedCast<int>(config.m_height).value_or(720);

        // SAFETY: the class atom and instance are live; the returned window is
        // recorded in state and torn down by the destructor.
        state->m_window = CreateWindowExW(
            0,
            k_windowClassName.data(),
            title.c_str(),
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            width,
            height,
            nullptr,
            nullptr,
            state->m_instance,
            nullptr
        );
        if (state->m_window == nullptr)
        {
            return shellHresult(
                "create the window",
                static_cast<HRESULT>(GetLastError())
            );
        }

        // SAFETY: state outlives the window, so publishing its address for the
        // window procedure to read back cannot dangle.
        auto const userData = reinterpret_cast<LONG_PTR>(state.get());
        SetWindowLongPtrW(state->m_window, GWLP_USERDATA, userData);

        auto swapChainDesc                        = DXGI_SWAP_CHAIN_DESC{};
        swapChainDesc.BufferCount                 = k_swapChainBufferCount;
        swapChainDesc.BufferDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.BufferUsage                 = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.OutputWindow                = state->m_window;
        swapChainDesc.SampleDesc.Count = 1U;
        swapChainDesc.Windowed         = TRUE;
        swapChainDesc.SwapEffect                  = DXGI_SWAP_EFFECT_DISCARD;

        auto const featureLevels = std::array{
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_0,
        };
        auto const driverTypes = std::array{
            D3D_DRIVER_TYPE_HARDWARE,
            D3D_DRIVER_TYPE_WARP,
        };

        auto deviceResult   = HRESULT{E_FAIL};
        auto selectedLevel  = D3D_FEATURE_LEVEL{};
        for (auto const driverType : driverTypes)
        {
            // SAFETY: the descriptor and feature-level array are live for the
            // call, which writes the created device, context, and swap chain into
            // the empty com_ptr slots below.
            deviceResult = D3D11CreateDeviceAndSwapChain(
                nullptr,
                driverType,
                nullptr,
                0U,
                featureLevels.data(),
                static_cast<UINT>(featureLevels.size()),
                D3D11_SDK_VERSION,
                &swapChainDesc,
                state->m_swapChain.put(),
                state->m_device.put(),
                &selectedLevel,
                state->m_context.put()
            );
            if (SUCCEEDED(deviceResult))
            {
                break;
            }
            state->m_swapChain = nullptr;
            state->m_device    = nullptr;
            state->m_context   = nullptr;
        }
        if (FAILED(deviceResult) || !state->m_device || !state->m_swapChain)
        {
            return shellHresult("create the Direct3D 11 device", deviceResult);
        }

        UF_TRY_VALUE(
            renderTarget,
            createRenderTarget(*state->m_device, *state->m_swapChain)
        );
        state->m_renderTarget = std::move(renderTarget);

        // SAFETY: the window handle is live; showing and updating it retains no
        // pointer and only queues paint work.
        static_cast<void>(ShowWindow(state->m_window, SW_SHOWDEFAULT));
        static_cast<void>(UpdateWindow(state->m_window));

        if (ImGui::CreateContext() == nullptr)
        {
            return shellFailure("workbench GUI shell failed to create the ImGui context");
        }
        state->m_imguiContext = true;

        // Persist the docking layout between launches, so the panels come back
        // where they were left rather than floating. A smoke run is excluded: it
        // draws a handful of frames with no layout of its own and would overwrite
        // the arrangement the user actually made.
        auto const settingsPath = state->m_smokeFrames.has_value()
            ? std::nullopt
            : layoutSettingsPath();
        if (settingsPath.has_value())
        {
            state->m_layoutSettingsPath = settingsPath->string();
            ImGui::GetIO().IniFilename  = state->m_layoutSettingsPath.c_str();
        }
        else
        {
            ImGui::GetIO().IniFilename = nullptr;
        }
        // Enable docking so the panels can be arranged against each other; the
        // dock host is submitted each frame in drawWorkbench. The vendored ImGui
        // is the docking branch, so the flag and DockSpaceOverViewport are
        // available.
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        ImGui::StyleColorsDark();

        // Load a CJK-capable system font so CJK window titles render as text
        // instead of missing-glyph boxes; ImGui's built-in font is ASCII-only.
        // Existence is checked first because ImGui asserts (in debug builds) on a
        // font file it cannot read; when the font is absent the built-in font is
        // kept.
        {
            auto systemRoot = std::array<wchar_t, MAX_PATH>{};
            // SAFETY: GetWindowsDirectoryW writes at most the passed capacity and
            // returns the character count; systemRoot owns MAX_PATH writable units
            // for the duration of the call and no pointer is retained.
            auto const rootLength = GetWindowsDirectoryW(
                systemRoot.data(),
                static_cast<UINT>(systemRoot.size())
            );
            if (rootLength != 0U && rootLength < systemRoot.size())
            {
                auto fontPath = std::filesystem::path{
                    std::wstring_view{systemRoot.data(), rootLength}
                };
                fontPath /= L"Fonts";
                fontPath /= L"msyh.ttc";
                auto existsError = std::error_code{};
                if (std::filesystem::exists(fontPath, existsError))
                {
                    auto const narrowPath = fontPath.string();
                    auto& fonts           = *ImGui::GetIO().Fonts;
                    static_cast<void>(fonts.AddFontFromFileTTF(
                        narrowPath.c_str(),
                        18.0F,
                        nullptr,
                        fonts.GetGlyphRangesChineseFull()
                    ));
                }
            }
        }

        if (!ImGui_ImplWin32_Init(state->m_window))
        {
            return shellFailure("workbench GUI shell failed to initialize the Win32 backend");
        }
        state->m_win32Backend = true;

        if (!ImGui_ImplDX11_Init(state->m_device.get(), state->m_context.get()))
        {
            return shellFailure("workbench GUI shell failed to initialize the Direct3D 11 backend");
        }
        state->m_dx11Backend = true;

        UF_TRY_VALUE(textures, TextureCache::create(*state->m_device));
        state->m_textures = std::move(textures);

        return GuiShell{std::move(state)};
    }

    auto GuiShell::textures() noexcept -> TextureCache&
    {
        return *m_state->m_textures;
    }

    auto GuiShell::run(GuiFrameCallback const& callback) -> Status
    {
        auto& shell = *m_state;
        auto renderedFrames = uint32{0};
        auto quit           = false;

        while (!quit)
        {
            auto message = MSG{};
            // SAFETY: message is writable for the call; PeekMessageW drains the
            // queue without retaining a pointer to it.
            while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE) != FALSE)
            {
                static_cast<void>(TranslateMessage(&message));
                static_cast<void>(DispatchMessageW(&message));
                if (message.message == WM_QUIT)
                {
                    quit = true;
                }
            }
            if (quit)
            {
                break;
            }

            if (shell.m_pendingWidth != 0U && shell.m_pendingHeight != 0U)
            {
                // SAFETY: DXGI ResizeBuffers requires every outstanding reference
                // to the swap chain's back buffers to be released first. The
                // render target view is still bound to the output-merger stage
                // from the previous frame's draw, a deferred reference that
                // resetting the com_ptr alone does not drop. Unbind all targets,
                // then ClearState to flush any other deferred back-buffer
                // references the immediate context holds, then release our own
                // view before resizing; the next frame's ImGui pass re-binds the
                // whole pipeline. The device, context, and swap chain stay owned
                // by shell throughout.
                shell.m_context->OMSetRenderTargets(0U, nullptr, nullptr);
                shell.m_context->ClearState();
                shell.m_renderTarget = nullptr;
                auto const resizeResult = shell.m_swapChain->ResizeBuffers(
                    0U,
                    shell.m_pendingWidth,
                    shell.m_pendingHeight,
                    DXGI_FORMAT_UNKNOWN,
                    0U
                );
                if (FAILED(resizeResult))
                {
                    return shellHresult("resize the swap chain", resizeResult);
                }
                UF_TRY_VALUE(
                    renderTarget,
                    createRenderTarget(*shell.m_device, *shell.m_swapChain)
                );
                shell.m_renderTarget  = std::move(renderTarget);
                shell.m_pendingWidth  = 0U;
                shell.m_pendingHeight = 0U;
            }

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            callback();

            ImGui::Render();

            auto const clearColor = std::array{0.10F, 0.10F, 0.12F, 1.0F};
            auto* p_renderTarget = shell.m_renderTarget.get();
            // SAFETY: p_renderTarget is the live target owned by shell; the
            // context borrows it for this synchronous binding and clear.
            shell.m_context->OMSetRenderTargets(1U, &p_renderTarget, nullptr);
            shell.m_context->ClearRenderTargetView(
                p_renderTarget,
                clearColor.data()
            );
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

            // SAFETY: the swap chain is live; Present schedules the frame and
            // retains no caller pointer.
            static_cast<void>(shell.m_swapChain->Present(1U, 0U));

            ++renderedFrames;
            if (
                shell.m_smokeFrames.has_value()
                && renderedFrames >= *shell.m_smokeFrames
            )
            {
                quit = true;
            }
        }

        return ok();
    }
}
