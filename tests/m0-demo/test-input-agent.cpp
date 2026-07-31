#include "test-helpers.hpp"

#include <input-agent-annotation.hpp>
#include <input-agent-cursor.hpp>
#include <input-agent-drive.hpp>
#include <input-agent-protocol.hpp>
#include <input-agent.hpp>
#include <path-validation.hpp>
#include <platform/windows-file-writer.hpp>

#include <controller/detail/audit-log-access.hpp>
#include <controller/input.hpp>
#include <core/types/integer.hpp>
#include <core/utility/scope-exit.hpp>
#include <domain/error.hpp>
#include <domain/frame.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <format>
#include <ios>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace uf::m0_demo
{
    namespace
    {
        [[nodiscard]]
        auto instantAt(
            MonotonicInstant::Duration duration
        ) -> MonotonicInstant
        {
            return MonotonicInstant::fromTimePoint(
                MonotonicInstant::TimePoint{duration}
            );
        }

        [[nodiscard]]
        auto deliveryTarget(
            TargetGeneration generation = TargetGeneration{}
        ) -> DeliveryTarget
        {
            auto target = DeliveryTarget::create(
                WindowHandle{0x1234},
                CaptureSessionId{1},
                generation,
                800,
                450
            );
            REQUIRE(target.has_value());
            return *std::move(target);
        }

        [[nodiscard]]
        auto observationLease(
            MonotonicInstant capturedAt,
            TargetGeneration generation = TargetGeneration{}
        ) -> ObservationLease
        {
            auto const transform = CoordinateTransform::create(
                Point<DesktopSpace>{0.0F, 0.0F},
                1.0F,
                1.0F,
                1,
                1
            );
            REQUIRE(transform.has_value());
            auto const pixels = std::make_shared<FrameBuffer const>(
                std::vector<std::byte>(4)
            );
            auto const frame = Frame::create(
                FrameId{1},
                CaptureSessionId{1},
                generation,
                capturedAt,
                1,
                1,
                4,
                PixelFormat::Bgra8,
                pixels,
                *transform
            );
            REQUIRE(frame.has_value());
            auto const lease = ObservationLease::forFrame(
                *frame,
                k_defaultMaxActionFrameAge
            );
            REQUIRE(lease.has_value());
            return *lease;
        }

        [[nodiscard]]
        auto parsedClick(std::string_view line) -> InputAgentClickCommand
        {
            auto command = parseInputAgentCommand(line);
            REQUIRE(command.has_value());
            auto const* click = std::get_if<InputAgentClickCommand>(
                &*command
            );
            REQUIRE(click != nullptr);
            return *click;
        }

        [[nodiscard]]
        auto parsedKey(std::string_view line) -> InputAgentKeyCommand
        {
            auto command = parseInputAgentCommand(line);
            REQUIRE(command.has_value());
            auto const* key = std::get_if<InputAgentKeyCommand>(
                &*command
            );
            REQUIRE(key != nullptr);
            return *key;
        }

        [[nodiscard]]
        auto parsedScroll(std::string_view line) -> InputAgentScrollCommand
        {
            auto command = parseInputAgentCommand(line);
            REQUIRE(command.has_value());
            auto const* scrollCommand = std::get_if<InputAgentScrollCommand>(
                &*command
            );
            REQUIRE(scrollCommand != nullptr);
            return *scrollCommand;
        }

        [[nodiscard]]
        auto createTemporaryDirectory(std::string_view role) -> std::filesystem::path
        {
            auto const token = std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();
            auto const path = std::filesystem::temp_directory_path()
                / std::format("umbraflow-{}-{}", role, token);
            auto error = std::error_code{};
            auto const created = std::filesystem::create_directory(path, error);
            REQUIRE(created);
            REQUIRE_FALSE(error);
            return path;
        }

        auto removeAllBestEffort(std::filesystem::path const& path) noexcept -> void
        {
            try
            {
                auto error = std::error_code{};
                static_cast<void>(std::filesystem::remove_all(path, error));
            }
            catch (...)
            {
            }
        }

        auto writeFile(
            std::filesystem::path const& path,
            std::string_view content,
            std::ios::openmode mode
        ) -> void
        {
            auto stream = std::ofstream{path, std::ios::binary | mode};
            REQUIRE(stream.is_open());
            stream.write(
                content.data(),
                static_cast<std::streamsize>(content.size())
            );
            stream.flush();
            REQUIRE(stream.good());
        }
    }

    TEST_CASE("m0 input-agent parses every bounded command operation")
    {
        auto const capture = parseInputAgentCommand(
            R"({"op":"capture","out":"shots\/one.png"})"
        );
        REQUIRE(capture.has_value());
        auto const* captureCommand = std::get_if<InputAgentCaptureCommand>(
            &*capture
        );
        REQUIRE(captureCommand != nullptr);
        CHECK(captureCommand->output == std::filesystem::path{"shots/one.png"});

        auto const click = parsedClick(
            R"({"op":"click","x":12.5,"y":4e1,"out_before":"before.png",)"
            R"("out_after":"after.png","settle_ms":250})"
        );
        CHECK(click.x == 12.5F);
        CHECK(click.y == 40.0F);
        CHECK(click.outputBefore == std::filesystem::path{"before.png"});
        CHECK(click.outputAfter == std::filesystem::path{"after.png"});
        CHECK(
            click.settle
            == std::chrono::duration_cast<MonotonicInstant::Duration>(
                std::chrono::milliseconds{250}
            )
        );

        auto const defaultSettle = parsedClick(
            R"({"op":"click","x":1,"y":2,"out_before":"before.png","out_after":"after.png"})"
        );
        CHECK(defaultSettle.settle == k_defaultInputAgentSettle);

        auto const quit = parseInputAgentCommand(
            "  { \"op\" : \"quit\" }  "
        );
        REQUIRE(quit.has_value());
        CHECK(std::holds_alternative<InputAgentQuitCommand>(*quit));
    }

    TEST_CASE("m0 input-agent rejects malformed and unrecognized commands")
    {
        auto const cases = std::array<std::string_view, 11>{
            "",
            "{}",
            R"({"op":"run","out":"anything"})",
            R"({"op":"capture"})",
            R"({"op":"capture","out":""})",
            R"({"op":"capture","out":"bad\u0000path.png"})",
            R"({"op":"capture","out":"a.png","extra":1})",
            R"({"op":"quit","x":1})",
            R"({"op":"click","x":1,"y":2,"out_before":"a.png"})",
            R"({"op":"click","x":1,"x":2,"y":2,"out_before":"a.png","out_after":"b.png"})",
            R"({"op":"click","x":1,"y":2,"out_before":"a.png","out_after":"b.png","settle_ms":1.5})",
        };

        for (auto const line : cases)
        {
            auto const result = parseInputAgentCommand(line);
            REQUIRE_FALSE(result.has_value());
            test_m0_demo::requireErrorKind(
                result.error(),
                AutomationErrorKind::InvalidResource
            );
        }
    }

    TEST_CASE("m0 input-agent parses a key command over every supported family")
    {
        auto const letter = parsedKey(
            R"({"op":"key","key":"E","out_before":"a.png","out_after":"b.png",)"
            R"("settle_ms":2000})"
        );
        CHECK(letter.key.virtualKey() == uint16{0x0045U});
        CHECK_FALSE(letter.key.isExtended());
        CHECK(letter.outputBefore == std::filesystem::path{"a.png"});
        CHECK(letter.outputAfter == std::filesystem::path{"b.png"});
        CHECK(
            letter.settle
            == std::chrono::duration_cast<MonotonicInstant::Duration>(
                std::chrono::milliseconds{2000}
            )
        );

        auto const digit = parsedKey(
            R"({"op":"key","key":"1","out_before":"a.png","out_after":"b.png"})"
        );
        CHECK(digit.key.virtualKey() == uint16{0x0031U});
        CHECK(digit.settle == k_defaultInputAgentSettle);

        auto const function = parsedKey(
            R"({"op":"key","key":"F3","out_before":"a.png","out_after":"b.png"})"
        );
        CHECK(function.key.virtualKey() == uint16{0x0072U});
    }

    TEST_CASE("m0 input-agent parses a scroll command with its wheel delta")
    {
        auto const scrollCommand = parsedScroll(
            R"({"op":"scroll","x":12.5,"y":4e1,"delta":-3,"out_before":"before.png",)"
            R"("out_after":"after.png","settle_ms":250})"
        );
        CHECK(scrollCommand.x == 12.5F);
        CHECK(scrollCommand.y == 40.0F);
        CHECK(scrollCommand.delta.notches() == -3);
        CHECK(scrollCommand.delta.rawUnits() == int16{-360});
        CHECK(scrollCommand.outputBefore == std::filesystem::path{"before.png"});
        CHECK(scrollCommand.outputAfter == std::filesystem::path{"after.png"});
        CHECK(
            scrollCommand.settle
            == std::chrono::duration_cast<MonotonicInstant::Duration>(
                std::chrono::milliseconds{250}
            )
        );

        auto const defaultSettle = parsedScroll(
            R"({"op":"scroll","x":1,"y":2,"delta":1,"out_before":"a.png","out_after":"b.png"})"
        );
        CHECK(defaultSettle.settle == k_defaultInputAgentSettle);

        // The largest notch count whose raw delta still fits the word wParam
        // encodes it in.
        auto const boundary = parsedScroll(
            std::format(
                R"({{"op":"scroll","x":1,"y":2,"delta":{},"out_before":"a.png","out_after":"b.png"}})",
                k_maxWheelNotches
            )
        );
        CHECK(boundary.delta.notches() == k_maxWheelNotches);
    }

    TEST_CASE("m0 input-agent rejects scroll commands it could not deliver")
    {
        for (auto const line : std::array<std::string_view, 13>{
            R"({"op":"scroll","x":1,"y":2,"out_before":"a.png","out_after":"b.png"})",
            R"({"op":"scroll","x":1,"y":2,"delta":0,"out_before":"a.png","out_after":"b.png"})",
            R"({"op":"scroll","x":1,"y":2,"delta":274,"out_before":"a.png","out_after":"b.png"})",
            R"({"op":"scroll","x":1,"y":2,"delta":-274,"out_before":"a.png","out_after":"b.png"})",
            R"({"op":"scroll","x":1,"y":2,"delta":1.5,"out_before":"a.png","out_after":"b.png"})",
            R"({"op":"scroll","x":1,"y":2,"delta":1,"delta":2,"out_before":"a.png","out_after":"b.png"})",
            R"({"op":"scroll","delta":1,"out_before":"a.png","out_after":"b.png"})",
            R"({"op":"scroll","x":1,"y":2,"delta":1,"out_before":"a.png"})",
            R"({"op":"scroll","x":1,"y":2,"delta":1,"key":"E","out_before":"a.png","out_after":"b.png"})",
            R"({"op":"click","x":1,"y":2,"delta":1,"out_before":"a.png","out_after":"b.png"})",
            R"({"op":"key","key":"E","delta":1,"out_before":"a.png","out_after":"b.png"})",
            R"({"op":"capture","out":"a.png","delta":1})",
            R"({"op":"quit","delta":1})",
        })
        {
            CAPTURE(line);
            auto const result = parseInputAgentCommand(line);
            REQUIRE_FALSE(result.has_value());
            test_m0_demo::requireErrorKind(
                result.error(),
                AutomationErrorKind::InvalidResource
            );
        }

        // A wheel that moves nothing must be legible as the mistyped command it
        // is, never as an accepted no-op.
        auto const still = parseInputAgentCommand(
            R"({"op":"scroll","x":1,"y":2,"delta":0,"out_before":"a.png","out_after":"b.png"})"
        );
        REQUIRE_FALSE(still.has_value());
        CHECK(still.error().message().contains("non-zero"));

        auto const missing = parseInputAgentCommand(
            R"({"op":"scroll","x":1,"y":2,"out_before":"a.png","out_after":"b.png"})"
        );
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().message().contains("requires a delta field"));
    }

    TEST_CASE("m0 input-agent key result line reports delivery separately from ok")
    {
        auto const delivered = InputAgentActionResult{
            .beforeFrame = uint64{7},
            .afterFrame  = uint64{8},
            .delivered   = true,
            .error       = std::nullopt,
        };
        CHECK(
            serializeInputAgentActionResult("key", delivered)
            == R"({"op":"key","ok":true,"before_frame_id":7,)"
               R"("after_frame_id":8,"delivered":true,"error":null})"
        );

        auto rejected = fail(
            AutomationErrorKind::ActionRejected,
            "unsupported key"
        );
        auto const failed = InputAgentActionResult{
            .beforeFrame = uint64{7},
            .afterFrame  = std::nullopt,
            .delivered   = false,
            .error       = std::move(rejected).error(),
        };
        auto const line = serializeInputAgentActionResult("key", failed);
        CHECK(line.starts_with(R"({"op":"key","ok":false,)"));
        CHECK(line.contains(R"("delivered":false)"));
        CHECK(line.contains("unsupported key"));
        CHECK_FALSE(line.contains(R"("delivered":true)"));
    }

    TEST_CASE("m0 input-agent rejects unknown key names and misplaced key fields")
    {
        for (auto const line : std::array<std::string_view, 11>{
            R"({"op":"key","out_before":"a.png","out_after":"b.png"})",
            R"({"op":"key","key":"","out_before":"a.png","out_after":"b.png"})",
            R"({"op":"key","key":"e","out_before":"a.png","out_after":"b.png"})",
            R"({"op":"key","key":"F13","out_before":"a.png","out_after":"b.png"})",
            R"({"op":"key","key":"ENTER","out_before":"a.png","out_after":"b.png"})",
            R"({"op":"key","key":"E","out_before":"a.png"})",
            R"({"op":"key","key":"E","x":1,"y":2,"out_before":"a.png","out_after":"b.png"})",
            R"({"op":"key","key":"E","out":"c.png","out_before":"a.png","out_after":"b.png"})",
            R"({"op":"click","key":"E","x":1,"y":2,"out_before":"a.png","out_after":"b.png"})",
            R"({"op":"capture","key":"E","out":"c.png"})",
            R"({"op":"quit","key":"E"})",
        })
        {
            CAPTURE(line);
            auto const result = parseInputAgentCommand(line);
            REQUIRE_FALSE(result.has_value());
            test_m0_demo::requireErrorKind(
                result.error(),
                AutomationErrorKind::InvalidResource
            );
        }

        // An unrecognized name must be legible as one, and must never produce a
        // command that the agent could go on to deliver.
        auto const unknown = parseInputAgentCommand(
            R"({"op":"key","key":"CTRL","out_before":"a.png","out_after":"b.png"})"
        );
        REQUIRE_FALSE(unknown.has_value());
        CHECK(unknown.error().message().contains("unsupported key"));
        CHECK(unknown.error().message().contains("CTRL"));
    }

    TEST_CASE("m0 input-agent key frames must be fresh non-aliasing files")
    {
        auto const directory = createTemporaryDirectory("input-agent-key-frames");
        auto const cleanup = scopeExit(
            [cleanupPath = directory]() noexcept
            {
                removeAllBestEffort(cleanupPath);
            }
        );
        auto const outputDirectory = directory / "out";
        auto error = std::error_code{};
        REQUIRE(std::filesystem::create_directory(outputDirectory, error));
        REQUIRE_FALSE(error);
        auto const canonical = canonicalizeOutputDirectory(outputDirectory);
        REQUIRE(canonical.has_value());
        auto const queue = directory / "queue.jsonl";
        auto const results = directory / "results.jsonl";
        writeFile(queue, "", std::ios::trunc);
        writeFile(results, "", std::ios::trunc);

        auto const fresh = resolveInputAgentFramePaths(
            "before.png",
            "after.png",
            *canonical,
            queue,
            results,
            "key"
        );
        REQUIRE(fresh.has_value());
        CHECK(fresh->before == *canonical / "before.png");
        CHECK(fresh->after == *canonical / "after.png");

        auto const aliased = resolveInputAgentFramePaths(
            "same.png",
            "same.png",
            *canonical,
            queue,
            results,
            "key"
        );
        REQUIRE_FALSE(aliased.has_value());
        CHECK(
            aliased.error().message().contains(
                "input-agent key out_before and out_after paths alias"
            )
        );

        writeFile(outputDirectory / "before.png", "stale", std::ios::trunc);
        auto const stale = resolveInputAgentFramePaths(
            "before.png",
            "after.png",
            *canonical,
            queue,
            results,
            "key"
        );
        REQUIRE_FALSE(stale.has_value());
        test_m0_demo::requireErrorKind(
            stale.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK(stale.error().message().contains("key out_before"));
        CHECK(
            stale.error().message().contains(
                "input-agent outputs must be fresh files"
            )
        );
    }

    TEST_CASE("m0 input-agent rejects settle times above the bounded wait")
    {
        auto const boundary = parsedClick(
            R"({"op":"click","x":1,"y":2,"out_before":"a.png","out_after":"b.png","settle_ms":5000})"
        );
        CHECK(boundary.settle == k_maximumInputAgentSettle);

        auto const result = parseInputAgentCommand(
            R"({"op":"click","x":1,"y":2,"out_before":"a.png","out_after":"b.png","settle_ms":5001})"
        );
        REQUIRE_FALSE(result.has_value());
        test_m0_demo::requireErrorKind(
            result.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK(result.error().message().contains("must not exceed 5000"));
    }

    TEST_CASE("m0 input-agent rejects malformed UTF-8 command strings")
    {
        auto command = std::string{R"({"op":"capture","out":"bad-)"};
        command += static_cast<char>(0xC3U);
        command += static_cast<char>(0x28U);
        command += R"(.png"})";

        auto const result = parseInputAgentCommand(command);
        REQUIRE_FALSE(result.has_value());
        test_m0_demo::requireErrorKind(
            result.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK(result.error().message().contains("valid UTF-8"));
    }

    TEST_CASE("m0 input-agent confines output paths to its canonical directory")
    {
        auto const token = std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();
        auto const outputDirectory = std::filesystem::temp_directory_path()
            / std::format("umbraflow-input-agent-output-{}", token);
        auto error = std::error_code{};
        auto const created = std::filesystem::create_directory(
            outputDirectory,
            error
        );
        REQUIRE(created);
        REQUIRE_FALSE(error);
        auto const cleanupPath = std::make_shared<std::filesystem::path const>(
            outputDirectory
        );
        auto const cleanup = scopeExit(
            [cleanupPath]() noexcept
            {
                auto cleanupError = std::error_code{};
                static_cast<void>(
                    std::filesystem::remove(*cleanupPath, cleanupError)
                );
            }
        );

        auto const canonicalDirectory = canonicalizeOutputDirectory(
            outputDirectory
        );
        REQUIRE(canonicalDirectory.has_value());
        auto const inside = resolveConfinedOutputPath(
            *canonicalDirectory,
            "inside.png",
            "test output"
        );
        REQUIRE(inside.has_value());
        CHECK(inside->parent_path() == *canonicalDirectory);

        for (auto const& escaped : std::array{
            std::filesystem::path{"../escape.png"},
            outputDirectory.parent_path() / "absolute-escape.png",
        })
        {
            auto const result = resolveConfinedOutputPath(
                *canonicalDirectory,
                escaped,
                "test output"
            );
            REQUIRE_FALSE(result.has_value());
            test_m0_demo::requireErrorKind(
                result.error(),
                AutomationErrorKind::InvalidResource
            );
        }
    }

    TEST_CASE("m0 input-agent queue reader preserves incremental line framing")
    {
        auto const directory = createTemporaryDirectory("input-agent-queue-lines");
        auto const cleanup = scopeExit(
            [cleanupPath = directory]() noexcept
            {
                removeAllBestEffort(cleanupPath);
            }
        );
        auto const queue = directory / "queue.jsonl";
        writeFile(queue, "", std::ios::trunc);

        auto reader = InputAgentQueueReader::create(queue, uintmax{});
        REQUIRE(reader.has_value());

        writeFile(queue, "partial", std::ios::app);
        auto first = reader->readAvailable();
        REQUIRE(first.has_value());
        CHECK(first->empty());

        writeFile(queue, "-one\r\nsecond\nthird", std::ios::app);
        auto second = reader->readAvailable();
        REQUIRE(second.has_value());
        REQUIRE(second->size() == 2U);
        CHECK((*second)[0].text == "partial-one");
        CHECK((*second)[1].text == "second");
        // "partial-one\r\n" is thirteen bytes and "second\n" seven, so the
        // position an entry reports is the offset just past its own newline.
        CHECK((*second)[0].consumedBytes == uintmax{13});
        CHECK((*second)[1].consumedBytes == uintmax{20});

        writeFile(queue, "-tail\nfour\n", std::ios::app);
        auto third = reader->readAvailable();
        REQUIRE(third.has_value());
        REQUIRE(third->size() == 2U);
        CHECK((*third)[0].text == "third-tail");
        CHECK((*third)[1].text == "four");
        CHECK((*third)[0].consumedBytes == uintmax{31});
        CHECK((*third)[1].consumedBytes == uintmax{36});
    }

    TEST_CASE("m0 input-agent queue reader rejects truncation")
    {
        auto const directory = createTemporaryDirectory("input-agent-queue-truncate");
        auto const cleanup = scopeExit(
            [cleanupPath = directory]() noexcept
            {
                removeAllBestEffort(cleanupPath);
            }
        );
        auto const queue = directory / "queue.jsonl";
        writeFile(queue, "first\n", std::ios::trunc);

        auto reader = InputAgentQueueReader::create(queue, uintmax{});
        REQUIRE(reader.has_value());
        auto const first = reader->readAvailable();
        REQUIRE(first.has_value());
        REQUIRE(first->size() == 1U);

        writeFile(queue, "x", std::ios::trunc);
        auto const truncated = reader->readAvailable();
        REQUIRE_FALSE(truncated.has_value());
        test_m0_demo::requireErrorKind(
            truncated.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK(truncated.error().message().contains("was truncated"));
    }

    TEST_CASE("m0 input-agent queue reader bounds unterminated commands")
    {
        auto const directory = createTemporaryDirectory("input-agent-queue-limit");
        auto const cleanup = scopeExit(
            [cleanupPath = directory]() noexcept
            {
                removeAllBestEffort(cleanupPath);
            }
        );
        auto const queue = directory / "queue.jsonl";
        writeFile(queue, "", std::ios::trunc);

        auto reader = InputAgentQueueReader::create(queue, uintmax{});
        REQUIRE(reader.has_value());
        auto constexpr maximumPendingBytes = std::size_t{1024} * 1024U;
        auto constexpr readsPerMebibyte = std::size_t{16};
        auto const maximumLine = std::string(maximumPendingBytes, 'a');
        writeFile(queue, maximumLine, std::ios::app);
        for (auto index = std::size_t{}; index < readsPerMebibyte; ++index)
        {
            auto const chunk = reader->readAvailable();
            REQUIRE(chunk.has_value());
            CHECK(chunk->empty());
        }

        writeFile(queue, "\n", std::ios::app);
        auto const boundary = reader->readAvailable();
        REQUIRE(boundary.has_value());
        REQUIRE(boundary->size() == 1U);
        CHECK(boundary->front().text.size() == maximumPendingBytes);

        auto oversizedLine = std::string(maximumPendingBytes + 1U, 'b');
        oversizedLine += '\n';
        writeFile(queue, oversizedLine, std::ios::app);
        for (auto index = std::size_t{}; index < readsPerMebibyte; ++index)
        {
            auto const chunk = reader->readAvailable();
            REQUIRE(chunk.has_value());
            CHECK(chunk->empty());
        }
        auto const oversized = reader->readAvailable();
        REQUIRE_FALSE(oversized.has_value());
        test_m0_demo::requireErrorKind(
            oversized.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK(oversized.error().message().contains("exceeding 1048576 bytes"));
    }

    TEST_CASE("m0 input-agent resumes a restarted queue instead of replaying it")
    {
        // The defect this guards: the agent tailed its queue by an offset that
        // lived only in memory, so a restart re-read from byte zero and drove
        // every command already in the file into the live target a second time.
        auto const directory = createTemporaryDirectory("input-agent-resume");
        auto const cleanup = scopeExit(
            [cleanupPath = directory]() noexcept
            {
                removeAllBestEffort(cleanupPath);
            }
        );
        auto const queue = directory / "queue.jsonl";
        auto const cursorPath = inputAgentQueueCursorPath(queue);
        auto const history = std::string{
            R"({"op":"key","key":"e"})"
            "\n"
            R"({"op":"key","key":"1"})"
            "\n"
            R"({"op":"key","key":"2"})"
            "\n"
        };
        writeFile(queue, history, std::ios::trunc);

        auto const firstExtent = measureInputAgentQueue(queue);
        REQUIRE(firstExtent.has_value());
        CHECK(firstExtent->framedCommands == uintmax{3});
        CHECK(firstExtent->framedBytes == history.size());
        CHECK(firstExtent->totalBytes == history.size());

        auto const absent = readInputAgentQueueCursor(cursorPath, queue);
        REQUIRE(absent.has_value());
        CHECK_FALSE(absent->has_value());
        auto const firstStart = resolveInputAgentQueueStart(
            *absent,
            *firstExtent,
            InputAgentQueueStart::Beginning
        );
        REQUIRE(firstStart.has_value());

        {
            auto cursor = InputAgentQueueCursor::open(
                cursorPath,
                queue,
                *firstStart
            );
            REQUIRE(cursor.has_value());
            auto reader = InputAgentQueueReader::create(
                queue,
                firstStart->consumedBytes
            );
            REQUIRE(reader.has_value());
            auto const entries = reader->readAvailable();
            REQUIRE(entries.has_value());
            REQUIRE(entries->size() == 3U);
            for (auto const& entry : *entries)
            {
                auto const advanced = cursor->advance(entry.consumedBytes);
                REQUIRE(advanced.has_value());
            }
            CHECK(cursor->position().consumedCommands == uintmax{3});
            CHECK(cursor->position().consumedBytes == history.size());
        }
        // The agent dies here, for one of the ordinary reasons: an idle
        // timeout, or a linker overwriting the executable underneath it.

        auto const recorded = readInputAgentQueueCursor(cursorPath, queue);
        REQUIRE(recorded.has_value());
        REQUIRE(recorded->has_value());
        CHECK((*recorded)->consumedCommands == uintmax{3});
        CHECK((*recorded)->consumedBytes == history.size());

        auto const secondExtent = measureInputAgentQueue(queue);
        REQUIRE(secondExtent.has_value());
        auto const secondStart = resolveInputAgentQueueStart(
            *recorded,
            *secondExtent,
            InputAgentQueueStart::Refuse
        );
        REQUIRE(secondStart.has_value());

        auto restarted = InputAgentQueueReader::create(
            queue,
            secondStart->consumedBytes
        );
        REQUIRE(restarted.has_value());
        auto const replayed = restarted->readAvailable();
        REQUIRE(replayed.has_value());
        CHECK(replayed->empty());

        // Resuming is not the same as going deaf: what the operator appends
        // after the restart still reaches the target exactly once.
        auto const appended = std::string{
            R"({"op":"key","key":"3"})"
            "\n"
        };
        writeFile(queue, appended, std::ios::app);
        auto const fresh = restarted->readAvailable();
        REQUIRE(fresh.has_value());
        REQUIRE(fresh->size() == 1U);
        CHECK((*fresh)[0].text == R"({"op":"key","key":"3"})");
        CHECK((*fresh)[0].consumedBytes == history.size() + appended.size());
    }

    TEST_CASE("m0 input-agent will not guess a start for an uncursored queue")
    {
        // A half-written fourth command trails the three framed ones, which is
        // what an operator appending to a live queue looks like mid-write.
        auto const extent = InputAgentQueueExtent{
            .framedBytes    = uintmax{69},
            .framedCommands = uintmax{3},
            .totalBytes     = uintmax{80},
        };
        auto const none = std::optional<InputAgentQueuePosition>{};

        auto const refused = resolveInputAgentQueueStart(
            none,
            extent,
            InputAgentQueueStart::Refuse
        );
        REQUIRE_FALSE(refused.has_value());
        test_m0_demo::requireErrorKind(
            refused.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK(refused.error().message().contains("--queue-start"));

        auto const beginning = resolveInputAgentQueueStart(
            none,
            extent,
            InputAgentQueueStart::Beginning
        );
        REQUIRE(beginning.has_value());
        CHECK(*beginning == InputAgentQueuePosition{});

        // End stops at the last newline rather than at the file's end, or the
        // operator's next append would be spliced onto half a command.
        auto const end = resolveInputAgentQueueStart(
            none,
            extent,
            InputAgentQueueStart::End
        );
        REQUIRE(end.has_value());
        CHECK(end->consumedBytes == uintmax{69});
        CHECK(end->consumedCommands == uintmax{3});

        // An empty queue is the one unambiguous case, so it never asks.
        auto const empty = resolveInputAgentQueueStart(
            none,
            InputAgentQueueExtent{},
            InputAgentQueueStart::Refuse
        );
        REQUIRE(empty.has_value());
        CHECK(*empty == InputAgentQueuePosition{});

        // A recorded position outranks the policy, so an operator who has
        // learned to type --queue-start cannot replay a session with it.
        auto const recorded = std::optional<InputAgentQueuePosition>{
            InputAgentQueuePosition{
                .consumedBytes    = uintmax{69},
                .consumedCommands = uintmax{3},
            }
        };
        auto const resumed = resolveInputAgentQueueStart(
            recorded,
            extent,
            InputAgentQueueStart::Beginning
        );
        REQUIRE(resumed.has_value());
        CHECK(*resumed == *recorded);

        auto const stale = std::optional<InputAgentQueuePosition>{
            InputAgentQueuePosition{
                .consumedBytes    = uintmax{81},
                .consumedCommands = uintmax{4},
            }
        };
        auto const rejected = resolveInputAgentQueueStart(
            stale,
            extent,
            InputAgentQueueStart::Beginning
        );
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().message().contains("truncated or replaced"));
    }

    TEST_CASE("m0 input-agent queue cursor refuses every partial write")
    {
        auto const queue = std::filesystem::path{"C:/session/queue.jsonl"};
        auto const path = inputAgentQueueCursorPath(queue);
        auto const text = serializeInputAgentQueueCursor(
            InputAgentQueueCursorRecord{
                .queue    = queue,
                .position = InputAgentQueuePosition{
                    .consumedBytes    = uintmax{4213},
                    .consumedCommands = uintmax{47},
                },
            }
        );

        // The directory is read by hand during annotation sessions, so the
        // file has to say what it means without the reader knowing the code.
        CHECK(text.starts_with("# umbraflow input-agent queue cursor\n"));
        CHECK(text.contains("\nqueue=C:/session/queue.jsonl\n"));
        CHECK(text.contains("\nconsumed-bytes=4213\n"));
        CHECK(text.ends_with("\nconsumed-commands=47\n"));

        auto const parsed = parseInputAgentQueueCursor(text, path);
        REQUIRE(parsed.has_value());
        CHECK(parsed->queue == queue);
        CHECK(parsed->position.consumedBytes == uintmax{4213});
        CHECK(parsed->position.consumedCommands == uintmax{47});

        // The cursor is rewritten whole, so every proper prefix of it is a
        // write that died halfway. None may parse: a shorter number that still
        // looks plausible is exactly what would replay the difference.
        for (auto length = std::size_t{}; length < text.size(); ++length)
        {
            auto const truncated = parseInputAgentQueueCursor(
                std::string_view{text}.substr(0, length),
                path
            );
            REQUIRE_FALSE(truncated.has_value());
        }
    }

    TEST_CASE("m0 input-agent queue cursor refuses a foreign or unreadable position")
    {
        auto const directory = createTemporaryDirectory("input-agent-cursor");
        auto const cleanup = scopeExit(
            [cleanupPath = directory]() noexcept
            {
                removeAllBestEffort(cleanupPath);
            }
        );
        auto const queue = directory / "queue.jsonl";
        auto const other = directory / "other.jsonl";
        auto const cursorPath = inputAgentQueueCursorPath(queue);
        CHECK(cursorPath == directory / "queue.jsonl.cursor");

        writeFile(
            cursorPath,
            serializeInputAgentQueueCursor(
                InputAgentQueueCursorRecord{
                    .queue    = other,
                    .position = InputAgentQueuePosition{
                        .consumedBytes    = uintmax{40},
                        .consumedCommands = uintmax{2},
                    },
                }
            ),
            std::ios::trunc
        );
        auto const foreign = readInputAgentQueueCursor(cursorPath, queue);
        REQUIRE_FALSE(foreign.has_value());
        test_m0_demo::requireErrorKind(
            foreign.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK(foreign.error().message().contains("records queue"));

        // A cursor nobody can read is a refusal, never an absence: reading it
        // as "no position recorded" is how a full replay would come back.
        writeFile(cursorPath, "consumed-bytes=40\n", std::ios::trunc);
        auto const unreadable = readInputAgentQueueCursor(cursorPath, queue);
        REQUIRE_FALSE(unreadable.has_value());
        CHECK(unreadable.error().message().contains("is missing"));
    }

    TEST_CASE("m0 input-agent file writer validates the opened output path")
    {
        auto const directory = createTemporaryDirectory("input-agent-output-handle");
        auto const cleanup = scopeExit(
            [cleanupPath = directory]() noexcept
            {
                removeAllBestEffort(cleanupPath);
            }
        );
        auto const allowedDirectory = directory / "allowed";
        auto const outsideDirectory = directory / "outside";
        auto const nestedRoot = allowedDirectory / "nested";
        auto const nestedDirectory = nestedRoot / "deeper";
        auto error = std::error_code{};
        REQUIRE(std::filesystem::create_directory(allowedDirectory, error));
        REQUIRE_FALSE(error);
        REQUIRE(std::filesystem::create_directory(outsideDirectory, error));
        REQUIRE_FALSE(error);
        REQUIRE(std::filesystem::create_directory(nestedRoot, error));
        REQUIRE_FALSE(error);
        REQUIRE(std::filesystem::create_directory(nestedDirectory, error));
        REQUIRE_FALSE(error);

        auto const canonicalAllowed = canonicalizeOutputDirectory(
            allowedDirectory
        );
        REQUIRE(canonicalAllowed.has_value());
        auto const outsidePath = outsideDirectory / "escaped.png";
        auto const rejected = platform::FileWriter::createExclusive(
            outsidePath,
            *canonicalAllowed
        );
        REQUIRE_FALSE(rejected.has_value());
        test_m0_demo::requireErrorKind(
            rejected.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK_FALSE(std::filesystem::exists(outsidePath));

        auto const unsafeComponent = allowedDirectory / "carrier.png:stream";
        auto const unsafe = platform::FileWriter::createExclusive(
            unsafeComponent,
            *canonicalAllowed
        );
        REQUIRE_FALSE(unsafe.has_value());
        test_m0_demo::requireErrorKind(
            unsafe.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK_FALSE(std::filesystem::exists(allowedDirectory / "carrier.png"));

        auto const allowedPath = nestedDirectory / "capture.png";
        auto const movedDirectory = outsideDirectory / "moved";
        {
            auto writer = platform::FileWriter::createExclusive(
                allowedPath,
                *canonicalAllowed
            );
            if (!writer.has_value())
            {
                FAIL(writer.error().message());
            }
            CHECK(std::filesystem::exists(allowedPath));

            auto const duplicate = platform::FileWriter::createExclusive(
                allowedPath,
                *canonicalAllowed
            );
            REQUIRE_FALSE(duplicate.has_value());
            CHECK(std::filesystem::exists(allowedPath));

            error.clear();
            std::filesystem::rename(nestedRoot, movedDirectory, error);
            CHECK(error);
            CHECK(std::filesystem::exists(allowedPath));
        }

        error.clear();
        std::filesystem::rename(nestedRoot, movedDirectory, error);
        CHECK_FALSE(error);
        CHECK(std::filesystem::exists(movedDirectory / "deeper" / "capture.png"));
    }

    TEST_CASE("m0 input-agent clears per-command audit state across many clicks")
    {
        auto audit          = AuditLog{};
        auto maximumRecords = std::size_t{};
        for (auto command = std::size_t{}; command < 10'000U; ++command)
        {
            for (auto message = uint32{}; message < 3U; ++message)
            {
                controller_detail::AuditLogAccess::record(
                    audit,
                    WindowHandle{0x1234},
                    message,
                    0U,
                    0
                );
            }
            maximumRecords = std::max(maximumRecords, audit.size());
            clearInputAgentCommandAudit(audit);
        }

        CHECK(maximumRecords == 3U);
        CHECK(audit.empty());
    }

    TEST_CASE("m0 input-agent click coordinates fail closed at client bounds")
    {
        auto const target = deliveryTarget();
        auto const now = instantAt(MonotonicInstant::Duration{10});
        auto const lease = observationLease(now);
        auto const valid = parsedClick(
            R"({"op":"click","x":799.9,"y":449.9,"out_before":"a.png","out_after":"b.png"})"
        );
        CHECK(
            validateInputAgentPointerAction(
                target,
                lease,
                Point<ClientSpace>{valid.x, valid.y},
                now
            )
        );

        auto const cases = std::array<std::string_view, 4>{
            R"({"op":"click","x":-0.1,"y":1,"out_before":"a.png","out_after":"b.png"})",
            R"({"op":"click","x":1,"y":-0.1,"out_before":"a.png","out_after":"b.png"})",
            R"({"op":"click","x":800,"y":1,"out_before":"a.png","out_after":"b.png"})",
            R"({"op":"click","x":1,"y":450,"out_before":"a.png","out_after":"b.png"})",
        };
        for (auto const line : cases)
        {
            auto const click = parsedClick(line);
            auto const result = validateInputAgentPointerAction(
                target,
                lease,
                Point<ClientSpace>{click.x, click.y},
                now
            );

            REQUIRE_FALSE(result.has_value());
            test_m0_demo::requireErrorKind(
                result.error(),
                AutomationErrorKind::ActionRejected
            );
        }
    }

    TEST_CASE("m0 input-agent click validation rejects a stale generation")
    {
        auto const next = TargetGeneration{}.next();
        REQUIRE(next.has_value());
        auto const now = instantAt(MonotonicInstant::Duration{10});
        auto const result = validateInputAgentPointerAction(
            deliveryTarget(*next),
            observationLease(now),
            Point<ClientSpace>{1.0F, 1.0F},
            now
        );

        REQUIRE_FALSE(result.has_value());
        test_m0_demo::requireErrorKind(
            result.error(),
            AutomationErrorKind::StaleObservation
        );
    }
}
