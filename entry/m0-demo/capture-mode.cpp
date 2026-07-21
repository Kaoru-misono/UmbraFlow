#include "capture-mode.hpp"

#include "capture-output.hpp"
#include "log-jsonl.hpp"
#include "path-validation.hpp"
#include "target-setup.hpp"

#include <controller/capture.hpp>
#include <controller/discovery.hpp>
#include <controller/dpi.hpp>
#include <controller/target.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>
#include <domain/error.hpp>
#include <domain/ids.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace uf::m0_demo
{
    namespace
    {
        [[nodiscard]]
        auto dpiDeclarationName(DpiDeclaration declaration) noexcept -> std::string_view
        {
            switch (declaration)
            {
            case DpiDeclaration::Declared: return "Declared";
            case DpiDeclaration::AlreadyDeclared: return "AlreadyDeclared";
            }
            return "Unknown";
        }

        [[nodiscard]]
        auto captureFps(
            uint32 frameCount,
            MonotonicInstant::Duration elapsed
        ) noexcept -> double
        {
            auto const seconds = std::chrono::duration<double>{elapsed}.count();
            if (seconds <= 0.0)
            {
                return 0.0;
            }
            return static_cast<double>(frameCount) / seconds;
        }

        [[nodiscard]]
        auto runCaptureWithLog(
            CaptureArgs const& args,
            JsonlLog& log
        ) -> Status
        {
            UF_TRY_VALUE(dpiDeclaration, ensurePerMonitorAwareV2());
            UF_TRY(
                log.write(
                    LogLine{"setup", "dpi_declared"}
                        .outcome("ok")
                        .detail(std::string{dpiDeclarationName(dpiDeclaration)})
                )
            );

            UF_TRY_VALUE(candidates, enumerateCandidates());
            auto const selector = buildSelector(args.m_selector);
            UF_TRY_VALUE(resolved, resolveTarget(candidates, selector));
            auto const client = resolved.clientSize();
            auto const process = resolved.identity().process();
            UF_TRY(
                log.write(
                    LogLine{"discovery", "resolved"}
                        .outcome("ok")
                        .detail(
                            std::format(
                                "pid={} client={}x{}",
                                process.value(),
                                client.width(),
                                client.height()
                            )
                        )
                )
            );

            UF_TRY_VALUE(
                session,
                createCaptureSession(
                    resolved,
                    SessionId{1},
                    WgcCaptureOptions{}
                )
            );
            auto const hygiene = session.hygiene();
            UF_TRY(
                log.write(
                    LogLine{"setup", "session_created"}
                        .outcome("ok")
                        .detail(
                            std::format(
                                "os_build={} cursor_capture_disabled={} borderless_supported={} border_required={}",
                                hygiene.m_osBuild,
                                hygiene.m_cursorCaptureDisabled,
                                hygiene.m_borderlessSupported,
                                hygiene.m_borderRequired
                            )
                        )
                )
            );

            auto const captureStarted = MonotonicInstant::now();
            auto captureFinished = captureStarted;
            for (auto index = uint32{0}; index < args.m_frames; ++index)
            {
                UF_TRY_VALUE_CONTEXT(
                    frame,
                    session.capture(),
                    std::format("capturing frame {} of {}", index + 1U, args.m_frames)
                );
                captureFinished = MonotonicInstant::now();
                auto const output = indexedOutputPath(
                    args.m_output,
                    index,
                    args.m_frames
                );
                UF_TRY_CONTEXT(
                    writeFramePng(frame, output),
                    std::format("writing captured frame {}", index + 1U)
                );

                auto const deltaWidth = (
                    static_cast<int64>(frame.width())
                    - static_cast<int64>(client.width())
                );
                auto const deltaHeight = (
                    static_cast<int64>(frame.height())
                    - static_cast<int64>(client.height())
                );
                auto const elapsed = captureFinished.saturatingDurationSince(
                    captureStarted
                );
                auto const fps = captureFps(index + 1U, elapsed);
                auto const detail = std::format(
                    "index={}/{} frame={}x{} client={}x{} delta=({},{}) capture_fps={:.2f} output={}",
                    index + 1U,
                    args.m_frames,
                    frame.width(),
                    frame.height(),
                    client.width(),
                    client.height(),
                    deltaWidth,
                    deltaHeight,
                    fps,
                    output.string()
                );
                std::cerr << "m0-demo capture: " << detail << '\n';
                UF_TRY(
                    log.write(
                        LogLine{"capture", "frame_written"}
                            .frame(frame)
                            .outcome("ok")
                            .detail(detail)
                    )
                );

                if (
                    index + 1U < args.m_frames
                    && args.m_interval > MonotonicInstant::Duration::zero()
                )
                {
                    std::this_thread::sleep_for(args.m_interval);
                }
            }
            UF_TRY(session.close());

            auto const elapsed = captureFinished.saturatingDurationSince(
                captureStarted
            );
            auto const fps = captureFps(args.m_frames, elapsed);
            auto const seconds = std::chrono::duration<double>{elapsed}.count();
            auto const detail = std::format(
                "frames={} elapsed_s={:.3f} capture_fps={:.2f}",
                args.m_frames,
                seconds,
                fps
            );
            std::cerr << "m0-demo capture: " << detail << '\n';
            return log.write(
                LogLine{"run", "capture_summary"}
                    .outcome("ok")
                    .detail(detail)
            );
        }
    }

    auto validateCaptureOutputPaths(CaptureArgs const& args) -> Status
    {
        if (!args.m_log)
        {
            return ok();
        }

        UF_TRY_VALUE(
            canonicalLog,
            canonicalizePathForComparison(*args.m_log, "capture log")
        );
        for (auto index = uint32{0}; index < args.m_frames; ++index)
        {
            auto const output = indexedOutputPath(
                args.m_output,
                index,
                args.m_frames
            );
            UF_TRY_VALUE(
                canonicalOutput,
                canonicalizePathForComparison(output, "capture output")
            );
            UF_TRY_VALUE(
                aliasesLog,
                canonicalPathsAlias(canonicalLog, canonicalOutput)
            );
            if (aliasesLog)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "capture log path {} aliases output PNG path {}",
                        args.m_log->string(),
                        output.string()
                    )
                );
            }
        }
        return ok();
    }

    auto runCapture(
        std::span<std::string const> raw
    ) -> Status
    {
        UF_TRY_VALUE(args, parseCaptureArguments(raw));
        UF_TRY(validateCaptureOutputPaths(args));
        UF_TRY_VALUE(log, JsonlLog::create(args.m_log));

        auto outcome = runCaptureWithLog(args, log);
        auto terminalWrite = ok();
        if (!outcome)
        {
            terminalWrite = log.write(
                LogLine{"run", "fatal"}
                    .outcome("error")
                    .detail(formatAutomationError(outcome.error()))
            );
        }
        auto flush = log.flush();
        if (!outcome)
        {
            return std::unexpected{std::move(outcome).error()};
        }

        UF_TRY(terminalWrite);
        UF_TRY(flush);
        return ok();
    }
}
