#include "core/Behind.h"

namespace eluvian
{
namespace
{

// winuser.h's ShowWindow commands and SetWindowPos flags (10.0.22621.0).
constexpr int kShowNormal = 1;                // SW_SHOWNORMAL: activates
constexpr int kShowMinimized = 2;             // SW_SHOWMINIMIZED: activates
constexpr int kShowMaximized = 3;             // SW_SHOWMAXIMIZED: activates, and has no non-activating twin
constexpr int kShowNoActivate = 4;            // SW_SHOWNOACTIVATE
constexpr int kShow = 5;                      // SW_SHOW: activates
constexpr int kShowMinNoActive = 7;           // SW_SHOWMINNOACTIVE
constexpr int kShowNa = 8;                    // SW_SHOWNA
constexpr int kRestore = 9;                   // SW_RESTORE: activates
constexpr std::uint32_t kNoZOrder = 0x0004;   // SWP_NOZORDER
constexpr std::uint32_t kNoActivate = 0x0010; // SWP_NOACTIVATE

} // namespace

int BehindShowCommand(int command)
{
    switch (command)
    {
    case kShowNormal:
    case kRestore:
        return kShowNoActivate;
    case kShowMinimized:
        return kShowMinNoActive;
    case kShow:
    case kShowMaximized: // shown as it is: a maximise would take the front
        return kShowNa;
    default:
        return command;
    }
}

std::uint32_t BehindPositionFlags(std::uint32_t flags)
{
    return flags | kNoActivate | kNoZOrder;
}

} // namespace eluvian
