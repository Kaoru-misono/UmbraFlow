#include <task/task-loader.hpp>

#include <core/error/result.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <vector>

namespace uf::task
{
    namespace
    {
        [[nodiscard]]
        auto uniqueProjectRoot() -> std::filesystem::path
        {
            static auto counter = 0;
            ++counter;
            return std::filesystem::temp_directory_path()
                / ("uf-task-loader-" + std::to_string(counter));
        }

        void writeTask(
            std::filesystem::path const& root,
            std::string_view name,
            std::string_view source
        )
        {
            auto const tasksDir = root / "tasks";
            std::filesystem::create_directories(tasksDir);
            auto stream = std::ofstream{
                tasksDir / (std::string{name} + ".luau"),
                std::ios::binary | std::ios::trunc
            };
            REQUIRE(stream.is_open());
            stream << source;
        }
    }

    TEST_CASE("loadTask loads a name-addressed task and hashes its source")
    {
        auto const root = uniqueProjectRoot();
        std::filesystem::remove_all(root);
        writeTask(root, "daily", "return 1\n");

        auto const loaded = loadTask(root, "daily");
        REQUIRE(loaded.has_value());
        CHECK(loaded->name == "daily");
        CHECK(loaded->source == "return 1\n");
        // A SHA-256 digest is 32 bytes, so its hex form is 64 characters.
        CHECK(loaded->hash.hex().size() == 64U);

        std::filesystem::remove_all(root);
    }

    TEST_CASE("loadTask yields the same hash for identical content across projects")
    {
        auto const rootA = uniqueProjectRoot();
        auto const rootB = uniqueProjectRoot();
        std::filesystem::remove_all(rootA);
        std::filesystem::remove_all(rootB);
        writeTask(rootA, "daily", "return 41\n");
        writeTask(rootB, "daily", "return 41\n");

        auto const a = loadTask(rootA, "daily");
        auto const b = loadTask(rootB, "daily");
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        CHECK(a->hash == b->hash);

        std::filesystem::remove_all(rootA);
        std::filesystem::remove_all(rootB);
    }

    TEST_CASE("loadTask hash changes when a single byte of the source changes")
    {
        auto const root = uniqueProjectRoot();
        std::filesystem::remove_all(root);
        writeTask(root, "daily", "return 41\n");
        auto const before = loadTask(root, "daily");
        REQUIRE(before.has_value());

        writeTask(root, "daily", "return 42\n");
        auto const after = loadTask(root, "daily");
        REQUIRE(after.has_value());

        CHECK(after->hash != before->hash);

        std::filesystem::remove_all(root);
    }

    TEST_CASE("loadTask rejects an unsafe or empty task name before any read")
    {
        // The project need not exist: every one of these names must be refused by
        // the name check, never resolved to a path that could escape tasks/.
        auto const root = uniqueProjectRoot();
        std::filesystem::remove_all(root);

        auto const names = std::vector<std::string_view>{
            "",
            ".",
            "..",
            "a/b",
            "a\\b",
            "../daily",
            "da.ily",
            "C:/abs",
        };

        for (auto const& name : names)
        {
            auto const loaded = loadTask(root, name);
            REQUIRE_FALSE(loaded.has_value());
            CHECK(
                automationErrorKind(loaded.error())
                == AutomationErrorKind::InvalidResource
            );
        }
    }

    TEST_CASE("loadTask reports a missing task and lists the ones that exist")
    {
        auto const root = uniqueProjectRoot();
        std::filesystem::remove_all(root);
        writeTask(root, "daily", "return 1\n");
        writeTask(root, "weekly", "return 2\n");

        auto const loaded = loadTask(root, "monthly");
        REQUIRE_FALSE(loaded.has_value());
        CHECK(
            automationErrorKind(loaded.error())
            == AutomationErrorKind::InvalidResource
        );

        auto const message = loaded.error().message();
        CHECK(message.find("monthly") != std::string::npos);
        CHECK(message.find("daily") != std::string::npos);
        CHECK(message.find("weekly") != std::string::npos);

        std::filesystem::remove_all(root);
    }

    TEST_CASE("loadTask reports a missing task when the tasks directory is absent")
    {
        auto const root = uniqueProjectRoot();
        std::filesystem::remove_all(root);

        auto const loaded = loadTask(root, "daily");
        REQUIRE_FALSE(loaded.has_value());
        CHECK(
            automationErrorKind(loaded.error())
            == AutomationErrorKind::InvalidResource
        );
    }

    TEST_CASE("luauRuntimeVersion is a stable non-empty stamp")
    {
        auto const version = luauRuntimeVersion();
        CHECK_FALSE(version.empty());
        CHECK(version == luauRuntimeVersion());
        CHECK(version.find("luau") != std::string::npos);
    }
}
