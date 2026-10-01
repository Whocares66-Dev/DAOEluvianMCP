#include "core/LogTail.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>

using namespace eluvian;

namespace
{

// A file of its own under the temporary folder, removed after.
class TempLog
{
  public:
    TempLog()
        : path_(std::filesystem::temp_directory_path() /
                ("eluvian_test_" + std::to_string(std::random_device{}()) + ".log"))
    {
    }
    ~TempLog()
    {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }
    TempLog(const TempLog &) = delete;
    TempLog &operator=(const TempLog &) = delete;

    void Write(const std::string &text, bool append = true) const
    {
        std::ofstream out(path_, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
        out << text;
    }
    [[nodiscard]] const std::filesystem::path &Path() const
    {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("a read returns the complete lines and the cursor after them", "[log]")
{
    const TempLog log;
    log.Write("one\r\ntwo\nthree, still being writ");
    auto page = ReadLog(log.Path(), 0, std::nullopt, 100, 1 << 20);
    CHECK(page.lines == std::vector<std::string>{"one", "two"});
    CHECK(page.next == 9);
    CHECK_FALSE(page.restarted);
    CHECK_FALSE(page.truncated);

    log.Write("ten\nfour\n");
    page = ReadLog(log.Path(), page.next, std::nullopt, 100, 1 << 20);
    CHECK(page.lines == std::vector<std::string>{"three, still being written", "four"});
    CHECK(page.next == page.size);
}

TEST_CASE("a filter keeps the lines it matches, and the cursor passes the rest", "[log]")
{
    const TempLog log;
    log.Write("round 1\nAedan: attack\nround 2\nDog: attack\n");
    const auto page = ReadLog(log.Path(), 0, std::regex("attack"), 100, 1 << 20);
    CHECK(page.lines == std::vector<std::string>{"Aedan: attack", "Dog: attack"});
    CHECK(page.next == page.size);
}

TEST_CASE("the line limit stops before the line it would exceed", "[log]")
{
    const TempLog log;
    log.Write("a\nb\nc\n");
    auto page = ReadLog(log.Path(), 0, std::nullopt, 2, 1 << 20);
    CHECK(page.lines == std::vector<std::string>{"a", "b"});
    CHECK(page.truncated);
    page = ReadLog(log.Path(), page.next, std::nullopt, 2, 1 << 20);
    CHECK(page.lines == std::vector<std::string>{"c"});
    CHECK_FALSE(page.truncated);
}

TEST_CASE("the byte limit leaves the rest for the next read", "[log]")
{
    const TempLog log;
    log.Write("aaaa\nbbbb\ncccc\n");
    auto page = ReadLog(log.Path(), 0, std::nullopt, 100, 7);
    CHECK(page.lines == std::vector<std::string>{"aaaa"});
    CHECK(page.truncated);
    page = ReadLog(log.Path(), page.next, std::nullopt, 100, 1 << 20);
    CHECK(page.lines == std::vector<std::string>{"bbbb", "cccc"});
}

TEST_CASE("a file shorter than the cursor began again, and is read from its start", "[log]")
{
    const TempLog log;
    log.Write("a long first run\nof the game\n");
    const auto first = ReadLog(log.Path(), 0, std::nullopt, 100, 1 << 20);
    log.Write("restarted\n", false);
    const auto page = ReadLog(log.Path(), first.next, std::nullopt, 100, 1 << 20);
    CHECK(page.restarted);
    CHECK(page.lines == std::vector<std::string>{"restarted"});
}

TEST_CASE("a missing file is an error", "[log]")
{
    CHECK_THROWS_AS(ReadLog("C:/no/such/eluvian.log", 0, std::nullopt, 1, 1), std::runtime_error);
}
