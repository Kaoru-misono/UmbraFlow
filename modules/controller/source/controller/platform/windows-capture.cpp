#include "controller/capture.hpp"

#include "controller/detail/capture-d3d.hpp"
#include "controller/detail/capture-os-build.hpp"
#include "controller/detail/capture-stall.hpp"
#include "controller/detail/capture-wgc.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>
#include <core/utility/scope-exit.hpp>
#include <domain/error.hpp>

#pragma warning(push, 0)
#include <Windows.h>

#include <d3d11.h>
#include <d3d11_4.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <inspectable.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/base.h>
#pragma warning(pop)

#include <atomic>
#include <bit>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

extern "C" NTSYSAPI auto NTAPI RtlGetVersion(
    PRTL_OSVERSIONINFOW p_versionInformation
) -> LONG;

namespace uf
{
    namespace
    {
        using D3d11DeviceComPtr = winrt::com_ptr<ID3D11Device>;
        using D3d11ContextComPtr = winrt::com_ptr<ID3D11DeviceContext>;
        using D3d11TextureComPtr = winrt::com_ptr<ID3D11Texture2D>;
        using CaptureFrame = winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame;
        using CaptureFramePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
        using CaptureFramePoolStatics =
            winrt::Windows::Graphics::Capture::IDirect3D11CaptureFramePoolStatics2;
        using CaptureItem = winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
        using CaptureSession = winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
        using CaptureSessionStatics =
            winrt::Windows::Graphics::Capture::IGraphicsCaptureSessionStatics;
        using Direct3DDevice =
            winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
        using DxgiInterfaceAccess =
            ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;
        using PixelFormat = winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
        constexpr auto k_framePoolBufferCount = int32{2};
        constexpr auto k_capturePixelFormat = PixelFormat::B8G8R8A8UIntNormalized;

        [[nodiscard]]
        auto windowMarkerSequence() noexcept -> std::atomic_uint64_t&
        {
            static auto s_sequence = std::atomic_uint64_t{1U};
            return s_sequence;
        }

        [[nodiscard]]
        auto captureUnavailable(std::string message) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::CaptureUnavailable,
                std::move(message)
            );
        }

        [[nodiscard]]
        auto captureHresult(
            std::string_view context,
            HRESULT result
        ) -> std::unexpected<Error>
        {
            return captureUnavailable(
                std::format(
                    "{}: HRESULT {:#010x}",
                    context,
                    static_cast<uint32>(result)
                )
            );
        }

        template <typename Function>
            requires std::invocable<Function&>
        [[nodiscard]]
        auto winrtCall(
            std::string_view context,
            Function&& function
        ) -> Result<std::invoke_result_t<Function&>>
        {
            using Return = std::invoke_result_t<Function&>;

            try
            {
                if constexpr (std::is_void_v<Return>)
                {
                    std::invoke(function);
                    return ok();
                }
                else
                {
                    return std::invoke(function);
                }
            }
            catch (winrt::hresult_error const& error)
            {
                return captureHresult(
                    context,
                    static_cast<HRESULT>(error.code())
                );
            }
        }

        [[nodiscard]]
        auto toNativeHandle(WindowHandle handle) noexcept -> HWND
        {
            // SAFETY: WindowHandle stores the pointer-sized integer representation copied
            // from an HWND. bit_cast restores those exact bits as the opaque token without
            // dereferencing it.
            return std::bit_cast<HWND>(handle.value());
        }

        struct D3d11Objects final
        {
            D3d11DeviceComPtr  m_device{};
            D3d11ContextComPtr m_context{};
        };

        struct D3dDevice final
        {
            D3d11DeviceComPtr  m_device{};
            D3d11ContextComPtr m_context{};
            Direct3DDevice     m_runtimeDevice{};
        };

        [[nodiscard]]
        auto createD3d11(D3D_DRIVER_TYPE driverType) -> Result<D3d11Objects>
        {
            auto device  = winrt::com_ptr<ID3D11Device>{};
            auto context = winrt::com_ptr<ID3D11DeviceContext>{};
            // SAFETY: The out-parameters receive owned device/context interfaces. A null
            // adapter selects the default adapter, and no feature-level array is supplied.
            auto const result = D3D11CreateDevice(
                nullptr,
                driverType,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                nullptr,
                0,
                D3D11_SDK_VERSION,
                device.put(),
                nullptr,
                context.put()
            );
            if (FAILED(result))
            {
                return captureHresult("D3D11CreateDevice", result);
            }
            if (!device)
            {
                return captureUnavailable("D3D11CreateDevice returned no device");
            }
            if (!context)
            {
                return captureUnavailable("D3D11CreateDevice returned no context");
            }

            UF_TRY_VALUE(
                multithread,
                winrtCall(
                    "ID3D11DeviceContext as ID3D11Multithread",
                    [&context]
                    {
                        return context.as<ID3D11Multithread>();
                    }
                )
            );
            if (!multithread)
            {
                return captureUnavailable(
                    "ID3D11DeviceContext returned no ID3D11Multithread"
                );
            }
            // SAFETY: The capture consumer and free-threaded WGC frame pool can use this
            // D3D11 device concurrently. Driver serialization is behavior-preserving
            // hardening beyond the Rust reference and protects immediate-context access.
            multithread->SetMultithreadProtected(TRUE);

            return D3d11Objects{std::move(device), std::move(context)};
        }

        [[nodiscard]]
        auto createD3dDevice() -> Result<D3dDevice>
        {
            auto created = createD3d11(D3D_DRIVER_TYPE_HARDWARE);
            if (!created)
            {
                created = createD3d11(D3D_DRIVER_TYPE_WARP);
            }
            if (!created)
            {
                return std::unexpected{std::move(created).error()};
            }
            auto native = *std::move(created);

            UF_TRY_VALUE(
                dxgiDevice,
                winrtCall(
                    "ID3D11Device as IDXGIDevice",
                    [&native]
                    {
                        return native.m_device.as<IDXGIDevice>();
                    }
                )
            );

            auto inspectable = winrt::com_ptr<IInspectable>{};
            // SAFETY: dxgiDevice is a live IDXGIDevice. The ABI helper returns a newly
            // referenced WinRT device through the owned IInspectable out-parameter.
            auto const wrapped = CreateDirect3D11DeviceFromDXGIDevice(
                dxgiDevice.get(),
                inspectable.put()
            );
            if (FAILED(wrapped))
            {
                return captureHresult("CreateDirect3D11DeviceFromDXGIDevice", wrapped);
            }
            if (!inspectable)
            {
                return captureUnavailable(
                    "CreateDirect3D11DeviceFromDXGIDevice returned no device"
                );
            }

            UF_TRY_VALUE(
                runtimeDevice,
                winrtCall(
                    "IInspectable as IDirect3DDevice",
                    [&inspectable]
                    {
                        return inspectable.as<Direct3DDevice>();
                    }
                )
            );
            if (!runtimeDevice)
            {
                return captureUnavailable(
                    "IInspectable returned no IDirect3DDevice"
                );
            }

            return D3dDevice{
                std::move(native.m_device),
                std::move(native.m_context),
                std::move(runtimeDevice)
            };
        }

        class StagingTexture final
        {
            D3d11TextureComPtr m_texture{};
            uint32             m_width{};
            uint32             m_height{};
            DXGI_FORMAT m_format{DXGI_FORMAT_UNKNOWN};

        public:
            [[nodiscard]]
            auto getOrCreate(
                ID3D11Device& device,
                D3D11_TEXTURE2D_DESC const& sourceDescription
            ) -> Result<winrt::com_ptr<ID3D11Texture2D>>
            {
                auto const reusable = (
                    m_texture
                    && m_width == sourceDescription.Width
                    && m_height == sourceDescription.Height
                    && m_format == sourceDescription.Format
                );
                if (!reusable)
                {
                    auto stagingDescription = sourceDescription;
                    stagingDescription.Usage = D3D11_USAGE_STAGING;
                    stagingDescription.BindFlags = 0;
                    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    stagingDescription.MiscFlags = 0;

                    auto staging = winrt::com_ptr<ID3D11Texture2D>{};
                    // SAFETY: The descriptor requests an owned CPU-readable staging texture;
                    // staging receives that interface and no subresource data is read.
                    auto const created = device.CreateTexture2D(
                        &stagingDescription,
                        nullptr,
                        staging.put()
                    );
                    if (FAILED(created))
                    {
                        return captureHresult("CreateTexture2D staging", created);
                    }
                    if (!staging)
                    {
                        return captureUnavailable(
                            "CreateTexture2D returned no staging texture"
                        );
                    }

                    m_texture = std::move(staging);
                    m_width   = sourceDescription.Width;
                    m_height  = sourceDescription.Height;
                    m_format  = sourceDescription.Format;
                }

                if (!m_texture)
                {
                    return captureUnavailable("staging texture missing after creation");
                }
                return m_texture;
            }
        };

        struct SurfaceReadback final
        {
            uint32                 m_sourceWidth{};
            uint32                 m_sourceHeight{};
            std::vector<std::byte> m_pixels{};
        };

        [[nodiscard]]
        auto readbackSurface(
            StagingTexture& stagingCache,
            ID3D11Device& device,
            ID3D11DeviceContext& context,
            CaptureFrame const& frame,
            controller_detail::ClientCropRect const& crop
        ) -> Result<SurfaceReadback>
        {
            UF_TRY_VALUE(
                surface,
                winrtCall(
                    "Direct3D11CaptureFrame::Surface",
                    [&frame]
                    {
                        return frame.Surface();
                    }
                )
            );
            UF_TRY_VALUE(
                access,
                winrtCall(
                    "Surface as IDirect3DDxgiInterfaceAccess",
                    [&surface]
                    {
                        return surface.as<DxgiInterfaceAccess>();
                    }
                )
            );

            auto texture = winrt::com_ptr<ID3D11Texture2D>{};
            // SAFETY: access owns the captured surface's DXGI object. GetInterface returns
            // an added ID3D11Texture2D reference through texture's out-parameter.
            auto const textureResult = access->GetInterface(
                __uuidof(ID3D11Texture2D),
                texture.put_void()
            );
            if (FAILED(textureResult))
            {
                return captureHresult("GetInterface ID3D11Texture2D", textureResult);
            }
            if (!texture)
            {
                return captureUnavailable("captured surface returned no ID3D11Texture2D");
            }

            auto sourceDescription = D3D11_TEXTURE2D_DESC{};
            // SAFETY: texture is a live ID3D11Texture2D and sourceDescription is a writable
            // local that receives its descriptor for the duration of this call.
            texture->GetDesc(&sourceDescription);
            auto const sourceWidth = sourceDescription.Width;
            auto const sourceHeight = sourceDescription.Height;
            UF_TRY(crop.ensureWithinSource(sourceWidth, sourceHeight));

            auto stagingDescription   = sourceDescription;
            stagingDescription.Width  = crop.width();
            stagingDescription.Height = crop.height();
            UF_TRY_VALUE(
                staging,
                stagingCache.getOrCreate(device, stagingDescription)
            );

            auto const sourceBox = D3D11_BOX{
                .left = crop.offsetX(),
                .top = crop.offsetY(),
                .front = 0,
                .right = crop.right(),
                .bottom = crop.bottom(),
                .back = 1,
            };
            // SAFETY: crop validation proves sourceBox is inside texture, and staging is
            // client-sized with the same format, so the copy cannot exceed either resource.
            context.CopySubresourceRegion(
                staging.get(),
                0,
                0,
                0,
                0,
                texture.get(),
                0,
                &sourceBox
            );

            auto mapped = D3D11_MAPPED_SUBRESOURCE{};
            // SAFETY: staging was created with CPU read access. Map initializes mapped and
            // its pointer remains valid until the paired Unmap scope guard executes.
            auto const mappedResult = context.Map(
                staging.get(),
                0,
                D3D11_MAP_READ,
                0,
                &mapped
            );
            if (FAILED(mappedResult))
            {
                return captureHresult("Map staging", mappedResult);
            }
            auto unmap = scopeExit(
                [&context, &staging]() noexcept
                {
                    // SAFETY: This exactly pairs the successful Map above while staging and
                    // context remain alive, so no mapping escapes the function.
                    context.Unmap(staging.get(), 0);
                }
            );

            if (mapped.pData == nullptr)
            {
                return captureUnavailable("mapped staging pointer was null");
            }
            auto const rowPitch = static_cast<std::size_t>(mapped.RowPitch);
            auto const heightSize = checkedCast<std::size_t>(crop.height());
            if (!heightSize)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "mapped staging height does not fit size_t"
                );
            }
            auto const mappedLength = checkedMultiply(rowPitch, *heightSize);
            if (!mappedLength)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "mapped staging extent overflow"
                );
            }

            // SAFETY: A successful Map exposes at least RowPitch * Height readable bytes.
            // The span is consumed before Unmap runs and never escapes this function.
            auto const mappedBytes = std::span<std::byte const>{
                static_cast<std::byte const*>(mapped.pData),
                *mappedLength
            };
            UF_TRY_VALUE(
                pixels,
                controller_detail::readbackBgra8(
                    mappedBytes,
                    rowPitch,
                    crop.width(),
                    crop.height()
                )
            );

            return SurfaceReadback{
                sourceWidth,
                sourceHeight,
                std::move(pixels)
            };
        }

        [[nodiscard]]
        auto currentOsBuild() -> Result<uint32>
        {
            auto information = RTL_OSVERSIONINFOW{};
            auto const informationSize = checkedCast<ULONG>(sizeof(information));
            if (!informationSize)
            {
                return captureUnavailable("RTL_OSVERSIONINFOW size does not fit ULONG");
            }
            information.dwOSVersionInfoSize = *informationSize;

            // SAFETY: information is fully initialized with its exact size. RtlGetVersion
            // writes only that live structure, retains no pointer, and returns NTSTATUS.
            auto const status = RtlGetVersion(&information);
            if (status != 0)
            {
                return captureUnavailable(
                    std::format("RtlGetVersion failed with status {}", status)
                );
            }

            return static_cast<uint32>(information.dwBuildNumber);
        }

        [[nodiscard]]
        auto clientOriginOnDesktop(HWND windowHandle) -> Result<POINT>
        {
            auto clientOrigin = POINT{.x = 0, .y = 0};
            // SAFETY: ClientToScreen translates the live in/out clientOrigin point for the
            // observed target HWND and retains neither the handle nor the pointer.
            if (ClientToScreen(windowHandle, &clientOrigin) == FALSE)
            {
                return captureUnavailable(
                    "ClientToScreen failed to translate the client origin"
                );
            }
            return clientOrigin;
        }

        // WGC CreateForWindow captures DWM-composed non-client chrome. Resolve the client's
        // frame-relative origin from ClientToScreen(0, 0) minus the visible DWM extended-frame
        // top-left. GetWindowRect is deliberately not used because its invisible resize border
        // would introduce a monitor/DPI-dependent crop and click offset.
        [[nodiscard]]
        auto resolveClientCrop(
            HWND windowHandle,
            uint32 frameWidth,
            uint32 frameHeight,
            ClientGeometry const& client
        ) -> Result<controller_detail::ClientCropRect>
        {
            UF_TRY_VALUE(clientOrigin, clientOriginOnDesktop(windowHandle));

            auto bounds = RECT{};
            auto const boundsSize = checkedCast<DWORD>(sizeof(bounds));
            if (!boundsSize)
            {
                return captureUnavailable("RECT size does not fit DWORD");
            }
            // SAFETY: bounds is a live writable RECT and boundsSize is its exact byte size;
            // DwmGetWindowAttribute retains neither the HWND token nor the buffer pointer.
            auto const boundsResult = DwmGetWindowAttribute(
                windowHandle,
                DWMWA_EXTENDED_FRAME_BOUNDS,
                &bounds,
                *boundsSize
            );
            if (FAILED(boundsResult))
            {
                return captureHresult(
                    "DwmGetWindowAttribute(EXTENDED_FRAME_BOUNDS)",
                    boundsResult
                );
            }

            auto const offsetX = checkedSubtract(
                static_cast<int32>(clientOrigin.x),
                static_cast<int32>(bounds.left)
            );
            if (!offsetX)
            {
                return captureUnavailable("client x offset computation overflowed");
            }
            auto const offsetY = checkedSubtract(
                static_cast<int32>(clientOrigin.y),
                static_cast<int32>(bounds.top)
            );
            if (!offsetY)
            {
                return captureUnavailable("client y offset computation overflowed");
            }
            auto const extendedWidth = checkedSubtract(
                static_cast<int32>(bounds.right),
                static_cast<int32>(bounds.left)
            );
            if (!extendedWidth)
            {
                return captureUnavailable(
                    "extended frame bounds width computation overflowed"
                );
            }
            auto const extendedHeight = checkedSubtract(
                static_cast<int32>(bounds.bottom),
                static_cast<int32>(bounds.top)
            );
            if (!extendedHeight)
            {
                return captureUnavailable(
                    "extended frame bounds height computation overflowed"
                );
            }

            UF_TRY_VALUE(
                clientExtent,
                controller_detail::clientIntegerExtent(client)
            );
            return controller_detail::ClientCropRect::create(
                {frameWidth, frameHeight},
                {*extendedWidth, *extendedHeight},
                {*offsetX, *offsetY},
                clientExtent
            );
        }

        struct CapturedArrival final
        {
            CaptureFrame     m_frame;
            MonotonicInstant m_arrivedAt;
        };

        struct FrameSlot final
        {
            // The FrameArrived callback and capture consumer share this state. Every
            // m_latest access is serialized by m_mutex; m_arrived only publishes changes
            // made while that mutex is held.
            std::mutex                     m_mutex{};
            std::condition_variable        m_arrived{};
            std::optional<CapturedArrival> m_latest{};
            bool                           m_acceptingFrames{true};
            std::atomic_bool               m_itemClosed{false};
            std::atomic<HRESULT> m_callbackFailure{S_OK};
        };

        auto recordFrameCallbackFailure(
            std::shared_ptr<FrameSlot> const& p_slot,
            HRESULT failure
        ) noexcept -> void
        {
            try
            {
                auto lock = std::lock_guard{p_slot->m_mutex};
                p_slot->m_callbackFailure.store(
                    failure,
                    std::memory_order_release
                );
            }
            catch (...)
            {
                p_slot->m_callbackFailure.store(
                    failure,
                    std::memory_order_release
                );
            }
            p_slot->m_arrived.notify_all();
        }

        class WindowInstanceMarker final
        {
            HWND          m_window;
            std::wstring  m_name;
            winrt::handle m_token;

            WindowInstanceMarker(
                HWND window,
                std::wstring name,
                winrt::handle token
            ) noexcept
                : m_window{window}
                , m_name{std::move(name)}
                , m_token{std::move(token)}
            {
            }

            auto disarm() noexcept -> void
            {
                m_window = nullptr;
            }

            auto bestEffortClose() noexcept -> void
            {
                try
                {
                    static_cast<void>(close());
                }
                catch (...)
                {
                }
            }

        public:
            WindowInstanceMarker(WindowInstanceMarker const&) = delete;
            auto operator=(WindowInstanceMarker const&) -> WindowInstanceMarker& = delete;

            WindowInstanceMarker(WindowInstanceMarker&& other) noexcept
                : m_window{std::exchange(other.m_window, nullptr)}
                , m_name{std::move(other.m_name)}
                , m_token{std::move(other.m_token)}
            {
            }

            auto operator=(WindowInstanceMarker&&) -> WindowInstanceMarker& = delete;

            ~WindowInstanceMarker() { bestEffortClose(); }

            [[nodiscard]]
            static auto create(HWND window) -> Result<WindowInstanceMarker>
            {
                auto token = winrt::handle{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
                if (!token)
                {
                    return captureUnavailable(
                        std::format(
                            "failed to allocate capture window identity token (Win32 error {})",
                            GetLastError()
                        )
                    );
                }

                auto const sequence = windowMarkerSequence().fetch_add(
                    1U,
                    std::memory_order_relaxed
                );
                if (sequence == 0U)
                {
                    return captureUnavailable(
                        "capture window identity sequence was exhausted"
                    );
                }
                auto name = std::format(
                    L"UmbraFlow.WgcCapture.{}.{}",
                    GetCurrentProcessId(),
                    sequence
                );
                if (SetPropW(window, name.c_str(), token.get()) == FALSE)
                {
                    return captureUnavailable(
                        std::format(
                            "failed to bind capture session to the target window instance (Win32 error {})",
                            GetLastError()
                        )
                    );
                }

                return WindowInstanceMarker{
                    window,
                    std::move(name),
                    std::move(token)
                };
            }

            [[nodiscard]] auto matches() const noexcept -> bool
            {
                return m_window != nullptr
                    && m_token
                    && GetPropW(m_window, m_name.c_str()) == m_token.get();
            }

            [[nodiscard]] auto window() const noexcept -> HWND { return m_window; }

            [[nodiscard]]
            auto close() -> Status
            {
                if (m_window == nullptr)
                {
                    return ok();
                }
                if (
                    !m_token
                    || m_name.empty()
                    || GetPropW(m_window, m_name.c_str()) != m_token.get()
                )
                {
                    disarm();
                    return ok();
                }

                SetLastError(ERROR_SUCCESS);
                auto const removed = RemovePropW(m_window, m_name.c_str());
                if (removed == nullptr)
                {
                    auto const error = GetLastError();
                    if (GetPropW(m_window, m_name.c_str()) != m_token.get())
                    {
                        disarm();
                        return ok();
                    }
                    return captureUnavailable(
                        std::format(
                            "failed to release capture window identity marker (Win32 error {})",
                            error
                        )
                    );
                }

                disarm();
                if (removed != m_token.get())
                {
                    return captureUnavailable(
                        "capture window identity marker changed while it was being released"
                    );
                }
                return ok();
            }
        };

        template <typename Closable>
        auto closeIgnoringErrors(Closable const& object) noexcept -> void
        {
            try
            {
                object.Close();
            }
            catch (...)
            {
            }
        }

        [[nodiscard]]
        auto clearLatestFrame(std::shared_ptr<FrameSlot> const& p_slot) -> Status
        {
            auto lock = std::lock_guard{p_slot->m_mutex};
            if (!p_slot->m_latest)
            {
                return ok();
            }

            UF_TRY(
                winrtCall(
                    "Direct3D11CaptureFrame::Close",
                    [&p_slot]
                    {
                        p_slot->m_latest->m_frame.Close();
                    }
                )
            );
            p_slot->m_latest.reset();
            return ok();
        }
    }
}

namespace uf
{
    class WgcCaptureSession::Impl final
    {
        // Serializes the consumer-side session, geometry, stall, and D3D state. The
        // callback never takes this mutex and communicates only through m_frameSlot.
        std::mutex         m_operationMutex{};
        D3d11DeviceComPtr  m_device;
        D3d11ContextComPtr m_context;
        // Retained so the projected device outlives the frame pool created from it.
        Direct3DDevice   m_runtimeDevice;
        CaptureFramePool m_framePool;
        CaptureSession   m_session;
        // Retained so the capture item outlives the session that references it.
        CaptureItem                             m_item;
        winrt::event_token                      m_frameArrivedToken;
        winrt::event_token                      m_itemClosedToken;
        std::shared_ptr<FrameSlot>              m_frameSlot;
        controller_detail::StallTracker         m_stall;
        SessionId                               m_sessionId;
        controller_detail::FrameIdCounter       m_frameIds;
        TargetGeneration                        m_targetGeneration;
        WindowInstanceMarker                    m_windowMarker;
        ClientGeometry                          m_client;
        controller_detail::ClientCropRect       m_crop;
        WgcCaptureOptions                       m_options;
        controller_detail::CaptureGeometryState m_geometry;
        StagingTexture                          m_staging;
        CaptureHygiene                          m_hygiene;
        bool                                    m_sessionOpen;
        bool                                    m_frameArrivedRegistered;
        bool                                    m_itemClosedRegistered;
        bool                                    m_framePoolOpen;
        bool                                    m_closed;

    public:
        Impl(Impl const&) = delete;
        auto operator=(Impl const&) -> Impl& = delete;
        Impl(Impl&&) = delete;
        auto operator=(Impl&&) -> Impl& = delete;

        Impl(
            D3dDevice d3d,
            CaptureFramePool framePool,
            CaptureSession session,
            CaptureItem item,
            winrt::event_token frameArrivedToken,
            winrt::event_token itemClosedToken,
            std::shared_ptr<FrameSlot> p_frameSlot,
            MonotonicInstant startedAt,
            SessionId sessionId,
            TargetGeneration targetGeneration,
            WindowInstanceMarker windowMarker,
            ClientGeometry client,
            controller_detail::ClientCropRect crop,
            WgcCaptureOptions options,
            controller_detail::CaptureGeometryState geometry,
            CaptureHygiene hygiene
        ) noexcept
            : m_device{std::move(d3d.m_device)}
            , m_context{std::move(d3d.m_context)}
            , m_runtimeDevice{std::move(d3d.m_runtimeDevice)}
            , m_framePool{std::move(framePool)}
            , m_session{std::move(session)}
            , m_item{std::move(item)}
            , m_frameArrivedToken{frameArrivedToken}
            , m_itemClosedToken{itemClosedToken}
            , m_frameSlot{std::move(p_frameSlot)}
            , m_stall{options.captureStallTimeout(), startedAt}
            , m_sessionId{sessionId}
            , m_frameIds{}
            , m_targetGeneration{targetGeneration}
            , m_windowMarker{std::move(windowMarker)}
            , m_client{client}
            , m_crop{crop}
            , m_options{options}
            , m_geometry{geometry}
            , m_staging{}
            , m_hygiene{hygiene}
            , m_sessionOpen{true}
            , m_frameArrivedRegistered{true}
            , m_itemClosedRegistered{true}
            , m_framePoolOpen{true}
            , m_closed{false}
        {
        }

    private:
        [[nodiscard]]
        auto waitForFrame(
            MonotonicInstant::Duration timeout
        ) -> Result<CapturedArrival>
        {
            auto lock = std::unique_lock{m_frameSlot->m_mutex};
            static_cast<void>(
                m_frameSlot->m_arrived.wait_for(
                    lock,
                    timeout,
                    [this]
                    {
                        return m_frameSlot->m_latest.has_value()
                            || m_frameSlot->m_itemClosed.load(
                                std::memory_order_acquire
                            )
                            || FAILED(
                                m_frameSlot->m_callbackFailure.load(
                                    std::memory_order_acquire
                                )
                            );
                    }
                )
            );

            if (m_frameSlot->m_itemClosed.load(std::memory_order_acquire))
            {
                return captureUnavailable(
                    "capture item was closed; rebuild the session from a freshly resolved target"
                );
            }

            auto const callbackFailure = m_frameSlot->m_callbackFailure.load(
                std::memory_order_acquire
            );
            if (FAILED(callbackFailure))
            {
                return captureHresult(
                    "Direct3D11CaptureFramePool::FrameArrived callback",
                    callbackFailure
                );
            }

            if (m_frameSlot->m_latest)
            {
                auto arrival = std::move(*m_frameSlot->m_latest);
                m_frameSlot->m_latest.reset();
                return arrival;
            }

            lock.unlock();
            UF_TRY(m_stall.check(MonotonicInstant::now()));
            return fail(
                AutomationErrorKind::CaptureStalled,
                std::format(
                    "no new frame within {} monotonic clock ticks",
                    timeout.count()
                )
            );
        }

        [[nodiscard]]
        auto teardownUnlocked() -> Status
        {
            m_closed = true;
            {
                auto frameLock = std::lock_guard{m_frameSlot->m_mutex};
                m_frameSlot->m_acceptingFrames = false;
            }
            auto result = ok();
            auto retainFirstError = [&result](Status next) -> void
            {
                if (result && !next)
                {
                    result = std::unexpected{std::move(next).error()};
                }
            };

            if (m_sessionOpen)
            {
                auto closed = winrtCall(
                    "GraphicsCaptureSession::Close",
                    [this]
                    {
                        m_session.Close();
                    }
                );
                if (closed)
                {
                    m_sessionOpen = false;
                }
                retainFirstError(std::move(closed));
            }
            if (m_frameArrivedRegistered)
            {
                auto revoked = winrtCall(
                    "revoke Direct3D11CaptureFramePool::FrameArrived",
                    [this]
                    {
                        m_framePool.FrameArrived(m_frameArrivedToken);
                    }
                );
                if (revoked)
                {
                    m_frameArrivedRegistered = false;
                }
                retainFirstError(std::move(revoked));
            }
            if (m_itemClosedRegistered)
            {
                auto revoked = winrtCall(
                    "revoke GraphicsCaptureItem::Closed",
                    [this]
                    {
                        m_item.Closed(m_itemClosedToken);
                    }
                );
                if (revoked)
                {
                    m_itemClosedRegistered = false;
                }
                retainFirstError(std::move(revoked));
            }
            if (m_framePoolOpen)
            {
                auto closed = winrtCall(
                    "Direct3D11CaptureFramePool::Close",
                    [this]
                    {
                        m_framePool.Close();
                    }
                );
                if (closed)
                {
                    m_framePoolOpen = false;
                }
                retainFirstError(std::move(closed));
            }
            retainFirstError(clearLatestFrame(m_frameSlot));
            retainFirstError(m_windowMarker.close());
            return result;
        }

        [[nodiscard]]
        auto validateTargetInstanceUnlocked() -> Status
        {
            if (m_frameSlot->m_itemClosed.load(std::memory_order_acquire))
            {
                return captureUnavailable(
                    "capture item was closed; rebuild the session from a freshly resolved target"
                );
            }

            if (!m_windowMarker.matches())
            {
                return captureUnavailable(
                    "capture target window identity changed; rebuild the capture session"
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto currentClientOrigin() -> Result<POINT>
        {
            UF_TRY(validateTargetInstanceUnlocked());

            auto const nativeWindow = m_windowMarker.window();
            UF_TRY_VALUE(origin, clientOriginOnDesktop(nativeWindow));
            if (m_frameSlot->m_itemClosed.load(std::memory_order_acquire))
            {
                return captureUnavailable(
                    "capture item closed while refreshing client position; rebuild the capture session"
                );
            }

            if (!m_windowMarker.matches())
            {
                return captureUnavailable(
                    "capture target window identity changed while refreshing client position"
                );
            }
            return origin;
        }

    public:
        [[nodiscard]]
        static auto create(
            WindowHandle windowHandle,
            SessionId sessionId,
            TargetGeneration targetGeneration,
            ClientGeometry client,
            WgcCaptureOptions options
        ) -> Result<std::unique_ptr<Impl>>
        {
            auto const nativeWindow = toNativeHandle(windowHandle);
            UF_TRY_VALUE(
                windowMarker,
                WindowInstanceMarker::create(nativeWindow)
            );

            UF_TRY_VALUE(
                captureSessionStatics,
                winrtCall(
                    "GraphicsCaptureSession activation factory",
                    []
                    {
                        return winrt::get_activation_factory<
                            CaptureSession,
                            CaptureSessionStatics
                        >();
                    }
                )
            );
            if (!captureSessionStatics)
            {
                return captureUnavailable(
                    "GraphicsCaptureSession returned no activation factory"
                );
            }
            UF_TRY_VALUE(
                supported,
                winrtCall(
                    "GraphicsCaptureSession::IsSupported",
                    [&captureSessionStatics]
                    {
                        // get_activation_factory returned this checked non-null
                        // projection; Clang cannot model its generated ABI storage.
                        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
                        return captureSessionStatics.IsSupported();
                    }
                )
            );
            if (!supported)
            {
                return captureUnavailable(
                    "Windows Graphics Capture is not supported on this system"
                );
            }

            UF_TRY_VALUE(osBuild, currentOsBuild());
            if (!controller_detail::cursorCaptureSupported(osBuild))
            {
                return captureUnavailable(
                    std::format(
                        "cursor capture cannot be disabled on OS build {}; a clean frame requires build {}+",
                        osBuild,
                        controller_detail::k_cursorCaptureMinBuild
                    )
                );
            }
            auto const hasBorderlessSupport =
                controller_detail::borderlessSupported(osBuild);
            if (options.requireBorderless())
            {
                return captureUnavailable(
                    std::format(
                        "borderless capture requested but not grantable at this stage (OS build {}, borderless_supported={})",
                        osBuild,
                        hasBorderlessSupport
                    )
                );
            }

            UF_TRY_VALUE(d3d, createD3dDevice());
            UF_TRY_VALUE(
                interop,
                winrtCall(
                    "GraphicsCaptureItem interop factory",
                    []
                    {
                        return winrt::get_activation_factory<
                            CaptureItem,
                            IGraphicsCaptureItemInterop
                        >();
                    }
                )
            );

            auto item = CaptureItem{nullptr};
            // SAFETY: get_activation_factory is the C++/WinRT activation path. The
            // interop call consumes only the target HWND token and initializes item's
            // projected ABI slot with an owned GraphicsCaptureItem reference.
            auto const itemResult = interop->CreateForWindow(
                nativeWindow,
                winrt::guid_of<CaptureItem>(),
                winrt::put_abi(item)
            );
            if (FAILED(itemResult))
            {
                return captureHresult("CreateForWindow", itemResult);
            }
            if (!item)
            {
                return captureUnavailable("CreateForWindow returned no capture item");
            }

            auto frameSlot             = std::make_shared<FrameSlot>();
            auto slotForClosedCallback = frameSlot;
            auto const itemClosedHandler = winrt::Windows::Foundation::TypedEventHandler<
                CaptureItem,
                winrt::Windows::Foundation::IInspectable
            >{
                [p_slot = std::move(slotForClosedCallback)](
                    CaptureItem const&,
                    winrt::Windows::Foundation::IInspectable const&
                ) noexcept
                {
                    try
                    {
                        // Update the wait predicate while holding the same mutex used
                        // by waitForFrame, so notification cannot be lost between its
                        // predicate check and transition to the waiting state.
                        auto lock = std::lock_guard{p_slot->m_mutex};
                        p_slot->m_itemClosed.store(
                            true,
                            std::memory_order_release
                        );
                    }
                    catch (...)
                    {
                        p_slot->m_itemClosed.store(
                            true,
                            std::memory_order_release
                        );
                    }
                    p_slot->m_arrived.notify_all();
                }
            };
            UF_TRY_VALUE(
                itemClosedToken,
                winrtCall(
                    "GraphicsCaptureItem::Closed",
                    [&item, &itemClosedHandler]
                    {
                        return item.Closed(itemClosedHandler);
                    }
                )
            );
            auto itemClosedCleanup = scopeExit(
                [&item, itemClosedToken]() noexcept
                {
                    try
                    {
                        item.Closed(itemClosedToken);
                    }
                    catch (...)
                    {
                    }
                }
            );

            if (
                !windowMarker.matches()
                || frameSlot->m_itemClosed.load(std::memory_order_acquire)
            )
            {
                return captureUnavailable(
                    "capture target changed while its capture item was being created"
                );
            }

            UF_TRY_VALUE(
                poolSize,
                winrtCall(
                    "GraphicsCaptureItem::Size",
                    [&item]
                    {
                        return item.Size();
                    }
                )
            );
            UF_TRY_VALUE(
                geometry,
                controller_detail::CaptureGeometryState::create(
                    controller_detail::CaptureSize{
                        poolSize.Width,
                        poolSize.Height,
                    }
                )
            );
            auto const [frameWidth, frameHeight] = geometry.expectedSize();
            UF_TRY_VALUE(
                crop,
                resolveClientCrop(
                    nativeWindow,
                    frameWidth,
                    frameHeight,
                    client
                )
            );
            if (!windowMarker.matches())
            {
                return captureUnavailable(
                    "capture target changed while resolving its client geometry"
                );
            }

            UF_TRY_VALUE(
                framePoolStatics,
                winrtCall(
                    "Direct3D11CaptureFramePool activation factory",
                    []
                    {
                        return winrt::get_activation_factory<
                            CaptureFramePool,
                            CaptureFramePoolStatics
                        >();
                    }
                )
            );
            if (!framePoolStatics)
            {
                return captureUnavailable(
                    "Direct3D11CaptureFramePool returned no activation factory"
                );
            }
            UF_TRY_VALUE(
                framePool,
                winrtCall(
                    "Direct3D11CaptureFramePool::CreateFreeThreaded",
                    [&d3d, &framePoolStatics, &poolSize]
                    {
                        // Both the statics projection and runtime device were checked
                        // non-null; Clang cannot model the generated ABI storage.
                        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
                        return framePoolStatics.CreateFreeThreaded(
                            d3d.m_runtimeDevice,
                            k_capturePixelFormat,
                            k_framePoolBufferCount,
                            poolSize
                        );
                    }
                )
            );
            if (!framePool)
            {
                return captureUnavailable(
                    "CreateFreeThreaded returned no frame pool"
                );
            }
            UF_TRY_VALUE(
                session,
                winrtCall(
                    "CreateCaptureSession",
                    [&framePool, &item]
                    {
                        return framePool.CreateCaptureSession(item);
                    }
                )
            );
            if (!session)
            {
                return captureUnavailable(
                    "CreateCaptureSession returned no capture session"
                );
            }
            UF_TRY(
                winrtCall(
                    "GraphicsCaptureSession::IsCursorCaptureEnabled(false)",
                    [&session]
                    {
                        session.IsCursorCaptureEnabled(false);
                    }
                )
            );

            auto borderRequired = true;
            if (hasBorderlessSupport)
            {
                UF_TRY_VALUE(
                    queriedBorderRequired,
                    winrtCall(
                        "GraphicsCaptureSession::IsBorderRequired",
                        [&session]
                        {
                            return session.IsBorderRequired();
                        }
                    )
                );
                borderRequired = queriedBorderRequired;
            }

            auto slotForCallback = frameSlot;
            auto const handler = winrt::Windows::Foundation::TypedEventHandler<
                CaptureFramePool,
                winrt::Windows::Foundation::IInspectable
            >{
                [p_slot = std::move(slotForCallback)](
                    CaptureFramePool const& sender,
                    winrt::Windows::Foundation::IInspectable const&
                ) noexcept
                {
                    try
                    {
                        auto frame = sender.TryGetNextFrame();
                        if (!frame)
                        {
                            return;
                        }

                        {
                            auto lock = std::unique_lock{p_slot->m_mutex};
                            if (!p_slot->m_acceptingFrames)
                            {
                                lock.unlock();
                                closeIgnoringErrors(frame);
                                return;
                            }
                            auto const arrivedAt = MonotonicInstant::now();
                            p_slot->m_latest = CapturedArrival{
                                std::move(frame),
                                arrivedAt
                            };
                        }
                        p_slot->m_arrived.notify_one();
                    }
                    catch (winrt::hresult_error const& error)
                    {
                        recordFrameCallbackFailure(
                            p_slot,
                            static_cast<HRESULT>(error.code())
                        );
                    }
                    catch (...)
                    {
                        recordFrameCallbackFailure(p_slot, E_UNEXPECTED);
                    }
                }
            };
            UF_TRY_VALUE(
                frameArrivedToken,
                winrtCall(
                    "FrameArrived",
                    [&framePool, &handler]
                    {
                        return framePool.FrameArrived(handler);
                    }
                )
            );

            auto setupCleanup = scopeExit(
                [
                    &framePool,
                    &session,
                    &frameSlot,
                    frameArrivedToken
                ]() noexcept
                {
                    try
                    {
                        framePool.FrameArrived(frameArrivedToken);
                    }
                    catch (...)
                    {
                    }
                    closeIgnoringErrors(session);
                    closeIgnoringErrors(framePool);
                    try
                    {
                        static_cast<void>(clearLatestFrame(frameSlot));
                    }
                    catch (...)
                    {
                    }
                }
            );
            if (
                !windowMarker.matches()
                || frameSlot->m_itemClosed.load(std::memory_order_acquire)
            )
            {
                return captureUnavailable(
                    "capture target changed before capture could start"
                );
            }
            UF_TRY(
                winrtCall(
                    "StartCapture",
                    [&session]
                    {
                        // CreateCaptureSession established a live projected session;
                        // Clang's analyzer loses that invariant in generated dispatch.
                        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
                        session.StartCapture();
                    }
                )
            );
            auto const startedAt = MonotonicInstant::now();

            auto p_impl = std::make_unique<Impl>(
                std::move(d3d),
                std::move(framePool),
                std::move(session),
                std::move(item),
                frameArrivedToken,
                itemClosedToken,
                std::move(frameSlot),
                startedAt,
                sessionId,
                targetGeneration,
                std::move(windowMarker),
                client,
                crop,
                options,
                geometry,
                CaptureHygiene{
                    .m_osBuild               = osBuild,
                    .m_cursorCaptureDisabled = true,
                    .m_borderlessSupported   = hasBorderlessSupport,
                    .m_borderRequired        = borderRequired,
                }
            );
            setupCleanup.release();
            itemClosedCleanup.release();
            return p_impl;
        }

        ~Impl()
        {
            try
            {
                static_cast<void>(close());
            }
            catch (...)
            {
            }
        }

        [[nodiscard]] auto capture() -> Result<Frame>
        {
            auto operationLock = std::lock_guard{m_operationMutex};
            if (m_closed)
            {
                return captureUnavailable("capture called on a closed session");
            }

            UF_TRY(m_geometry.ensureActive());
            UF_TRY_VALUE(
                arrival,
                waitForFrame(m_options.captureStallTimeout())
            );
            auto frame = std::move(arrival.m_frame);
            auto frameClose = scopeExit(
                [&frame]() noexcept
                {
                    closeIgnoringErrors(frame);
                }
            );

            m_stall.onFrameArrived(arrival.m_arrivedAt);
            UF_TRY(m_stall.check(MonotonicInstant::now()));

            UF_TRY_VALUE(
                contentSize,
                winrtCall(
                    "Direct3D11CaptureFrame::ContentSize",
                    [&frame]
                    {
                        return frame.ContentSize();
                    }
                )
            );
            UF_TRY_VALUE(
                confirmedContentSize,
                m_geometry.observeContentSize(
                    controller_detail::CaptureSize{
                        contentSize.Width,
                        contentSize.Height,
                    }
                )
            );

            auto readback = readbackSurface(
                m_staging,
                *m_device,
                *m_context,
                frame,
                m_crop
            );
            if (!readback)
            {
                return std::unexpected{std::move(readback).error()};
            }
            UF_TRY(
                winrtCall(
                    "Direct3D11CaptureFrame::Close",
                    [&frame]
                    {
                        frame.Close();
                    }
                )
            );
            frameClose.release();
            auto surface = *std::move(readback);

            UF_TRY(
                m_geometry.observeSurfaceSize(
                    confirmedContentSize,
                    surface.m_sourceWidth,
                    surface.m_sourceHeight
                )
            );

            auto const width = m_crop.width();
            auto const height = m_crop.height();
            auto const widthSize = checkedCast<std::size_t>(width);
            if (!widthSize)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "capture width does not fit size_t"
                );
            }
            auto const stride = checkedMultiply(
                *widthSize,
                bytesPerPixel(uf::PixelFormat::Bgra8)
            );
            if (!stride)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "capture stride overflow"
                );
            }

            UF_TRY_VALUE(
                clientOrigin,
                currentClientOrigin()
            );
            UF_TRY_VALUE(
                currentClient,
                ClientGeometry::create(
                    Point<DesktopSpace>{
                        static_cast<float>(clientOrigin.x),
                        static_cast<float>(clientOrigin.y)
                    },
                    m_client.width(),
                    m_client.height()
                )
            );
            UF_TRY_VALUE(
                transform,
                currentClient.transformFor(width, height)
            );
            UF_TRY_VALUE(id, m_frameIds.nextId());
            auto pixels = std::make_shared<FrameBuffer const>(
                std::move(surface.m_pixels)
            );
            return Frame::create(
                id,
                m_sessionId,
                m_targetGeneration,
                arrival.m_arrivedAt,
                width,
                height,
                *stride,
                uf::PixelFormat::Bgra8,
                std::move(pixels),
                transform
            );
        }

        [[nodiscard]]
        auto validateTargetInstance() -> Status
        {
            auto operationLock = std::lock_guard{m_operationMutex};
            if (m_closed)
            {
                return captureUnavailable(
                    "target instance validation called on a closed capture session"
                );
            }
            return validateTargetInstanceUnlocked();
        }

        [[nodiscard]]
        auto close() -> Status
        {
            auto operationLock = std::lock_guard{m_operationMutex};
            return teardownUnlocked();
        }

        [[nodiscard]] auto hygiene() const noexcept -> CaptureHygiene { return m_hygiene; }
        [[nodiscard]] auto sessionId() const noexcept -> SessionId { return m_sessionId; }
        [[nodiscard]]
        auto targetGeneration() const noexcept -> TargetGeneration
        {
            return m_targetGeneration;
        }
    };

    WgcCaptureSession::WgcCaptureSession(std::unique_ptr<Impl> p_impl) noexcept
        : m_impl{std::move(p_impl)}
    {
    }

    WgcCaptureSession::WgcCaptureSession(WgcCaptureSession&&) noexcept = default;
    WgcCaptureSession::~WgcCaptureSession() = default;

    auto WgcCaptureSession::create(
        WindowHandle windowHandle,
        SessionId sessionId,
        TargetGeneration targetGeneration,
        ClientGeometry client,
        WgcCaptureOptions options
    ) -> Result<WgcCaptureSession>
    {
        UF_TRY_VALUE(
            p_impl,
            Impl::create(
                windowHandle,
                sessionId,
                targetGeneration,
                client,
                options
            )
        );
        return WgcCaptureSession{std::move(p_impl)};
    }

    auto WgcCaptureSession::capture() -> Result<Frame>
    {
        if (!m_impl)
        {
            return captureUnavailable("capture called on a moved-from session");
        }
        return m_impl->capture();
    }

    auto WgcCaptureSession::validateTargetInstance() -> Status
    {
        if (!m_impl)
        {
            return captureUnavailable(
                "target instance validation called on a moved-from session"
            );
        }
        return m_impl->validateTargetInstance();
    }

    auto WgcCaptureSession::close() -> Status
    {
        if (!m_impl)
        {
            return ok();
        }
        return m_impl->close();
    }

    auto WgcCaptureSession::hygiene() const noexcept -> CaptureHygiene
    {
        UF_CHECK(m_impl != nullptr);
        return m_impl->hygiene();
    }

    auto WgcCaptureSession::sessionId() const noexcept -> SessionId
    {
        UF_CHECK(m_impl != nullptr);
        return m_impl->sessionId();
    }

    auto WgcCaptureSession::targetGeneration() const noexcept -> TargetGeneration
    {
        UF_CHECK(m_impl != nullptr);
        return m_impl->targetGeneration();
    }
}
