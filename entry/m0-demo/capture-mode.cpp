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
#include <domain/error.hpp>
#include <domain/ids.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace
{
    [[nodiscard]]
    auto dpiDeclarationName(uf::DpiDeclaration declaration) noexcept -> std::string_view
    {
        switch (declaration)
        {
        case uf::DpiDeclaration::Declared: return "Declared";
        case uf::DpiDeclaration::AlreadyDeclared: return "AlreadyDeclared";
        }
        return "Unknown";
    }

    [[nodiscard]]
    auto captureFps(
        std::uint32_t frameCount,
        uf::MonotonicInstant::Duration elapsed
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
        uf::m0_demo::CaptureArgs const& args,
        uf::m0_demo::JsonlLog& log
    ) -> uf::Status
    {
        UF_TRY_VALUE(dpiDeclaration, uf::ensurePerMonitorAwareV2());
        UF_TRY(
            log.write(
                uf::m0_demo::LogLine{"setup", "dpi_declared"}
                    .outcome("ok")
                    .detail(std::string{dpiDeclarationName(dpiDeclaration)})
            )
        );

        UF_TRY_VALUE(candidates, uf::enumerateCandidates());
        auto const selector = uf::m0_demo::buildSelector(args.m_selector);
        UF_TRY_VALUE(resolved, uf::resolveTarget(candidates, selector));
        auto const client = resolved.clientSize();
        auto const process = resolved.identity().process();
        UF_TRY(
            log.write(
                uf::m0_demo::LogLine{"discovery", "resolved"}
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
            uf::m0_demo::createCaptureSession(
                resolved,
                uf::SessionId{1},
                uf::WgcCaptureOptions{}
            )
        );
        auto const hygiene = session.hygiene();
        UF_TRY(
            log.write(
                uf::m0_demo::LogLine{"setup", "session_created"}
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

        auto const captureStarted = uf::MonotonicInstant::now();
        auto captureFinished = captureStarted;
        for (auto index = std::uint32_t{0}; index < args.m_frames; ++index)
        {
            UF_TRY_VALUE_CONTEXT(
                frame,
                session.capture(),
                std::format("capturing frame {} of {}", index + 1U, args.m_frames)
            );
            captureFinished = uf::MonotonicInstant::now();
            auto const output = uf::m0_demo::indexedOutputPath(
                args.m_output,
                index,
                args.m_frames
            );
            UF_TRY_CONTEXT(
                uf::m0_demo::writeFramePng(frame, output),
                std::format("writing captured frame {}", index + 1U)
            );

            auto const deltaWidth = (
                static_cast<std::int64_t>(frame.width())
                - static_cast<std::int64_t>(client.width())
            );
            auto const deltaHeight = (
                static_cast<std::int64_t>(frame.height())
                - static_cast<std::int64_t>(client.height())
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
                    uf::m0_demo::LogLine{"capture", "frame_written"}
                        .frame(frame)
                        .outcome("ok")
                        .detail(detail)
                )
            );

            if (
                index + 1U < args.m_frames
                && args.m_interval > uf::MonotonicInstant::Duration::zero()
            )
            {
                std::this_thread::sleep_for(args.m_interval);
            }
        }
        session.close();

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
            uf::m0_demo::LogLine{"run", "capture_summary"}
                .outcome("ok")
                .detail(detail)
        );
    }
}

namespace uf::m0_demo
{
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
        for (auto index = std::uint32_t{0}; index < args.m_frames; ++index)
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
