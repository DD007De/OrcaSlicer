#ifndef slic3r_GUI_SSWCPApiMode_hpp_
#define slic3r_GUI_SSWCPApiMode_hpp_

// API mode without dialogs, used by the sw_Slicer* SSWCP commands.
//
// API mode is OFF by default: normal GUI behaviour does not change. Only the sw_Slicer*
// handlers enable it, around their own call, with an RAII Scope.
// At the places in the app code where a modal dialog would otherwise appear, the app picks the
// dialog's default answer in API mode and records with note() which choice was made
// automatically. The handler returns notes() in its reply.
//
// Header-only: inline functions with static locals. Everything runs on the GUI thread, so no
// synchronization is needed.

#include <algorithm>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace sswcp_api {

namespace detail {
inline int& depth()
{
    static int d = 0;
    return d;
}
inline std::vector<std::string>& notes_mut()
{
    static std::vector<std::string> n;
    return n;
}
} // namespace detail

// Is API mode active (inside at least one Scope)?
inline bool active() { return detail::depth() > 0; }

// Record an automatically made choice (only in API mode). A text that is already recorded is not
// added again (settings checks can run several times during one command).
inline void note(const std::string& text)
{
    if (!active())
        return;
    std::vector<std::string>& n = detail::notes_mut();
    if (std::find(n.begin(), n.end(), text) == n.end())
        n.push_back(text);
}

// The recorded choices of the current (or most recently closed) outermost scope.
inline const std::vector<std::string>& notes() { return detail::notes_mut(); }

// RAII: enables API mode while the scope is alive. Scopes may be nested; the outermost
// scope clears the notes of a previous call when it is entered.
struct Scope
{
    Scope()
    {
        if (detail::depth()++ == 0)
            detail::notes_mut().clear();
    }
    ~Scope() { --detail::depth(); }
    Scope(const Scope&)            = delete;
    Scope& operator=(const Scope&) = delete;
};

}}} // namespace Slic3r::GUI::sswcp_api

#endif // slic3r_GUI_SSWCPApiMode_hpp_
