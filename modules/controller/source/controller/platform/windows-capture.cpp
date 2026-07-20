#include "controller/capture.hpp"

#include "controller/detail/capture-d3d.hpp"
#include "controller/detail/capture-os-build.hpp"
#include "controller/detail/capture-stall.hpp"
#include "controller/detail/capture-wgc.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
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

#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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

namespace
{
    using CaptureFrame = winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame;
    using CaptureFramePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
    using CaptureItem = winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
    using CaptureSession = winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
    using Direct3DDevice =
        winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
    using DxgiInterfaceAccess =
        ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;
    using PixelFormat = winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
    constexpr auto framePoolBufferCount = std::int32_t{2};
    constexpr auto capturePixelFormat = PixelFormat::B8G8R8A8UIntNormalized;

    [[nodiscard]]
    auto captureUnavailable(std::string message) -> std::unexpected<uf::Error>
    {
        return uf::fail(
            uf::AutomationErrorKind::CaptureUnavailable,
            std::move(message)
        );
    }

    [[nodiscard]]
    auto captureHresult(
        std::string_view context,
        HRESULT result
    ) -> std::unexpected<uf::Error>
    {
        return captureUnavailable(
            std::format(
                "{}: HRESULT {:#010x}",
                context,
                static_cast<std::uint32_t>(result)
            )
        );
    }

    template <typename Function>
        requires std::invocable<Function&>
    [[nodiscard]]
    auto winrtCall(
        std::string_view context,
        Function&& function
    ) -> uf::Result<std::invoke_result_t<Function&>>
    {
        using Return = std::invoke_result_t<Function&>;

        try
        {
            if constexpr (std::is_void_v<Return>)
            {
                std::invoke(function);
                return uf::ok();
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
    auto toNativeHandle(uf::WindowHandle handle) noexcept -> HWND
    {
        // SAFETY: WindowHandle stores the pointer-sized integer representation copied
        // from an HWND. The boundary restores that opaque token without dereferencing it.
        return reinterpret_cast<HWND>(handle.value());
    }

    struct D3d11Objects final
    {
        winrt::com_ptr<ID3D11Device> m_device;
        winrt::com_ptr<ID3D11DeviceContext> m_context;
    };

    struct D3dDevice final
    {
        winrt::com_ptr<ID3D11Device> m_device;
        winrt::com_ptr<ID3D11DeviceContext> m_context;
        Direct3DDevice m_runtimeDevice;
    };

    [[nodiscard]]
    auto createD3d11(D3D_DRIVER_TYPE driverType) -> uf::Result<D3d11Objects>
    {
        auto device = winrt::com_ptr<ID3D11Device>{};
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
    auto createD3dDevice() -> uf::Result<D3dDevice>
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

        return D3dDevice{
            std::move(native.m_device),
            std::move(native.m_context),
            std::move(runtimeDevice)
        };
    }

    class StagingTexture final
    {
        winrt::com_ptr<ID3D11Texture2D> m_texture;
        std::uint32_t m_width{};
        std::uint32_t m_height{};
        DXGI_FORMAT m_format{DXGI_FORMAT_UNKNOWN};

    public:
        [[nodiscard]]
        auto getOrCreate(
            ID3D11Device& device,
            D3D11_TEXTURE2D_DESC const& sourceDescription
        ) -> uf::Result<winrt::com_ptr<ID3D11Texture2D>>
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
                m_width = sourceDescription.Width;
                m_height = sourceDescription.Height;
                m_format = sourceDescription.Format;
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
        std::uint32_t m_sourceWidth;
        std::uint32_t m_sourceHeight;
        std::vector<std::byte> m_pixels;
    };

    [[nodiscard]]
    auto readbackSurface(
        StagingTexture& stagingCache,
        ID3D11Device& device,
        ID3D11DeviceContext& context,
        CaptureFrame const& frame,
        uf::controller_detail::ClientCropRect const& crop
    ) -> uf::Result<SurfaceReadback>
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

        auto stagingDescription = sourceDescription;
        stagingDescription.Width = crop.width();
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
        auto unmap = uf::scopeExit(
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
        auto const heightSize = uf::checkedCast<std::size_t>(crop.height());
        if (!heightSize)
        {
            return uf::fail(
                uf::AutomationErrorKind::InternalInvariant,
                "mapped staging height does not fit size_t"
            );
        }
        auto const mappedLength = uf::checkedMultiply(rowPitch, *heightSize);
        if (!mappedLength)
        {
            return uf::fail(
                uf::AutomationErrorKind::InternalInvariant,
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
            uf::controller_detail::readbackBgra8(
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
    auto currentOsBuild() -> uf::Result<std::uint32_t>
    {
        auto information = RTL_OSVERSIONINFOW{};
        auto const informationSize = uf::checkedCast<ULONG>(sizeof(information));
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

        return static_cast<std::uint32_t>(information.dwBuildNumber);
    }

    // WGC CreateForWindow captures DWM-composed non-client chrome. Resolve the client's
    // frame-relative origin from ClientToScreen(0, 0) minus the visible DWM extended-frame
    // top-left. GetWindowRect is deliberately not used because its invisible resize border
    // would introduce a monitor/DPI-dependent crop and click offset.
    [[nodiscard]]
    auto resolveClientCrop(
        HWND windowHandle,
        std::uint32_t frameWidth,
        std::uint32_t frameHeight,
        uf::ClientGeometry const& client
    ) -> uf::Result<uf::controller_detail::ClientCropRect>
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

        auto bounds = RECT{};
        auto const boundsSize = uf::checkedCast<DWORD>(sizeof(bounds));
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

        auto const offsetX = uf::checkedSubtract(
            static_cast<std::int32_t>(clientOrigin.x),
            static_cast<std::int32_t>(bounds.left)
        );
        if (!offsetX)
        {
            return captureUnavailable("client x offset computation overflowed");
        }
        auto const offsetY = uf::checkedSubtract(
            static_cast<std::int32_t>(clientOrigin.y),
            static_cast<std::int32_t>(bounds.top)
        );
        if (!offsetY)
        {
            return captureUnavailable("client y offset computation overflowed");
        }
        auto const extendedWidth = uf::checkedSubtract(
            static_cast<std::int32_t>(bounds.right),
            static_cast<std::int32_t>(bounds.left)
        );
        if (!extendedWidth)
        {
            return captureUnavailable(
                "extended frame bounds width computation overflowed"
            );
        }
        auto const extendedHeight = uf::checkedSubtract(
            static_cast<std::int32_t>(bounds.bottom),
            static_cast<std::int32_t>(bounds.top)
        );
        if (!extendedHeight)
        {
            return captureUnavailable(
                "extended frame bounds height computation overflowed"
            );
        }

        UF_TRY_VALUE(
            clientExtent,
            uf::controller_detail::clientIntegerExtent(client)
        );
        return uf::controller_detail::ClientCropRect::create(
            {frameWidth, frameHeight},
            {*extendedWidth, *extendedHeight},
            {*offsetX, *offsetY},
            clientExtent
        );
    }

    struct CapturedArrival final
    {
        CaptureFrame m_frame;
        uf::MonotonicInstant m_arrivedAt;
    };

    struct FrameSlot final
    {
        // The FrameArrived callback and capture consumer share this state. Every
        // m_latest access is serialized by m_mutex; m_arrived only publishes changes
        // made while that mutex is held.
        std::mutex m_mutex;
        std::condition_variable m_arrived;
        std::optional<CapturedArrival> m_latest;
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

    auto clearLatestFrame(std::shared_ptr<FrameSlot> const& p_slot) noexcept -> void
    {
        try
        {
            auto lock = std::lock_guard{p_slot->m_mutex};
            if (p_slot->m_latest)
            {
                closeIgnoringErrors(p_slot->m_latest->m_frame);
                p_slot->m_latest.reset();
            }
        }
        catch (...)
        {
        }
    }
}

namespace uf
{
    class WgcCaptureSession::Impl final
    {
        // Serializes the consumer-side session, geometry, stall, and D3D state. The
        // callback never takes this mutex and communicates only through m_frameSlot.
        std::mutex m_operationMutex;
        winrt::com_ptr<ID3D11Device> m_device;
        winrt::com_ptr<ID3D11DeviceContext> m_context;
        // Retained so the projected device outlives the frame pool created from it.
        Direct3DDevice m_runtimeDevice;
        CaptureFramePool m_framePool;
        CaptureSession m_session;
        // Retained so the capture item outlives the session that references it.
        CaptureItem m_item;
        winrt::event_token m_frameArrivedToken;
        std::shared_ptr<FrameSlot> m_frameSlot;
        controller_detail::StallTracker m_stall;
        SessionId m_sessionId;
        controller_detail::FrameIdCounter m_frameIds;
        TargetGeneration m_targetGeneration;
        ClientGeometry m_client;
        controller_detail::ClientCropRect m_crop;
        WgcCaptureOptions m_options;
        controller_detail::CaptureGeometryState m_geometry;
        StagingTexture m_staging;
        CaptureHygiene m_hygiene;
        bool m_closed;

    public:
        Impl(
            D3dDevice d3d,
            CaptureFramePool framePool,
            CaptureSession session,
            CaptureItem item,
            winrt::event_token frameArrivedToken,
            std::shared_ptr<FrameSlot> p_frameSlot,
            MonotonicInstant startedAt,
            SessionId sessionId,
            TargetGeneration targetGeneration,
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
            , m_frameSlot{std::move(p_frameSlot)}
            , m_stall{options.captureStallTimeout(), startedAt}
            , m_sessionId{sessionId}
            , m_frameIds{}
            , m_targetGeneration{targetGeneration}
            , m_client{client}
            , m_crop{crop}
            , m_options{options}
            , m_geometry{geometry}
            , m_staging{}
            , m_hygiene{hygiene}
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
                        return m_frameSlot->m_latest.has_value();
                    }
                )
            );

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

        auto teardownUnlocked() noexcept -> void
        {
            if (m_closed)
            {
                return;
            }

            m_closed = true;
            m_framePool.FrameArrived(m_frameArrivedToken);
            closeIgnoringErrors(m_session);
            closeIgnoringErrors(m_framePool);
            clearLatestFrame(m_frameSlot);
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
            UF_TRY_VALUE(
                supported,
                winrtCall(
                    "GraphicsCaptureSession::IsSupported",
                    []
                    {
                        return CaptureSession::IsSupported();
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
                        controller_detail::g_cursorCaptureMinBuild
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
                toNativeHandle(windowHandle),
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
                    toNativeHandle(windowHandle),
                    frameWidth,
                    frameHeight,
                    client
                )
            );

            UF_TRY_VALUE(
                framePool,
                winrtCall(
                    "Direct3D11CaptureFramePool::CreateFreeThreaded",
                    [&d3d, &poolSize]
                    {
                        return CaptureFramePool::CreateFreeThreaded(
                            d3d.m_runtimeDevice,
                            capturePixelFormat,
                            framePoolBufferCount,
                            poolSize
                        );
                    }
                )
            );
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
            auto const borderResult = winrtCall(
                "GraphicsCaptureSession::IsBorderRequired",
                [&session]
                {
                    return session.IsBorderRequired();
                }
            );
            if (borderResult)
            {
                borderRequired = *borderResult;
            }

            auto frameSlot = std::make_shared<FrameSlot>();
            auto slotForCallback = frameSlot;
            auto poolForCallback = framePool;
            auto const handler = winrt::Windows::Foundation::TypedEventHandler<
                CaptureFramePool,
                winrt::Windows::Foundation::IInspectable
            >{
                [
                    p_slot = std::move(slotForCallback),
                    pool = std::move(poolForCallback)
                ](
                    CaptureFramePool const&,
                    winrt::Windows::Foundation::IInspectable const&
                ) noexcept
                {
                    try
                    {
                        auto frame = pool.TryGetNextFrame();
                        if (!frame)
                        {
                            return;
                        }

                        {
                            auto lock = std::lock_guard{p_slot->m_mutex};
                            auto const arrivedAt = MonotonicInstant::now();
                            p_slot->m_latest = CapturedArrival{
                                std::move(frame),
                                arrivedAt
                            };
                        }
                        p_slot->m_arrived.notify_one();
                    }
                    catch (...)
                    {
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
                    framePool.FrameArrived(frameArrivedToken);
                    closeIgnoringErrors(session);
                    closeIgnoringErrors(framePool);
                    clearLatestFrame(frameSlot);
                }
            );
            UF_TRY(
                winrtCall(
                    "StartCapture",
                    [&session]
                    {
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
                std::move(frameSlot),
                startedAt,
                sessionId,
                targetGeneration,
                client,
                crop,
                options,
                geometry,
                CaptureHygiene{
                    .m_osBuild = osBuild,
                    .m_cursorCaptureDisabled = true,
                    .m_borderlessSupported = hasBorderlessSupport,
                    .m_borderRequired = borderRequired,
                }
            );
            setupCleanup.release();
            return p_impl;
        }

        ~Impl() { close(); }

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
            closeIgnoringErrors(frame);
            frameClose.release();
            if (!readback)
            {
                return std::unexpected{std::move(readback).error()};
            }
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

            UF_TRY_VALUE(transform, m_client.transformFor(width, height));
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

        auto close() noexcept -> void
        {
            try
            {
                auto operationLock = std::lock_guard{m_operationMutex};
                teardownUnlocked();
            }
            catch (...)
            {
            }
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
    auto WgcCaptureSession::operator=(
        WgcCaptureSession&&
    ) noexcept -> WgcCaptureSession& = default;
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

    auto WgcCaptureSession::close() noexcept -> void
    {
        if (m_impl)
        {
            m_impl->close();
        }
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
