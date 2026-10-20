// sw_Slicer* commands of SSWCP_SliceProject_Instance: live control of the slicer
// (presets, slicing, preview and the 3D toolbar) over the SSWCP protocol.
#include "SSWCP.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "nlohmann/json.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/IMSlider.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/GUI_Geometry.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoPainterBase.hpp"
#include "slic3r/GUI/Gizmos/GizmoObjectManipulation.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoEmboss.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoMeasure.hpp"
#include "slic3r/GUI/Jobs/Job.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/SSWCPApiMode.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/WebPreprintDialog.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "slic3r/Utils/WxFontUtils.hpp"
#include "slic3r/Utils/PrintHost.hpp"
#include "slic3r/Utils/ASCIIFolding.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/BrimEarsPoint.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/MultiPoint.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/TextConfiguration.hpp"
#include "libslic3r/EmbossShape.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <wx/font.h>
#include <wx/fontenum.h>
#include <wx/weakref.h>
#ifdef _WIN32
// SetForegroundWindow() for the send dialog of a cloud-connected printer.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace nlohmann;

namespace Slic3r { namespace GUI {

// --- Presets ------------------------------------------------------------------
// Map a type string ("print"/"filament"/"printer") to a Preset::Type.
// Throws std::runtime_error for an unknown type; the caller reports it as a regular error.
static Preset::Type sw_slicer_preset_type(const std::string& type)
{
    if (type == "print")    return Preset::TYPE_PRINT;
    if (type == "filament") return Preset::TYPE_FILAMENT;
    if (type == "printer")  return Preset::TYPE_PRINTER;
    throw std::runtime_error("unknown config type '" + type + "' (expected print/filament/printer)");
}

// Side-effect-free probe: confirms that this build supports the sw_Slicer* API and returns
// the sorted list of available sw_Slicer* commands. Clients should detect individual
// commands through 'commands'; 'phase' is only a coarse API revision number.
void SSWCP_SliceProject_Instance::sw_SlicerPing()
{
    try {
        m_res_data["patched"]  = true;
        m_res_data["phase"]    = 7;
        m_res_data["commands"] = SSWCP::slicer_commands();
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail();
    }
}

// Read the effective value(s) of the requested keys from the active preset.
// Unknown keys are returned as JSON null (not an error).
void SSWCP_SliceProject_Instance::sw_SlicerGetConfig()
{
    try {
        std::string type = m_param_data.at("type").get<std::string>();
        Tab* tab = wxGetApp().get_tab(sw_slicer_preset_type(type));
        if (tab == nullptr || tab->get_config() == nullptr) {
            handle_general_fail(-1, "no active tab/config for this type");
            return;
        }
        const DynamicPrintConfig* cfg = tab->get_config();
        for (const auto& k : m_param_data.at("keys")) {
            std::string key = k.get<std::string>();
            if (cfg->optptr(key) != nullptr)
                m_res_data[key] = cfg->opt_serialize(key);
            else
                m_res_data[key] = nullptr;
        }
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Set one key and apply it to the active preset (like a manual edit in the GUI; the preset
// is NOT saved). Returns {key, old, new} so the caller can verify the change. An invalid
// value yields a regular error (set_deserialize_strict throws), not a crash.
void SSWCP_SliceProject_Instance::sw_SlicerSetConfig()
{
    try {
        std::string type  = m_param_data.at("type").get<std::string>();
        std::string key   = m_param_data.at("key").get<std::string>();
        std::string value = m_param_data.at("value").get<std::string>();
        Tab* tab = wxGetApp().get_tab(sw_slicer_preset_type(type));
        if (tab == nullptr || tab->get_config() == nullptr) {
            handle_general_fail(-1, "no active tab/config for this type");
            return;
        }
        const DynamicPrintConfig* cfg = tab->get_config();
        std::string old_val = (cfg->optptr(key) != nullptr) ? cfg->opt_serialize(key) : std::string();
        // Validate on a COPY: an invalid value yields an error without changing anything.
        DynamicPrintConfig probe = *cfg;
        try {
            probe.set_deserialize_strict(key, value);
        } catch (std::exception& e) {
            std::string err = "invalid value for '" + key + "': " + e.what();
            handle_general_fail(-1, wxString::FromUTF8(err.c_str()));
            return;
        }
        std::string new_val = probe.opt_serialize(key);
        m_res_data["key"] = key;
        m_res_data["old"] = old_val;
        m_res_data["new"] = new_val;
        // Reply FIRST, then apply lightly. Tab::load_config is skipped on purpose: it calls
        // update() -> on_config_changed, the heavy Plater reaction that blocks the UI for
        // ~0.3 s. Instead only the value is set, the preset is marked as modified and the
        // field is refreshed, in a separate CallAfter so the UI can repaint in between.
        // The new value is picked up by the next explicit slice (sw_SlicerSlice).
        Preset::Type ptype = sw_slicer_preset_type(type);
        send_to_js();
        finish_job();
        wxGetApp().CallAfter([ptype, key, value]() {
            Tab* t = wxGetApp().get_tab(ptype);
            if (t == nullptr || t->get_config() == nullptr)
                return;
            try {
                t->get_config()->set_deserialize_strict(key, value);
                t->update_dirty();   // preset "(modified)" + dirty markers
                t->reload_config();  // refreshes the visible field
            } catch (std::exception&) {
            }
        });
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- Slicing, status and statistics -------------------------------------------

// Start a (re)slice of the current plate. Returns immediately; slicing runs in the
// background (poll with sw_SlicerGetStatus). The return value of reslice() says
// nothing about success and is therefore ignored.
void SSWCP_SliceProject_Instance::sw_SlicerSlice()
{
    try {
        wxGetApp().plater()->reslice();
        m_res_data["started"] = true;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Defined with sw_SlicerSendToPrinter: the state of the last upload, or null if there was none,
// and the one-time result notification of a finished upload (shown here, on the GUI thread).
static json sw_slicer_upload_status_json();
static void sw_slicer_upload_notify();

// Poll status: whether a slice is running, a rough progress (0/100, -1 = invalid),
// and whether the slice result is valid. Also whether a G-code export or a "slice all" run is
// in progress, the last upload to the printer (upload, if any) and with all:true every plate.
void SSWCP_SliceProject_Instance::sw_SlicerGetStatus()
{
    try {
        Plater*    plater = wxGetApp().plater();
        PartPlate* plate  = plater->get_partplate_list().get_curr_plate();
        m_res_data["slicing"] = plater->is_background_process_slicing();
        m_res_data["percent"] = (plate != nullptr) ? static_cast<int>(plate->get_slicing_percent()) : 0;
        m_res_data["valid"]   = (plate != nullptr) ? plate->is_slice_result_valid() : false;
        m_res_data["exporting"]   = plater->is_export_gcode_scheduled();
        m_res_data["slicing_all"] = plater->is_slicing_all();
        const bool all = m_param_data.count("all") && m_param_data.at("all").is_boolean() && m_param_data.at("all").get<bool>();
        if (all) {
            PartPlateList& plates = plater->get_partplate_list();
            json           list   = json::array();
            for (int i = 0; i < plates.get_plate_count(); ++i) {
                PartPlate* p = plates.get_plate(i);
                json       e;
                e["plate"]   = i;
                e["empty"]   = p->empty();
                e["valid"]   = p->is_slice_result_valid();
                e["ready"]   = p->is_slice_result_ready_for_print();
                e["percent"] = static_cast<int>(p->get_slicing_percent());
                list.push_back(e);
            }
            m_res_data["plates"]    = list;
            m_res_data["all_ready"] = plates.is_all_slice_results_ready_for_print();
        }
        const json upload = sw_slicer_upload_status_json();
        if (!upload.is_null())
            m_res_data["upload"] = upload;
        sw_slicer_upload_notify();
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Print time (Normal mode, seconds) and material volume per extruder (mm^3).
// Without a valid slice result: time_s = null and an empty volume list.
void SSWCP_SliceProject_Instance::sw_SlicerGetStats()
{
    try {
        PartPlate*            plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
        GCodeProcessorResult* res   = (plate != nullptr) ? plate->get_slice_result() : nullptr;
        if (plate == nullptr || res == nullptr || !plate->is_slice_result_valid()) {
            m_res_data["time_s"] = nullptr;
            m_res_data["per_extruder_volume_mm3"] = json::array();
            send_to_js();
            finish_job();
            return;
        }
        const PrintEstimatedStatistics& ps = res->print_statistics;
        m_res_data["time_s"] =
            ps.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time;
        json vols = json::array();
        for (const auto& kv : ps.total_volumes_per_extruder)
            vols.push_back(kv.second);
        m_res_data["per_extruder_volume_mm3"] = vols;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- View and layer -----------------------------------------------------------

// Switch between the 3D view, the Preview and (if available) the assembly view.
void SSWCP_SliceProject_Instance::sw_SlicerShowView()
{
    try {
        std::string view = m_param_data.at("view").get<std::string>();
        if (view != "3D" && view != "Preview" && view != "Assemble") {
            handle_general_fail(-1, "view must be '3D', 'Preview' or 'Assemble'");
            return;
        }
        if (view == "Assemble" && !wxGetApp().plater()->has_assmeble_view()) {
            handle_general_fail(-1, "this app has no assembly view");
            return;
        }
        wxGetApp().plater()->select_view_3D(view);
        m_res_data["view"] = view;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Move the layer preview to layer N (1-based) or to the layer closest to height z.
// Sets the upper slider value and requests an extra frame so the change becomes visible.
void SSWCP_SliceProject_Instance::sw_SlicerGoToLayer()
{
    try {
        GLCanvas3D* canvas = wxGetApp().plater()->get_preview_canvas3D();
        if (canvas == nullptr) {
            handle_general_fail(-1, "no preview canvas (open the Preview first)");
            return;
        }
        IMSlider* slider = canvas->get_gcode_viewer().get_layers_slider();
        if (slider == nullptr) {
            handle_general_fail(-1, "no layer slider available (has the plate been sliced?)");
            return;
        }
        const int maxv = slider->GetMaxValue();
        const std::vector<double>& zs = canvas->get_gcode_layers_zs();
        int tick = 0;
        if (m_param_data.count("layer")) {
            tick = m_param_data.at("layer").get<int>() - 1;  // 1-based layer -> 0-based tick
        } else if (m_param_data.count("z")) {
            double z = m_param_data.at("z").get<double>();
            double best = 1e18;
            for (size_t i = 0; i < zs.size(); ++i) {
                double d = zs[i] - z;
                if (d < 0) d = -d;
                if (d < best) { best = d; tick = static_cast<int>(i); }
            }
        } else {
            handle_general_fail(-1, "specify 'layer' (1-based) or 'z'");
            return;
        }
        if (tick < 0) tick = 0;
        if (tick > maxv) tick = maxv;
        slider->SetHigherValue(tick);
        canvas->set_as_dirty();
        canvas->request_extra_frame();
        m_res_data["layer"]       = tick + 1;
        m_res_data["layer_count"] = maxv + 1;
        if (tick >= 0 && tick < static_cast<int>(zs.size()))
            m_res_data["z_upper"] = zs[tick];
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- 3D toolbar: common helpers -----------------------------------------------
// All helpers throw std::runtime_error with a readable message; the handlers catch it
// and return it as a regular error (handle_general_fail).

// The 3D canvas (view3D). Throws if it is not available (yet).
static GLCanvas3D* sw_slicer_canvas()
{
    Plater* plater = wxGetApp().plater();
    GLCanvas3D* canvas = (plater != nullptr) ? plater->get_view3D_canvas3D() : nullptr;
    if (canvas == nullptr)
        throw std::runtime_error("no 3D canvas available");
    return canvas;
}

// Resolve the object index: params["object"] if present, otherwise the (single) object of the
// current selection. Throws for an empty plate, an index out of range or no selection.
static int sw_slicer_resolve_object(const json& params)
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        throw std::runtime_error("no active plater");
    const Model& model = plater->model();
    const int n = static_cast<int>(model.objects.size());
    if (n == 0)
        throw std::runtime_error("no model on the plate (load a model first)");
    if (params.count("object") && !params.at("object").is_null()) {
        const int obj = params.at("object").get<int>();
        if (obj < 0 || obj >= n)
            throw std::runtime_error("object " + std::to_string(obj) + " does not exist (there are " +
                                     std::to_string(n) + " objects, 0-based)");
        return obj;
    }
    const int obj = sw_slicer_canvas()->get_selection().get_object_idx();
    if (obj < 0 || obj >= n)
        throw std::runtime_error("no object specified and not exactly one object is selected");
    return obj;
}

// Check instance/volume against the model (-1 = not specified). Throws on error.
static void sw_slicer_check_indices(int obj, int inst, int vol)
{
    const ModelObject* mo = wxGetApp().plater()->model().objects[obj];
    if (inst >= static_cast<int>(mo->instances.size()) || inst < -1)
        throw std::runtime_error("instance " + std::to_string(inst) + " does not exist (object " + std::to_string(obj) +
                                 " has " + std::to_string(mo->instances.size()) + " instance(s))");
    if (vol >= static_cast<int>(mo->volumes.size()) || vol < -1)
        throw std::runtime_error("volume " + std::to_string(vol) + " does not exist (object " + std::to_string(obj) +
                                 " has " + std::to_string(mo->volumes.size()) + " volume(s))");
}

// Select object/instance/volume, both in the 3D canvas and in the object list.
// vol/inst = -1 means "not specified".
static void sw_slicer_select(int obj, int inst, int vol)
{
    GLCanvas3D* canvas = sw_slicer_canvas();
    Selection&  s      = canvas->get_selection();
    s.clear();
    if (vol >= 0)
        s.add_volume(static_cast<unsigned int>(obj), static_cast<unsigned int>(vol), std::max(inst, 0), true);
    else if (inst >= 0)
        s.add_instance(static_cast<unsigned int>(obj), static_cast<unsigned int>(inst), true);
    else
        s.add_object(static_cast<unsigned int>(obj), true);
    wxGetApp().obj_list()->update_selections();
    canvas->update_gizmos_on_off_state();
    canvas->set_as_dirty();
}

// Names of the toolbar gizmos as used by the API.
struct SwSlicerGizmoName
{
    const char*            name;
    GLGizmosManager::EType type;
};
static const SwSlicerGizmoName sw_slicer_gizmo_names[] = {
    {"move", GLGizmosManager::Move},
    {"rotate", GLGizmosManager::Rotate},
    {"scale", GLGizmosManager::Scale},
    {"flatten", GLGizmosManager::Flatten},
    {"cut", GLGizmosManager::Cut},
    {"mesh_boolean", GLGizmosManager::MeshBoolean},
    {"support", GLGizmosManager::FdmSupports},
    {"seam", GLGizmosManager::Seam},
    {"fuzzy_skin", GLGizmosManager::FuzzySkin},
    {"color", GLGizmosManager::MmSegmentation},
    {"text", GLGizmosManager::Emboss},
    {"measure", GLGizmosManager::Measure},
    {"assemble", GLGizmosManager::Assembly},
    {"brim_ears", GLGizmosManager::BrimEars},
};

static std::string sw_slicer_gizmo_name(GLGizmosManager::EType type)
{
    for (const SwSlicerGizmoName& g : sw_slicer_gizmo_names)
        if (g.type == type)
            return g.name;
    // Only used in error messages; these cannot be opened through the API.
    if (type == GLGizmosManager::Svg)
        return "svg";
    if (type == GLGizmosManager::Simplify)
        return "simplify";
    return "tool " + std::to_string(static_cast<int>(type));
}

// Refuse if a gizmo other than {none, move, rotate, scale} is open: the API never closes the
// user's tool unasked, and an open (paint) gizmo could overwrite a change.
static void sw_slicer_require_free_gizmo()
{
    const GLGizmosManager::EType t = sw_slicer_canvas()->get_gizmos_manager().get_current_type();
    if (t == GLGizmosManager::Undefined || t == GLGizmosManager::Move || t == GLGizmosManager::Rotate ||
        t == GLGizmosManager::Scale)
        return;
    throw std::runtime_error("close the tool '" + sw_slicer_gizmo_name(t) + "' in the app first");
}

// Refuse while the job worker (arrange, orient, ...) is busy: a change during a running
// job would disturb that job or be silently skipped by the app.
static void sw_slicer_require_idle()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        throw std::runtime_error("no active plater");
    if (!plater->get_ui_job_worker().is_idle())
        throw std::runtime_error("the app is still busy with a previous task");
}

// Undo/redo only in the 3D view (same condition as Plater::can_undo/can_redo).
static void sw_slicer_require_view3D()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        throw std::runtime_error("no active plater");
    if (!plater->is_view3D_shown())
        throw std::runtime_error("undo/redo is only possible in the 3D view (open the 3D view first)");
}

// The current selection as JSON: {empty, object, instance, volumes:[model volume idx...],
// objects?:[obj idx...]}. object/instance are null if the selection does not belong to exactly
// one object/instance; 'objects' is only present for a selection spanning several objects.
static json sw_slicer_selection_json()
{
    const Selection& s = sw_slicer_canvas()->get_selection();
    json r;
    r["empty"] = s.is_empty();
    const int obj  = s.get_object_idx();
    const int inst = s.get_instance_idx();
    r["object"]    = (obj >= 0) ? json(obj) : json(nullptr);
    r["instance"]  = (inst >= 0) ? json(inst) : json(nullptr);
    std::set<int> vols;
    std::set<int> objs;
    if (!s.is_empty()) {
        for (unsigned int idx : s.get_volume_idxs()) {
            const GLVolume* v = s.get_volume(idx);
            if (v != nullptr && v->volume_idx() >= 0)
                vols.insert(v->volume_idx());
            // Real objects only (the wipe tower has object_idx >= 1000).
            if (v != nullptr && v->object_idx() >= 0 && v->object_idx() < 1000)
                objs.insert(v->object_idx());
        }
    }
    r["volumes"] = std::vector<int>(vols.begin(), vols.end());
    // For a selection spanning several objects (object is then null): the object indices.
    if (objs.size() > 1)
        r["objects"] = std::vector<int>(objs.begin(), objs.end());
    return r;
}

static const char* sw_slicer_volume_type(ModelVolumeType type)
{
    switch (type) {
    case ModelVolumeType::NEGATIVE_VOLUME:    return "negative";
    case ModelVolumeType::PARAMETER_MODIFIER: return "modifier";
    case ModelVolumeType::SUPPORT_BLOCKER:    return "support_blocker";
    case ModelVolumeType::SUPPORT_ENFORCER:   return "support_enforcer";
    default:                                  return "part";
    }
}

// --- 3D toolbar: objects and selection ----------------------------------------

// All objects with plate, number of instances and volumes (0-based indices, in the
// order of model.objects and mo->volumes).
static json sw_slicer_objects_json()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        throw std::runtime_error("no active plater");
    Model&         model  = plater->model();
    PartPlateList& plates = plater->get_partplate_list();
    json objects = json::array();
    for (size_t i = 0; i < model.objects.size(); ++i) {
        const ModelObject* mo = model.objects[i];
        json o;
        o["object"] = static_cast<int>(i);
        o["name"]   = mo->name;
        int plate = -1;
        if (!mo->instances.empty()) {
            plate = plates.find_instance_belongs(static_cast<int>(i), 0);
            if (plate < 0)
                plate = plates.find_instance(static_cast<int>(i), 0);
        }
        o["plate"]     = (plate >= 0) ? plate : -1;
        o["instances"] = static_cast<int>(mo->instances.size());
        json vols = json::array();
        for (size_t v = 0; v < mo->volumes.size(); ++v) {
            const ModelVolume* mv = mo->volumes[v];
            json jv;
            jv["volume"] = static_cast<int>(v);
            jv["name"]   = mv->name;
            jv["type"]   = sw_slicer_volume_type(mv->type());
            vols.push_back(jv);
        }
        o["volumes"] = vols;
        objects.push_back(o);
    }
    return objects;
}

void SSWCP_SliceProject_Instance::sw_SlicerListObjects()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        m_res_data["objects"] = sw_slicer_objects_json();
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Select {object, instance?, volume?}, several whole objects with {objects:[...]},
// or clear the selection with {clear:true}.
// The selection stays afterwards, so the user can see what the API is working on.
void SSWCP_SliceProject_Instance::sw_SlicerSelect()
{
    try {
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        GLCanvas3D* canvas = sw_slicer_canvas();
        auto has = [this](const char* key) { return m_param_data.count(key) && !m_param_data.at(key).is_null(); };
        if (has("objects")) {
            // Several whole objects (all instances), e.g. for the assembly tool, which needs
            // at least two selected objects. Cannot be combined with
            // object/instance/volume/clear.
            for (const char* key : {"object", "instance", "volume"})
                if (has(key))
                    throw std::runtime_error(std::string("'objects' cannot be combined with '") + key + "'");
            if (m_param_data.count("clear") && m_param_data.at("clear").is_boolean() && m_param_data.at("clear").get<bool>())
                throw std::runtime_error("specify 'objects' or 'clear', not both");
            const json& arr = m_param_data.at("objects");
            if (!arr.is_array() || arr.empty())
                throw std::runtime_error("'objects' must be a non-empty list of object indices");
            const int        n = static_cast<int>(wxGetApp().plater()->model().objects.size());
            std::vector<int> objs;
            for (const json& j : arr) {
                if (!j.is_number_integer() || j.get<long long>() < 0 || j.get<long long>() >= n)
                    throw std::runtime_error("object index " + j.dump() + " in 'objects' does not exist (there are " +
                                             std::to_string(n) + " objects, 0-based)");
                const int obj = j.get<int>();
                if (std::find(objs.begin(), objs.end(), obj) != objs.end())
                    throw std::runtime_error("object index " + std::to_string(obj) + " appears twice in 'objects'");
                objs.push_back(obj);
            }
            Selection& s = canvas->get_selection();
            s.clear();
            for (size_t i = 0; i < objs.size(); ++i)
                s.add_object(static_cast<unsigned int>(objs[i]), i == 0);
            wxGetApp().obj_list()->update_selections();
            canvas->update_gizmos_on_off_state();
            canvas->set_as_dirty();
        } else if (m_param_data.count("clear") && m_param_data.at("clear").get<bool>()) {
            canvas->get_selection().clear();
            wxGetApp().obj_list()->update_selections();
            canvas->update_gizmos_on_off_state();
            canvas->set_as_dirty();
        } else {
            // "object": null counts as not specified.
            if (!m_param_data.count("object") || m_param_data.at("object").is_null()) {
                handle_general_fail(-1, wxString::FromUTF8("specify 'object' or 'clear': true"));
                return;
            }
            const int obj  = sw_slicer_resolve_object(m_param_data);
            const int inst = m_param_data.count("instance") ? m_param_data.at("instance").get<int>() : -1;
            const int vol  = m_param_data.count("volume") ? m_param_data.at("volume").get<int>() : -1;
            sw_slicer_check_indices(obj, inst, vol);
            sw_slicer_select(obj, inst, vol);
        }
        m_res_data["selection"] = sw_slicer_selection_json();
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Read the current selection (this is how the user points at something: click it in the GUI).
void SSWCP_SliceProject_Instance::sw_SlicerGetSelection()
{
    try {
        m_res_data = sw_slicer_selection_json();
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Is the job worker (arrange, orient, ...) busy? Deliberately NOT
// last_arrange_job_is_finished(): that one has a side effect.
void SSWCP_SliceProject_Instance::sw_SlicerJobStatus()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        m_res_data["busy"] = !plater->get_ui_job_worker().is_idle();
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- 3D toolbar: undo/redo (API actions only) ---------------------------------

static bool sw_slicer_is_api_snapshot(const std::string& name) { return name.rfind("API: ", 0) == 0; }

// Undo: only if the step Plater::undo() would revert is an "API: " snapshot.
// Same loop as Plater::priv::undo() over the main undo stack.
void SSWCP_SliceProject_Instance::sw_SlicerUndo()
{
    try {
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        sw_slicer_require_view3D();
        Plater*                               plater = wxGetApp().plater();
        const UndoRedo::Stack&                stack  = plater->undo_redo_stack_main();
        const std::vector<UndoRedo::Snapshot>& snaps = stack.snapshots();
        const size_t                          before = stack.active_snapshot_time();
        auto it = std::lower_bound(snaps.begin(), snaps.end(), UndoRedo::Snapshot(before));
        if (snaps.empty() || it == snaps.begin()) {
            handle_general_fail(-1, wxString::FromUTF8("there is nothing to undo"));
            return;
        }
        while (--it != snaps.begin() && !UndoRedo::snapshot_modifies_project(*it));
        if (it == snaps.begin()) {
            handle_general_fail(-1, wxString::FromUTF8("there is nothing to undo"));
            return;
        }
        const std::string name = it->name;
        if (!sw_slicer_is_api_snapshot(name)) {
            const std::string err = "the last step was not an API action (" + name + "); undo refused";
            handle_general_fail(-1, wxString::FromUTF8(err.c_str()));
            return;
        }
        plater->undo();
        if (plater->undo_redo_stack_main().active_snapshot_time() == before) {
            handle_general_fail(-1, wxString::FromUTF8("undo was not performed (for example in the assembly view)"));
            return;
        }
        m_res_data["undone"] = name;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Redo: only if the step Plater::redo() would repeat is an "API: " snapshot.
// Same loop as Plater::priv::redo(): the first project-modifying snapshot from the active one.
void SSWCP_SliceProject_Instance::sw_SlicerRedo()
{
    try {
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        sw_slicer_require_view3D();
        Plater*                               plater = wxGetApp().plater();
        const UndoRedo::Stack&                stack  = plater->undo_redo_stack_main();
        const std::vector<UndoRedo::Snapshot>& snaps = stack.snapshots();
        const size_t                          before = stack.active_snapshot_time();
        auto it = std::lower_bound(snaps.begin(), snaps.end(), UndoRedo::Snapshot(before));
        while (it != snaps.end() && !UndoRedo::snapshot_modifies_project(*it++));
        if (it == snaps.end()) {
            handle_general_fail(-1, wxString::FromUTF8("there is nothing to redo"));
            return;
        }
        const std::string name = std::prev(it)->name;
        if (!sw_slicer_is_api_snapshot(name)) {
            const std::string err = "the next step is not an API action (" + name + "); redo refused";
            handle_general_fail(-1, wxString::FromUTF8(err.c_str()));
            return;
        }
        plater->redo();
        if (plater->undo_redo_stack_main().active_snapshot_time() == before) {
            handle_general_fail(-1, wxString::FromUTF8("redo was not performed"));
            return;
        }
        m_res_data["redone"] = name;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- 3D toolbar: open/close a tool (gizmo) ------------------------------------

// Open a toolbar gizmo like a click on its button. open_gizmo() TOGGLES: if the gizmo is
// already open, it is not called again (that would close it).
// Optional 'options' {tool?:string, <key>:number|bool...} for the four paint gizmos (through
// GLGizmoPainterBase::set_tool / set_tool_option), applied AFTER a successful open (or right
// away if the gizmo was already open). The reply then contains applied{} and ignored[].
void SSWCP_SliceProject_Instance::sw_SlicerOpenGizmo()
{
    try {
        const std::string name = m_param_data.at("gizmo").get<std::string>();
        const SwSlicerGizmoName* found = nullptr;
        for (const SwSlicerGizmoName& g : sw_slicer_gizmo_names)
            if (name == g.name)
                found = &g;
        if (found == nullptr) {
            const std::string err = "unknown tool '" + name +
                "' (expected move|rotate|scale|flatten|cut|mesh_boolean|support|seam|fuzzy_skin|color|text|measure|assemble|brim_ears)";
            handle_general_fail(-1, wxString::FromUTF8(err.c_str()));
            return;
        }
        // Validate the options first (before opening): 'tool' is a string, the rest a number
        // or true/false (bools become 0/1).
        std::string                                   opt_tool;
        bool                                          has_opt_tool = false;
        std::vector<std::pair<std::string, double>>   opt_values;
        std::vector<std::string>                      opt_keys; // all keys (nlohmann objects: alphabetical)
        const bool has_options = m_param_data.count("options") && !m_param_data.at("options").is_null();
        if (has_options) {
            const json& opts = m_param_data.at("options");
            if (!opts.is_object())
                throw std::runtime_error("'options' must be an object");
            for (auto it = opts.begin(); it != opts.end(); ++it) {
                const std::string& key = it.key();
                const json&        val = it.value();
                opt_keys.push_back(key);
                if (key == "tool") {
                    if (!val.is_string())
                        throw std::runtime_error("option 'tool' must be a string");
                    opt_tool     = val.get<std::string>();
                    has_opt_tool = true;
                } else if (val.is_boolean()) {
                    opt_values.emplace_back(key, val.get<bool>() ? 1.0 : 0.0);
                } else if (val.is_number() && std::isfinite(val.get<double>())) {
                    opt_values.emplace_back(key, val.get<double>());
                } else {
                    throw std::runtime_error("option '" + key + "' must be a number or true/false");
                }
            }
        }
        GLCanvas3D*      canvas = sw_slicer_canvas();
        GLGizmosManager& gm     = canvas->get_gizmos_manager();
        if (gm.get_current_type() != found->type) {
            if (!gm.open_gizmo(found->type)) {
                handle_general_fail(-1, wxString::FromUTF8("the tool cannot be opened; select a suitable object first"));
                return;
            }
        }
        canvas->set_as_dirty();
        canvas->request_extra_frame();
        // open_gizmo() can return true while the gizmo immediately deactivates itself again
        // (for example when the required selection is missing): check the result.
        const bool opened = (gm.get_current_type() == found->type);
        if (!opened) {
            const std::string err = "tool '" + name + "' closed immediately (required selection missing?)";
            handle_general_fail(-1, wxString::FromUTF8(err.c_str()));
            return;
        }
        m_res_data["gizmo"]  = name;
        m_res_data["opened"] = opened;
        {
            json applied = json::object();
            json ignored = json::array();
            GLGizmoPainterBase* painter = nullptr;
            if (found->type == GLGizmosManager::FdmSupports || found->type == GLGizmosManager::Seam ||
                found->type == GLGizmosManager::FuzzySkin || found->type == GLGizmosManager::MmSegmentation)
                painter = dynamic_cast<GLGizmoPainterBase*>(gm.get_gizmo(found->type));
            if (painter == nullptr) {
                for (const std::string& key : opt_keys)
                    ignored.push_back(key);
            } else {
                // First the tool, then the options (they belong to the chosen tool).
                if (has_opt_tool) {
                    if (painter->set_tool(opt_tool))
                        applied["tool"] = opt_tool;
                    else
                        ignored.push_back("tool");
                }
                for (const auto& kv : opt_values) {
                    if (painter->set_tool_option(kv.first, kv.second))
                        applied[kv.first] = m_param_data.at("options").at(kv.first);
                    else
                        ignored.push_back(kv.first);
                }
                canvas->set_as_dirty();
                canvas->request_extra_frame();
            }
            m_res_data["applied"] = applied;
            m_res_data["ignored"] = ignored;
        }
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Close the open tool through reset_all_gizmos() (never open_gizmo(Undefined)).
void SSWCP_SliceProject_Instance::sw_SlicerCloseGizmo()
{
    try {
        GLCanvas3D*      canvas   = sw_slicer_canvas();
        GLGizmosManager& gm       = canvas->get_gizmos_manager();
        const bool       was_open = gm.get_current_type() != GLGizmosManager::Undefined;
        canvas->reset_all_gizmos();
        canvas->set_as_dirty();
        canvas->request_extra_frame();
        m_res_data["closed"]   = (gm.get_current_type() == GLGizmosManager::Undefined);
        m_res_data["was_open"] = was_open;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- 3D toolbar: add model and plate ------------------------------------------

// Load one file as a model, in API mode (dialogs are skipped; the choices made automatically
// are returned in auto_choices). One undo step "API: Add model".
void SSWCP_SliceProject_Instance::sw_SlicerAddModel()
{
    try {
        const std::string path = m_param_data.at("path").get<std::string>();
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        if (path.empty() || !boost::filesystem::exists(boost::filesystem::path(path))) {
            const std::string err = "file does not exist: " + path;
            handle_general_fail(-1, wxString::FromUTF8(err.c_str()));
            return;
        }
        // Zip archives and G-code take their own paths in the app (with dialogs); not via the API.
        if (boost::algorithm::iends_with(path, ".zip") || boost::algorithm::iends_with(path, ".gcode")) {
            handle_general_fail(-1, wxString::FromUTF8("zip and G-code files cannot be added as a model"));
            return;
        }
        // Same condition as Plater::load_files(wxArrayString): no models in preview mode.
        if (plater->only_gcode_mode() || plater->using_exported_file()) {
            handle_general_fail(-1, wxString::FromUTF8("cannot add models: the app is in G-code preview mode"));
            return;
        }
        if (!plater->can_add_model()) {
            handle_general_fail(-1, wxString::FromUTF8("cannot add a model right now (slicing in progress)"));
            return;
        }
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();

        const size_t             count_before = plater->model().objects.size();
        std::vector<size_t>      loaded;
        std::vector<std::string> choices;
        {
            sswcp_api::Scope     api_scope;
            Plater::TakeSnapshot snapshot(plater, "API: Add model");
            loaded  = plater->load_files(std::vector<std::string>{path}, LoadStrategy::LoadModel, false);
            choices = sswcp_api::notes();
        }

        json added = json::array();
        if (!loaded.empty()) {
            for (size_t idx : loaded)
                added.push_back(static_cast<int>(idx));
        } else {
            // Fallback: compare the object count before and after loading.
            const size_t count_after = plater->model().objects.size();
            for (size_t i = count_before; i < count_after; ++i)
                added.push_back(static_cast<int>(i));
        }
        if (added.empty()) {
            std::string err = "nothing was loaded from " + path;
            for (const std::string& c : choices)
                err += "; " + c;
            handle_general_fail(-1, wxString::FromUTF8(err.c_str()));
            return;
        }
        m_res_data["objects_added"] = added;
        m_res_data["auto_choices"]  = choices;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Add an empty plate and select it (like the toolbar button "Add plate").
void SSWCP_SliceProject_Instance::sw_SlicerAddPlate()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        if (!plater->can_add_plate()) {
            handle_general_fail(-1, wxString::FromUTF8("cannot add a plate (maximum reached or slicing in progress)"));
            return;
        }
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        PartPlateList& plates       = plater->get_partplate_list();
        const int      count_before = plates.get_plate_count();
        int            count        = count_before;
        {
            Plater::TakeSnapshot snapshot(plater, "API: Add plate");
            plates.create_plate(); // may return -1 (e.g. maximum reached)
            count = plates.get_plate_count();
            if (count > count_before) {
                plates.select_plate(count - 1);
                plater->update();
            }
        }
        if (count <= count_before) {
            handle_general_fail(-1, wxString::FromUTF8("could not add a plate (maximum reached?)"));
            return;
        }
        m_res_data["plate"]       = count - 1;
        m_res_data["plate_count"] = count;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- 3D toolbar: orient and arrange (asynchronous) ----------------------------
// Both run through the app's job worker: the reply is {started:true}; the client polls the
// job status (sw_SlicerJobStatus). If the worker is busy, the request is refused (the app
// would silently skip the action).

static void sw_slicer_require_async_ready(Plater* plater)
{
    sw_slicer_require_free_gizmo();
    if (plater->model().objects.empty())
        throw std::runtime_error("no model on the plate (load a model first)");
    sw_slicer_require_idle();
}

void SSWCP_SliceProject_Instance::sw_SlicerOrient()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        const std::string scope = m_param_data.count("scope") ? m_param_data.at("scope").get<std::string>() : std::string();
        if (scope != "selection" && scope != "plate") {
            handle_general_fail(-1, wxString::FromUTF8("scope must be 'selection' or 'plate'"));
            return;
        }
        sw_slicer_require_async_ready(plater);
        if (m_param_data.count("min_area")) {
            const bool min_area = m_param_data.at("min_area").get<bool>();
            // The orient job reads the settings of the current canvas; set them on both.
            sw_slicer_canvas()->get_orient_settings().min_area = min_area;
            if (GLCanvas3D* cur = plater->canvas3D())
                cur->get_orient_settings().min_area = min_area;
        }
        plater->set_prepare_state(scope == "plate" ? Job::PREPARE_STATE_MENU : Job::PREPARE_STATE_DEFAULT);
        {
            Plater::TakeSnapshot snapshot(plater, "API: Orient");
            plater->orient();
        }
        m_res_data["started"] = true;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

void SSWCP_SliceProject_Instance::sw_SlicerArrange()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        const std::string scope = m_param_data.count("scope") ? m_param_data.at("scope").get<std::string>() : std::string();
        if (scope != "all" && scope != "plate") {
            handle_general_fail(-1, wxString::FromUTF8("scope must be 'all' or 'plate'"));
            return;
        }
        if (m_param_data.count("spacing") && m_param_data.at("spacing").get<double>() < 0.0) {
            handle_general_fail(-1, wxString::FromUTF8("spacing must be 0 or greater (0 = automatic)"));
            return;
        }
        sw_slicer_require_async_ready(plater);

        // Modify the (non-const) arrange settings. The arrange job reads them from the current
        // canvas; if that is not the view3D canvas, set them on both.
        std::vector<GLCanvas3D*> canvases{sw_slicer_canvas()};
        if (GLCanvas3D* cur = plater->canvas3D())
            if (cur != canvases.front())
                canvases.push_back(cur);
        for (GLCanvas3D* c : canvases) {
            GLCanvas3D::ArrangeSettings& s = c->get_arrange_settings();
            if (m_param_data.count("spacing"))
                s.distance = static_cast<float>(m_param_data.at("spacing").get<double>());
            if (m_param_data.count("rotation"))
                s.enable_rotation = m_param_data.at("rotation").get<bool>();
            if (m_param_data.count("align_y"))
                s.align_to_y_axis = m_param_data.at("align_y").get<bool>();
            if (m_param_data.count("multi_material_same_plate"))
                s.allow_multi_materials_on_same_plate = m_param_data.at("multi_material_same_plate").get<bool>();
        }
        const GLCanvas3D::ArrangeSettings& used = canvases.back()->get_arrange_settings();
        json settings;
        settings["spacing"]                   = used.distance;
        settings["rotation"]                  = used.enable_rotation;
        settings["align_y"]                   = used.align_to_y_axis;
        settings["multi_material_same_plate"] = used.allow_multi_materials_on_same_plate;

        plater->set_prepare_state(scope == "plate" ? Job::PREPARE_STATE_MENU : Job::PREPARE_STATE_DEFAULT);
        {
            Plater::TakeSnapshot snapshot(plater, "API: Arrange");
            plater->arrange();
        }
        m_res_data["started"]  = true;
        m_res_data["settings"] = settings;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- 3D toolbar: move / rotate / scale ----------------------------------------
// Through GizmoObjectManipulation::on_change(), the same route as the input fields of the
// gizmos (including the plate update). Each action is one undo step "API: ...".

static const char* const sw_slicer_axes[3] = {"x", "y", "z"};

static bool sw_slicer_any_axis(const json& params)
{
    for (const char* a : sw_slicer_axes)
        if (params.count(a))
            return true;
    return false;
}

// Are all specified axes exactly 0? A relative move/rotation of 0 changes nothing
// and therefore gets no undo step.
static bool sw_slicer_all_axes_zero(const json& params)
{
    for (const char* a : sw_slicer_axes)
        if (params.count(a) && params.at(a).get<double>() != 0.0)
            return false;
    return true;
}

// Common preparation: resolve and select object/instance/volume, set the coordinate
// system, refresh and check the manipulation cache.
// Without 'object' the current selection applies (including the selected instance/volume).
static GizmoObjectManipulation& sw_slicer_prepare_manipulation(const json& params, ECoordinatesType coords,
                                                               int& obj, int& inst, int& vol)
{
    sw_slicer_require_free_gizmo();
    sw_slicer_require_idle();
    GLCanvas3D* canvas = sw_slicer_canvas();
    const bool  from_selection = !(params.count("object") && !params.at("object").is_null());
    obj  = sw_slicer_resolve_object(params);
    inst = params.count("instance") ? params.at("instance").get<int>() : 0;
    vol  = params.count("volume") ? params.at("volume").get<int>() : -1;
    if (from_selection) {
        const Selection& cur = canvas->get_selection();
        if (!params.count("instance") && cur.get_instance_idx() >= 0)
            inst = cur.get_instance_idx();
        if (!params.count("volume") && cur.is_single_volume_or_modifier()) {
            const GLVolume* v = cur.get_first_volume();
            if (v != nullptr && v->volume_idx() >= 0)
                vol = v->volume_idx();
        }
    }
    if (inst < 0)
        throw std::runtime_error("instance must be 0 or greater");
    sw_slicer_check_indices(obj, inst, vol);
    sw_slicer_select(obj, inst, vol);

    GizmoObjectManipulation& m = canvas->get_gizmos_manager().get_object_manipulation();
    m.set_coordinates_type(coords);
    m.UpdateAndShow(true);
    const Selection& sel = canvas->get_selection();
    if (!m.get_cache().is_valid() || !(sel.is_single_full_instance() || sel.is_single_volume_or_modifier()))
        throw std::runtime_error("select a single object or volume");
    return m;
}

// RAII: remember the coordinate type of the manipulation panel and restore it when leaving
// the scope (also on the error path), so the user's panel choice stays intact.
// Create the guard BEFORE sw_slicer_prepare_manipulation() (which changes the type).
struct SwSlicerCoordsRestore
{
    GizmoObjectManipulation& m;
    const ECoordinatesType   old_type;
    explicit SwSlicerCoordsRestore(GizmoObjectManipulation& manip) : m(manip), old_type(manip.get_coordinates_type()) {}
    ~SwSlicerCoordsRestore()
    {
        try {
            m.set_coordinates_type(old_type); // does UpdateAndShow + canvas refresh itself
        } catch (...) {
            // never let an exception escape from a destructor
        }
    }
    SwSlicerCoordsRestore(const SwSlicerCoordsRestore&)            = delete;
    SwSlicerCoordsRestore& operator=(const SwSlicerCoordsRestore&) = delete;
};

// Conversion factor mm -> unit of on_change() ("position"/"size" are in inches if the app
// uses inches; on_change then converts back to mm itself).
static double sw_slicer_mm_to_ui(const GizmoObjectManipulation& m)
{
    return m.m_imperial_units ? GizmoObjectManipulation::mm_to_in : 1.0;
}

static json sw_slicer_vec3(const Vec3d& v) { return json::array({v.x(), v.y(), v.z()}); }

void SSWCP_SliceProject_Instance::sw_SlicerMove()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        const std::string mode = m_param_data.count("mode") ? m_param_data.at("mode").get<std::string>() : std::string();
        if (mode != "absolute" && mode != "relative") {
            handle_general_fail(-1, wxString::FromUTF8("mode must be 'absolute' or 'relative'"));
            return;
        }
        const std::string coords = m_param_data.count("coords") ? m_param_data.at("coords").get<std::string>() : std::string("world");
        if (coords != "world" && coords != "object") {
            handle_general_fail(-1, wxString::FromUTF8("coords must be 'world' or 'object'"));
            return;
        }
        if (!sw_slicer_any_axis(m_param_data)) {
            handle_general_fail(-1, wxString::FromUTF8("specify at least one of x, y, z (mm)"));
            return;
        }
        int obj = -1, inst = 0, vol = -1;
        SwSlicerCoordsRestore coords_restore(sw_slicer_canvas()->get_gizmos_manager().get_object_manipulation());
        GizmoObjectManipulation& m = sw_slicer_prepare_manipulation(
            m_param_data, coords == "object" ? ECoordinatesType::Instance : ECoordinatesType::World, obj, inst, vol);
        const double             to_ui = sw_slicer_mm_to_ui(m);
        std::vector<std::string> notes;
        if (mode == "relative" && sw_slicer_all_axes_zero(m_param_data)) {
            notes.push_back("move of 0; nothing changed");
        } else {
            Plater::TakeSnapshot snapshot(plater, "API: Move");
            for (int a = 0; a < 3; ++a) {
                if (!m_param_data.count(sw_slicer_axes[a]))
                    continue;
                const double v = m_param_data.at(sw_slicer_axes[a]).get<double>();
                m.UpdateAndShow(true);
                const double target = (mode == "relative") ? m.get_cache().position(a) + v : v;
                m.on_change("position", a, target * to_ui);
            }
        }
        m.UpdateAndShow(true);
        m_res_data["object"]   = obj;
        m_res_data["instance"] = inst;
        if (vol >= 0)
            m_res_data["volume"] = vol;
        m_res_data["coords"]   = coords;
        m_res_data["position"] = sw_slicer_vec3(m.get_cache().position);
        m_res_data["notes"]    = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

void SSWCP_SliceProject_Instance::sw_SlicerRotate()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        const std::string mode = m_param_data.count("mode") ? m_param_data.at("mode").get<std::string>() : std::string();
        if (mode != "relative" && mode != "absolute") {
            handle_general_fail(-1, wxString::FromUTF8("mode must be 'relative' or 'absolute'"));
            return;
        }
        if (!sw_slicer_any_axis(m_param_data)) {
            handle_general_fail(-1, wxString::FromUTF8("specify at least one of x, y, z (degrees)"));
            return;
        }
        int obj = -1, inst = 0, vol = -1;
        SwSlicerCoordsRestore coords_restore(sw_slicer_canvas()->get_gizmos_manager().get_object_manipulation());
        // Always world coordinates (predictable, independent of the user's panel choice);
        // the guard restores that choice afterwards.
        GizmoObjectManipulation& m = sw_slicer_prepare_manipulation(m_param_data, ECoordinatesType::World, obj, inst, vol);
        const std::string        key = (mode == "relative") ? "rotation" : "absolute_rotation";
        std::vector<std::string> notes;
        if (mode == "relative" && sw_slicer_all_axes_zero(m_param_data)) {
            notes.push_back("rotation of 0; nothing changed");
        } else {
            Plater::TakeSnapshot snapshot(plater, "API: Rotate");
            for (int a = 0; a < 3; ++a) {
                if (!m_param_data.count(sw_slicer_axes[a]))
                    continue;
                const double v = m_param_data.at(sw_slicer_axes[a]).get<double>();
                m.UpdateAndShow(true);
                m.on_change(key, a, v);
            }
        }
        m.UpdateAndShow(true);
        // Absolute Euler angles (degrees): of the volume if one was specified,
        // otherwise of the instance.
        const ModelObject* mo  = plater->model().objects[obj];
        const Vec3d        rot = ((vol >= 0) ? mo->volumes[vol]->get_rotation() : mo->instances[inst]->get_rotation()) *
                          (180.0 / PI);
        m_res_data["object"]   = obj;
        m_res_data["instance"] = inst;
        if (vol >= 0)
            m_res_data["volume"] = vol;
        m_res_data["rotation"] = sw_slicer_vec3(rot);
        m_res_data["notes"]    = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

void SSWCP_SliceProject_Instance::sw_SlicerScale()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        const std::string unit = m_param_data.count("unit") ? m_param_data.at("unit").get<std::string>() : std::string();
        if (unit != "percent" && unit != "mm") {
            handle_general_fail(-1, wxString::FromUTF8("unit must be 'percent' or 'mm'"));
            return;
        }
        const std::string coords = m_param_data.count("coords") ? m_param_data.at("coords").get<std::string>() : std::string("world");
        ECoordinatesType  ctype  = ECoordinatesType::World;
        if (coords == "object")
            ctype = ECoordinatesType::Instance;
        else if (coords == "part")
            ctype = ECoordinatesType::Local;
        else if (coords != "world") {
            handle_general_fail(-1, wxString::FromUTF8("coords must be 'world', 'object' or 'part'"));
            return;
        }
        const bool uniform = m_param_data.count("uniform") ? m_param_data.at("uniform").get<bool>() : true;
        if (!sw_slicer_any_axis(m_param_data)) {
            handle_general_fail(-1, wxString::FromUTF8("specify at least one of x, y, z"));
            return;
        }
        for (const char* a : sw_slicer_axes) {
            if (m_param_data.count(a) && m_param_data.at(a).get<double>() <= 0.0) {
                const std::string err = std::string(unit == "percent" ? "scale" : "size") + " for " + a +
                                        " must be greater than 0";
                handle_general_fail(-1, wxString::FromUTF8(err.c_str()));
                return;
            }
        }
        int obj = -1, inst = 0, vol = -1;
        SwSlicerCoordsRestore coords_restore(sw_slicer_canvas()->get_gizmos_manager().get_object_manipulation());
        GizmoObjectManipulation& m = sw_slicer_prepare_manipulation(m_param_data, ctype, obj, inst, vol);
        m.set_uniform_scaling(uniform);
        const std::string key   = (unit == "percent") ? "scale" : "size";
        const double      to_ui = (unit == "mm") ? sw_slicer_mm_to_ui(m) : 1.0;
        {
            Plater::TakeSnapshot snapshot(plater, "API: Scale");
            for (int a = 0; a < 3; ++a) {
                if (!m_param_data.count(sw_slicer_axes[a]))
                    continue;
                const double v = m_param_data.at(sw_slicer_axes[a]).get<double>();
                m.UpdateAndShow(true);
                m.on_change(key, a, v * to_ui);
                if (uniform)
                    break; // uniform: apply only the first specified axis
            }
        }
        m.UpdateAndShow(true);
        m_res_data["object"]   = obj;
        m_res_data["instance"] = inst;
        if (vol >= 0)
            m_res_data["volume"] = vol;
        m_res_data["uniform"]       = uniform;
        m_res_data["scale_percent"] = sw_slicer_vec3(m.get_cache().scale);
        m_res_data["size_mm"]       = sw_slicer_vec3(m.get_cache().size);
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- 3D toolbar: common helpers for split, lay on face and cut ----------------

// Optional bool parameter (null or missing = default value).
static bool sw_slicer_get_bool(const json& params, const char* key, bool def)
{
    return (params.count(key) && !params.at(key).is_null()) ? params.at(key).get<bool>() : def;
}

// Read [x,y,z] with three finite numbers. Returns false for any other format.
static bool sw_slicer_parse_vec3(const json& j, Vec3d& out)
{
    if (!j.is_array() || j.size() != 3)
        return false;
    for (size_t i = 0; i < 3; ++i) {
        if (!j[i].is_number())
            return false;
        const double d = j[i].get<double>();
        if (!std::isfinite(d))
            return false;
        out[static_cast<int>(i)] = d;
    }
    return true;
}

static std::string sw_slicer_fmt_mm(double v)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2) << v;
    return ss.str();
}

// Instance index: params["instance"] if present; otherwise (without 'object', i.e. from the
// selection) the selected instance; otherwise 0. Same rule as for Move/Rotate/Scale.
static int sw_slicer_resolve_instance(const json& params)
{
    if (params.count("instance") && !params.at("instance").is_null())
        return params.at("instance").get<int>();
    const bool from_selection = !(params.count("object") && !params.at("object").is_null());
    if (from_selection) {
        const int cur = sw_slicer_canvas()->get_selection().get_instance_idx();
        if (cur >= 0)
            return cur;
    }
    return 0;
}

// Number of real parts (MODEL_PART; no modifiers, blockers or enforcers).
static int sw_slicer_count_model_parts(const ModelObject* mo)
{
    int n = 0;
    for (const ModelVolume* v : mo->volumes)
        if (v->is_model_part())
            ++n;
    return n;
}

// --- 3D toolbar: split ----------------------------------------------------------

// Split to objects. Plater::split_object() works on the canvas selection and checks nothing
// itself (the menu item uses can_split); therefore the checks are done up front, like
// ObjectList::is_splittable(true), plus a refusal for cut objects.
void SSWCP_SliceProject_Instance::sw_SlicerSplitObjects()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        const int    obj   = sw_slicer_resolve_object(m_param_data);
        Model&       model = plater->model();
        ModelObject* mo    = model.objects[obj];

        bool splittable = !mo->is_cut();
        if (splittable) {
            if (mo->volumes.size() == 1)
                splittable = mo->volumes.front()->is_splittable();
            else
                splittable = sw_slicer_count_model_parts(mo) >= 2;
        }
        if (!splittable) {
            handle_general_fail(-1, wxString::FromUTF8("this object cannot be split into separate objects"));
            return;
        }

        const ObjectID mo_id    = mo->id(); // do not keep a pointer: the object gets deleted
        const size_t   n_before = model.objects.size();
        sw_slicer_select(obj, -1, -1);
        std::vector<std::string> notes;
        {
            sswcp_api::Scope     api_scope;
            Plater::TakeSnapshot snapshot(plater, "API: Split to objects");
            plater->split_object();
            notes = sswcp_api::notes();
        }
        // split_object() only sets the canvas selection; synchronize the object list.
        wxGetApp().obj_list()->update_selections();

        const size_t n_after = model.objects.size();
        // Failed: nothing was added (original object removed + >= 2 new ones) and the
        // original object is still at the same position.
        if (n_after <= n_before && static_cast<size_t>(obj) < n_after && model.objects[obj]->id() == mo_id) {
            std::string err = "splitting produced nothing";
            for (const std::string& n : notes)
                err += "; " + n;
            handle_general_fail(-1, wxString::FromUTF8(err.c_str()));
            return;
        }
        // The original was removed (later indices shift by one); the new objects are
        // appended at the end: indices n_before-1 ... n_after-1.
        json objects = json::array();
        for (size_t i = n_before - 1; i < n_after; ++i)
            objects.push_back(static_cast<int>(i));
        m_res_data["objects"] = objects;
        m_res_data["count"]   = static_cast<int>(objects.size());
        m_res_data["notes"]   = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Split to parts. Plater::split_volume() -> ObjectList::split() works on the selection in the
// OBJECT LIST: for an object with several volumes the volume itself must be selected
// (otherwise split() silently does nothing). select_item() also synchronizes the canvas.
void SSWCP_SliceProject_Instance::sw_SlicerSplitParts()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        const int    obj   = sw_slicer_resolve_object(m_param_data);
        Model&       model = plater->model();
        ModelObject* mo    = model.objects[obj];
        if (mo->is_cut()) {
            handle_general_fail(-1, wxString::FromUTF8("this object is a cut object and cannot be split"));
            return;
        }
        const bool has_volume = m_param_data.count("volume") && !m_param_data.at("volume").is_null();
        if (mo->volumes.size() > 1 && !has_volume) {
            const std::string err = "object " + std::to_string(obj) + " has " + std::to_string(mo->volumes.size()) +
                                    " volumes; specify 'volume' (0-based)";
            handle_general_fail(-1, wxString::FromUTF8(err.c_str()));
            return;
        }
        const int v = has_volume ? m_param_data.at("volume").get<int>() : 0;
        if (v < 0)
            throw std::runtime_error("volume " + std::to_string(v) + " does not exist");
        sw_slicer_check_indices(obj, -1, v);
        if (!mo->volumes[v]->is_splittable()) {
            handle_general_fail(-1, wxString::FromUTF8("this volume consists of a single piece and cannot be split"));
            return;
        }

        const size_t vols_before = mo->volumes.size();
        wxGetApp().obj_list()->select_item(ObjectVolumeID{mo, mo->volumes.size() > 1 ? mo->volumes[v] : nullptr});
        std::vector<std::string> notes;
        {
            sswcp_api::Scope     api_scope;
            Plater::TakeSnapshot snapshot(plater, "API: Split to parts");
            plater->split_volume();
            notes = sswcp_api::notes();
        }
        sw_slicer_canvas()->set_as_dirty();

        // split() keeps the same ModelObject; the object index does not change.
        if (static_cast<size_t>(obj) >= model.objects.size() || model.objects[obj] != mo ||
            mo->volumes.size() <= vols_before) {
            std::string err = "splitting produced nothing";
            for (const std::string& n : notes)
                err += "; " + n;
            handle_general_fail(-1, wxString::FromUTF8(err.c_str()));
            return;
        }
        json vols = json::array();
        for (size_t i = 0; i < mo->volumes.size(); ++i) {
            json jv;
            jv["volume"] = static_cast<int>(i);
            jv["name"]   = mo->volumes[i]->name;
            vols.push_back(jv);
        }
        m_res_data["object"]  = obj;
        m_res_data["volumes"] = vols;
        m_res_data["notes"]   = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- 3D toolbar: lay on face ------------------------------------------------------

// Lay an object on the face with the given normal, like a click in GLGizmoFlatten.
// FRAME of 'normal': object frame AFTER the volume matrix and BEFORE the instance transformation
// (this is how GLGizmoFlatten passes its m_planes normals to Selection::flattening_rotate(),
// which converts it to world coordinates with the instance matrix). For an object with a
// single volume without its own volume transformation these are simply the mesh coordinates.
void SSWCP_SliceProject_Instance::sw_SlicerLayOnFace()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        Vec3d n = Vec3d::Zero();
        if (!m_param_data.count("normal") || !sw_slicer_parse_vec3(m_param_data.at("normal"), n)) {
            handle_general_fail(-1, wxString::FromUTF8("normal must be a list of three finite numbers [x,y,z]"));
            return;
        }
        if (n.norm() < 1e-9) {
            handle_general_fail(-1, wxString::FromUTF8("normal must not be a zero vector"));
            return;
        }
        n.normalize(); // flattening_rotate() expects a normalized vector (assert)

        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        const int obj  = sw_slicer_resolve_object(m_param_data);
        const int inst = sw_slicer_resolve_instance(m_param_data);
        if (inst < 0)
            throw std::runtime_error("instance must be 0 or greater");
        sw_slicer_check_indices(obj, inst, -1);

        // Select the full instance; add_instance() sets the selection mode to Instance
        // (clear first: add_instance() skips setting the mode if the same volumes are
        // already selected).
        GLCanvas3D* canvas = sw_slicer_canvas();
        Selection&  sel    = canvas->get_selection();
        sel.clear();
        sel.add_instance(static_cast<unsigned int>(obj), static_cast<unsigned int>(inst), true);
        wxGetApp().obj_list()->update_selections();
        canvas->update_gizmos_on_off_state();
        if (sel.get_mode() != Selection::Instance || !sel.is_single_full_instance()) {
            handle_general_fail(-1, wxString::FromUTF8("could not select exactly one full instance"));
            return;
        }

        {
            Plater::TakeSnapshot snapshot(plater, "API: Lay on face");
            sel.flattening_rotate(n);
            // This exact string: do_rotate() treats it specially (the object is placed on the
            // bed afterwards, even if it sank).
            canvas->do_rotate("Gizmo-Place on Face");
        }
        // do_rotate() posts EVT_GLCANVAS_INSTANCE_ROTATED asynchronously; update here.
        plater->update();
        if (GizmoObjectManipulation* om = wxGetApp().obj_manipul())
            om->set_dirty();
        canvas->set_as_dirty();

        const Vec3d rot = plater->model().objects[obj]->instances[inst]->get_rotation() * (180.0 / PI);
        m_res_data["object"]   = obj;
        m_res_data["instance"] = inst;
        m_res_data["rotation"] = sw_slicer_vec3(rot);
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- 3D toolbar: cut (plane + groove) -----------------------------------------------

// Local copy of update_object_cut_id() from GLGizmoCut.cpp (not exposed in a header).
// Without connectors/dowels (dowels_count = 0): connectors are not supported by the API.
static void sw_slicer_update_object_cut_id(CutObjectBase& cut_id, ModelObjectCutAttributes attributes)
{
    // do not keep cut information if the result does not contain all parts of the object
    if (!attributes.has(ModelObjectCutAttribute::KeepUpper) || !attributes.has(ModelObjectCutAttribute::KeepLower) ||
        attributes.has(ModelObjectCutAttribute::InvalidateCutInfo))
        return;
    if (cut_id.id().invalid())
        cut_id.init();
    int cut_obj_cnt = -1;
    if (attributes.has(ModelObjectCutAttribute::KeepUpper))
        cut_obj_cnt++;
    if (attributes.has(ModelObjectCutAttribute::KeepLower))
        cut_obj_cnt++;
    if (cut_obj_cnt > 0)
        cut_id.increase_check_sum(size_t(cut_obj_cnt));
}

// Local copy of synchronize_model_after_cut() from GLGizmoCut.cpp.
static void sw_slicer_synchronize_model_after_cut(Model& model, const CutObjectBase& cut_id)
{
    for (ModelObject* o : model.objects)
        if (o->is_cut() && o->cut_id.has_same_id(cut_id) && !o->cut_id.is_equal(cut_id))
            o->cut_id.copy(cut_id);
}

// RAII: restore a CutObjectBase to the saved value unless 'armed' has been set to false.
struct SwSlicerCutIdRestore
{
    CutObjectBase&      target;
    const CutObjectBase saved;
    bool                armed{true};
    explicit SwSlicerCutIdRestore(CutObjectBase& t) : target(t), saved(t) {}
    ~SwSlicerCutIdRestore()
    {
        if (armed)
            target.copy(saved);
    }
    SwSlicerCutIdRestore(const SwSlicerCutIdRestore&)            = delete;
    SwSlicerCutIdRestore& operator=(const SwSlicerCutIdRestore&) = delete;
};

// Cut an object with an (optionally tilted) plane at height z, or with a groove
// (tongue-and-groove). Directly through libslic3r/CutUtils (Cut) + apply_cut_object_to_model,
// without the gizmo and without dialogs (the gizmo's Windows repair prompt becomes a note).
void SSWCP_SliceProject_Instance::sw_SlicerCut()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        const std::string mode = (m_param_data.count("mode") && !m_param_data.at("mode").is_null()) ?
                                     m_param_data.at("mode").get<std::string>() : std::string("planar");
        if (mode != "planar" && mode != "groove") {
            handle_general_fail(-1, wxString::FromUTF8("mode must be 'planar' or 'groove'"));
            return;
        }
        const bool planar             = (mode == "planar");
        bool       keep_upper         = sw_slicer_get_bool(m_param_data, "keep_upper", true);
        bool       keep_lower         = sw_slicer_get_bool(m_param_data, "keep_lower", true);
        bool       flip_upper         = sw_slicer_get_bool(m_param_data, "flip_upper", false);
        bool       flip_lower         = sw_slicer_get_bool(m_param_data, "flip_lower", false);
        bool       place_on_cut_upper = sw_slicer_get_bool(m_param_data, "place_on_cut_upper", true);
        bool       place_on_cut_lower = sw_slicer_get_bool(m_param_data, "place_on_cut_lower", false);
        const bool as_parts           = sw_slicer_get_bool(m_param_data, "as_parts", false);

        // Normalize the options like the gizmo UI does (GLGizmoCut3D::render_cut_plane_input_window).
        std::vector<std::string> notes;
        if (planar && as_parts) {
            // "Cut to parts": both halves are kept (as parts of one object); placing and
            // flipping are then disabled. Without this Cut crashes with keep_upper=false.
            if (!keep_upper || !keep_lower || place_on_cut_upper || place_on_cut_lower || flip_upper || flip_lower)
                notes.push_back("as parts: both halves kept, place/flip ignored");
            keep_upper = keep_lower = true;
            place_on_cut_upper = place_on_cut_lower = false;
            flip_upper = flip_lower = false;
        }
        // "Place on cut" and "Flip" are mutually exclusive per half; placing wins.
        if (place_on_cut_upper && flip_upper) {
            flip_upper = false;
            notes.push_back("upper half: place_on_cut and flip both requested; only place_on_cut applied");
        }
        if (place_on_cut_lower && flip_lower) {
            flip_lower = false;
            notes.push_back("lower half: place_on_cut and flip both requested; only place_on_cut applied");
        }

        Vec3d rot_deg = Vec3d::Zero();
        if (m_param_data.count("rotation") && !m_param_data.at("rotation").is_null() &&
            !sw_slicer_parse_vec3(m_param_data.at("rotation"), rot_deg)) {
            handle_general_fail(-1, wxString::FromUTF8("rotation must be a list of three finite numbers [x,y,z] (degrees)"));
            return;
        }

        // Groove: angles arrive in degrees, Cut::Groove expects radians (mm for the rest).
        Slic3r::Cut::Groove groove;
        if (!planar) {
            if (!m_param_data.count("groove") || !m_param_data.at("groove").is_object()) {
                handle_general_fail(-1, wxString::FromUTF8("invalid groove: specify groove {depth, width, flaps_angle, angle}"));
                return;
            }
            const json& g = m_param_data.at("groove");
            for (const char* key : {"depth", "width", "flaps_angle", "angle"}) {
                if (!g.count(key) || !g.at(key).is_number() || !std::isfinite(g.at(key).get<double>())) {
                    const std::string err = std::string("invalid groove: '") + key + "' is missing or not a number";
                    handle_general_fail(-1, wxString::FromUTF8(err.c_str()));
                    return;
                }
            }
            const double depth       = g.at("depth").get<double>();
            const double width       = g.at("width").get<double>();
            const double flaps_deg   = g.at("flaps_angle").get<double>();
            const double angle_deg   = g.at("angle").get<double>();
            const double depth_tol   = (g.count("depth_tolerance") && g.at("depth_tolerance").is_number()) ? g.at("depth_tolerance").get<double>() : 0.1;
            const double width_tol   = (g.count("width_tolerance") && g.at("width_tolerance").is_number()) ? g.at("width_tolerance").get<double>() : 0.1;
            if (depth <= 0.0 || width <= 0.0) {
                handle_general_fail(-1, wxString::FromUTF8("invalid groove: depth and width must be greater than 0 (mm)"));
                return;
            }
            // Same limits as the input fields in GLGizmoCut3D (flap angle 30-120, groove angle 0-15 degrees).
            if (flaps_deg < 30.0 || flaps_deg > 120.0) {
                handle_general_fail(-1, wxString::FromUTF8("invalid groove: flaps_angle must be between 30 and 120 degrees"));
                return;
            }
            if (angle_deg < 0.0 || angle_deg > 15.0) {
                handle_general_fail(-1, wxString::FromUTF8("invalid groove: angle must be between 0 and 15 degrees"));
                return;
            }
            groove.depth           = static_cast<float>(depth);
            groove.width           = static_cast<float>(width);
            groove.flaps_angle     = static_cast<float>(flaps_deg * PI / 180.0);
            groove.angle           = static_cast<float>(angle_deg * PI / 180.0);
            groove.depth_init      = groove.depth;
            groove.width_init      = groove.width;
            groove.flaps_angle_init = groove.flaps_angle;
            groove.angle_init      = groove.angle;
            groove.depth_tolerance = static_cast<float>(depth_tol);
            groove.width_tolerance = static_cast<float>(width_tol);
            // Same check as GLGizmoCut3D::has_valid_groove() (the geometric part).
            const float flaps_width = -2.f * groove.depth / std::tan(groove.flaps_angle);
            if (flaps_width > groove.width) {
                const std::string err = "invalid groove: the flaps are wider than the groove (-2*depth/tan(flaps_angle) = " +
                                        sw_slicer_fmt_mm(flaps_width) + " > width " + sw_slicer_fmt_mm(groove.width) + ")";
                handle_general_fail(-1, wxString::FromUTF8(err.c_str()));
                return;
            }
        }

        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        const int obj  = sw_slicer_resolve_object(m_param_data);
        const int inst = sw_slicer_resolve_instance(m_param_data);
        if (inst < 0)
            throw std::runtime_error("instance must be 0 or greater");
        sw_slicer_check_indices(obj, inst, -1);
        Model&       model = plater->model();
        ModelObject* mo    = model.objects[obj];
        if (!mo->cut_connectors.empty()) {
            handle_general_fail(-1, wxString::FromUTF8(
                "object has unapplied connectors; use the cut tool in the app or remove them"));
            return;
        }

        // Bounding box of the instance in plate/world coordinates.
        const BoundingBoxf3 bb = mo->instance_bounding_box(static_cast<size_t>(inst));
        double z = bb.center().z();
        if (m_param_data.count("z") && !m_param_data.at("z").is_null())
            z = m_param_data.at("z").get<double>();
        if (!std::isfinite(z) || z <= bb.min.z() || z >= bb.max.z()) {
            const std::string err = "cut height is outside the object (z between " + sw_slicer_fmt_mm(bb.min.z()) + " and " +
                                    sw_slicer_fmt_mm(bb.max.z()) + ")";
            handle_general_fail(-1, wxString::FromUTF8(err.c_str()));
            return;
        }
        if (!keep_upper && !keep_lower) {
            handle_general_fail(-1, wxString::FromUTF8("keep at least one half"));
            return;
        }
        // Groove: the full groove depth (z +/- depth/2) must lie inside the object.
        if (!planar) {
            const double half = 0.5 * double(groove.depth);
            if (z - half <= bb.min.z() || z + half >= bb.max.z()) {
                const std::string err = "the groove (z " + sw_slicer_fmt_mm(z) + " +/- " + sw_slicer_fmt_mm(half) +
                                        ") falls outside the object (z between " + sw_slicer_fmt_mm(bb.min.z()) + " and " +
                                        sw_slicer_fmt_mm(bb.max.z()) + ")";
                handle_general_fail(-1, wxString::FromUTF8(err.c_str()));
                return;
            }
        }

        // Cut matrix exactly like GLGizmoCut3D::get_cut_matrix(): plane center (world) minus the
        // instance offset, times the rotation. sla_shift_z is 0 for FFF.
        const Transform3d R = Slic3r::Geometry::rotation_transform(rot_deg * (PI / 180.0));
        const Vec3d       center(bb.center().x(), bb.center().y(), z);
        const Transform3d cut_matrix = Slic3r::Geometry::translation_transform(center - mo->instances[inst]->get_offset()) * R;

        ModelObjectCutAttributes attrs = only_if(keep_upper, ModelObjectCutAttribute::KeepUpper) |
                                         only_if(keep_lower, ModelObjectCutAttribute::KeepLower) |
                                         only_if(as_parts, ModelObjectCutAttribute::KeepAsParts) |
                                         only_if(place_on_cut_upper, ModelObjectCutAttribute::PlaceOnCutUpper) |
                                         only_if(place_on_cut_lower, ModelObjectCutAttribute::PlaceOnCutLower) |
                                         only_if(flip_upper, ModelObjectCutAttribute::FlipUpper) |
                                         only_if(flip_lower, ModelObjectCutAttribute::FlipLower);
        // Planar cut: InvalidateCutInfo, so the halves do not become linked cut objects
        // (that would block splitting/deleting later). Like the gizmo, only if the object is
        // not a cut object yet; an existing cut object keeps its link consistent.
        if (planar && mo->cut_id.id().invalid())
            attrs = attrs | ModelObjectCutAttribute::InvalidateCutInfo;

        if (!planar && as_parts)
            notes.push_back("as_parts is not supported for a groove cut: two separate objects were created");

        // Expected number of resulting objects; fewer means the cut (partly) misses the
        // object: Cut::post_process silently drops empty halves.
        const size_t expected = (keep_upper && keep_lower && !(planar && as_parts)) ? 2 : 1;

        const size_t n_before = model.objects.size();
        {
            // API mode: apply_cut_object_to_model -> load_model_objects could otherwise show a
            // modal dialog (e.g. "Object too large").
            sswcp_api::Scope     api_scope;
            Plater::TakeSnapshot snapshot(plater, "API: Cut");

            // Restore the cut_id of 'mo' if something goes wrong before apply (also on an
            // exception in perform_*). Disarmed right before apply: apply deletes 'mo'.
            SwSlicerCutIdRestore cut_id_restore(mo->cut_id);
            sw_slicer_update_object_cut_id(mo->cut_id, attrs);

            // 'cut' must stay alive as long as 'objs' is used: the objects are owned by the
            // Model inside Cut (its destructor cleans them up). apply_cut_object_to_model()
            // copies them into the plater model.
            Slic3r::Cut            cut(mo, inst, cut_matrix, attrs);
            const ModelObjectPtrs& objs = planar ? cut.perform_with_plane() : cut.perform_with_groove(groove, R, false);
            if (objs.empty())
                throw std::runtime_error("the cut produced no objects");
            if (objs.size() < expected)
                throw std::runtime_error("the cut does not intersect the object (" + std::to_string(objs.size()) + " of the " +
                                         std::to_string(expected) + " expected parts; check z, rotation and the groove)");

            // Replaces the gizmo's Windows repair dialog: report only, do not repair.
            for (const ModelObject* o : objs) {
                for (const ModelVolume* v : o->volumes) {
                    if (v->is_model_part() && its_num_open_edges(v->mesh().its) > 0) {
                        notes.push_back(o->name + ": cut has open edges (not repaired)");
                        break;
                    }
                }
            }

            // Save cut_id before apply: apply deletes 'mo' (the pointer is invalid afterwards).
            const CutObjectBase cut_id = mo->cut_id;
            cut_id_restore.armed       = false; // 'mo' is about to be deleted; do not touch it anymore
            plater->apply_cut_object_to_model(static_cast<size_t>(obj), objs);
            mo = nullptr;
            sw_slicer_synchronize_model_after_cut(plater->model(), cut_id);
            const std::vector<std::string>& api_notes = sswcp_api::notes();
            notes.insert(notes.end(), api_notes.begin(), api_notes.end());
        }
        wxGetApp().obj_list()->update_selections();
        sw_slicer_canvas()->set_as_dirty();

        // The original was removed; the new objects are appended at the end.
        const size_t n_after = model.objects.size();
        json objects = json::array();
        for (size_t i = n_before - 1; i < n_after; ++i)
            objects.push_back(static_cast<int>(i));
        m_res_data["objects"] = objects;
        m_res_data["notes"]   = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- 3D toolbar: variable layer height ----------------------------------------------

// Optional number parameter: false if it is present but not a finite number.
static bool sw_slicer_get_number(const json& params, const char* key, double def, double& out)
{
    out = def;
    if (!params.count(key) || params.at(key).is_null())
        return true;
    const json& j = params.at(key);
    if (!j.is_number())
        return false;
    out = j.get<double>();
    return std::isfinite(out);
}

// Variable layer height (adaptive / smooth / reset), directly through libslic3r.
// NOT through GLCanvas3D::adaptive_layer_height_profile() / smooth_... / reset_...: those use
// LayersEditing::m_model_object, which is null while the layer height editor is closed (crash).
// Same conditions as Plater::priv::layers_height_allowed(): FFF only and an object that is
// not completely below the plate.
void SSWCP_SliceProject_Instance::sw_SlicerLayerHeight()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        const std::string action = (m_param_data.count("action") && m_param_data.at("action").is_string()) ?
                                       m_param_data.at("action").get<std::string>() : std::string();
        if (action != "adaptive" && action != "smooth" && action != "reset") {
            handle_general_fail(-1, wxString::FromUTF8("action must be 'adaptive', 'smooth' or 'reset'"));
            return;
        }
        double quality = 0.5;
        if (!sw_slicer_get_number(m_param_data, "quality", 0.5, quality) || quality < 0.0 || quality > 1.0) {
            handle_general_fail(-1, wxString::FromUTF8("quality must be a number between 0 and 1"));
            return;
        }
        long long radius = 5;
        if (m_param_data.count("radius") && !m_param_data.at("radius").is_null()) {
            const json& r = m_param_data.at("radius");
            if (!r.is_number_integer() || r.get<long long>() < 1 || r.get<long long>() > 10) {
                handle_general_fail(-1, wxString::FromUTF8("radius must be an integer between 1 and 10"));
                return;
            }
            radius = r.get<long long>();
        }
        const bool keep_min = sw_slicer_get_bool(m_param_data, "keep_min", false);

        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        // With the layer height editor open the canvas keeps its own copy of the profile, which
        // would overwrite our change on the next edit.
        if (sw_slicer_canvas()->is_layers_editing_enabled()) {
            handle_general_fail(-1, wxString::FromUTF8("close the layer height editor (variable layer height) in the app first"));
            return;
        }
        if (plater->printer_technology() != ptFFF) {
            handle_general_fail(-1, wxString::FromUTF8("variable layer height is only available for FDM printers"));
            return;
        }
        const int    obj = sw_slicer_resolve_object(m_param_data);
        ModelObject* mo  = plater->model().objects[obj];
        if (!(mo->max_z() > SINKING_Z_THRESHOLD)) {
            handle_general_fail(-1, wxString::FromUTF8("the object lies completely below the plate"));
            return;
        }
        sw_slicer_select(obj, -1, -1);

        // Compute the new profile first (without changing anything); only then, and only if
        // something actually changes, take a snapshot and set the profile.
        std::vector<std::string> notes;
        std::vector<coordf_t>    result; // the profile the slicer actually uses
        bool                     changed = false;
        if (action == "reset") {
            changed = !mo->layer_height_profile.empty();
            if (!changed)
                notes.push_back("no variable layer height was set; nothing changed");
        } else {
            // Same source as LayersEditing::update_slicing_parameters() (GLCanvas3D.cpp).
            const SlicingParameters sp = PrintObject::slicing_parameters(wxGetApp().preset_bundle->full_config(), *mo,
                                                                         float(mo->max_z()),
                                                                         plater->fff_print().shrinkage_compensation());
            std::vector<coordf_t> prof;
            if (action == "adaptive") {
                prof = layer_height_profile_adaptive(sp, *mo, float(quality));
            } else {
                // Smoothing works on the current profile. update_layer_height_profile() fills an
                // empty profile with the default profile and rejects a profile that no longer
                // matches the object height (like the GUI does before smoothing).
                prof = mo->layer_height_profile.get();
                PrintObject::update_layer_height_profile(*mo, sp, prof);
                prof = smooth_height_profile(prof, sp, HeightProfileSmoothingParams(static_cast<unsigned int>(radius), keep_min));
            }
            // Check like the backend: update_layer_height_profile() validates against the
            // UNcompensated object height and silently replaces a deviating profile with the
            // default profile (e.g. with Z shrinkage compensation != 100%). The API would then
            // report a false success; refuse in that case without changing anything (no snapshot).
            std::vector<coordf_t> check = prof;
            PrintObject::update_layer_height_profile(*mo, sp, check);
            if (prof.empty() || check != prof) {
                handle_general_fail(-1, wxString::FromUTF8("the slicer rejects this layer height profile (Z shrinkage compensation != 100%?)"));
                return;
            }
            result  = check;
            changed = (mo->layer_height_profile.get() != result);
            if (!changed)
                notes.push_back("the profile was already identical; nothing changed");
        }

        if (changed) {
            {
                Plater::TakeSnapshot snapshot(plater, "API: Variable layer height - " + action);
                if (action == "reset")
                    mo->layer_height_profile.clear();
                else
                    mo->layer_height_profile.set(result);
            }
            // Like LayersEditing after adaptive/smooth/reset: update the info items and reslice
            // (Print::apply sees the new timestamp of layer_height_profile).
            wxGetApp().obj_list()->update_info_items(static_cast<size_t>(obj));
            plater->schedule_background_process();
            sw_slicer_canvas()->set_as_dirty();
        }

        // Profile = [z0, h0, z1, h1, ...]; the heights are at the odd indices.
        double min_h = 0.0, max_h = 0.0;
        bool   have_h = false;
        for (size_t i = 1; i < result.size(); i += 2) {
            if (!have_h) {
                min_h = max_h = result[i];
                have_h        = true;
            } else {
                min_h = std::min(min_h, double(result[i]));
                max_h = std::max(max_h, double(result[i]));
            }
        }
        m_res_data["object"]     = obj;
        m_res_data["action"]     = action;
        m_res_data["points"]     = static_cast<int>(result.size() / 2);
        m_res_data["min_height"] = have_h ? json(min_h) : json(nullptr);
        m_res_data["max_height"] = have_h ? json(max_h) : json(nullptr);
        m_res_data["notes"]      = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- 3D toolbar: brim ears ------------------------------------------------------------

// Default head diameter, same formula as GLGizmoBrimEars::get_brim_default_radius():
// initial_layer_line_width (absolute w.r.t. nozzle_diameter[0]) x 16. Note: the gizmo calls
// this "radius" but uses it as a diameter.
static double sw_slicer_brim_default_head_diameter()
{
    const ConfigOptionFloats* nozzle =
        wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
    if (nozzle == nullptr || nozzle->values.empty())
        throw std::runtime_error("no nozzle_diameter in the printer profile; specify head_diameter");
    const double nozzle_diameter = nozzle->get_at(0);
    const DynamicPrintConfig& print_cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    return print_cfg.get_abs_value("initial_layer_line_width", nozzle_diameter) * 16.0;
}

// Plate point (world XY, mm) -> object coordinates, exactly like GLGizmoBrimEars: an ear lies
// on the bottom (world z = -0.0001) and is transformed back with the inverse instance matrix.
static BrimPoint sw_slicer_brim_point(const Transform3d& trsf, double x, double y, float head_front_radius)
{
    const Vec3d world_pos(x, y, -0.0001);
    const Vec3d object_pos = trsf.inverse() * world_pos;
    return BrimPoint(object_pos.cast<float>(), head_front_radius);
}

// Add the point unless an equal point exists (BrimPoint::operator==), like
// GLGizmoBrimEars::add_point_to_cache(). Returns true if the point was added.
static bool sw_slicer_brim_add_unique(BrimPoints& points, const BrimPoint& p)
{
    for (const BrimPoint& q : points)
        if (q == p)
            return false;
    points.push_back(p);
    return true;
}

// Copy of GLGizmoBrimEars::first_layer_slicer() using the public libslic3r API, for
// instance 0 (like the backend make_brim_ears): cross-section at world z = 0.1 of all
// MODEL_PART volumes minus the NEGATIVE_VOLUME volumes.
static ExPolygons sw_slicer_brim_first_layer(const ModelObject* mo)
{
    const std::vector<float> slice_height(1, 0.1f);
    MeshSlicingParamsEx      params;
    params.mode           = MeshSlicingParams::SlicingMode::Regular;
    params.closing_radius = 0.1f;
    params.extra_offset   = 0.05f;
    params.resolution     = 0.01;
    ExPolygons        part_ex;
    ExPolygons        negative_ex;
    const Transform3d inst_trsf = mo->instances[0]->get_matrix();
    for (const ModelVolume* mv : mo->volumes) {
        if (mv->type() != ModelVolumeType::MODEL_PART && mv->type() != ModelVolumeType::NEGATIVE_VOLUME)
            continue;
        indexed_triangle_set volume_its = mv->mesh().its;
        if (volume_its.indices.empty())
            continue;
        const Transform3d   trsf = inst_trsf * mv->get_matrix();
        MeshSlicingParamsEx params_ex(params);
        params_ex.trafo = params_ex.trafo * trsf;
        if (params_ex.trafo.rotation().determinant() < 0.)
            its_flip_triangles(volume_its);
        const std::vector<ExPolygons> layers = slice_mesh_ex(volume_its, slice_height, params_ex);
        if (layers.empty())
            continue;
        if (mv->type() == ModelVolumeType::MODEL_PART)
            part_ex = union_ex(part_ex, layers.front());
        else
            negative_ex = union_ex(negative_ex, layers.front());
    }
    return diff_ex(part_ex, negative_ex);
}

// Copy of GLGizmoBrimEars::generate_points().
static Points sw_slicer_brim_generate_points(Slic3r::Polygon& obj_polygon, float ear_detection_length, float brim_ears_max_angle,
                                             bool is_outer)
{
    const coordf_t angle_threshold = (180 - brim_ears_max_angle) * PI / 180.0;
    Points         pt_ears;
    if (ear_detection_length > 0) {
        double detect_length = ear_detection_length / SCALING_FACTOR;
        Points points        = obj_polygon.points;
        points.push_back(points.front());
        points = MultiPoint::_douglas_peucker(points, detect_length);
        if (points.size() > 4) {
            points.erase(points.end() - 1);
        }
        obj_polygon.points = points;
    }
    append(pt_ears, is_outer ? obj_polygon.convex_points(angle_threshold) : obj_polygon.concave_points(angle_threshold));
    return pt_ears;
}

// Brim ears: generate automatically (added to the existing ones), clear, or set a custom list
// (replaces the existing ones). 'set' points are world XY coordinates in mm (the same frame as
// the positions reported by sw_SlicerMove; on the first plate equal to plate coordinates).
void SSWCP_SliceProject_Instance::sw_SlicerBrimEars()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        const std::string action = (m_param_data.count("action") && m_param_data.at("action").is_string()) ?
                                       m_param_data.at("action").get<std::string>() : std::string();
        if (action != "auto" && action != "clear" && action != "set") {
            handle_general_fail(-1, wxString::FromUTF8("action must be 'auto', 'clear' or 'set'"));
            return;
        }
        const bool has_head = m_param_data.count("head_diameter") && !m_param_data.at("head_diameter").is_null();
        double     head_diameter = 0.0;
        // The default value is only needed (and only computed) for auto/set without head_diameter.
        const double head_def = (has_head || action == "clear") ? 1.0 : sw_slicer_brim_default_head_diameter();
        if (!sw_slicer_get_number(m_param_data, "head_diameter", head_def, head_diameter) ||
            head_diameter <= 0.0) {
            handle_general_fail(-1, wxString::FromUTF8("head_diameter must be a number greater than 0 (mm)"));
            return;
        }
        double max_angle = 125.0;
        if (!sw_slicer_get_number(m_param_data, "max_angle", 125.0, max_angle) || max_angle < 0.0 || max_angle > 180.0) {
            handle_general_fail(-1, wxString::FromUTF8("max_angle must be a number between 0 and 180 degrees"));
            return;
        }
        double detection_radius = 1.0;
        if (!sw_slicer_get_number(m_param_data, "detection_radius", 1.0, detection_radius) || detection_radius < 0.0) {
            handle_general_fail(-1, wxString::FromUTF8("detection_radius must be a number of 0 or greater (mm)"));
            return;
        }
        // 'set': list of [x,y] pairs, world XY in mm (on the first plate equal to plate coordinates).
        std::vector<Vec2d> set_points;
        if (action == "set") {
            const char* err = "points must be a non-empty list of [x,y] pairs: two finite numbers, world XY in mm (on the first plate equal to plate coordinates)";
            if (!m_param_data.count("points") || !m_param_data.at("points").is_array() || m_param_data.at("points").empty()) {
                handle_general_fail(-1, wxString::FromUTF8(err));
                return;
            }
            for (const json& jp : m_param_data.at("points")) {
                if (!jp.is_array() || jp.size() != 2 || !jp[0].is_number() || !jp[1].is_number()) {
                    handle_general_fail(-1, wxString::FromUTF8(err));
                    return;
                }
                const double x = jp[0].get<double>();
                const double y = jp[1].get<double>();
                if (!std::isfinite(x) || !std::isfinite(y)) {
                    handle_general_fail(-1, wxString::FromUTF8(err));
                    return;
                }
                set_points.emplace_back(x, y);
            }
        }

        // An open brim ears gizmo is covered by this check (its cache would overwrite our change).
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        const int    obj = sw_slicer_resolve_object(m_param_data);
        ModelObject* mo  = plater->model().objects[obj];
        if (mo->instances.empty())
            throw std::runtime_error("object " + std::to_string(obj) + " has no instances");
        sw_slicer_select(obj, -1, -1);

        const float              head_front_radius = float(head_diameter / 2.0);
        const Transform3d        trsf              = mo->instances[0]->get_matrix();
        std::vector<std::string> notes;

        // Compute the new point list first (without changing anything).
        BrimPoints new_points;
        if (action == "set") {
            for (const Vec2d& p : set_points)
                sw_slicer_brim_add_unique(new_points, sw_slicer_brim_point(trsf, p.x(), p.y(), head_front_radius));
            if (new_points.size() < set_points.size())
                notes.push_back(std::to_string(set_points.size() - new_points.size()) +
                                " duplicate point(s) skipped (world XY in mm; on the first plate equal to plate coordinates)");
        } else if (action == "auto") {
            // auto: like GLGizmoBrimEars::auto_generate(); new points are added to the
            // existing ones, duplicates are skipped.
            new_points                   = mo->brim_points;
            const ExPolygons first_layer = sw_slicer_brim_first_layer(mo);
            if (first_layer.empty())
                notes.push_back("no cross-section found on the first layer (z = 0.1 mm); no points generated; "
                                "place the object on the bed first");
            auto add_point = [&](const Point& p) {
                // Same float rounding as the gizmo (world_pos = {float(x), float(y), -0.0001}).
                const double x = double(float(p.x() * SCALING_FACTOR));
                const double y = double(float(p.y() * SCALING_FACTOR));
                sw_slicer_brim_add_unique(new_points, sw_slicer_brim_point(trsf, x, y, head_front_radius));
            };
            for (const ExPolygon& ex_poly : first_layer) {
                Slic3r::Polygon  out_poly   = ex_poly.contour;
                Slic3r::Polygons inner_poly = ex_poly.holes;
                polygons_reverse(inner_poly);
                for (const Point& p : sw_slicer_brim_generate_points(out_poly, float(detection_radius), float(max_angle), true))
                    add_point(p);
                for (Slic3r::Polygon& pl : inner_poly)
                    for (const Point& p : sw_slicer_brim_generate_points(pl, float(detection_radius), float(max_angle), false))
                        add_point(p);
            }
        } // clear: new_points stays empty

        // Brim ears only work with brim_type 'painted': set it on the object if it is not
        // already (object setting, otherwise the print setting).
        bool need_brim_type = false;
        if (action != "clear" && !new_points.empty()) {
            const DynamicPrintConfig& print_cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
            BrimType                  effective = btPainted;
            if (mo->config.has("brim_type"))
                effective = mo->config.get().opt_enum<BrimType>("brim_type");
            else if (print_cfg.has("brim_type"))
                effective = print_cfg.opt_enum<BrimType>("brim_type");
            need_brim_type = (effective != btPainted);
        }

        // Only take a snapshot (undo step) and reslice if something actually changes.
        const bool points_changed = (new_points != mo->brim_points);
        bool       brim_type_set  = false;
        if (points_changed || need_brim_type) {
            Plater::TakeSnapshot snapshot(plater, "API: Brim ears " + action);
            if (points_changed)
                mo->brim_points = std::move(new_points);
            if (need_brim_type) {
                mo->config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btPainted));
                wxGetApp().obj_list()->object_config_options_changed({mo, nullptr});
                brim_type_set = true;
            }
        } else {
            notes.push_back(action == "clear" ? "there were no brim ears; nothing changed" : "brim ears unchanged; nothing changed");
        }

        // With a raft the backend ignores brim ears.
        {
            const DynamicPrintConfig& print_cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
            int raft_layers = 0;
            if (mo->config.has("raft_layers"))
                raft_layers = mo->config.opt_int("raft_layers");
            else if (print_cfg.has("raft_layers"))
                raft_layers = print_cfg.opt_int("raft_layers");
            if (raft_layers > 0)
                notes.push_back("raft enabled: brim ears are ignored");
        }

        if (points_changed || brim_type_set) {
            plater->set_plater_dirty(true);
            plater->changed_object(obj);
            sw_slicer_canvas()->set_as_dirty();
        }

        m_res_data["object"]        = obj;
        m_res_data["action"]        = action;
        m_res_data["points"]        = static_cast<int>(mo->brim_points.size());
        m_res_data["brim_type_set"] = brim_type_set;
        m_res_data["notes"]         = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- 3D toolbar: painting (support / seam / fuzzy skin / color) -------------------

static bool sw_slicer_has_param(const json& params, const char* key)
{
    return params.count(key) && !params.at(key).is_null();
}

// The FacetsAnnotation belonging to a paint type (type has already been checked).
static FacetsAnnotation& sw_slicer_paint_facets(ModelVolume* mv, const std::string& type)
{
    if (type == "support")
        return mv->supported_facets;
    if (type == "seam")
        return mv->seam_facets;
    if (type == "fuzzy_skin")
        return mv->fuzzy_skin_facets;
    return mv->mmu_segmentation_facets; // color
}

// Read 'state' per paint type. Throws with a readable message for an invalid value.
static EnforcerBlockerType sw_slicer_paint_state(const json& params, const std::string& type)
{
    if (!sw_slicer_has_param(params, "state"))
        throw std::runtime_error("specify 'state' (required for whole, height_band and overhangs)");
    const json& j = params.at("state");
    if (type == "color") {
        const int n_filaments = wxGetApp().filaments_cnt();
        if (j.is_string() && j.get<std::string>() == "none")
            return EnforcerBlockerType::NONE;
        if (!j.is_number_integer())
            throw std::runtime_error("invalid state for color: expected a filament number 1.." +
                                     std::to_string(n_filaments) + " or 'none'");
        const long long f = j.get<long long>();
        if (f < 1 || f > n_filaments)
            throw std::runtime_error("filament " + std::to_string(f) + " does not exist (this setup has " +
                                     std::to_string(n_filaments) + " filament(s))");
        return static_cast<EnforcerBlockerType>(static_cast<int>(f));
    }
    const std::string s = j.is_string() ? j.get<std::string>() : std::string();
    if (type == "fuzzy_skin") {
        if (s == "on")
            return EnforcerBlockerType::FUZZY_SKIN;
        if (s == "none")
            return EnforcerBlockerType::NONE;
        throw std::runtime_error("invalid state for fuzzy_skin (expected on|none)");
    }
    // support and seam
    if (s == "enforce")
        return EnforcerBlockerType::ENFORCER;
    if (s == "block")
        return EnforcerBlockerType::BLOCKER;
    if (s == "none")
        return EnforcerBlockerType::NONE;
    throw std::runtime_error("invalid state for " + type + " (expected enforce|block|none)");
}

// Result of sw_slicer_paint(); sw_SlicerPaint and the alias sw_SlicerSetColorFromLayer
// each build their own reply from it.
struct SwSlicerPaintResult
{
    int    object          = -1;
    bool   painted         = false;
    int    volumes_painted = 0;
    int    facets          = 0;     // num_facets(state) after the action (0 for erase_all)
    bool   prime_tower     = false; // only meaningful for color
    double z_from          = 0.0;   // lower bound used for height_band (0 for whole or layer <= 1)
};

// Shared implementation of sw_SlicerPaint (and the alias). Throws std::runtime_error with a
// readable message on error; nothing is changed in that case.
//
// Stacking: whole/height_band/overhangs start from the EXISTING painting (deserialize of the
// current facets, always with the default max_ebt = ExtruderMax) and add to it.
// Refreshes like the paint gizmos do (update_model_object), so deliberately NO changed_object()
// (which among other things calls ensure_on_bed): update_info_items + schedule_background_process,
// and for color also notify_instance_update + notify_filament_usage_changed.
static SwSlicerPaintResult sw_slicer_paint(const json& params)
{
    if (!sw_slicer_has_param(params, "type") || !params.at("type").is_string())
        throw std::runtime_error("specify 'type' (support|seam|fuzzy_skin|color)");
    const std::string type = params.at("type").get<std::string>();
    if (type != "support" && type != "seam" && type != "fuzzy_skin" && type != "color")
        throw std::runtime_error("unknown paint type '" + type + "' (expected support|seam|fuzzy_skin|color)");
    if (!sw_slicer_has_param(params, "action") || !params.at("action").is_string())
        throw std::runtime_error("specify 'action' (erase_all|whole|height_band|overhangs)");
    const std::string action = params.at("action").get<std::string>();
    if (action != "erase_all" && action != "whole" && action != "height_band" && action != "overhangs")
        throw std::runtime_error("unknown action '" + action + "' (expected erase_all|whole|height_band|overhangs)");
    if (action == "overhangs" && type != "support")
        throw std::runtime_error("'overhangs' is only available for type 'support'");

    SwSlicerPaintResult res;
    const bool          erase = (action == "erase_all");
    EnforcerBlockerType state = EnforcerBlockerType::NONE;
    if (!erase)
        state = sw_slicer_paint_state(params, type);

    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        throw std::runtime_error("no active plater");

    // Height band in plate coordinates: [band_z, band_z + band_h].
    float band_z = -1.0e6f;
    float band_h = 2.0e6f;
    if (action == "height_band") {
        const bool has_layer = sw_slicer_has_param(params, "layer");
        if (has_layer && type != "color")
            throw std::runtime_error("'layer' is only available for type 'color'; specify 'z_from' (mm)");
        if (has_layer && sw_slicer_has_param(params, "z_from"))
            throw std::runtime_error("specify 'layer' or 'z_from', not both");
        if (has_layer && sw_slicer_has_param(params, "z_to"))
            throw std::runtime_error("'layer' and 'z_to' cannot be combined; use 'z_from' and 'z_to'");
        if (has_layer) {
            // Bottom of layer N = top Z of layer N-1 = zs[N-2] from the preview;
            // layer <= 1 = the whole model.
            if (!params.at("layer").is_number_integer())
                throw std::runtime_error("'layer' must be an integer (1-based)");
            const int layer = params.at("layer").get<int>();
            if (layer > 1) {
                GLCanvas3D* preview = plater->get_preview_canvas3D();
                if (preview == nullptr)
                    throw std::runtime_error("no preview canvas; slice first or specify 'z_from'");
                const std::vector<double>& zs  = preview->get_gcode_layers_zs();
                const size_t               idx = static_cast<size_t>(layer - 2);
                if (idx >= zs.size())
                    throw std::runtime_error("layer out of range or not sliced yet; slice first "
                                             "(and open the Preview) or specify 'z_from'");
                res.z_from = zs[idx];
                band_z     = static_cast<float>(res.z_from);
                band_h     = 1.0e5f;
            }
        } else {
            if (!sw_slicer_has_param(params, "z_from"))
                throw std::runtime_error(type == "color" ? "specify 'z_from' (mm) or 'layer' (1-based)"
                                                         : "specify 'z_from' (mm, plate coordinates)");
            double z_from = 0.0, z_to = 0.0;
            if (!sw_slicer_get_number(params, "z_from", 0.0, z_from))
                throw std::runtime_error("'z_from' must be a finite number (mm)");
            res.z_from = z_from;
            band_z     = static_cast<float>(z_from);
            band_h     = 1.0e5f;
            if (sw_slicer_has_param(params, "z_to")) {
                if (!sw_slicer_get_number(params, "z_to", 0.0, z_to))
                    throw std::runtime_error("'z_to' must be a finite number (mm)");
                if (!(z_to > z_from))
                    throw std::runtime_error("'z_to' must be greater than 'z_from'");
                band_h = static_cast<float>(z_to - z_from);
            }
        }
    }

    double angle = 0.0;
    if (action == "overhangs") {
        if (!sw_slicer_has_param(params, "angle"))
            throw std::runtime_error("specify 'angle' (degrees, 0 < angle <= 90)");
        if (!sw_slicer_get_number(params, "angle", 0.0, angle) || !(angle > 0.0 && angle <= 90.0))
            throw std::runtime_error("'angle' must be between 0 (exclusive) and 90 degrees");
    }

    sw_slicer_require_free_gizmo();
    sw_slicer_require_idle();
    const int obj = sw_slicer_resolve_object(params);
    res.object    = obj;
    int vol       = -1;
    if (sw_slicer_has_param(params, "volume")) {
        if (!params.at("volume").is_number_integer())
            throw std::runtime_error("'volume' must be an integer (0-based)");
        vol = params.at("volume").get<int>();
        if (vol < 0)
            throw std::runtime_error("volume " + std::to_string(vol) + " does not exist");
        sw_slicer_check_indices(obj, -1, vol);
    }
    ModelObject* mo = plater->model().objects[obj];
    if (vol >= 0 && !mo->volumes[vol]->is_model_part())
        throw std::runtime_error("volume " + std::to_string(vol) +
                                 " is not a model part; only model parts can be painted");
    if (!erase && mo->instances.empty())
        throw std::runtime_error("object " + std::to_string(obj) + " has no instances");
    // Make the selection visible on the target (the volume if specified), like the other handlers.
    sw_slicer_select(obj, -1, vol);

    // First build the new selector per volume without changing anything; an undo snapshot is
    // only taken if at least one volume actually changes.
    struct Pending
    {
        ModelVolume*                      mv;
        std::unique_ptr<TriangleSelector> sel; // nullptr for erase_all
    };
    std::vector<Pending> pending;
    for (size_t i = 0; i < mo->volumes.size(); ++i) {
        ModelVolume* mv = mo->volumes[i];
        if (mv == nullptr || !mv->is_model_part())
            continue;
        if (vol >= 0 && static_cast<int>(i) != vol)
            continue;
        FacetsAnnotation& facets = sw_slicer_paint_facets(mv, type);
        if (erase) {
            if (!facets.empty())
                pending.push_back({mv, nullptr});
            continue;
        }
        const TriangleMesh& mesh = mv->mesh();
        if (mesh.its.indices.empty())
            continue;

        auto sel = std::make_unique<TriangleSelector>(mesh);
        // Always with the default max_ebt (ExtruderMax): a smaller one asserts in debug builds
        // with mixed-filament painting.
        sel->deserialize(facets.get_data());

        if (action == "overhangs") {
            // Rebuilt from GLGizmoFdmSupports::select_facets_by_angle, but on the selector of
            // THIS volume (the original calls request_update_render_data on .back()).
            // Meaning: triangles whose normal lies within 'angle' degrees of straight down.
            const float       threshold    = static_cast<float>(Geometry::deg2rad(angle));
            const Transform3d trafo_matrix = mo->instances.front()->get_matrix_no_offset() * mv->get_matrix_no_offset();
            const Vec3f down  = (trafo_matrix.inverse() * (-Vec3d::UnitZ())).cast<float>().normalized();
            const Vec3f limit = (trafo_matrix.inverse() * Vec3d(std::sin(threshold), 0, -std::cos(threshold))).cast<float>().normalized();
            const float dot_limit = limit.dot(down);
            const indexed_triangle_set& its = mesh.its;
            int idx = 0;
            for (const stl_triangle_vertex_indices& face : its.indices) {
                if (its_face_normal(its, face).dot(down) > dot_limit)
                    sel->set_facet(idx, state);
                ++idx;
            }
        } else {
            // HeightRange cursor + select_patch with triangle_splitting, like the "Height range"
            // tool. The cursor compares absolute plate Z, so the trafo MUST include the
            // translation (instance * volume).
            const Transform3d                     to_plate = mo->instances.front()->get_matrix() * mv->get_matrix();
            const Vec3f                           camera_pos(0.0f, 0.0f, 1.0f); // ignored by HeightRange
            const TriangleSelector::ClippingPlane clp;                          // offset=FLT_MAX -> inactive
            std::unique_ptr<TriangleSelector::Cursor> cursor =
                TriangleSelector::SinglePointCursor::cursor_factory(band_z, camera_pos, band_h, to_plate, clp);
            sel->select_patch(/*facet_start=*/0, std::move(cursor), state,
                              /*trafo_no_translate=*/to_plate, /*triangle_splitting=*/true,
                              /*highlight_by_angle_deg=*/0.0f);
        }
        sel->garbage_collect();
        res.facets += sel->num_facets(state);
        if (sel->serialize() != facets.get_data())
            pending.push_back({mv, std::move(sel)});
    }

    if (!pending.empty()) {
        Plater::TakeSnapshot snapshot(plater, "API: Paint " + type + " " + action);
        for (Pending& p : pending) {
            FacetsAnnotation& facets = sw_slicer_paint_facets(p.mv, type);
            if (erase) {
                facets.reset();
                ++res.volumes_painted;
            } else if (facets.set(*p.sel)) {
                ++res.volumes_painted;
            }
        }
    }
    res.painted = res.volumes_painted > 0;

    if (res.painted) {
        // Like update_model_object() of the paint gizmos.
        wxGetApp().obj_list()->update_info_items(static_cast<size_t>(obj));
        if (type == "color")
            plater->get_partplate_list().notify_instance_update(obj, 0);
        plater->schedule_background_process();
        if (type == "color")
            plater->notify_filament_usage_changed();
        sw_slicer_canvas()->set_as_dirty();
    }

    // Color change with a filament state: enable enable_prime_tower so a real wipe/prime tower
    // is generated (print preset; applied lightly like sw_SlicerSetConfig).
    // Only if something was actually painted and there are at least two filaments.
    if (type == "color" && !erase && state != EnforcerBlockerType::NONE && res.painted &&
        wxGetApp().filaments_cnt() >= 2) {
        if (Tab* ptab = wxGetApp().get_tab(Preset::TYPE_PRINT)) {
            if (DynamicPrintConfig* pcfg = ptab->get_config()) {
                try {
                    pcfg->set_deserialize_strict("enable_prime_tower", "1");
                    ptab->update_dirty();
                    ptab->reload_config();
                    res.prime_tower = true;
                } catch (std::exception&) {}
            }
        }
    }
    return res;
}

// Paint support / seam / fuzzy skin / color parametrically (see sw_slicer_paint).
// Params: {type, object?, volume?, action, state?, z_from?, z_to?, layer?, angle?}.
void SSWCP_SliceProject_Instance::sw_SlicerPaint()
{
    try {
        const SwSlicerPaintResult r    = sw_slicer_paint(m_param_data);
        const std::string         type = m_param_data.at("type").get<std::string>();
        m_res_data["object"]          = r.object;
        m_res_data["type"]            = type;
        m_res_data["action"]          = m_param_data.at("action").get<std::string>();
        m_res_data["painted"]         = r.painted;
        m_res_data["volumes_painted"] = r.volumes_painted;
        m_res_data["facets"]          = r.facets;
        if (type == "color")
            m_res_data["prime_tower"] = r.prime_tower;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- Color change from layer N (alias) ------------------------------------------
// Params: {extruder:int (1-based filament), layer:int (1-based) | z:float (mm), object?}.
// Delegates to sw_slicer_paint {type:"color", action:"height_band", state:extruder}: filament
// N applies from (the bottom of) layer N up to the top. This STACKS on the existing painting
// (to start over: sw_SlicerPaint erase_all), works on the selected or specified object,
// creates one undo step and refreshes like the paint gizmo.
void SSWCP_SliceProject_Instance::sw_SlicerSetColorFromLayer()
{
    try {
        if (!sw_slicer_has_param(m_param_data, "extruder") || !m_param_data.at("extruder").is_number_integer())
            throw std::runtime_error("specify 'extruder' (1-based filament number)");
        const int extruder    = m_param_data.at("extruder").get<int>();
        const int n_filaments = wxGetApp().filaments_cnt();
        if (extruder < 1 || extruder > n_filaments) {
            handle_general_fail(-1, wxString::Format(
                "extruder %d does not exist (this setup has %d filament(s))", extruder, n_filaments));
            return;
        }
        json p;
        p["type"]   = "color";
        p["action"] = "height_band";
        p["state"]  = extruder;
        if (sw_slicer_has_param(m_param_data, "object")) {
            p["object"] = m_param_data.at("object");
        } else {
            // Without 'object': the (single) selected object, otherwise the first object.
            // An empty plate yields the error from sw_slicer_resolve_object.
            Plater*   plater = wxGetApp().plater();
            const int n      = (plater != nullptr) ? static_cast<int>(plater->model().objects.size()) : 0;
            if (n > 0) {
                const int sel = sw_slicer_canvas()->get_selection().get_object_idx();
                p["object"]   = (sel >= 0 && sel < n) ? sel : 0;
            }
        }
        if (sw_slicer_has_param(m_param_data, "layer")) {
            // Like get<int>(): a number such as 5.0 also counts as layer 5.
            if (!m_param_data.at("layer").is_number())
                throw std::runtime_error("'layer' must be a number (1-based)");
            p["layer"] = m_param_data.at("layer").get<int>();
        } else if (sw_slicer_has_param(m_param_data, "z"))
            p["z_from"] = m_param_data.at("z");
        else
            throw std::runtime_error("specify 'layer' (1-based) or 'z' (mm)");

        const SwSlicerPaintResult r = sw_slicer_paint(p);
        m_res_data["painted"]         = r.painted;
        m_res_data["extruder"]        = extruder;
        m_res_data["z_from"]          = r.z_from;
        m_res_data["filament_count"]  = n_filaments;
        m_res_data["prime_tower"]     = r.prime_tower;
        m_res_data["volumes_painted"] = r.volumes_painted;
        m_res_data["facets_painted"]  = r.facets;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- 3D toolbar: common helpers for text, mesh boolean, measure and assemble ---------

// Required index parameter (integer >= 0). Throws for a missing or invalid value.
static int sw_slicer_get_index(const json& params, const char* key)
{
    if (!sw_slicer_has_param(params, key))
        throw std::runtime_error(std::string("specify '") + key + "' (0-based)");
    const json& j = params.at(key);
    if (!j.is_number_integer() || j.get<long long>() < 0)
        throw std::runtime_error(std::string("'") + key + "' must be an integer >= 0");
    return j.get<int>();
}

// Find the indices (object, volume) of a volume in the model. Returns false if it is not
// (or no longer) in the model.
static bool sw_slicer_find_volume(const Model& model, const ModelVolume* mv, int& obj, int& vol)
{
    for (size_t o = 0; o < model.objects.size(); ++o) {
        const ModelVolumePtrs& vols = model.objects[o]->volumes;
        for (size_t v = 0; v < vols.size(); ++v)
            if (vols[v] == mv) {
                obj = static_cast<int>(o);
                vol = static_cast<int>(v);
                return true;
            }
    }
    return false;
}

// --- 3D toolbar: text (Emboss) ---------------------------------------------------------

// Limits of the Emboss gizmo (static 'limits' in GLGizmoEmboss.cpp, not reachable from
// outside): letter height size_in_mm 0.1-1000 mm, depth (emboss) 0.01-1e4 mm. The gizmo clamps;
// the API refuses, so the caller does not silently get a different size.
static constexpr double SW_SLICER_TEXT_HEIGHT_MIN = 0.1;
static constexpr double SW_SLICER_TEXT_HEIGHT_MAX = 1000.0;
static constexpr double SW_SLICER_TEXT_DEPTH_MIN  = 0.01;
static constexpr double SW_SLICER_TEXT_DEPTH_MAX  = 1e4;

// Optional size in mm within [min_mm, max_mm]. std::nullopt if missing; throws for an
// invalid value.
static std::optional<double> sw_slicer_get_positive_mm(const json& params, const char* key, double min_mm, double max_mm)
{
    if (!sw_slicer_has_param(params, key))
        return std::nullopt;
    double v = 0.0;
    if (!sw_slicer_get_number(params, key, 0.0, v) || !std::isfinite(v) || v < min_mm || v > max_mm) {
        std::ostringstream ss;
        ss << "'" << key << "' must be a number between " << min_mm << " and " << max_mm << " (mm)";
        throw std::runtime_error(ss.str());
    }
    return v;
}

// Optional text: not empty and not only whitespace. std::nullopt if missing.
static std::optional<std::string> sw_slicer_get_text(const json& params)
{
    if (!sw_slicer_has_param(params, "text"))
        return std::nullopt;
    const json& j = params.at("text");
    if (!j.is_string())
        throw std::runtime_error("'text' must be a string");
    const std::string s     = j.get<std::string>();
    bool              blank = true;
    for (unsigned char c : s)
        if (!std::isspace(c)) {
            blank = false;
            break;
        }
    if (blank)
        throw std::runtime_error("'text' must not be empty or consist only of whitespace");
    return s;
}

// Optional system font by name (like the font list of the Emboss gizmo, see
// GLGizmoEmboss::select_facename). Throws if the font does not exist or cannot be loaded.
static std::optional<wxFont> sw_slicer_get_font(const json& params)
{
    if (!sw_slicer_has_param(params, "font"))
        return std::nullopt;
    const json& j = params.at("font");
    if (!j.is_string())
        throw std::runtime_error("'font' must be a string (name of a system font)");
    const std::string name = j.get<std::string>();
    const wxString    face = wxString::FromUTF8(name.c_str());
    if (name.empty() || !wxFontEnumerator::IsValidFacename(face))
        throw std::runtime_error("font '" + name + "' not found");
    wxFont font(wxFontInfo().FaceName(face).Encoding(wxFONTENCODING_SYSTEM));
    if (!font.IsOk() || !WxFontUtils::can_load(font))
        throw std::runtime_error("font '" + name + "' not found");
    return font;
}

// The text fields that can be specified for AddText/EditText.
struct SwSlicerTextStyle
{
    std::optional<std::string> text;
    std::optional<double>      height_mm;
    std::optional<double>      depth_mm;
    std::optional<wxFont>      font;

    bool has_style() const { return height_mm.has_value() || depth_mm.has_value() || font.has_value(); }
};

static SwSlicerTextStyle sw_slicer_parse_text_style(const json& params)
{
    SwSlicerTextStyle st;
    st.text      = sw_slicer_get_text(params);
    st.height_mm = sw_slicer_get_positive_mm(params, "height_mm", SW_SLICER_TEXT_HEIGHT_MIN, SW_SLICER_TEXT_HEIGHT_MAX);
    st.depth_mm  = sw_slicer_get_positive_mm(params, "depth_mm", SW_SLICER_TEXT_DEPTH_MIN, SW_SLICER_TEXT_DEPTH_MAX);
    st.font      = sw_slicer_get_font(params);
    return st;
}

// Set the specified fields in the text configuration of a volume. This only changes the
// configuration; the mesh follows with sw_slicer_re_emboss. Same fields as the gizmo:
// height = style.prop.size_in_mm, depth = emboss_shape->projection.depth, font = style.path/
// type + face_name etc. (like StyleManager::set_wx_font; the height is kept).
static void sw_slicer_apply_text_style(ModelVolume& mv, const SwSlicerTextStyle& st)
{
    TextConfiguration& tc = *mv.text_configuration;
    if (st.text.has_value())
        tc.text = *st.text;
    if (st.font.has_value()) {
        const float size = tc.style.prop.size_in_mm;
        tc.style.path    = WxFontUtils::store_wxFont(*st.font);
        tc.style.type    = WxFontUtils::get_current_type();
        WxFontUtils::update_property(tc.style.prop, *st.font);
        tc.style.prop.size_in_mm = size;
    }
    if (st.height_mm.has_value())
        tc.style.prop.size_in_mm = static_cast<float>(*st.height_mm);
    if (st.depth_mm.has_value())
        mv.emboss_shape->projection.depth = *st.depth_mm;
}

// Total wait budget for the job worker within one handler. Worker::wait_for_idle itself
// applies its timeout per message (BlockingWait per consume_one), not in total; therefore we
// wait in short slices until the deadline. Messages (finalize etc.) are delivered on the GUI
// thread this way as well. Throws when the budget is exceeded (the job is then still running).
class SwSlicerJobBudget
{
public:
    explicit SwSlicerJobBudget(unsigned total_ms)
        : m_total_ms(total_ms), m_end(std::chrono::steady_clock::now() + std::chrono::milliseconds(total_ms))
    {}

    void wait_for_idle(Plater* plater) const
    {
        auto& w = plater->get_ui_job_worker();
        while (!w.is_idle()) {
            const long long left =
                std::chrono::duration_cast<std::chrono::milliseconds>(m_end - std::chrono::steady_clock::now()).count();
            if (left <= 0)
                throw std::runtime_error("the text job did not finish within " + std::to_string(m_total_ms / 1000) +
                                         " s (the app is still busy)");
            // Never pass 0: that means "wait forever".
            w.wait_for_idle(static_cast<unsigned>(std::clamp<long long>(left, 1, 200)));
        }
    }

private:
    unsigned                              m_total_ms;
    std::chrono::steady_clock::time_point m_end;
};

// Regenerate the mesh of a text volume (GLGizmoEmboss::re_emboss) and wait until the job has
// finished; its finalize then runs here on the GUI thread (so within the caller's API mode).
// On success the volume gets a new ObjectID (UpdateJob::update_volume -> set_new_unique_id);
// that is how a failed job (empty mesh, font cannot be loaded) is detected.
// Throws when the budget is exhausted (the job is then still running).
static bool sw_slicer_re_emboss(Plater* plater, ModelVolume& mv, const SwSlicerJobBudget& budget)
{
    const ObjectID old_id = mv.id();
    if (!GLGizmoEmboss::re_emboss(mv))
        return false;
    budget.wait_for_idle(plater);
    return mv.id() != old_id;
}

// Add a 3D text, like "Add text" in the context menu without a mouse position
// (GLGizmoEmboss::create_volume). With 'object' the text is placed on or next to that object;
// without 'object' it becomes a new object on the bed. The job runs synchronously
// (wait_for_idle) in API mode, so error messages of the job become notes instead of dialogs.
// Afterwards height/depth/font are applied if given (re_emboss) and the Emboss gizmo that the
// job opened itself is closed. Params: {text, object?, height_mm?, depth_mm?, font?, part_type?}.
void SSWCP_SliceProject_Instance::sw_SlicerAddText()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();

        const SwSlicerTextStyle st = sw_slicer_parse_text_style(m_param_data);
        if (!st.text.has_value())
            throw std::runtime_error("specify 'text' (not empty and not only whitespace)");

        ModelVolumeType type = ModelVolumeType::MODEL_PART;
        if (sw_slicer_has_param(m_param_data, "part_type")) {
            const json&       j  = m_param_data.at("part_type");
            const std::string pt = j.is_string() ? j.get<std::string>() : std::string();
            if (pt == "part")
                type = ModelVolumeType::MODEL_PART;
            else if (pt == "negative")
                type = ModelVolumeType::NEGATIVE_VOLUME;
            else if (pt == "modifier")
                type = ModelVolumeType::PARAMETER_MODIFIER;
            else
                throw std::runtime_error("unknown part_type (expected part|negative|modifier)");
        }
        const bool has_object = sw_slicer_has_param(m_param_data, "object");
        // Without an object the job creates a new object and ignores the type (always a part).
        if (!has_object && type != ModelVolumeType::MODEL_PART)
            throw std::runtime_error("part_type 'negative' or 'modifier' requires an existing object: specify 'object'");

        GLCanvas3D*      canvas = sw_slicer_canvas();
        GLGizmosManager& gm     = canvas->get_gizmos_manager();
        GLGizmoEmboss*   emboss = dynamic_cast<GLGizmoEmboss*>(gm.get_gizmo(GLGizmosManager::Emboss));
        if (emboss == nullptr)
            throw std::runtime_error("the text tool is not available");

        Model& model = plater->model();
        if (has_object) {
            const int obj = sw_slicer_resolve_object(m_param_data);
            if (model.objects[obj]->is_cut())
                throw std::runtime_error("text cannot be placed on a cut object");
            sw_slicer_select(obj, -1, -1);
        } else {
            // Empty selection: start_create_volume_without_position then creates a new object.
            canvas->get_selection().clear();
            wxGetApp().obj_list()->update_selections();
            canvas->update_gizmos_on_off_state();
            canvas->set_as_dirty();
        }

        // ObjectIDs of all existing volumes: the new text volume is the only one missing.
        std::set<size_t> old_ids;
        for (const ModelObject* mo : model.objects)
            for (const ModelVolume* mv : mo->volumes)
                old_ids.insert(mv->id().id);

        ModelVolume*             new_mv   = nullptr;
        bool                     restyled = true;
        std::vector<std::string> notes;
        const SwSlicerJobBudget  budget(30000); // total for create + restyle
        // The job's finalize opens the Emboss gizmo. That is our own tool (the free-gizmo check
        // allowed no other one to be open), so it may be closed; also on the error path,
        // otherwise that open gizmo blocks every following API call.
        auto close_emboss = [&gm, canvas]() {
            if (gm.get_current_type() == GLGizmosManager::Emboss)
                canvas->reset_all_gizmos();
        };
        {
            sswcp_api::Scope     api_scope;
            Plater::TakeSnapshot snapshot(plater, "API: Add text");
            try {
                if (emboss->create_volume(type, *st.text)) {
                    budget.wait_for_idle(plater);
                    for (ModelObject* mo : model.objects) {
                        for (ModelVolume* mv : mo->volumes)
                            if (mv->is_text() && old_ids.count(mv->id().id) == 0) {
                                new_mv = mv;
                                break;
                            }
                        if (new_mv != nullptr)
                            break;
                    }
                }
                if (new_mv != nullptr && st.has_style() && new_mv->emboss_shape.has_value()) {
                    // The text itself is already correct; only height/depth/font.
                    SwSlicerTextStyle style = st;
                    style.text.reset();
                    const TextConfiguration old_tc   = *new_mv->text_configuration;
                    const EmbossProjection  old_proj = new_mv->emboss_shape->projection;
                    sw_slicer_apply_text_style(*new_mv, style);
                    restyled = sw_slicer_re_emboss(plater, *new_mv, budget);
                    if (!restyled) {
                        *new_mv->text_configuration          = old_tc;
                        new_mv->emboss_shape->projection = old_proj;
                    }
                }
            } catch (...) {
                // Close within the snapshot scope: no separate "leave gizmo" step.
                close_emboss();
                canvas->set_as_dirty();
                throw;
            }
            close_emboss();
            notes = sswcp_api::notes();
        }
        canvas->set_as_dirty();
        canvas->request_extra_frame();

        // Resolve object/volume again: create_volume sorts the volumes
        // (reorder_volumes_and_get_selection) and re_emboss assigns a new ObjectID.
        int obj = -1, vol = -1;
        if (new_mv == nullptr || !sw_slicer_find_volume(model, new_mv, obj, vol)) {
            std::string err = "the text could not be created";
            for (const std::string& n : notes)
                err += "; " + n;
            handle_general_fail(-1, wxString::FromUTF8(err.c_str()));
            return;
        }
        if (!restyled)
            notes.push_back("height/depth/font could not be applied; the text was created in the default style");
        m_res_data["object"] = obj;
        m_res_data["volume"] = vol;
        m_res_data["name"]   = new_mv->name;
        m_res_data["text"]   = new_mv->text_configuration->text;
        m_res_data["notes"]  = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Change text, height, depth and/or font of an existing text volume and regenerate the mesh
// (re_emboss), synchronously and in API mode. If that fails, the old configuration is
// restored. The Emboss gizmo must not be open (free-gizmo check): it would put its own style
// over our change. Params: {object, volume, text?, height_mm?, depth_mm?, font?}.
void SSWCP_SliceProject_Instance::sw_SlicerEditText()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        const int obj = sw_slicer_resolve_object(m_param_data);
        const int vol = sw_slicer_get_index(m_param_data, "volume");
        sw_slicer_check_indices(obj, -1, vol);
        Model&       model = plater->model();
        ModelVolume* mv    = model.objects[obj]->volumes[vol];
        if (!mv->is_text() || !mv->emboss_shape.has_value())
            throw std::runtime_error("this volume is not a text");

        const SwSlicerTextStyle st = sw_slicer_parse_text_style(m_param_data);
        if (!st.text.has_value() && !st.has_style())
            throw std::runtime_error("specify at least one field to change (text, height_mm, depth_mm or font)");

        // Like in the app: the text is selected while editing (re_emboss reads the selection).
        sw_slicer_select(obj, -1, vol);
        const TextConfiguration  old_tc   = *mv->text_configuration;
        const EmbossProjection   old_proj = mv->emboss_shape->projection;
        bool                     ok       = false;
        std::vector<std::string> notes;
        {
            sswcp_api::Scope     api_scope;
            Plater::TakeSnapshot snapshot(plater, "API: Edit text");
            sw_slicer_apply_text_style(*mv, st);
            ok = sw_slicer_re_emboss(plater, *mv, SwSlicerJobBudget(30000));
            if (!ok) {
                *mv->text_configuration      = old_tc;
                mv->emboss_shape->projection = old_proj;
            }
            notes = sswcp_api::notes();
        }
        sw_slicer_canvas()->set_as_dirty();
        sw_slicer_canvas()->request_extra_frame();
        if (!ok) {
            std::string err = "regenerating the text failed (font?)";
            for (const std::string& n : notes)
                err += "; " + n;
            handle_general_fail(-1, wxString::FromUTF8(err.c_str()));
            return;
        }
        // re_emboss keeps the volume in place (only a new ObjectID).
        int r_obj = obj, r_vol = vol;
        sw_slicer_find_volume(model, mv, r_obj, r_vol);
        m_res_data["object"] = r_obj;
        m_res_data["volume"] = r_vol;
        m_res_data["name"]   = mv->name;
        m_res_data["text"]   = mv->text_configuration->text;
        m_res_data["notes"]  = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- 3D toolbar: mesh boolean ----------------------------------------------------------

// Mesh boolean between two volumes of one object, like GLGizmoMeshBoolean (mcut). Both
// meshes are taken to the object frame (volume matrix, like m_src/m_tool.trafo in the gizmo).
// The result becomes a new volume; add_volume() centers the geometry with offset = bbox
// center, so it ends up at the geometrically correct position (deliberately NOT the gizmo's
// set_offset(old offset)). The source volume is always removed; the tool volume only with
// delete_input (default true). Difference from the app: there union always removes the tool
// volume; the API respects delete_input:false for union as well.
// Refuses (before the snapshot) if no solid part (MODEL_PART) would remain afterwards,
// like ObjectList::del_subobject_from_object ("Deleting the last solid part is not allowed").
// Params: {object, src_volume, tool_volume, operation, delete_input?}.
void SSWCP_SliceProject_Instance::sw_SlicerMeshBoolean()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        if (!sw_slicer_has_param(m_param_data, "object"))
            throw std::runtime_error("specify 'object' (0-based)");
        const int    obj   = sw_slicer_resolve_object(m_param_data);
        Model&       model = plater->model();
        ModelObject* mo    = model.objects[obj];
        // Deleting a volume of a cut object asks for confirmation in the app (dialog).
        if (mo->is_cut())
            throw std::runtime_error("this object is a cut object; mesh boolean is not possible there");
        if (mo->volumes.size() < 2)
            throw std::runtime_error("object " + std::to_string(obj) +
                                     " has only one volume; mesh boolean needs two");
        const int src_idx  = sw_slicer_get_index(m_param_data, "src_volume");
        const int tool_idx = sw_slicer_get_index(m_param_data, "tool_volume");
        sw_slicer_check_indices(obj, -1, src_idx);
        sw_slicer_check_indices(obj, -1, tool_idx);
        if (src_idx == tool_idx)
            throw std::runtime_error("src_volume and tool_volume must differ");

        if (!sw_slicer_has_param(m_param_data, "operation") || !m_param_data.at("operation").is_string())
            throw std::runtime_error("specify 'operation' (union|difference|intersection)");
        const std::string op = m_param_data.at("operation").get<std::string>();
        std::string       opts, suffix;
        if (op == "union") {
            opts   = "UNION";
            suffix = "union";
        } else if (op == "difference") {
            opts   = "A_NOT_B";
            suffix = "difference";
        } else if (op == "intersection") {
            opts   = "INTERSECTION";
            suffix = "intersection";
        } else {
            throw std::runtime_error("unknown operation '" + op + "' (expected union|difference|intersection)");
        }
        if (sw_slicer_has_param(m_param_data, "delete_input") && !m_param_data.at("delete_input").is_boolean())
            throw std::runtime_error("'delete_input' must be true or false");
        const bool delete_input = sw_slicer_get_bool(m_param_data, "delete_input", true);

        // Compute first; an empty result is an error without any change and without an undo step.
        const ModelVolume* src  = mo->volumes[src_idx];
        const ModelVolume* tool = mo->volumes[tool_idx];
        TriangleMesh       a    = src->mesh();
        a.transform(src->get_matrix(), true);
        TriangleMesh b = tool->mesh();
        b.transform(tool->get_matrix(), true);
        std::vector<TriangleMesh> res;
        Slic3r::MeshBoolean::mcut::make_boolean(a, b, res, opts);
        if (res.empty() || res.front().empty())
            throw std::runtime_error("the boolean operation produced no result (do the volumes touch?)");

        std::vector<int> removed{src_idx};
        if (delete_input)
            removed.push_back(tool_idx);
        std::sort(removed.begin(), removed.end());

        // Last solid part: count the MODEL_PARTs that remain, plus the result (which gets the
        // type of src). Zero -> refuse, without any change or snapshot.
        int parts_left = (src->type() == ModelVolumeType::MODEL_PART) ? 1 : 0;
        for (size_t i = 0; i < mo->volumes.size(); ++i)
            if (mo->volumes[i]->is_model_part() &&
                std::find(removed.begin(), removed.end(), static_cast<int>(i)) == removed.end())
                ++parts_left;
        if (parts_left == 0)
            throw std::runtime_error("no solid part would remain after this operation; use delete_input:false "
                                     "or choose a solid part as src_volume");

        sw_slicer_select(obj, -1, -1);
        ModelVolume* nv = nullptr;
        {
            Plater::TakeSnapshot snapshot(plater, "API: Mesh boolean");
            nv       = mo->add_volume(std::move(res.front()), src->type());
            nv->name = src->name + " - " + suffix;
            nv->config.apply(src->config);
            nv->set_material_id(src->material_id());
            // add_volume() appends at the end, so the old indices are still valid; highest first.
            for (auto it = removed.rbegin(); it != removed.rend(); ++it)
                mo->delete_volume(static_cast<size_t>(*it));
            src  = nullptr; // deleted
            tool = nullptr;

            // Same refresh as the gizmo: scene, then rebuild the volumes in the object list
            // (reorder_volumes_and_get_selection also sorts and calls changed_object). If only
            // one volume remains, the volume rows disappear from the list.
            plater->update();
            ObjectList* obj_list = wxGetApp().obj_list();
            if (mo->volumes.size() > 1) {
                wxDataViewItemArray items =
                    obj_list->reorder_volumes_and_get_selection(obj, [nv](const ModelVolume* v) { return v == nv; });
                if (!items.IsEmpty())
                    obj_list->select_item(items.front());
            } else {
                obj_list->update_info_items(static_cast<size_t>(obj));
                obj_list->add_volumes_to_object_in_list(static_cast<size_t>(obj));
                plater->changed_object(obj);
                sw_slicer_select(obj, -1, -1);
            }
        }
        sw_slicer_canvas()->set_as_dirty();
        sw_slicer_canvas()->request_extra_frame();

        int r_obj = obj, r_vol = -1;
        if (!sw_slicer_find_volume(model, nv, r_obj, r_vol))
            throw std::runtime_error("the new volume cannot be found");
        m_res_data["object"]          = r_obj;
        m_res_data["volume"]          = r_vol;
        m_res_data["name"]            = nv->name;
        m_res_data["volumes_removed"] = removed;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- 3D toolbar: measure and assemble --------------------------------------------------
// The user picks the features in the app; the API reads the result and performs the
// assembly actions. Selecting features through the API is not supported.

// The open measure or assembly tool (GLGizmoAssembly is a GLGizmoMeasure). name becomes
// "measure" or "assemble". Throws if the requested tool is not open.
static GLGizmoMeasure* sw_slicer_measure_gizmo(std::string& name, bool assembly_only)
{
    GLGizmosManager&             gm = sw_slicer_canvas()->get_gizmos_manager();
    const GLGizmosManager::EType t  = gm.get_current_type();
    if (t == GLGizmosManager::Assembly)
        name = "assemble";
    else if (t == GLGizmosManager::Measure && !assembly_only)
        name = "measure";
    else if (assembly_only)
        throw std::runtime_error("open the assembly tool first (sw_SlicerOpenGizmo with 'assemble')");
    else
        throw std::runtime_error("open the measure or assembly tool first (sw_SlicerOpenGizmo with 'measure' or 'assemble')");
    GLGizmoMeasure* g = dynamic_cast<GLGizmoMeasure*>(gm.get_gizmo(t));
    if (g == nullptr)
        throw std::runtime_error("the measure tool is not available");
    return g;
}

// The measurement as JSON. Like show_distance_xyz_ui(): only with two features, the direct
// distance only if it differs from the perpendicular one, and distance_xyz only if non-zero.
// Always in mm and degrees (the app's inch display does not apply here).
static json sw_slicer_measurement_json(const GLGizmoMeasure& g, const std::string& name)
{
    json r;
    r["gizmo"]          = name;
    const int features  = g.api_selected_feature_count();
    r["features"]       = features;
    bool has_data       = false;
    if (features == 2) {
        const Measure::MeasurementResult& m = g.get_measurement_result();
        if (m.angle.has_value()) {
            r["angle_deg"] = Geometry::rad2deg(m.angle->angle);
            has_data       = true;
        }
        if (m.distance_infinite.has_value()) {
            r["distance"] = m.distance_infinite->dist;
            has_data      = true;
        }
        const bool show_strict = m.distance_strict.has_value() &&
                                 (!m.distance_infinite.has_value() ||
                                  std::abs(m.distance_strict->dist - m.distance_infinite->dist) > EPSILON);
        if (show_strict) {
            r["direct_distance"] = m.distance_strict->dist;
            has_data             = true;
        }
        if (m.distance_xyz.has_value() && m.distance_xyz->norm() > EPSILON) {
            r["distance_xyz"] = sw_slicer_vec3(*m.distance_xyz);
            has_data          = true;
        }
    }
    r["has_data"] = has_data;
    return r;
}

// Read the measurement of the open measure or assembly tool. Without a (complete) measurement:
// has_data:false, not an error.
void SSWCP_SliceProject_Instance::sw_SlicerGetMeasurement()
{
    try {
        std::string           name;
        const GLGizmoMeasure* g = sw_slicer_measure_gizmo(name, false);
        m_res_data              = sw_slicer_measurement_json(*g, name);
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Assembly action on the two picked faces (GLGizmoMeasure::api_check_assembly_action
// + api_run_assembly_action). The assembly tool has to be open, so no free-gizmo check;
// idle is required. Exactly one undo step "API: Assemble <action>"; an action that would
// change nothing yields done:false with a note and no undo step.
// Reply: {action, done, measurement, notes}. Params: {action, value?}.
void SSWCP_SliceProject_Instance::sw_SlicerAssemble()
{
    try {
        sw_slicer_require_idle();
        std::string     name;
        GLGizmoMeasure* g = sw_slicer_measure_gizmo(name, true);
        if (!sw_slicer_has_param(m_param_data, "action") || !m_param_data.at("action").is_string())
            throw std::runtime_error("specify 'action' (parallel|center_coincidence|flip|parallel_distance|rotate_around_center)");
        const std::string action = m_param_data.at("action").get<std::string>();
        double            value  = 0.0;
        if (action == "parallel_distance" || action == "rotate_around_center") {
            if (!sw_slicer_has_param(m_param_data, "value"))
                throw std::runtime_error("action '" + action + "' requires 'value'");
            if (!sw_slicer_get_number(m_param_data, "value", 0.0, value))
                throw std::runtime_error("'value' must be a finite number");
        }
        std::string err;
        bool        no_op = false;
        if (!g->api_check_assembly_action(action, value, err, no_op)) {
            handle_general_fail(-1, wxString::FromUTF8(err.c_str()));
            return;
        }
        std::vector<std::string> notes;
        if (no_op) {
            // Like the gizmo's input fields: do nothing, so no undo step either.
            notes.push_back(action == "parallel_distance" ? "the faces are already at this distance; nothing changed"
                                                          : "rotation angle 0; nothing changed");
        } else {
            // One named undo step: the gizmo's set_* functions take GizmoAction snapshots
            // themselves; after leaving the gizmo those are renamed to "Gizmo action"
            // (reduce_noisy_snapshots) and would be refused by the API undo. TakeSnapshot
            // suppresses them (m_prevent_snapshots) and takes an Action snapshot itself, which
            // reduce_noisy_snapshots does not rename.
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), "API: Assemble " + action);
            g->api_run_assembly_action(action, value);
        }
        if (!no_op && action == "parallel_distance") {
            // GLCanvas3D::do_move() puts an object that would float above the bed after the
            // move back on the bed ("Fixes flying instances"). The requested distance is then
            // not reached; report that instead of silently returning done:true.
            const Measure::AssemblyAction& a = g->get_assembly_action();
            if (!a.has_parallel_distance || std::abs(a.parallel_distance - value) > 0.01) {
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(2) << "requested distance " << value << " mm not reached";
                if (a.has_parallel_distance)
                    ss << " (now " << a.parallel_distance << " mm)";
                ss << ": the app puts an object that would float above the bed back on the bed; "
                      "move the other object first or pick the other face as face 2";
                notes.push_back(ss.str());
            }
        }
        m_res_data["action"]      = action;
        m_res_data["done"]        = !no_op;
        m_res_data["measurement"] = sw_slicer_measurement_json(*g, name);
        m_res_data["notes"]       = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Clear the picked features in the open measure or assembly tool (like the Delete key).
void SSWCP_SliceProject_Instance::sw_SlicerResetMeasurement()
{
    try {
        std::string     name;
        GLGizmoMeasure* g = sw_slicer_measure_gizmo(name, false);
        g->api_reset_features();
        m_res_data["reset"] = true;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- Project lifecycle: common helpers ----------------------------------------
// All helpers throw std::runtime_error with a readable message; the handlers catch it and
// return it as a regular error (handle_general_fail).

// Does the project have unsaved changes? The check of Plater::close_with_confirm(), plus a
// rule for an empty model, which up_to_date() always treats as clean: an empty model is dirty
// only if the undo stack holds a project-changing step since the last save, new project or
// load (for example "delete all" on a modified project). A fresh or new empty project is
// clean. Unsaved preset edits are reported separately (presets_dirty).
static bool sw_slicer_project_dirty()
{
    Plater* plater = wxGetApp().plater();
    if (!plater->up_to_date(false, false))
        return true;
    return plater->model().objects.empty() && plater->undo_redo_stack_main().project_modified();
}

// Do the presets have unsaved edits?
static bool sw_slicer_presets_dirty() { return wxGetApp().has_current_preset_changes(); }

// Project switches, saving and deleting are refused while slicing is in progress or while a
// project is being loaded (the loading progress dialog lets other requests through).
static void sw_slicer_require_not_slicing()
{
    Plater* plater = wxGetApp().plater();
    if (plater->is_background_process_slicing())
        throw std::runtime_error("not possible while slicing is in progress");
    if (plater->is_loading_project())
        throw std::runtime_error("not possible while a project is being loaded");
}

// A project switch would silently abort a scheduled G-code export.
static void sw_slicer_require_no_export()
{
    if (wxGetApp().plater()->is_export_gcode_scheduled())
        throw std::runtime_error("an export is still running; wait until it has finished");
}

// Optional flag that counts only if it is exactly true (missing, null, false or another type = not set).
static bool sw_slicer_flag_true(const json& params, const char* key)
{
    return params.count(key) && params.at(key).is_boolean() && params.at(key).get<bool>();
}

// New/open project clear the undo stack: unsaved changes may only be dropped on explicit request.
static void sw_slicer_require_discard(const json& params)
{
    if (sw_slicer_project_dirty() && !sw_slicer_flag_true(params, "discard_unsaved"))
        throw std::runtime_error(
            "the project has unsaved changes; save it first (sw_SlicerSaveProject) or pass discard_unsaved:true");
}

// What to do with unsaved preset edits: "" (there are none), "keep" or "discard".
// Without allow_keep only "discard" is accepted.
static std::string sw_slicer_preset_choice(const json& params, bool allow_keep)
{
    if (!sw_slicer_presets_dirty())
        return "";
    if (!sw_slicer_has_param(params, "preset_changes"))
        throw std::runtime_error(allow_keep ? "the presets have unsaved changes; pass preset_changes:'keep' or 'discard'" :
                                              "the presets have unsaved changes; pass preset_changes:'discard'");
    const json&       j      = params.at("preset_changes");
    const std::string choice = j.is_string() ? j.get<std::string>() : std::string();
    if (choice == "discard" || (allow_keep && choice == "keep"))
        return choice;
    throw std::runtime_error(allow_keep ? "preset_changes must be 'keep' or 'discard'" : "preset_changes must be 'discard'");
}

// Discard unsaved preset edits, like the "Discard" button of the unsaved-changes dialog
// (same steps as GUI_App::check_and_keep_current_preset_changes()).
static void sw_slicer_discard_preset_changes()
{
    GUI_App&                app  = wxGetApp();
    const PrinterTechnology tech = app.preset_bundle->printers.get_edited_preset().printer_technology();
    for (Tab* tab : app.tabs_list)
        if (tab->supports_printer_technology(tech) && tab->current_preset_is_dirty())
            tab->get_presets()->discard_current_changes();
    app.load_current_presets(false);
}

// Path parameter as a boost path (the JSON string is UTF-8).
static boost::filesystem::path sw_slicer_path_from_utf8(const std::string& s) { return into_path(wxString::FromUTF8(s)); }

// Path as a UTF-8 string (for replies and error messages).
static std::string sw_slicer_path_to_utf8(const boost::filesystem::path& p) { return into_u8(from_path(p)); }

// Output file parameter: an absolute path with one of the suffixes (case-insensitive), in an
// existing folder, with a legal file name.
static boost::filesystem::path sw_slicer_output_path(const json& params, const char* key, std::initializer_list<const char*> suffixes)
{
    const std::string k = key;
    if (!sw_slicer_has_param(params, key) || !params.at(key).is_string() || params.at(key).get<std::string>().empty())
        throw std::runtime_error("'" + k + "' must be a non-empty string");
    const std::string             s = params.at(key).get<std::string>();
    const boost::filesystem::path p = sw_slicer_path_from_utf8(s);
    if (!p.is_absolute())
        throw std::runtime_error("'" + k + "' must be an absolute path");
    bool        suffix_ok = false;
    std::string expected;
    for (const char* suffix : suffixes) {
        suffix_ok = suffix_ok || boost::algorithm::iends_with(s, suffix);
        expected += (expected.empty() ? "" : " or ") + std::string(suffix);
    }
    if (!suffix_ok)
        throw std::runtime_error("'" + k + "' must end with " + expected);
    boost::system::error_code ec;
    if (!boost::filesystem::is_directory(p.parent_path(), ec))
        throw std::runtime_error("folder does not exist: " + sw_slicer_path_to_utf8(p.parent_path()));
    if (Plater::has_illegal_filename_characters(sw_slicer_path_to_utf8(p.filename())))
        throw std::runtime_error("the file name contains characters that are not allowed: <>:/\\|?*\"");
    return p;
}

// Never replace another existing file unasked. The project's own file (own_file) may be
// replaced without a flag, like Ctrl+S.
static void sw_slicer_require_overwrite_allowed(const boost::filesystem::path& p, const json& params,
                                                const boost::filesystem::path& own_file = {})
{
    boost::system::error_code ec;
    if (boost::filesystem::is_directory(p, ec))
        throw std::runtime_error("path is a folder: " + sw_slicer_path_to_utf8(p));
    if (!boost::filesystem::exists(p, ec))
        return;
    const bool is_own = !own_file.empty() && boost::filesystem::equivalent(p, own_file, ec) && !ec;
    if (!is_own && !sw_slicer_flag_true(params, "overwrite"))
        throw std::runtime_error("file already exists: " + sw_slicer_path_to_utf8(p) + "; pass overwrite:true to replace it");
}

// Write through a temporary file next to the target, then replace the target. An existing
// target is left untouched if writing fails; the temporary file is removed on every error path.
static void sw_slicer_write_via_temp(const boost::filesystem::path& target, const std::function<bool(const boost::filesystem::path&)>& write)
{
    // x.3mf -> x.api-tmp-<random>.3mf, x.gcode.3mf -> x.gcode.api-tmp-<random>.3mf (the
    // extension stays). A unique name never touches an existing file of the user.
    boost::system::error_code ec;
    boost::filesystem::path   tmp;
    do {
        boost::filesystem::path tmp_name = target.stem();
        tmp_name += boost::filesystem::path(".api-tmp-");
        tmp_name += boost::filesystem::unique_path("%%%%%%%%");
        tmp_name += target.extension();
        tmp = target.parent_path() / tmp_name;
    } while (boost::filesystem::exists(tmp, ec));

    try {
        if (!write(tmp)) {
            boost::filesystem::remove(tmp, ec);
            throw std::runtime_error("writing the file failed: " + sw_slicer_path_to_utf8(target));
        }
        boost::filesystem::rename(tmp, target); // replaces an existing target
    } catch (...) {
        boost::filesystem::remove(tmp, ec);
        throw;
    }
}

// Plate parameter: params["plate"] if present (0-based), otherwise the current plate.
static int sw_slicer_resolve_plate(const json& params)
{
    PartPlateList& plates = wxGetApp().plater()->get_partplate_list();
    if (!sw_slicer_has_param(params, "plate"))
        return plates.get_curr_plate_index();
    const json& j     = params.at("plate");
    const int   count = plates.get_plate_count();
    if (!j.is_number_integer() || j.get<long long>() < 0 || j.get<long long>() >= count)
        throw std::runtime_error("plate " + j.dump() + " does not exist (there are " + std::to_string(count) +
                                 " plates, 0-based)");
    return j.get<int>();
}

// Deleting (part of) a cut object breaks the cut link: only on explicit request.
static void sw_slicer_require_break_cut(const ModelObject* mo, int obj, const json& params)
{
    if (mo->is_cut() && !sw_slicer_flag_true(params, "break_cut"))
        throw std::runtime_error("object " + std::to_string(obj) +
                                 " is part of a cut object; deleting it breaks the cut link; pass break_cut:true");
}

// Identity of all objects, instances and volumes, to report by name what a delete removed
// (indices shift after deleting). An item counts as still present if its ObjectID or its
// address is still in the model: ModelObject::delete_volume() gives the last remaining volume
// a new ID, so the ID alone would report that volume as deleted too. Deleting allocates no
// new model items, so an address cannot be reused in between.
namespace {
struct SwSlicerItemId
{
    size_t      id;
    const void* ptr;
    std::string name;
};
struct SwSlicerModelIds
{
    struct Object
    {
        SwSlicerItemId              self;
        std::vector<SwSlicerItemId> instances;
        std::vector<SwSlicerItemId> volumes;
    };
    std::vector<Object> objects;
};
struct SwSlicerItemSet
{
    std::set<size_t>      ids;
    std::set<const void*> ptrs;
    void add(size_t id, const void* ptr)
    {
        ids.insert(id);
        ptrs.insert(ptr);
    }
    bool has(const SwSlicerItemId& item) const { return ids.count(item.id) != 0 || ptrs.count(item.ptr) != 0; }
};
} // namespace

static SwSlicerModelIds sw_slicer_model_ids()
{
    SwSlicerModelIds ids;
    for (const ModelObject* mo : wxGetApp().plater()->model().objects) {
        SwSlicerModelIds::Object o;
        o.self = {mo->id().id, mo, mo->name};
        for (const ModelInstance* mi : mo->instances)
            o.instances.push_back({mi->id().id, mi, std::string()});
        for (const ModelVolume* mv : mo->volumes)
            o.volumes.push_back({mv->id().id, mv, mv->name});
        ids.objects.push_back(std::move(o));
    }
    return ids;
}

// {objects:[names], instances:[{object_name, instance}], volumes:[{object_name, volume_name}]}:
// what is gone compared to 'before'. A deleted object is reported only as an object.
static json sw_slicer_deleted_json(const SwSlicerModelIds& before)
{
    SwSlicerItemSet after_objects, after_instances, after_volumes;
    for (const ModelObject* mo : wxGetApp().plater()->model().objects) {
        after_objects.add(mo->id().id, mo);
        for (const ModelInstance* mi : mo->instances)
            after_instances.add(mi->id().id, mi);
        for (const ModelVolume* mv : mo->volumes)
            after_volumes.add(mv->id().id, mv);
    }
    json objects   = json::array();
    json instances = json::array();
    json volumes   = json::array();
    for (const SwSlicerModelIds::Object& o : before.objects) {
        if (!after_objects.has(o.self)) {
            objects.push_back(o.self.name);
            continue;
        }
        for (size_t i = 0; i < o.instances.size(); ++i)
            if (!after_instances.has(o.instances[i])) {
                json e;
                e["object_name"] = o.self.name;
                e["instance"]    = static_cast<int>(i);
                instances.push_back(e);
            }
        for (const SwSlicerItemId& v : o.volumes)
            if (!after_volumes.has(v)) {
                json e;
                e["object_name"] = o.self.name;
                e["volume_name"] = v.name;
                volumes.push_back(e);
            }
    }
    json r;
    r["objects"]   = objects;
    r["instances"] = instances;
    r["volumes"]   = volumes;
    return r;
}

static bool sw_slicer_deleted_any(const json& deleted)
{
    return !deleted.at("objects").empty() || !deleted.at("instances").empty() || !deleted.at("volumes").empty();
}

static std::string sw_slicer_join_notes(std::string text, const std::vector<std::string>& notes)
{
    for (const std::string& n : notes)
        text += "; " + n;
    return text;
}

// --- Project lifecycle: state, new, open, save -------------------------------------

// Read-only overview of the project: file, unsaved changes, objects, plates, background work.
void SSWCP_SliceProject_Instance::sw_SlicerGetProjectState()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        PartPlateList& plates = plater->get_partplate_list();
        m_res_data["path"]          = into_u8(plater->get_project_filename(".3mf"));
        m_res_data["project_name"]  = into_u8(plater->get_project_name());
        m_res_data["dirty"]         = sw_slicer_project_dirty();
        m_res_data["presets_dirty"] = sw_slicer_presets_dirty();
        m_res_data["objects"]       = static_cast<int>(plater->model().objects.size());
        m_res_data["plates"]        = plates.get_plate_count();
        m_res_data["current_plate"] = plates.get_curr_plate_index();
        m_res_data["slicing"]       = plater->is_background_process_slicing();
        m_res_data["exporting"]     = plater->is_export_gcode_scheduled();
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Start a new, empty project. Unsaved project changes require discard_unsaved:true and unsaved
// preset edits require preset_changes:'keep'|'discard'. Clears the undo stack: no undo step.
void SSWCP_SliceProject_Instance::sw_SlicerNewProject()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        sw_slicer_require_not_slicing();
        sw_slicer_require_no_export();
        sw_slicer_require_discard(m_param_data);
        const std::string presets   = sw_slicer_preset_choice(m_param_data, /*allow_keep=*/true);
        const bool        was_dirty = sw_slicer_project_dirty();

        std::vector<std::string> notes;
        {
            sswcp_api::Scope api_scope;
            if (presets == "discard")
                sw_slicer_discard_preset_changes();
            // skip_confirm: the checks above replace close_with_confirm(). Without the confirmation
            // the preset edits are kept (reset() reloads the edited presets).
            plater->new_project(/*skip_confirm=*/true, /*silent=*/false);
            notes = sswcp_api::notes();
        }
        if (was_dirty)
            notes.push_back("unsaved changes discarded");
        if (presets == "keep")
            notes.push_back("preset changes kept for the new project");
        else if (presets == "discard")
            notes.push_back("preset changes discarded");
        notes.push_back("this action cannot be undone");

        m_res_data["project_name"] = into_u8(plater->get_project_name());
        m_res_data["objects"]      = sw_slicer_objects_json();
        m_res_data["plates"]       = plater->get_partplate_list().get_plate_count();
        m_res_data["notes"]        = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Open a .3mf project (model and settings, without the load-type question). Same flags as
// sw_SlicerNewProject, but preset edits can only be discarded (the project brings its own
// presets). Clears the undo stack: no undo step.
void SSWCP_SliceProject_Instance::sw_SlicerOpenProject()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        const std::string path = (sw_slicer_has_param(m_param_data, "path") && m_param_data.at("path").is_string()) ?
                                     m_param_data.at("path").get<std::string>() :
                                     std::string();
        if (path.empty())
            throw std::runtime_error("specify 'path'");
        const boost::filesystem::path file = sw_slicer_path_from_utf8(path);
        boost::system::error_code     ec;
        if (!boost::filesystem::is_regular_file(file, ec))
            throw std::runtime_error("file does not exist: " + path);
        if (!boost::algorithm::iends_with(path, ".3mf"))
            throw std::runtime_error("only .3mf project files can be opened (use sw_SlicerAddModel for models)");
        if (boost::algorithm::iends_with(path, ".gcode.3mf"))
            throw std::runtime_error("sliced files (.gcode.3mf) open in preview-only mode and are not supported");
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        sw_slicer_require_not_slicing();
        sw_slicer_require_no_export();
        sw_slicer_require_discard(m_param_data);
        const std::string presets   = sw_slicer_preset_choice(m_param_data, /*allow_keep=*/false);
        const bool        was_dirty = sw_slicer_project_dirty();

        std::vector<std::string> choices;
        {
            sswcp_api::Scope api_scope;
            if (presets == "discard")
                sw_slicer_discard_preset_changes();
            // "<loadall>": load model and settings without asking (skips determine_load_type()).
            // close_with_confirm() runs, but is silent in API mode.
            plater->load_project(wxString::FromUTF8(path), "<loadall>");
            choices = sswcp_api::notes();
        }

        // load_project() returns nothing: it succeeded if the project now refers to the file.
        const boost::filesystem::path now = into_path(plater->get_project_filename(".3mf"));
        if (now.empty() || !boost::filesystem::equivalent(now, file, ec) || ec)
            throw std::runtime_error(sw_slicer_join_notes("the project could not be opened", choices));

        std::vector<std::string> notes;
        // For an empty model close_with_confirm() sees nothing to discard and adds no note.
        if (was_dirty && std::find(choices.begin(), choices.end(), "unsaved changes discarded (save dialog skipped)") == choices.end())
            notes.push_back("unsaved changes discarded");
        if (presets == "discard")
            notes.push_back("preset changes discarded");
        if (plater->using_exported_file())
            notes.push_back("the file contains sliced data; it was opened in preview-only mode");
        notes.push_back("this action cannot be undone");

        m_res_data["path"]         = sw_slicer_path_to_utf8(now);
        m_res_data["project_name"] = into_u8(plater->get_project_name());
        m_res_data["objects"]      = sw_slicer_objects_json();
        m_res_data["plates"]       = plater->get_partplate_list().get_plate_count();
        m_res_data["auto_choices"] = choices;
        m_res_data["notes"]        = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Same condition as publish() in Plater.cpp: an SVG part whose original file is not yet stored
// in the 3MF. export_3mf() then asks whether to store the SVGs with their local paths.
static bool sw_slicer_has_new_svg_files(const Model& model)
{
    for (const ModelObject* object : model.objects)
        for (const ModelVolume* volume : object->volumes)
            if (volume->emboss_shape.has_value() && !volume->text_configuration.has_value() &&
                volume->emboss_shape->svg_file.has_value() && volume->emboss_shape->svg_file->path_in_3mf.empty())
                return true;
    return false;
}

// Save the project as .3mf: without 'path' to the project's own file, like Ctrl+S. Another
// existing file is only replaced with overwrite:true. Writes through a temporary file.
// Saving changes no model data: no undo step.
void SSWCP_SliceProject_Instance::sw_SlicerSaveProject()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        // Stricter than MainFrame::can_save_as(): no open (paint) tool at all.
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        sw_slicer_require_not_slicing();
        if (plater->using_exported_file() || plater->only_gcode_mode())
            throw std::runtime_error("a G-code or sliced file is open; it cannot be saved as a project");
        if (plater->model().objects.empty())
            throw std::runtime_error("the project is empty; nothing to save");

        const boost::filesystem::path own = into_path(plater->get_project_filename(".3mf"));
        boost::filesystem::path       target;
        if (sw_slicer_has_param(m_param_data, "path")) {
            target = sw_slicer_output_path(m_param_data, "path", {".3mf"});
            if (boost::algorithm::iends_with(m_param_data.at("path").get<std::string>(), ".gcode.3mf"))
                throw std::runtime_error("use a .3mf name without .gcode");
        } else {
            if (own.empty())
                throw std::runtime_error("the project has no file name yet; pass 'path'");
            target = own;
        }
        sw_slicer_require_overwrite_allowed(target, m_param_data, own);
        boost::system::error_code ec;
        const bool replaces_other = boost::filesystem::exists(target, ec) &&
                                    !(!own.empty() && boost::filesystem::equivalent(target, own, ec) && !ec);

        const bool new_svg_files = sw_slicer_has_new_svg_files(plater->model());

        std::vector<std::string> notes;
        {
            sswcp_api::Scope api_scope;
            sw_slicer_write_via_temp(target, [plater](const boost::filesystem::path& tmp) {
                // Same strategy as Plater::save_project(), plus Silence: export_3mf() then does not
                // record the temporary file as project file (recent projects, backup origin), and
                // publish() skips the SVG "Private protection" question with the answer that keeps
                // the SVGs editable (they are stored with their local paths).
                return plater->export_3mf(tmp, SaveStrategy::SplitModel | SaveStrategy::ShareMesh |
                                                   SaveStrategy::FullPathSources | SaveStrategy::Silence) >= 0;
            });
            // Follow-up as in Plater::save_project(), with the real target path.
            Slic3r::remove_backup(plater->model(), false);
            plater->set_project_filename(from_path(target));
            // Record the origin for backup recovery, as a non-silent export does.
            try {
                Slic3r::save_string_file(plater->model().get_backup_path() + "/origin.txt", sw_slicer_path_to_utf8(target));
            } catch (...) {}
            plater->up_to_date(true, false);
            plater->up_to_date(true, true);
            wxGetApp().update_saved_preset_from_current_preset();
            plater->reset_project_dirty_after_save();
            plater->update_title_dirty_status();
            notes = sswcp_api::notes();
        }
        if (new_svg_files)
            notes.push_back("original SVG files stored in the project with their local paths (dialog skipped)");
        if (replaces_other)
            notes.push_back("existing file replaced");

        m_res_data["path"]         = sw_slicer_path_to_utf8(target);
        m_res_data["project_name"] = into_u8(plater->get_project_name());
        m_res_data["bytes"]        = static_cast<std::uint64_t>(boost::filesystem::file_size(target));
        m_res_data["notes"]        = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- Project lifecycle: delete --------------------------------------------------

// Delete exactly one of: {object, instance?|volume?}, {objects:[...]}, {scope:'selection'},
// {scope:'plate', plate?} or {scope:'all'}. Cut objects only with break_cut:true.
// One undo step "API: Delete..."; a no-op takes no snapshot.
void SSWCP_SliceProject_Instance::sw_SlicerDelete()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        const bool has_object  = sw_slicer_has_param(m_param_data, "object");
        const bool has_objects = sw_slicer_has_param(m_param_data, "objects");
        const bool has_scope   = sw_slicer_has_param(m_param_data, "scope");
        if (int(has_object) + int(has_objects) + int(has_scope) != 1)
            throw std::runtime_error("specify exactly one of: object, objects, scope");
        const std::string scope = has_scope ? (m_param_data.at("scope").is_string() ? m_param_data.at("scope").get<std::string>() : "?") :
                                              std::string();
        if (has_scope && scope != "selection" && scope != "plate" && scope != "all")
            throw std::runtime_error("scope must be 'selection', 'plate' or 'all'");
        const bool has_inst = sw_slicer_has_param(m_param_data, "instance");
        const bool has_vol  = sw_slicer_has_param(m_param_data, "volume");
        if (!has_object && (has_inst || has_vol))
            throw std::runtime_error("'instance' and 'volume' can only be used with 'object'");
        if (sw_slicer_has_param(m_param_data, "plate") && scope != "plate")
            throw std::runtime_error("'plate' can only be used with scope:'plate'");

        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        sw_slicer_require_not_slicing();
        Model&    model = plater->model();
        const int n     = static_cast<int>(model.objects.size());
        if (n == 0 && scope != "all")
            throw std::runtime_error("no model on the plate (load a model first)");

        std::vector<std::string> notes;
        const SwSlicerModelIds   before = sw_slicer_model_ids();
        auto reply = [&]() {
            m_res_data["deleted"] = sw_slicer_deleted_json(before);
            m_res_data["objects"] = sw_slicer_objects_json();
            m_res_data["notes"]   = notes;
            send_to_js();
            finish_job();
        };

        if (scope == "all") {
            if (n == 0) {
                notes.push_back("there are no objects; nothing changed");
                reply();
                return;
            }
            {
                sswcp_api::Scope     api_scope;
                Plater::TakeSnapshot snapshot(plater, "API: Delete all");
                plater->delete_all_objects_from_model();
                notes = sswcp_api::notes();
            }
            reply();
            return;
        }

        if (scope == "plate") {
            PartPlateList& plates = plater->get_partplate_list();
            const int      idx    = sw_slicer_resolve_plate(m_param_data);
            PartPlate*     plate  = plates.get_plate(idx);
            // Not PartPlate::get_objects(): that returns all model objects.
            if (plate->empty()) {
                notes.push_back("plate " + std::to_string(idx) + " has no objects; nothing changed");
                reply();
                return;
            }
            if (plate->is_locked())
                throw std::runtime_error("plate " + std::to_string(idx) + " is locked");
            // Selection::remove_curr_plate() takes the objects whose first instance is on the plate
            // (with all their instances, also those on other plates).
            int first_instances = 0;
            for (int i = 0; i < n; ++i)
                if (plate->contain_instance(i, 0)) {
                    sw_slicer_require_break_cut(model.objects[i], i, m_param_data);
                    ++first_instances;
                }
            if (first_instances == 0) {
                notes.push_back("plate " + std::to_string(idx) + " has no objects whose first instance is on it; nothing changed");
                reply();
                return;
            }
            {
                sswcp_api::Scope     api_scope;
                Plater::TakeSnapshot snapshot(plater, "API: Delete plate objects");
                if (plates.get_curr_plate_index() != idx &&
                    (plater->select_plate(idx) != 0 || plates.get_curr_plate_index() != idx))
                    throw std::runtime_error("plate " + std::to_string(idx) + " could not be selected");
                plater->remove_curr_plate_all();
                notes = sswcp_api::notes();
            }
            const json deleted = sw_slicer_deleted_json(before);
            if (!sw_slicer_deleted_any(deleted))
                throw std::runtime_error(sw_slicer_join_notes("nothing was deleted", notes));
            reply();
            return;
        }

        // object / objects / selection: through the canvas selection and Plater::remove_selected(),
        // which works on the selection of the current canvas.
        if (!plater->is_view3D_shown())
            throw std::runtime_error("deleting is only possible in the 3D view (open the 3D view first)");
        GLCanvas3D* canvas = sw_slicer_canvas();

        if (has_object) {
            const int obj = sw_slicer_resolve_object(m_param_data);
            if (has_inst && has_vol)
                throw std::runtime_error("specify instance or volume, not both");
            const int inst = has_inst ? sw_slicer_get_index(m_param_data, "instance") : -1;
            const int vol  = has_vol ? sw_slicer_get_index(m_param_data, "volume") : -1;
            sw_slicer_check_indices(obj, inst, vol);
            const ModelObject* mo       = model.objects[obj];
            int                sel_inst = inst;
            if (vol >= 0) {
                const ModelVolume* mv = mo->volumes[vol];
                if (mo->volumes.size() == 1)
                    throw std::runtime_error("this is the only part of the object; delete the object instead");
                // Checked up front: ObjectList::del_subobject_from_object() would show a dialog.
                if (mv->is_model_part() && sw_slicer_count_model_parts(mo) == 1)
                    throw std::runtime_error("deleting the last solid part is not allowed");
                if (mo->is_cut() && (mv->is_model_part() || mv->is_negative_volume()))
                    throw std::runtime_error("parts of a cut object cannot be deleted through the API");
            } else {
                if (inst >= 0 && mo->instances.size() == 1) {
                    notes.push_back("last instance: the whole object was deleted");
                    sel_inst = -1;
                }
                if (sel_inst < 0)
                    sw_slicer_require_break_cut(mo, obj, m_param_data);
            }
            sw_slicer_select(obj, sel_inst, vol);
        } else if (has_objects) {
            const json& arr = m_param_data.at("objects");
            if (!arr.is_array() || arr.empty())
                throw std::runtime_error("'objects' must be a non-empty list of object indices");
            std::vector<int> objs;
            for (const json& j : arr) {
                if (!j.is_number_integer() || j.get<long long>() < 0 || j.get<long long>() >= n)
                    throw std::runtime_error("object index " + j.dump() + " in 'objects' does not exist (there are " +
                                             std::to_string(n) + " objects, 0-based)");
                const int obj = j.get<int>();
                if (std::find(objs.begin(), objs.end(), obj) != objs.end())
                    throw std::runtime_error("object index " + std::to_string(obj) + " appears twice in 'objects'");
                objs.push_back(obj);
            }
            for (int obj : objs)
                sw_slicer_require_break_cut(model.objects[obj], obj, m_param_data);
            Selection& s = canvas->get_selection();
            s.clear();
            for (size_t i = 0; i < objs.size(); ++i)
                s.add_object(static_cast<unsigned int>(objs[i]), i == 0);
            wxGetApp().obj_list()->update_selections();
            canvas->update_gizmos_on_off_state();
            canvas->set_as_dirty();
        } else {
            // scope "selection": what the user selected.
            const Selection& s = canvas->get_selection();
            if (s.is_empty())
                throw std::runtime_error("nothing is selected");
            std::map<int, std::set<int>> volumes_by_object;
            for (unsigned int idx : s.get_volume_idxs()) {
                const GLVolume* v = s.get_volume(idx);
                if (v == nullptr)
                    continue;
                // The wipe tower has object_idx >= 1000.
                if (v->object_idx() >= 1000)
                    throw std::runtime_error("the wipe tower cannot be deleted");
                if (v->object_idx() >= 0 && v->object_idx() < n && v->volume_idx() >= 0)
                    volumes_by_object[v->object_idx()].insert(v->volume_idx());
            }
            if (s.is_wipe_tower())
                throw std::runtime_error("the wipe tower cannot be deleted");
            if (s.is_instance_mode()) {
                // Whole instances: the cut rule applies to every fully selected object.
                for (const auto& content : s.get_content()) {
                    const int obj = content.first;
                    if (obj >= 0 && obj < n && content.second.size() == model.objects[obj]->instances.size())
                        sw_slicer_require_break_cut(model.objects[obj], obj, m_param_data);
                }
            } else {
                // Single parts: the same up-front checks as for {object, volume}.
                for (const auto& ov : volumes_by_object) {
                    const ModelObject* mo     = model.objects[ov.first];
                    int                solids = 0;
                    for (int vol : ov.second) {
                        const ModelVolume* mv = mo->volumes[vol];
                        if (mo->is_cut() && (mv->is_model_part() || mv->is_negative_volume()))
                            throw std::runtime_error("parts of a cut object cannot be deleted through the API");
                        if (mv->is_model_part())
                            ++solids;
                    }
                    if (solids > 0 && solids >= sw_slicer_count_model_parts(mo))
                        throw std::runtime_error("the selection contains all solid parts of object " + std::to_string(ov.first) +
                                                 "; delete the object instead");
                }
            }
        }

        // Refuse without a snapshot if the app would not delete anything.
        if (!plater->can_delete())
            throw std::runtime_error("nothing to delete in the selection");
        {
            sswcp_api::Scope     api_scope;
            Plater::TakeSnapshot snapshot(plater, "API: Delete");
            plater->remove_selected();
            const std::vector<std::string>& api_notes = sswcp_api::notes();
            notes.insert(notes.end(), api_notes.begin(), api_notes.end());
        }
        const json deleted = sw_slicer_deleted_json(before);
        if (!sw_slicer_deleted_any(deleted))
            throw std::runtime_error(sw_slicer_join_notes("nothing was deleted", notes));
        reply();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Delete a plate (default: the current one). A plate with objects requires objects:'delete'
// (delete them too) or 'keep' (the app moves them to the last plate or off the plates).
// One undo step "API: Delete plate".
void SSWCP_SliceProject_Instance::sw_SlicerDeletePlate()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        sw_slicer_require_not_slicing();
        PartPlateList& plates = plater->get_partplate_list();
        const int      idx    = sw_slicer_resolve_plate(m_param_data);
        if (!plater->can_delete_plate())
            throw std::runtime_error("the last plate cannot be deleted");
        PartPlate* plate = plates.get_plate(idx);
        if (plate->is_locked())
            throw std::runtime_error("plate " + std::to_string(idx) + " is locked");
        std::string objects_choice;
        if (sw_slicer_has_param(m_param_data, "objects")) {
            const json& j  = m_param_data.at("objects");
            objects_choice = j.is_string() ? j.get<std::string>() : std::string("?");
            if (objects_choice != "delete" && objects_choice != "keep")
                throw std::runtime_error("objects must be 'delete' or 'keep'");
        }

        Model&     model     = plater->model();
        const int  n         = static_cast<int>(model.objects.size());
        const bool has_items = !plate->empty();
        if (has_items) {
            int count = 0;
            for (int i = 0; i < n; ++i) {
                const int n_inst = static_cast<int>(model.objects[i]->instances.size());
                for (int j = 0; j < n_inst; ++j)
                    if (plate->contain_instance(i, j)) {
                        ++count;
                        break;
                    }
            }
            if (objects_choice.empty())
                throw std::runtime_error("plate " + std::to_string(idx) + " contains " + std::to_string(count) +
                                         " object(s); pass objects:'delete' (delete them too) or 'keep' (they are moved off the plate)");
            if (objects_choice == "delete")
                for (int i = 0; i < n; ++i)
                    if (plate->contain_instance(i, 0))
                        sw_slicer_require_break_cut(model.objects[i], i, m_param_data);
        }

        std::vector<std::string> notes;
        int                      ret        = -1;
        bool                     moved_rest = false;
        {
            sswcp_api::Scope     api_scope;
            Plater::TakeSnapshot snapshot(plater, "API: Delete plate");
            if (has_items && objects_choice == "delete") {
                if (plates.get_curr_plate_index() != idx &&
                    (plater->select_plate(idx) != 0 || plates.get_curr_plate_index() != idx))
                    throw std::runtime_error("plate " + std::to_string(idx) + " could not be selected");
                plater->remove_curr_plate_all();
                // Instances of objects whose first instance is on another plate stay; delete_plate()
                // moves them like with 'keep'.
                if (!plates.get_plate(idx)->empty())
                    moved_rest = true;
            }
            // Defensive: no stale (possibly undo-restored) selection, and if the plate being deleted
            // is the current one, switch to another plate first through the full
            // Plater::select_plate() path. For idx == 0 PartPlateList::delete_plate() still lowers
            // the current index internally, but that index then refers to the same plate.
            plater->deselect_all();
            wxGetApp().obj_list()->update_selections();
            if (plates.get_curr_plate_index() == idx) {
                const int other = (idx > 0) ? idx - 1 : 1;
                if (plater->select_plate(other) != 0 || plates.get_curr_plate_index() != other)
                    throw std::runtime_error("could not switch away from plate " + std::to_string(idx) +
                                             (has_items && objects_choice == "delete" ?
                                                  "; its objects were already deleted (use Undo)" :
                                                  ""));
            }
            ret   = plater->delete_plate(idx);
            notes = sswcp_api::notes();
        }
        if (ret != 0)
            throw std::runtime_error(sw_slicer_join_notes("the plate could not be deleted", notes));
        if (has_items && objects_choice == "keep")
            notes.push_back("objects of the deleted plate were moved to the last plate or off the plates");
        if (moved_rest)
            notes.push_back("instances of objects whose first instance is on another plate were moved to the last plate or off the plates, not deleted");

        m_res_data["deleted_plate"] = idx;
        m_res_data["plate_count"]   = plates.get_plate_count();
        m_res_data["current_plate"] = plates.get_curr_plate_index();
        m_res_data["objects"]       = sw_slicer_objects_json();
        m_res_data["notes"]         = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- Slicing all plates, exporting and uploading ----------------------------------
// Exporting and uploading change no model data: no undo step.

// Slice all plates, like the "Slice all" button. Returns immediately; poll sw_SlicerGetStatus
// (slicing_all, slicing, and with all:true the state of every plate).
void SSWCP_SliceProject_Instance::sw_SlicerSliceAll()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        sw_slicer_require_not_slicing();
        sw_slicer_require_no_export();
        if (plater->model().objects.empty())
            throw std::runtime_error("no model on the plate (load a model first)");

        PartPlateList& plates    = plater->get_partplate_list();
        json           sliceable = json::array();
        for (int i = 0; i < plates.get_plate_count(); ++i)
            if (plater->is_plate_sliceable(i))
                sliceable.push_back(i);
        if (sliceable.empty())
            throw std::runtime_error("no plate can be sliced (empty, or blocked by a filament check)");

        std::vector<std::string> notes;
        bool                     ok = false;
        {
            sswcp_api::Scope api_scope;
            // Not silent: the preview is shown, as with the button.
            ok    = plater->slice_all_plates(/*silent=*/false);
            notes = sswcp_api::notes();
        }
        if (!ok)
            throw std::runtime_error(sw_slicer_join_notes("slicing all plates could not be started", notes));
        if (!plater->is_slicing_all())
            notes.push_back("no slicing started; the slice results may already be up to date (check sw_SlicerGetStatus with all:true)");

        m_res_data["started"] = true;
        m_res_data["plates"]  = sliceable;
        m_res_data["notes"]   = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Guards shared by the export and upload commands.
static void sw_slicer_require_export_possible()
{
    sw_slicer_require_free_gizmo();
    sw_slicer_require_idle();
    sw_slicer_require_not_slicing();
    if (wxGetApp().plater()->is_export_gcode_scheduled())
        throw std::runtime_error("another export is still running");
    if (wxGetApp().plater()->model().objects.empty())
        throw std::runtime_error("no model on the plate (load a model first)");
}

// The plate must have a slice result that can be printed (same check as MainFrame::can_export_gcode()).
static void sw_slicer_require_plate_ready(int n)
{
    if (!wxGetApp().plater()->get_partplate_list().get_plate(n)->is_slice_result_ready_for_print())
        throw std::runtime_error("plate " + std::to_string(n) + " has no valid slice result; slice it first (sw_SlicerSlice / sw_SlicerSliceAll)");
}

// Make plate n the current plate (the app exports the current plate). The selection stays visible.
static void sw_slicer_select_plate(int n)
{
    Plater*        plater = wxGetApp().plater();
    PartPlateList& plates = plater->get_partplate_list();
    if (plates.get_curr_plate_index() != n && (plater->select_plate(n) != 0 || plates.get_curr_plate_index() != n))
        throw std::runtime_error("plate " + std::to_string(n) + " could not be selected");
}

// Export the G-code of a sliced plate to 'path' (.gcode) without a dialog. The export runs in the
// background; poll sw_SlicerGetStatus until 'exporting' is false. An existing file is only replaced
// with overwrite:true.
void SSWCP_SliceProject_Instance::sw_SlicerExportGcode()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        sw_slicer_require_export_possible();
        const boost::filesystem::path target = sw_slicer_output_path(m_param_data, "path", {".gcode"});
        // The app passes the file name through the placeholder parser (PrintStatistics::finalize_output_path),
        // so the file actually written could differ from 'path'.
        const std::string stem = sw_slicer_path_to_utf8(target.stem());
        if (stem.find_first_of("{}[]") != std::string::npos)
            throw std::runtime_error("the file name must not contain { } [ or ] (the app treats them as placeholders)");
        sw_slicer_require_overwrite_allowed(target, m_param_data);
        boost::system::error_code ec;
        const bool                replaces = boost::filesystem::exists(target, ec);
        const int                 n        = sw_slicer_resolve_plate(m_param_data);
        sw_slicer_require_plate_ready(n);

        std::vector<std::string> notes;
        bool                     started = false;
        {
            sswcp_api::Scope api_scope;
            sw_slicer_select_plate(n);
            // The app writes the file itself, in the background (BackgroundSlicingProcess), to a
            // temporary file next to the target that replaces the target only after a successful export.
            started = plater->export_gcode_to(target);
            notes   = sswcp_api::notes();
        }
        if (!started)
            throw std::runtime_error(sw_slicer_join_notes("the export could not be started", notes));
        if (replaces)
            notes.push_back("existing file is replaced when the export has finished");

        m_res_data["started"] = true;
        m_res_data["path"]    = sw_slicer_path_to_utf8(target);
        m_res_data["plate"]   = n;
        m_res_data["notes"]   = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Export a sliced file (.gcode.3mf: G-code plus project data) of one plate (scope 'plate', default
// the current plate) or of all plates (scope 'all'). Synchronous; written through a temporary file.
// The project file name and the unsaved-changes state do not change.
void SSWCP_SliceProject_Instance::sw_SlicerExportSliced()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        std::string scope = "plate";
        if (sw_slicer_has_param(m_param_data, "scope")) {
            const json& j = m_param_data.at("scope");
            scope         = j.is_string() ? j.get<std::string>() : std::string("?");
        }
        if (scope != "plate" && scope != "all")
            throw std::runtime_error("scope must be 'plate' or 'all'");
        if (scope == "all" && sw_slicer_has_param(m_param_data, "plate"))
            throw std::runtime_error("'plate' can only be used with scope:'plate'");
        sw_slicer_require_export_possible();
        const boost::filesystem::path target = sw_slicer_output_path(m_param_data, "path", {".3mf"});
        sw_slicer_require_overwrite_allowed(target, m_param_data);
        boost::system::error_code ec;
        const bool                replaces = boost::filesystem::exists(target, ec);

        PartPlateList& plates   = plater->get_partplate_list();
        int            n        = -1;
        json           exported = json::array();
        if (scope == "plate") {
            n = sw_slicer_resolve_plate(m_param_data);
            sw_slicer_require_plate_ready(n);
            exported.push_back(n);
        } else {
            if (!plates.is_all_slice_results_ready_for_print())
                throw std::runtime_error("not all plates have a valid slice result; slice them first (sw_SlicerSliceAll)");
            for (int i = 0; i < plates.get_plate_count(); ++i)
                if (plates.get_plate(i)->is_slice_result_ready_for_print())
                    exported.push_back(i);
        }
        const bool new_svg_files = sw_slicer_has_new_svg_files(plater->model());

        std::vector<std::string> notes;
        {
            sswcp_api::Scope api_scope;
            // Like the GUI, export the current plate: export_3mf() stores the first-layer data of the
            // current plate.
            if (scope == "plate")
                sw_slicer_select_plate(n);
            const int idx = (scope == "all") ? PLATE_ALL_IDX : n;
            sw_slicer_write_via_temp(target, [plater, idx](const boost::filesystem::path& tmp) {
                // Same strategy as Plater::export_gcode_3mf(). With Silence, export_3mf() does not
                // record the file as project file and skips the SVG question.
                return plater->export_3mf(tmp, SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::WithGcode |
                                                   SaveStrategy::SkipModel, idx) >= 0;
            });
            notes = sswcp_api::notes();
        }
        if (!boost::algorithm::iends_with(m_param_data.at("path").get<std::string>(), ".gcode.3mf"))
            notes.push_back("a sliced file is usually named *.gcode.3mf");
        if (new_svg_files)
            notes.push_back("original SVG files stored with their local paths (dialog skipped)");
        if (replaces)
            notes.push_back("existing file replaced");

        m_res_data["path"]   = sw_slicer_path_to_utf8(target);
        m_res_data["bytes"]  = static_cast<std::uint64_t>(boost::filesystem::file_size(target));
        m_res_data["plates"] = exported;
        m_res_data["notes"]  = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// An upload without any progress for this long is cancelled (the transfer callback keeps running
// while curl waits, so a hanging connection is noticed).
static constexpr std::chrono::seconds SW_SLICER_UPLOAD_STALL_TIMEOUT{180};
// An upload whose thread shows no sign of life for this long no longer blocks a new upload.
static constexpr std::chrono::seconds SW_SLICER_UPLOAD_ABANDON_TIMEOUT{300};

// State of the last upload to the printer, shared with the upload thread. The thread holds its own
// reference, so the state outlives the static owner at shutdown. The thread only writes to the
// state while 'id' is still its own upload id.
namespace {
struct SwSlicerUploadState
{
    std::mutex  mutex;
    int         id = 0;
    std::string state = "idle"; // idle | uploading | done | failed
    int         progress = 0;   // 0..100
    std::string filename;
    std::string error;
    // The result notification is shown by sw_SlicerGetStatus on the GUI thread, never by the thread.
    bool        notified = true;
    // Stall detection.
    size_t                                last_ulnow   = 0;
    size_t                                last_ultotal = 0;
    std::chrono::steady_clock::time_point last_activity;
    bool                                  stalled = false;
};
} // namespace

static const std::shared_ptr<SwSlicerUploadState>& sw_slicer_upload_state()
{
    static const std::shared_ptr<SwSlicerUploadState> state = std::make_shared<SwSlicerUploadState>();
    return state;
}

static json sw_slicer_upload_status_json()
{
    const std::shared_ptr<SwSlicerUploadState>& s = sw_slicer_upload_state();
    std::lock_guard<std::mutex>                 lock(s->mutex);
    if (s->state == "idle")
        return json();
    json j;
    j["id"]       = s->id;
    j["state"]    = s->state;
    j["progress"] = s->progress;
    j["filename"] = s->filename;
    j["error"]    = s->error;
    return j;
}

// Called from sw_SlicerGetStatus (GUI thread, the Plater is alive): show the result notification of
// a finished upload once.
static void sw_slicer_upload_notify()
{
    const std::shared_ptr<SwSlicerUploadState>& s = sw_slicer_upload_state();
    bool                                        ok = false;
    std::string                                 filename, error;
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        if (s->notified || (s->state != "done" && s->state != "failed"))
            return;
        s->notified = true;
        ok          = s->state == "done";
        filename    = s->filename;
        error       = s->error;
    }
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return;
    if (ok)
        plater->send_gcode_finish(wxString::FromUTF8(filename));
    else
        plater->get_notification_manager()->push_notification(NotificationType::CustomNotification,
                                                              NotificationManager::NotificationLevel::ErrorNotificationLevel,
                                                              GUI::format(_L("Uploading %1% to the printer failed: %2%"), filename, error));
}

// Upload file name: a plain name ending in .gcode (no folder, no "..").
static bool sw_slicer_valid_upload_name(const std::string& name)
{
    return boost::algorithm::iends_with(name, ".gcode") && name.size() > std::string(".gcode").size() &&
           name.find_first_of("/\\") == std::string::npos && name.find("..") == std::string::npos &&
           !Plater::has_illegal_filename_characters(name);
}

// Base name of a printer preset as the app compares it with the connected printer
// (same rule as Plater::send_gcode_legacy(): system preset or its base, without parentheses).
static std::string sw_slicer_printer_model_name()
{
    PresetCollection& printers = wxGetApp().preset_bundle->printers;
    const Preset&     current  = printers.get_edited_preset();
    std::string       name;
    if (current.is_system) {
        name = current.name;
    } else if (const Preset* base = printers.get_preset_base(current)) {
        name = base->name;
    }
    name.erase(std::remove(name.begin(), name.end(), '('), name.end());
    name.erase(std::remove(name.begin(), name.end(), ')'), name.end());
    return name;
}

// Cloud (WAN) connection: hand the upload to the app's own send dialog, as Plater::send_gcode_legacy()
// does for the U1 (WebPreprintDialog), but always on the "send only" page (preSend, path=5) and without
// the PrintHostSendDialog (the API supplies the file name). The Flutter page uploads through the cloud
// and the user confirms there; this code never starts a print. Like the GUI, the dialog works on the
// current plate and sends that plate's G-code file (PartPlate::get_tmp_gcode_path()), so plate n is
// selected first. The dialog is modal: it is opened with CallAfter, after the API reply has been sent.
static void sw_slicer_send_via_app_dialog(int n, const std::string& filename, std::vector<std::string>& notes)
{
    MainFrame* frame = wxGetApp().mainframe;
    if (frame == nullptr)
        throw std::runtime_error("the main window is not available");
    if (wxGetApp().get_web_preprint_dialog() != nullptr)
        throw std::runtime_error("the app's send dialog is already open");
    sw_slicer_select_plate(n);
    const std::string         gcode_path = wxGetApp().plater()->get_partplate_list().get_plate(n)->get_tmp_gcode_path();
    boost::system::error_code ec;
    if (!boost::filesystem::is_regular_file(boost::filesystem::path(gcode_path), ec))
        throw std::runtime_error("the G-code of plate " + std::to_string(n) + " was not found; slice it again");

    // The Flutter page only works while the app is the active application: bring it to the front.
    if (frame->IsIconized())
        frame->Iconize(false);
    frame->Show(true);
    frame->Raise();
    bool in_front = true;
#ifdef _WIN32
    // Windows may refuse this while another application owns the foreground.
    const HWND hwnd = static_cast<HWND>(frame->GetHandle());
    ::SetForegroundWindow(hwnd);
    in_front = ::GetForegroundWindow() == hwnd;
#endif
    if (!in_front) {
        frame->RequestUserAttention();
        notes.push_back("the app could not bring itself to the front; click the app window so that the send dialog works");
    }
    notes.push_back("the printer is connected through the cloud; the app's send dialog was opened - confirm the upload there");

    const bool switch_to_device = wxGetApp().app_config->get_bool("open_device_tab_post_upload");
    // Queued on the main frame: if the frame is destroyed first, the call is dropped with it.
    wxWeakRef<MainFrame> weak_frame(frame);
    frame->CallAfter([weak_frame, gcode_path, filename, switch_to_device]() {
        if (!weak_frame || wxGetApp().get_web_preprint_dialog() != nullptr)
            return;
        WebPreprintDialog* dialog = new WebPreprintDialog();
        dialog->set_swtich_to_device(switch_to_device);
        // "Send only" page: upload, never print (Plater::send_gcode_legacy() with post action None).
        dialog->set_send_page(true);
        dialog->set_gcode_file_name(gcode_path);
        dialog->set_display_file_name(filename);
        dialog->run();
        if (dialog->is_finish() && weak_frame)
            weak_frame->select_tab(MainFrame::TabPosition::tpMonitor);
        delete dialog;
    });
}

// Upload the G-code of a sliced plate to the connected printer. Upload only: this command never
// starts a print. Requires confirm_upload:true; any other parameter than plate, filename and
// confirm_upload is refused. The upload runs on its own thread; poll sw_SlicerGetStatus -> upload.
void SSWCP_SliceProject_Instance::sw_SlicerSendToPrinter()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        // 1. Confirmation and parameters first, before any other check.
        if (!sw_slicer_flag_true(m_param_data, "confirm_upload"))
            throw std::runtime_error("uploading to the printer requires confirm_upload:true");
        for (auto it = m_param_data.begin(); it != m_param_data.end(); ++it)
            if (it.key() != "plate" && it.key() != "filename" && it.key() != "confirm_upload")
                throw std::runtime_error("unknown parameter '" + it.key() + "'; this command only uploads, it never starts a print");

        // 2. Guards.
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        sw_slicer_require_not_slicing();
        if (plater->model().objects.empty())
            throw std::runtime_error("no model on the plate (load a model first)");
        const std::shared_ptr<SwSlicerUploadState>& state     = sw_slicer_upload_state();
        bool                                        abandoned = false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->state == "uploading") {
                // Normally a stalled upload cancels itself; this covers a thread that hangs outside the transfer.
                if (std::chrono::steady_clock::now() - state->last_activity < SW_SLICER_UPLOAD_ABANDON_TIMEOUT)
                    throw std::runtime_error("an upload is still running");
                abandoned = true;
            }
        }

        // 3. Plate.
        const int  n     = sw_slicer_resolve_plate(m_param_data);
        PartPlate* plate = plater->get_partplate_list().get_plate(n);
        sw_slicer_require_plate_ready(n);

        // 4. Printer. The connected device tells whether the printer is reached over the LAN or
        // through the cloud (link_mode "wan").
        std::string connected_model;
        std::string connected_link_mode;
        for (const DeviceInfo& device : wxGetApp().app_config->get_devices())
            if (device.connected) {
                connected_model     = device.preset_name;
                connected_link_mode = device.link_mode;
            }
        // A cloud (WAN) connection has the cloud broker as its host, not the printer: never upload to
        // it. The app's own send dialog (Flutter page) handles the cloud upload instead.
        const bool                 wan = connected_link_mode == "wan";
        std::shared_ptr<PrintHost> host;
        if (!wan) {
            // LAN: only Moonraker hosts. Their upload() shows no dialog for an IP address; for a
            // ".local" host name it can show an address choice dialog, which must not run on this thread.
            wxGetApp().get_connect_host(host);
            if (!host)
                throw std::runtime_error("no printer is connected");
            // Moonraker and Moonraker_Mqtt (the Snapmaker connection) both report "Moonraker".
            if (std::string(host->get_name()) != "Moonraker")
                throw std::runtime_error(std::string("uploading is not supported for this printer connection (") + host->get_name() + ")");
            if (boost::algorithm::icontains(host->get_host(), ".local"))
                throw std::runtime_error("uploading is only supported for a printer connected by IP address");
        }

        std::vector<std::string> notes;
        // Same check as Plater::send_gcode_legacy(): the G-code must be made for the connected printer model.
        connected_model.erase(std::remove(connected_model.begin(), connected_model.end(), '('), connected_model.end());
        connected_model.erase(std::remove(connected_model.begin(), connected_model.end(), ')'), connected_model.end());
        const std::string current_model = sw_slicer_printer_model_name();
        if (connected_model.empty())
            notes.push_back("the model of the connected printer is unknown; not compared with the printer preset");
        else if (connected_model != current_model)
            throw std::runtime_error("the G-code is made for printer preset '" + current_model + "' but the connected printer is '" +
                                     connected_model + "'; select the matching printer preset and slice again");

        // 5. File name on the printer: 'filename' or the name the app proposes (as in send_gcode_legacy()).
        std::string filename;
        if (sw_slicer_has_param(m_param_data, "filename")) {
            const json& j = m_param_data.at("filename");
            filename      = j.is_string() ? j.get<std::string>() : std::string();
            if (!sw_slicer_valid_upload_name(filename))
                throw std::runtime_error("filename must be a plain file name ending in .gcode");
        } else {
            Print* print = plate->fff_print();
            if (print == nullptr)
                throw std::runtime_error("plate " + std::to_string(n) + " has no print data");
            std::string proposed = print->output_filepath("");
            if (!print->print_statistics().estimated_normal_print_time.empty())
                proposed = print->print_statistics().finalize_output_path(proposed);
            filename = boost::filesystem::path(Slic3r::fold_utf8_to_ascii(proposed)).filename().string();
            if (!sw_slicer_valid_upload_name(filename))
                throw std::runtime_error("the proposed file name '" + filename + "' is not usable; pass 'filename'");
        }

        if (wan) {
            // No upload by the API: the upload state is left unchanged.
            sw_slicer_send_via_app_dialog(n, filename, notes);
            m_res_data["mode"]     = "app_dialog";
            m_res_data["filename"] = filename;
            m_res_data["plate"]    = n;
            m_res_data["notes"]    = notes;
            send_to_js();
            finish_job();
            return;
        }

        // 6. Source: the plate's G-code, copied first so that a later reslice cannot change the
        // file while it is being uploaded (BackgroundSlicingProcess::prepare_upload() copies too).
        // Same conversion as send_gcode_legacy() (a plain std::string to path).
        const boost::filesystem::path source(plate->get_tmp_gcode_path());
        boost::system::error_code     ec;
        if (!boost::filesystem::is_regular_file(source, ec))
            throw std::runtime_error("the G-code of plate " + std::to_string(n) + " was not found; slice it again");
        const boost::filesystem::path upload_copy = boost::filesystem::temp_directory_path() /
                                                    boost::filesystem::unique_path(".api-upload-%%%%-%%%%-%%%%.gcode");
        boost::filesystem::copy_file(source, upload_copy, ec);
        if (ec) {
            boost::system::error_code ignore;
            boost::filesystem::remove(upload_copy, ignore);
            throw std::runtime_error("the G-code could not be prepared for the upload: " + ec.message());
        }

        PrintHostUpload upload_data;
        upload_data.use_3mf     = false;
        upload_data.source_path = upload_copy;
        upload_data.upload_path = boost::filesystem::path(filename);
        // Upload only: starting a print is a separate, explicitly confirmed printer command.
        upload_data.post_action = PrintHostPostUploadAction::None;

        int id = 0;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            id                   = ++state->id; // an abandoned thread no longer writes to the state
            state->state         = "uploading";
            state->progress      = 0;
            state->filename      = filename;
            state->error.clear();
            state->notified      = true;
            state->last_ulnow    = 0;
            state->last_ultotal  = 0;
            state->last_activity = std::chrono::steady_clock::now();
            state->stalled       = false;
        }

        // 7. The thread keeps its own references (host, state) and copies of the paths; it touches
        // neither the GUI, the Plater nor the model. The result notification is shown by
        // sw_SlicerGetStatus on the GUI thread.
        std::shared_ptr<SwSlicerUploadState> thread_state = state;
        try {
            std::thread([thread_state, id, host, upload_data, upload_copy]() {
                std::string error;
                bool        ok = false;
                try {
                    ok = host->upload(
                        upload_data,
                        [thread_state, id](Http::Progress progress, bool& cancel) {
                            const auto                  now = std::chrono::steady_clock::now();
                            std::lock_guard<std::mutex> lock(thread_state->mutex);
                            if (thread_state->id != id || thread_state->stalled) {
                                cancel = true; // abandoned or stalled (also stops the port 8080 retry)
                                return;
                            }
                            if (progress.ulnow != thread_state->last_ulnow || progress.ultotal != thread_state->last_ultotal) {
                                thread_state->last_ulnow    = progress.ulnow;
                                thread_state->last_ultotal  = progress.ultotal;
                                thread_state->last_activity = now;
                                if (progress.ultotal > 0)
                                    thread_state->progress = static_cast<int>(std::min<size_t>(100, progress.ulnow * 100 / progress.ultotal));
                            } else if (now - thread_state->last_activity > SW_SLICER_UPLOAD_STALL_TIMEOUT) {
                                thread_state->stalled = true;
                                cancel                = true;
                            }
                        },
                        [&error](wxString message) { error = into_u8(message); },
                        [](wxString /*tag*/, wxString /*status*/) {});
                } catch (const std::exception& e) {
                    ok    = false;
                    error = e.what();
                }
                boost::system::error_code ignore;
                boost::filesystem::remove(upload_copy, ignore);
                std::lock_guard<std::mutex> lock(thread_state->mutex);
                if (thread_state->id != id)
                    return; // abandoned: a newer upload owns the state
                if (thread_state->stalled) {
                    ok    = false;
                    error = "the upload made no progress for " + std::to_string(SW_SLICER_UPLOAD_STALL_TIMEOUT.count()) +
                            " s and was cancelled";
                } else if (!ok && error.empty()) {
                    error = "the upload failed";
                }
                thread_state->state = ok ? "done" : "failed";
                if (ok)
                    thread_state->progress = 100;
                thread_state->error    = ok ? std::string() : error;
                thread_state->notified = false;
            }).detach();
        } catch (const std::exception& e) {
            // The thread could not be started: nothing is uploading (the error is in this reply).
            boost::system::error_code ignore;
            boost::filesystem::remove(upload_copy, ignore);
            std::lock_guard<std::mutex> lock(state->mutex);
            state->state = "failed";
            state->error = e.what();
            throw std::runtime_error(std::string("the upload could not be started: ") + e.what());
        }

        if (abandoned)
            notes.push_back("the previous upload showed no activity for " + std::to_string(SW_SLICER_UPLOAD_ABANDON_TIMEOUT.count()) +
                            " s and was abandoned");
        if (Print* print = plate->fff_print()) {
            const std::vector<std::string>& scripts = print->config().post_process.values;
            if (std::any_of(scripts.begin(), scripts.end(), [](const std::string& script) { return !script.empty(); }))
                notes.push_back("post-processing scripts are not run for the upload (the plate's G-code is sent as sliced, like the app's own U1 upload)");
        }
        notes.push_back("a file with the same name on the printer is replaced");
        m_res_data["mode"]     = "upload";
        m_res_data["started"]  = true;
        m_res_data["id"]       = id;
        m_res_data["filename"] = filename;
        m_res_data["plate"]    = n;
        m_res_data["notes"]    = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

}} // namespace Slic3r::GUI
