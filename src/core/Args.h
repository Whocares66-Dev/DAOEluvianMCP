// A tool's arguments, read with errors an agent can act on: what was wrong,
// what was expected, and where to find a valid value. "Failure says what to
// do" (dev/DESIGN.md).
//
// Object ids are written as the logs write them, "0x7d9", in every answer;
// an argument may be that, a decimal string, or a number.
#pragma once

#include "core/Tool.h"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace eluvian
{

// "0x7d9": the object id as turnbased.log and DAOLib's logs write it.
std::string FormatId(std::uint32_t id);

// An object id from "0x7d9", "2009" or 2009; nullopt for anything else.
std::optional<std::uint32_t> ParseId(const Json &value);

class Args
{
  public:
    // Null reads as no arguments; anything but an object is a ToolError.
    Args(const Json &args, std::string_view tool);

    [[nodiscard]] bool Has(std::string_view key) const;
    [[nodiscard]] const Json &Raw() const
    {
        return args_;
    }

    [[nodiscard]] std::string String(std::string_view key) const;
    [[nodiscard]] std::string String(std::string_view key, std::string fallback) const;
    [[nodiscard]] double Number(std::string_view key) const;
    [[nodiscard]] double Number(std::string_view key, double fallback) const;
    [[nodiscard]] std::int64_t Integer(std::string_view key, std::int64_t fallback, std::int64_t min,
                                       std::int64_t max) const;
    [[nodiscard]] bool Bool(std::string_view key, bool fallback) const;

    // An object id; `what` names what it should be, for the error ("a
    // creature, as inspect kind=creatures lists them").
    [[nodiscard]] std::uint32_t Id(std::string_view key, std::string_view what) const;
    [[nodiscard]] std::optional<std::uint32_t> OptionalId(std::string_view key, std::string_view what) const;

    // One of `values`; the error lists them.
    [[nodiscard]] std::string Choice(std::string_view key, std::initializer_list<std::string_view> values) const;
    [[nodiscard]] std::string Choice(std::string_view key, std::initializer_list<std::string_view> values,
                                     std::string_view fallback) const;

  private:
    [[nodiscard]] const Json *Find(std::string_view key) const;
    [[noreturn]] void Fail(std::string_view key, std::string_view expected) const;

    Json args_;
    std::string tool_;
};

} // namespace eluvian
