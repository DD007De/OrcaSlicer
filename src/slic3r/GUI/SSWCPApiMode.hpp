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
// Inline functions with static locals; only transfer_options() is defined elsewhere (Tab.cpp).
// Everything runs on the GUI thread, so no synchronization is needed.

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {
class PresetCollection;
} // namespace Slic3r

namespace Slic3r { namespace GUI { namespace sswcp_api {

// The answer to the unsaved-changes dialog of a preset switch (Tab::may_discard_current_dirty_preset())
// in API mode. Cancel (the default): the switch is cancelled. Transfer: like the button "Transfer" of
// that dialog, the unsaved changes move to the newly selected preset; only where the dialog shows the
// button. A handler sets it with a PresetChoiceScope, inside a Scope.
enum class PresetChoice { Cancel, Transfer };

// The answer to the STEP mesh dialog (StepMeshDialog): the three values the dialog returns.
struct StepMeshAnswer
{
    double linear_deflection = 0.003;
    double angle_deflection  = 0.5;
    bool   split_compound    = false;
};

// The answers to the import questions of Plater::priv::load_files() and Plater::load_project() in API mode.
// Unset (the default): the default answer of API mode (No, with a note; the STEP mesh settings of the
// preferences). A handler sets them with a LoadChoicesScope, inside a Scope; the app notes every answer it uses.
// The *_asked flags are set by the app (in API mode) when the question comes up, so the handler can tell an
// answer that was not needed; it reads them before the LoadChoicesScope ends.
struct LoadChoices
{
    std::optional<bool>           single_object;         // "Multi-part object detected": load as one object with parts
    std::optional<bool>           scale_small_to_mm;     // "Object too small": scale to millimeters
    std::optional<StepMeshAnswer> step;                  // the STEP mesh dialog
    bool                          geometry_only = false; // ProjectDropDialog "Import geometry only" (load_project(path, "-"))
    mutable bool                  single_object_asked = false; // "Multi-part object detected" came up
    mutable bool                  scale_asked         = false; // "Object too small" came up
};

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
inline PresetChoice& preset_choice_mut()
{
    static PresetChoice c = PresetChoice::Cancel;
    return c;
}
inline LoadChoices& load_choices_mut()
{
    static LoadChoices c;
    return c;
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

// The answer to the unsaved-changes dialog of a preset switch; always Cancel outside API mode.
inline PresetChoice preset_choice() { return active() ? detail::preset_choice_mut() : PresetChoice::Cancel; }

// The answers to the import questions; all unset outside API mode.
inline const LoadChoices& load_choices()
{
    static const LoadChoices none;
    return active() ? detail::load_choices_mut() : none;
}

// RAII: enables API mode while the scope is alive. Scopes may be nested; the outermost
// scope clears the notes of a previous call when it is entered, and it starts and ends with
// the preset choice Cancel and without load choices.
struct Scope
{
    Scope()
    {
        if (detail::depth()++ == 0) {
            detail::notes_mut().clear();
            detail::preset_choice_mut() = PresetChoice::Cancel;
            detail::load_choices_mut()  = LoadChoices();
        }
    }
    ~Scope()
    {
        if (--detail::depth() == 0) {
            detail::preset_choice_mut() = PresetChoice::Cancel;
            detail::load_choices_mut()  = LoadChoices();
        }
    }
    Scope(const Scope&)            = delete;
    Scope& operator=(const Scope&) = delete;
};

// RAII: sets the answer to the unsaved-changes dialog while it is alive (use it inside a Scope).
struct PresetChoiceScope
{
    explicit PresetChoiceScope(PresetChoice choice) : m_previous(detail::preset_choice_mut()) { detail::preset_choice_mut() = choice; }
    ~PresetChoiceScope() { detail::preset_choice_mut() = m_previous; }
    PresetChoiceScope(const PresetChoiceScope&)            = delete;
    PresetChoiceScope& operator=(const PresetChoiceScope&) = delete;

private:
    PresetChoice m_previous;
};

// RAII: sets the answers to the import questions while it is alive (use it inside a Scope).
struct LoadChoicesScope
{
    explicit LoadChoicesScope(const LoadChoices& choices) : m_previous(detail::load_choices_mut()) { detail::load_choices_mut() = choices; }
    ~LoadChoicesScope() { detail::load_choices_mut() = m_previous; }
    LoadChoicesScope(const LoadChoicesScope&)            = delete;
    LoadChoicesScope& operator=(const LoadChoicesScope&) = delete;

private:
    LoadChoices m_previous;
};

// The options that the button "Transfer" of the unsaved-changes dialog moves for 'presets': the
// options UnsavedChangesDialog::update_tree() lists for that collection, all of them selected (this
// app has no check boxes in the dialog). Printer: "extruders_count" when the number of extruders
// changed. Keys of one element are "key#n". Defined in Tab.cpp.
std::vector<std::string> transfer_options(const PresetCollection& presets);

}}} // namespace Slic3r::GUI::sswcp_api

#endif // slic3r_GUI_SSWCPApiMode_hpp_
