// A test binary that aborts or fails a CRT assert (a mutant's, say) must
// fail and exit, not wait on a dialog: the debug CRT's box from a mutant run
// sat over the game for ten minutes in the desktop play test (2026-09-30),
// and a run left waiting holds tools/mutants.py up. The reports go to stderr
// instead, and abort skips Windows Error Reporting.
#if defined(_MSC_VER)

#include <crtdbg.h>
#include <cstdlib>
#include <initializer_list>

namespace
{

const bool kQuiet = [] {
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    for (const int kind : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT})
    {
        _CrtSetReportMode(kind, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        _CrtSetReportFile(kind, _CRTDBG_FILE_STDERR);
    }
    return true;
}();

} // namespace

#endif
