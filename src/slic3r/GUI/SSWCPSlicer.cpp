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
#include "slic3r/GUI/GUI_Preview.hpp"
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
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
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
#include "libslic3r/ParameterUtils.hpp"
#include "slic3r/GUI/ParamsPanel.hpp"
#include "slic3r/GUI/ObjectDataViewModel.hpp"
#include "slic3r/GUI/HttpServer.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <limits>
#include <locale>
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
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <wx/font.h>
#include <wx/fontenum.h>
#include <wx/image.h>
#include <wx/weakref.h>
#include <wx/glcanvas.h> // after the slic3r headers (GLEW must come before gl.h)
#ifdef _WIN32
// SetForegroundWindow() for the app's Flutter pages (send dialog, device page).
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
        m_res_data["phase"]    = 13;
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
// Defined with sw_SlicerListPrinters: the connected printer and a running connect request.
static json sw_slicer_printer_status_json();

// Poll status: whether a slice is running, a rough progress (0/100, -1 = invalid),
// and whether the slice result is valid. Also whether a G-code export or a "slice all" run is
// in progress, the last upload to the printer (upload, if any), the printer connection (printer)
// and with all:true every plate.
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
        m_res_data["printer"] = sw_slicer_printer_status_json();
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
        // Applied right away so a following command sees the new layer (only once the
        // viewer has G-code; before that it still holds no result to read).
        if (canvas->get_gcode_viewer().has_data())
            canvas->apply_gcode_slider_changes();
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

// Object actions that go through the current canvas (Plater::canvas3D(), for example when creating
// primitives or pasting clones) need the 3D view to be shown.
static void sw_slicer_require_3d_view()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        throw std::runtime_error("no active plater");
    if (!plater->is_view3D_shown())
        throw std::runtime_error("switch to the 3D view first (sw_SlicerShowView)");
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

// Set while sw_SlicerCalibrate creates its test project: the loading progress dialog of the test model
// lets other requests through, which must not change the project or the presets halfway.
static bool& sw_slicer_calib_in_progress()
{
    static bool busy = false;
    return busy;
}

static void sw_slicer_require_no_calibration_running()
{
    if (sw_slicer_calib_in_progress())
        throw std::runtime_error("not possible while a calibration project is being created");
}

// Project switches, saving and deleting are refused while slicing is in progress or while a
// project is being loaded (the loading progress dialog lets other requests through).
static void sw_slicer_require_not_slicing()
{
    sw_slicer_require_no_calibration_running();
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

static bool sw_slicer_calib_reset_mode(); // calibration section

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
        bool                     calib_reset = false;
        {
            sswcp_api::Scope api_scope;
            if (presets == "discard")
                sw_slicer_discard_preset_changes();
            // skip_confirm: the checks above replace close_with_confirm(). Without the confirmation
            // the preset edits are kept (reset() reloads the edited presets).
            plater->new_project(/*skip_confirm=*/true, /*silent=*/false);
            // Like load_files(): leave calibration mode, so a later model without a file import is not
            // sliced as a calibration test.
            calib_reset = sw_slicer_calib_reset_mode();
            notes = sswcp_api::notes();
        }
        if (calib_reset)
            notes.push_back("calibration mode reset");
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

// The Flutter pages of the app (send dialog, device page) only work while the app is the active
// application: bring the main window to the front. If Windows refuses, ask for the user's attention
// and add a note that names 'what' needs the active window.
static bool sw_slicer_bring_app_to_front(std::vector<std::string>& notes, const std::string& what)
{
    MainFrame* frame = wxGetApp().mainframe;
    if (frame == nullptr)
        return false;
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
        notes.push_back("the app could not bring itself to the front; click the app window so that " + what + " works");
    }
    return in_front;
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
    sw_slicer_bring_app_to_front(notes, "the send dialog");
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

// --- Object settings and filament -----------------------------------------------------

// Keys that can be set per object (is_part false) or per part, exactly like the settings tab of the
// object list (TabPrintObject / TabPrintPart): Preset::print_options() limited to PrintObjectConfig +
// PrintRegionConfig (object) or to PrintRegionConfig (part). Without 'extruder': the filament is set
// with sw_SlicerSetFilament, because the app also resets the part filaments for an object.
static const std::set<std::string>& sw_slicer_model_keys(bool is_part)
{
    auto build = [](bool part) {
        std::set<std::string> scope;
        for (const std::string& k : PrintRegionConfig().keys())
            scope.insert(k);
        if (!part)
            for (const std::string& k : PrintObjectConfig().keys())
                scope.insert(k);
        std::set<std::string> keys;
        for (const std::string& k : Preset::print_options())
            if (scope.count(k) != 0 && k != "extruder")
                keys.insert(k);
        return keys;
    };
    static const std::set<std::string> object_keys = build(false);
    static const std::set<std::string> part_keys   = build(true);
    return is_part ? part_keys : object_keys;
}

// {object, volume?} of the object settings and filament commands. 'object' is required (no
// fallback to the selection); without 'volume' the object itself is the target (vol = -1).
namespace {
struct SwSlicerModelTarget
{
    int          obj = -1;
    int          vol = -1;
    ModelObject* mo  = nullptr;
    ModelVolume* mv  = nullptr;
};
} // namespace

static SwSlicerModelTarget sw_slicer_model_target(const json& params)
{
    if (!sw_slicer_has_param(params, "object"))
        throw std::runtime_error("specify 'object' (0-based)");
    sw_slicer_get_index(params, "object"); // type check
    SwSlicerModelTarget t;
    t.obj = sw_slicer_resolve_object(params);
    t.vol = sw_slicer_has_param(params, "volume") ? sw_slicer_get_index(params, "volume") : -1;
    sw_slicer_check_indices(t.obj, -1, t.vol);
    t.mo = wxGetApp().plater()->model().objects[t.obj];
    t.mv = (t.vol >= 0) ? t.mo->volumes[t.vol] : nullptr;
    return t;
}

// The config that holds the overrides of the target: the object's or the part's.
static ModelConfig& sw_slicer_target_config(int obj, int vol)
{
    ModelObject* mo = wxGetApp().plater()->model().objects[obj];
    if (vol < 0)
        return mo->config;
    return mo->volumes[vol]->config;
}

// Effective value of 'key' for the target: the part's override, else the object's override, else
// the active print preset (including unsaved edits, like sw_SlicerGetConfig). null if unknown.
static json sw_slicer_effective_value(int obj, int vol, const std::string& key)
{
    if (vol >= 0) {
        const ModelConfig& vcfg = sw_slicer_target_config(obj, vol);
        if (vcfg.has(key))
            return vcfg.opt_serialize(key);
    }
    const ModelConfig& ocfg = sw_slicer_target_config(obj, -1);
    if (ocfg.has(key))
        return ocfg.opt_serialize(key);
    Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINT);
    if (tab == nullptr || tab->get_config() == nullptr || tab->get_config()->optptr(key) == nullptr)
        return nullptr;
    return tab->get_config()->opt_serialize(key);
}

// A part can only carry its own settings or filament if it is a solid part or a modifier (the same
// types as in the object list) and the object list shows it as a separate item: an object with a
// single part has no part item (and cut connectors are not listed), so the GUI cannot show or edit
// part settings there.
static void sw_slicer_require_editable_part(const SwSlicerModelTarget& t, const std::string& what)
{
    const ModelVolumeType type = t.mv->type();
    if (type != ModelVolumeType::MODEL_PART && type != ModelVolumeType::PARAMETER_MODIFIER)
        throw std::runtime_error(what + std::string(sw_slicer_volume_type(type)));
    ObjectDataViewModel* list_model = wxGetApp().obj_list()->GetModel();
    if (list_model == nullptr || !list_model->GetVolumeItem(list_model->GetObjectItem(t.mo), t.vol).IsOk())
        throw std::runtime_error("volume " + std::to_string(t.vol) + " of object " + std::to_string(t.obj) +
                                 " is not a separate part in the object list; set it on the object (omit 'volume')");
}

// The filament of a target: the 'extruder' override (0 = default for an object, or "follows the
// object" for a part).
static int sw_slicer_target_filament(const ModelConfig& cfg) { return cfg.has("extruder") ? cfg.opt_int("extruder") : 0; }

// Read the overrides (own settings) of an object or part, its filament and, with 'keys', the
// effective values. Read-only.
void SSWCP_SliceProject_Instance::sw_SlicerGetObjectConfig()
{
    try {
        if (wxGetApp().plater() == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        const SwSlicerModelTarget t   = sw_slicer_model_target(m_param_data);
        const ModelConfig&        cfg = sw_slicer_target_config(t.obj, t.vol);
        json                      overrides = json::object();
        for (const std::string& key : cfg.keys())
            if (key != "extruder")
                overrides[key] = cfg.opt_serialize(key);
        m_res_data["object"] = t.obj;
        if (t.vol >= 0)
            m_res_data["volume"] = t.vol;
        m_res_data["overrides"] = overrides;
        m_res_data["filament"]  = sw_slicer_target_filament(cfg);
        if (sw_slicer_has_param(m_param_data, "keys")) {
            const json& keys = m_param_data.at("keys");
            if (!keys.is_array())
                throw std::runtime_error("'keys' must be a list of strings");
            const std::set<std::string>& allowed   = sw_slicer_model_keys(t.vol >= 0);
            json                         effective = json::object();
            for (const json& k : keys) {
                if (!k.is_string())
                    throw std::runtime_error("'keys' must be a list of strings");
                const std::string key = k.get<std::string>();
                effective[key]        = allowed.count(key) != 0 ? sw_slicer_effective_value(t.obj, t.vol, key) : json(nullptr);
            }
            m_res_data["effective"] = effective;
        }
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Set or remove overrides of an object or part, with the key names of sw_SlicerSetConfig. All or
// nothing: every key and value is validated before anything is written. One undo step
// "API: Object settings"; a no-op takes no snapshot. The object is resliced afterwards.
void SSWCP_SliceProject_Instance::sw_SlicerSetObjectConfig()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        // 1. Guards.
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        sw_slicer_require_not_slicing();
        const SwSlicerModelTarget t       = sw_slicer_model_target(m_param_data);
        const bool                is_part = t.vol >= 0;

        // 2. values and/or reset.
        std::vector<std::pair<std::string, std::string>> values;
        std::vector<std::string>                         reset;
        if (sw_slicer_has_param(m_param_data, "values")) {
            const json& j = m_param_data.at("values");
            if (!j.is_object())
                throw std::runtime_error("'values' must be an object {key: value}");
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (it.key().empty())
                    throw std::runtime_error("'values' contains an empty key");
                if (!it.value().is_string())
                    throw std::runtime_error("the value of '" + it.key() + "' must be a string (like sw_SlicerSetConfig)");
                values.emplace_back(it.key(), it.value().get<std::string>());
            }
        }
        if (sw_slicer_has_param(m_param_data, "reset")) {
            const json& j = m_param_data.at("reset");
            if (!j.is_array())
                throw std::runtime_error("'reset' must be a list of keys");
            for (const json& k : j) {
                if (!k.is_string() || k.get<std::string>().empty())
                    throw std::runtime_error("'reset' must be a list of keys");
                const std::string key = k.get<std::string>();
                if (std::find(reset.begin(), reset.end(), key) != reset.end())
                    throw std::runtime_error("'" + key + "' appears twice in reset");
                reset.push_back(key);
            }
        }
        if (values.empty() && reset.empty())
            throw std::runtime_error("specify 'values' and/or 'reset'");
        for (const std::string& key : reset)
            for (const auto& kv : values)
                if (kv.first == key)
                    throw std::runtime_error("'" + key + "' appears in both values and reset");

        // 3. Type of the part.
        if (is_part)
            sw_slicer_require_editable_part(t, "settings cannot be set on a ");

        // 4. Validate everything before anything is written.
        ModelConfig&                 cfg     = sw_slicer_target_config(t.obj, t.vol);
        const std::set<std::string>& allowed = sw_slicer_model_keys(is_part);
        const std::string            scope   = is_part ? "per part" : "per object";
        // A key that is already an override (for example from an older project) may always be reset.
        auto check_key = [&](const std::string& key, bool existing_ok) {
            if (key == "extruder")
                throw std::runtime_error("use sw_SlicerSetFilament to change the filament");
            if (allowed.count(key) == 0 && !(existing_ok && cfg.has(key)))
                throw std::runtime_error("'" + key + "' cannot be set " + scope);
        };
        DynamicPrintConfig probe;
        for (const auto& kv : values) {
            check_key(kv.first, false);
            try {
                probe.set_deserialize_strict(kv.first, kv.second);
            } catch (std::exception& e) {
                throw std::runtime_error("invalid value for '" + kv.first + "': " + e.what());
            }
            // set_deserialize may resolve a legacy name or a shortcut to other keys.
            if (probe.option(kv.first) == nullptr)
                throw std::runtime_error("invalid value for '" + kv.first + "': the key was not set");
        }
        for (const std::string& key : reset)
            check_key(key, true);

        // 5. What actually changes.
        std::vector<std::string> notes;
        std::vector<std::string> to_set;
        std::vector<std::string> to_erase;
        for (const auto& kv : values)
            if (!cfg.has(kv.first) || cfg.opt_serialize(kv.first) != probe.opt_serialize(kv.first))
                to_set.push_back(kv.first);
        for (const std::string& key : reset) {
            if (cfg.has(key))
                to_erase.push_back(key);
            else
                notes.push_back("'" + key + "' had no override");
        }
        json changed    = json::object();
        json reset_done = json::array();
        auto reply      = [&]() {
            m_res_data["object"] = t.obj;
            if (is_part)
                m_res_data["volume"] = t.vol;
            m_res_data["changed"] = changed;
            m_res_data["reset"]   = reset_done;
            m_res_data["notes"]   = notes;
            send_to_js();
            finish_job();
        };
        if (to_set.empty() && to_erase.empty()) {
            notes.push_back("settings unchanged; nothing changed");
            reply();
            return;
        }

        // 6. Select the target, so the settings panel shows it.
        sw_slicer_select(t.obj, -1, t.vol);
        wxGetApp().obj_list()->select_item(ObjectVolumeID{t.mo, is_part ? t.mv : nullptr});

        // 7. Write, 8. refresh like TabPrintModel::on_value_change() / notify_changed().
        {
            sswcp_api::Scope     api_scope;
            Plater::TakeSnapshot snapshot(plater, "API: Object settings");
            for (const std::string& key : to_set) {
                json e;
                e["old"] = cfg.has(key) ? json(cfg.opt_serialize(key)) : json(nullptr);
                cfg.set_key_value(key, probe.option(key)->clone());
                e["new"]     = cfg.opt_serialize(key);
                changed[key] = e;
            }
            for (const std::string& key : to_erase) {
                cfg.erase(key);
                reset_done.push_back(key);
            }
            wxGetApp().obj_list()->object_config_options_changed({t.mo, is_part ? t.mv : nullptr});
            wxGetApp().params_panel()->notify_object_config_changed();
            // The settings panel caches the values of the selection: rebuild it for the new values.
            wxGetApp().obj_list()->part_selection_changed();
            plater->notify_filament_usage_changed();
            // 'new' after the refresh: a settings check may have corrected the value (see the notes).
            for (const std::string& key : to_set)
                changed[key]["new"] = cfg.has(key) ? json(cfg.opt_serialize(key)) : json(nullptr);
            const std::vector<std::string>& api_notes = sswcp_api::notes();
            notes.insert(notes.end(), api_notes.begin(), api_notes.end());
        }
        // Schedule the reslice afterwards (never reslice() here). The object is looked up again by
        // its ID: its index may change if the user deletes an object in between.
        const ObjectID object_id = t.mo->id();
        wxGetApp().CallAfter([object_id]() {
            Plater* p = wxGetApp().plater();
            if (p == nullptr)
                return;
            for (ModelObject* o : p->model().objects)
                if (o->id() == object_id) {
                    p->changed_object(*o);
                    return;
                }
        });
        reply();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Choose the filament of an object or part, like the filament column of the object list
// (ObjectList::set_extruder_for_selected_items). 0 = default (object) or "use the object's
// filament" (part). Setting the object's filament resets the filaments of its solid parts, as in
// the app. One undo step "API: Change filament".
void SSWCP_SliceProject_Instance::sw_SlicerSetFilament()
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
        const SwSlicerModelTarget t       = sw_slicer_model_target(m_param_data);
        const bool                is_part = t.vol >= 0;

        // Same limit as the app (set_extruder_for_selected_items silently ignores a larger number).
        const int n = static_cast<int>(plater->get_extruder_colors_from_plater_config().size());
        const json* jf = sw_slicer_has_param(m_param_data, "filament") ? &m_param_data.at("filament") : nullptr;
        if (jf == nullptr || !jf->is_number_integer() || jf->get<long long>() < 0 || jf->get<long long>() > n)
            throw std::runtime_error("filament must be between 0 and " + std::to_string(n) + " (0 = default)");
        const int filament = jf->get<int>();
        if (is_part)
            sw_slicer_require_editable_part(t, "the filament of a ");

        ModelConfig&             cfg     = sw_slicer_target_config(t.obj, t.vol);
        const int                current = sw_slicer_target_filament(cfg);
        std::vector<std::string> notes;
        // For a solid part, 0 makes the app store the object's filament number as shown in the list;
        // a modifier really stores 0 (it does not change the filament).
        ObjectDataViewModel* list_model     = wxGetApp().obj_list()->GetModel();
        const bool           follows_object = is_part && filament == 0 && t.mv->type() == ModelVolumeType::MODEL_PART;
        const int object_filament = (list_model != nullptr) ? list_model->GetExtruderNumber(list_model->GetObjectItem(t.mo)) : 0;
        const int expected        = follows_object ? object_filament : filament;
        const char* zero_note     = follows_object ? "0 means: use the object's filament" : "0 means: the modifier does not change the filament";
        auto      reply           = [&](int value) {
            m_res_data["object"] = t.obj;
            if (is_part)
                m_res_data["volume"] = t.vol;
            m_res_data["filament"] = value;
            m_res_data["notes"]    = notes;
            send_to_js();
            finish_job();
        };
        if (current == filament || current == expected) {
            if (is_part && filament == 0)
                notes.push_back(zero_note);
            notes.push_back("filament unchanged; nothing changed");
            reply(current);
            return;
        }
        if (!is_part) {
            for (const ModelVolume* mv : t.mo->volumes)
                if (mv->type() == ModelVolumeType::MODEL_PART && mv->config.has("extruder")) {
                    notes.push_back("part filaments reset to follow the object");
                    break;
                }
        } else if (filament == 0) {
            notes.push_back(zero_note);
        }

        // set_extruder_for_selected_items() works on the selection of the object list.
        ObjectList* list = wxGetApp().obj_list();
        list->select_item(ObjectVolumeID{t.mo, is_part ? t.mv : nullptr});
        if (list->get_selected_obj_idx() != t.obj || (is_part ? list->get_selected_model_volume() != t.mv : list->get_selected_model_volume() != nullptr))
            throw std::runtime_error("could not select the target in the object list");
        {
            sswcp_api::Scope     api_scope;
            Plater::TakeSnapshot snapshot(plater, "API: Change filament");
            // The app's own snapshot "Change Filaments" is suppressed; it also updates the plater
            // (reslice through the normal background process) and the filament usage.
            list->set_extruder_for_selected_items(filament);
            const std::vector<std::string>& api_notes = sswcp_api::notes();
            notes.insert(notes.end(), api_notes.begin(), api_notes.end());
        }
        const int now = sw_slicer_target_filament(cfg);
        if (now != expected)
            throw std::runtime_error(sw_slicer_join_notes("the filament was not changed", notes));
        reply(now);
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- Presets ------------------------------------------------------------------------------

static PresetCollection& sw_slicer_preset_collection(Preset::Type type)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (type == Preset::TYPE_PRINTER)
        return bundle->printers;
    if (type == Preset::TYPE_FILAMENT)
        return bundle->filaments;
    return bundle->prints;
}

// Required string parameter 'type' (printer/print/filament).
static std::string sw_slicer_type_param(const json& params)
{
    if (!sw_slicer_has_param(params, "type") || !params.at("type").is_string())
        throw std::runtime_error("specify 'type' (printer, print or filament)");
    return params.at("type").get<std::string>();
}

// The presets of a type as in the sidebar combo boxes: visible and (print/filament) compatible
// with the selected printer; with all:true every preset. 'selected' is the selected preset, for
// filaments the list of the slots (slot 1 first). Read-only.
void SSWCP_SliceProject_Instance::sw_SlicerListPresets()
{
    try {
        const std::string  type = sw_slicer_type_param(m_param_data);
        const Preset::Type pt   = sw_slicer_preset_type(type);
        const bool         all  = sw_slicer_flag_true(m_param_data, "all");
        const PresetCollection& collection = sw_slicer_preset_collection(pt);
        json presets = json::array();
        for (const Preset& preset : collection) {
            if (preset.is_default)
                continue;
            if (!all && (!preset.is_visible || (pt != Preset::TYPE_PRINTER && !preset.is_compatible)))
                continue;
            json e;
            e["name"]       = preset.name;
            e["system"]     = preset.is_system;
            e["user"]       = preset.is_user();
            e["compatible"] = preset.is_compatible;
            presets.push_back(e);
        }
        m_res_data["type"] = type;
        if (pt == Preset::TYPE_FILAMENT)
            m_res_data["selected"] = wxGetApp().preset_bundle->filament_presets;
        else
            m_res_data["selected"] = collection.get_selected_preset_name();
        m_res_data["presets"] = presets;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Select a printer, print or filament preset (filament: per slot, 1-based), like the sidebar combo
// boxes. Unsaved preset edits require preset_changes:'discard'. Presets are not part of the undo
// stack: no snapshot; the reply holds 'previous' to switch back. A printer switch is refused while a
// physical printer is selected; if the bed moved, the re-centred objects are reported.
void SSWCP_SliceProject_Instance::sw_SlicerSelectPreset()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        // 1. Guards.
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        sw_slicer_require_not_slicing();

        const std::string  type = sw_slicer_type_param(m_param_data);
        const Preset::Type pt   = sw_slicer_preset_type(type);
        if (!sw_slicer_has_param(m_param_data, "name") || !m_param_data.at("name").is_string() ||
            m_param_data.at("name").get<std::string>().empty())
            throw std::runtime_error("specify 'name' (see sw_SlicerListPresets)");
        const std::string name = m_param_data.at("name").get<std::string>();

        // 2. Filament slot (1-based).
        PresetBundle*                   bundle = wxGetApp().preset_bundle;
        const std::vector<std::string>& slots  = bundle->filament_presets;
        int                             slot   = 1;
        if (sw_slicer_has_param(m_param_data, "filament")) {
            if (pt != Preset::TYPE_FILAMENT)
                throw std::runtime_error("'filament' only applies to type 'filament'");
            const json& j = m_param_data.at("filament");
            if (!j.is_number_integer() || j.get<long long>() < 1 || j.get<long long>() > static_cast<long long>(slots.size()))
                throw std::runtime_error("filament slot must be between 1 and " + std::to_string(slots.size()));
            slot = j.get<int>();
        }
        if (pt == Preset::TYPE_FILAMENT && (slot < 1 || slot > static_cast<int>(slots.size())))
            throw std::runtime_error("filament slot must be between 1 and " + std::to_string(slots.size()));

        // 3. The preset exists (and is compatible).
        PresetCollection& collection = sw_slicer_preset_collection(pt);
        const Preset*     preset     = collection.find_preset(name, false, true);
        if (preset == nullptr || preset->is_default || !preset->is_visible)
            throw std::runtime_error("preset '" + name + "' does not exist");
        if (pt != Preset::TYPE_PRINTER && !preset->is_compatible)
            throw std::runtime_error("preset '" + name + "' is not compatible with the selected printer");

        // 4. Already selected?
        const std::string previous = (pt == Preset::TYPE_FILAMENT) ? slots[slot - 1] : collection.get_selected_preset_name();
        std::vector<std::string> notes;
        auto reply = [&](const json& moved_objects) {
            m_res_data["type"] = type;
            m_res_data["name"] = name;
            if (pt == Preset::TYPE_FILAMENT)
                m_res_data["filament"] = slot;
            m_res_data["previous"] = previous;
            if (!moved_objects.empty())
                m_res_data["moved_objects"] = moved_objects;
            m_res_data["notes"] = notes;
            send_to_js();
            finish_job();
        };
        if (previous == name) {
            notes.push_back("preset already selected; nothing changed");
            reply(json::array());
            return;
        }

        // 5. Physical printers are not supported by the API (decision F2-O1).
        if (pt == Preset::TYPE_PRINTER && bundle->physical_printers.has_selection())
            throw std::runtime_error("a physical printer is selected; switch printers in the app");

        // 6. Unsaved preset edits: only on explicit request, and then all of them are discarded first,
        // so the app's unsaved-changes dialog never comes up.
        const std::string choice = sw_slicer_preset_choice(m_param_data, /*allow_keep=*/false);

        // 7. Switch.
        std::vector<int> moved;
        bool             ok = false;
        {
            sswcp_api::Scope api_scope;
            if (choice == "discard")
                sw_slicer_discard_preset_changes();
            ok = plater->select_preset_by_name(pt, name, slot - 1, &moved);
            notes = sswcp_api::notes();
        }
        if (choice == "discard")
            notes.insert(notes.begin(), "preset changes discarded");
        // 8.
        if (!ok)
            throw std::runtime_error(sw_slicer_join_notes("the preset could not be selected", notes));

        // 9. Not undoable; objects moved because the bed changed (decision F2-O2).
        notes.push_back("not part of undo; use previous to switch back");
        json moved_objects = json::array();
        std::sort(moved.begin(), moved.end());
        moved.erase(std::unique(moved.begin(), moved.end()), moved.end());
        const ModelObjectPtrs& objects = plater->model().objects;
        for (int i : moved)
            if (i >= 0 && i < static_cast<int>(objects.size())) {
                json e;
                e["object"] = i;
                e["name"]   = objects[i]->name;
                moved_objects.push_back(e);
            }
        if (!moved_objects.empty())
            notes.push_back("the bed changed: " + std::to_string(moved_objects.size()) +
                            " object(s) were moved to the centre of their plate (not part of undo)");
        reply(moved_objects);
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- Filament slots ------------------------------------------------------------------------

// Does any plate have a filament order per layer range? The app adjusts it to the filament count.
static bool sw_slicer_any_other_layers_sequence()
{
    PartPlateList& plates = wxGetApp().plater()->get_partplate_list();
    for (int i = 0; i < plates.get_plate_count(); ++i)
        if (!plates.get_plate(i)->get_other_layers_print_sequence().empty())
            return true;
    return false;
}

static bool sw_slicer_is_hex_color(const std::string& s)
{
    if (s.size() != 7 || s[0] != '#')
        return false;
    return std::all_of(s.begin() + 1, s.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

// Add or remove physical filament slots, like the + and - buttons of the sidebar. Removing requires
// replace_with (a remaining slot that takes over the objects of the removed slots). Not part of the
// undo stack: no snapshot.
void SSWCP_SliceProject_Instance::sw_SlicerSetFilamentCount()
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

        Sidebar&      sidebar = plater->sidebar();
        PresetBundle* bundle  = wxGetApp().preset_bundle;
        // The physical slots, as Sidebar::add_filament() counts them.
        const int  previous = static_cast<int>(sidebar.combos_filament().size());
        const int  max      = static_cast<int>(MAXIMUM_EXTRUDER_NUMBER);
        const json* jc      = sw_slicer_has_param(m_param_data, "count") ? &m_param_data.at("count") : nullptr;
        if (jc == nullptr || !jc->is_number_integer() || jc->get<long long>() < 1 || jc->get<long long>() > max)
            throw std::runtime_error("count must be between 1 and " + std::to_string(max));
        const int count = jc->get<int>();

        std::string color;
        if (sw_slicer_has_param(m_param_data, "color")) {
            if (count <= previous)
                throw std::runtime_error("'color' only applies when adding filaments");
            const json& j = m_param_data.at("color");
            color         = j.is_string() ? j.get<std::string>() : std::string();
            if (!sw_slicer_is_hex_color(color))
                throw std::runtime_error("color must look like #RRGGBB");
        }
        const bool has_replace = sw_slicer_has_param(m_param_data, "replace_with");
        if (has_replace && count >= previous)
            throw std::runtime_error("'replace_with' only applies when removing filaments");

        std::vector<std::string> notes;
        auto reply = [&]() {
            const ConfigOptionStrings* colours = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
            json                       list    = json::array();
            for (size_t i = 0; i < bundle->filament_presets.size(); ++i) {
                json e;
                e["slot"]   = static_cast<int>(i + 1);
                e["preset"] = bundle->filament_presets[i];
                e["color"]  = (colours != nullptr && i < colours->values.size()) ? colours->values[i] : std::string();
                list.push_back(e);
            }
            m_res_data["count"]     = static_cast<int>(sidebar.combos_filament().size());
            m_res_data["previous"]  = previous;
            m_res_data["filaments"] = list;
            m_res_data["notes"]     = notes;
            send_to_js();
            finish_job();
        };
        if (count == previous) {
            notes.push_back("filament count unchanged; nothing changed");
            reply();
            return;
        }
        const bool had_layer_sequences = sw_slicer_any_other_layers_sequence();

        if (count > previous) {
            // Add, like Sidebar::add_filament() (the same checks, in add_custom_filament()).
            bool failed = false;
            {
                sswcp_api::Scope api_scope;
                for (int i = previous; i < count; ++i) {
                    const size_t before = sidebar.combos_filament().size();
                    sidebar.add_custom_filament(color.empty() ? Plater::get_next_color_for_filament() : wxColour(wxString::FromUTF8(color)));
                    if (sidebar.combos_filament().size() <= before) {
                        failed = true;
                        break;
                    }
                }
                notes = sswcp_api::notes();
            }
            if (failed)
                throw std::runtime_error(sw_slicer_join_notes("could not add a filament (maximum reached?); the app now has " +
                                                                  std::to_string(sidebar.combos_filament().size()) + " filament(s)",
                                                              notes));
        } else {
            // Remove, from the highest slot down, like the "delete filament" menu with a replacement.
            const json* jr = has_replace ? &m_param_data.at("replace_with") : nullptr;
            if (jr == nullptr || !jr->is_number_integer() || jr->get<long long>() < 1 || jr->get<long long>() > count)
                throw std::runtime_error("removing filaments requires replace_with (a slot between 1 and " + std::to_string(count) + ")");
            const int replace_with = jr->get<int>();
            // Checked up front: with a replacement the app removes dependent mixed filaments without asking.
            for (int slot = count + 1; slot <= previous; ++slot)
                if (!bundle->mixed_filaments.mixed_filaments_using_physical(static_cast<unsigned int>(slot)).empty())
                    throw std::runtime_error("filament " + std::to_string(slot) +
                                             " is used by mixed filament configurations; change those in the app first");
            // Removing the slot of the edited filament preset makes the filament tab switch presets
            // (forced): unsaved edits of that preset would be lost.
            if (bundle->filaments.current_is_dirty()) {
                const std::string& edited = bundle->filaments.get_edited_preset().name;
                for (int slot = count + 1; slot <= previous; ++slot)
                    if (static_cast<size_t>(slot - 1) < bundle->filament_presets.size() && bundle->filament_presets[slot - 1] == edited)
                        throw std::runtime_error("filament preset '" + edited +
                                                 "' has unsaved changes and is used by a slot that would be removed; save or discard the changes first");
            }
            bool failed = false;
            {
                sswcp_api::Scope api_scope;
                for (int slot = previous; slot > count; --slot) {
                    const size_t before = sidebar.combos_filament().size();
                    sidebar.delete_filament(static_cast<size_t>(slot - 1), replace_with - 1);
                    if (sidebar.combos_filament().size() >= before) {
                        failed = true;
                        break;
                    }
                }
                notes = sswcp_api::notes();
            }
            if (failed)
                throw std::runtime_error(sw_slicer_join_notes("could not remove a filament; the app now has " +
                                                                  std::to_string(sidebar.combos_filament().size()) + " filament(s)",
                                                              notes));
            notes.push_back("objects using removed filaments now use filament " + std::to_string(replace_with));
        }
        if (had_layer_sequences)
            notes.push_back("the filament order per layer range was adjusted to the new filament count");
        notes.push_back("not part of undo");
        if (count < previous)
            notes.push_back("removing filaments cannot be undone");
        reply();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- Plate settings -----------------------------------------------------------------------

// "To the end" of a layer range, as stored by the plate settings dialog.
static constexpr int SW_SLICER_LAYER_END = std::numeric_limits<int>::max() - 1; // same as MAX_LAYER_VALUE in PlateSettingsDialog.cpp
// The dialog accepts layer ranges from layer 2 (layer 1 has the first-layer sequence).
static constexpr int SW_SLICER_MIN_OTHER_LAYER = 2; // same as MIN_LAYER_VALUE in PlateSettingsDialog.cpp

static std::string sw_slicer_bed_type_name(BedType bt)
{
    if (bt == btDefault)
        return "default";
    for (const auto& kv : ConfigOptionEnum<BedType>::get_enum_values())
        if (kv.second == static_cast<int>(bt))
            return kv.first;
    return "default";
}

static std::string sw_slicer_print_seq_name(PrintSequence ps)
{
    if (ps == PrintSequence::ByDefault)
        return "default";
    for (const auto& kv : ConfigOptionEnum<PrintSequence>::get_enum_values())
        if (kv.second == static_cast<int>(ps))
            return kv.first;
    return "default";
}

static std::string sw_slicer_spiral_name(const PartPlate* pp)
{
    if (!pp->has_spiral_mode_config())
        return "default";
    return pp->get_spiral_vase_mode() ? "on" : "off";
}

static std::string sw_slicer_range_text(const LayerPrintSequence& r)
{
    return std::to_string(r.first.first) + "-" + (r.first.second >= SW_SLICER_LAYER_END ? std::string("end") : std::to_string(r.first.second));
}

static json sw_slicer_other_layers_json(const std::vector<LayerPrintSequence>& seqs)
{
    json list = json::array();
    for (const LayerPrintSequence& s : seqs) {
        json e;
        e["from"]  = s.first.first;
        e["to"]    = (s.first.second >= SW_SLICER_LAYER_END) ? json(nullptr) : json(s.first.second);
        e["order"] = s.second;
        list.push_back(e);
    }
    return list;
}

static json sw_slicer_plate_settings_json(int n)
{
    PartPlate* pp = wxGetApp().plater()->get_partplate_list().get_plate(n);
    json       r;
    r["plate"]                 = n;
    // get_bed_type(false): the plate's own value (true would fall back to the global bed type).
    r["bed_type"]              = sw_slicer_bed_type_name(pp->get_bed_type(false));
    r["print_sequence"]        = sw_slicer_print_seq_name(pp->get_print_seq());
    r["spiral_vase"]           = sw_slicer_spiral_name(pp);
    r["first_layer_sequence"]  = pp->get_first_layer_print_sequence();
    r["other_layers_sequence"] = sw_slicer_other_layers_json(pp->get_other_layers_print_sequence());
    return r;
}

// first_layer_sequence: null/[] (clear) or unique filament numbers 1..n.
static std::vector<int> sw_slicer_parse_first_layer_sequence(const json& j, int n)
{
    std::vector<int> seq;
    if (j.is_null())
        return seq;
    if (!j.is_array())
        throw std::runtime_error("first_layer_sequence must be null or a list of filament numbers");
    for (const json& e : j) {
        if (!e.is_number_integer() || e.get<long long>() < 1 || e.get<long long>() > n)
            throw std::runtime_error("first_layer_sequence must contain filament numbers between 1 and " + std::to_string(n));
        const int f = e.get<int>();
        if (std::find(seq.begin(), seq.end(), f) != seq.end())
            throw std::runtime_error("filament " + std::to_string(f) + " appears twice in first_layer_sequence");
        seq.push_back(f);
    }
    return seq;
}

// other_layers_sequence: null/[] (clear) or [{from, to, order}], sorted by 'from', without overlap,
// each order a full order of the filaments 1..n.
static std::vector<LayerPrintSequence> sw_slicer_parse_other_layers_sequence(const json& j, int n)
{
    std::vector<LayerPrintSequence> seqs;
    if (j.is_null())
        return seqs;
    if (!j.is_array())
        throw std::runtime_error("other_layers_sequence must be null or a list of {from, to, order}");
    for (size_t i = 0; i < j.size(); ++i) {
        const json&       e      = j[i];
        const std::string prefix = "other_layers_sequence[" + std::to_string(i) + "]";
        if (!e.is_object())
            throw std::runtime_error(prefix + " must be an object {from, to, order}");
        for (auto it = e.begin(); it != e.end(); ++it)
            if (it.key() != "from" && it.key() != "to" && it.key() != "order")
                throw std::runtime_error(prefix + " has an unknown key '" + it.key() + "'");
        if (e.count("from") == 0 || !e.at("from").is_number_integer() || e.at("from").get<long long>() < SW_SLICER_MIN_OTHER_LAYER ||
            e.at("from").get<long long>() >= SW_SLICER_LAYER_END)
            throw std::runtime_error(prefix + ".from must be an integer >= " + std::to_string(SW_SLICER_MIN_OTHER_LAYER));
        const int from = e.at("from").get<int>();
        int       to   = SW_SLICER_LAYER_END;
        if (e.count("to") != 0 && !e.at("to").is_null()) {
            const json& jt = e.at("to");
            if (!jt.is_number_integer() || jt.get<long long>() < from || jt.get<long long>() >= SW_SLICER_LAYER_END)
                throw std::runtime_error(prefix + ".to must be null or an integer >= from");
            to = jt.get<int>();
        } else if (e.count("to") == 0) {
            throw std::runtime_error(prefix + ".to must be null or an integer >= from");
        }
        const std::string order_error = prefix + ".order must contain each filament 1.." + std::to_string(n) + " exactly once";
        if (e.count("order") == 0 || !e.at("order").is_array() || static_cast<int>(e.at("order").size()) != n)
            throw std::runtime_error(order_error);
        std::vector<int>  order;
        std::vector<bool> seen(static_cast<size_t>(n) + 1, false);
        for (const json& f : e.at("order")) {
            if (!f.is_number_integer() || f.get<long long>() < 1 || f.get<long long>() > n || seen[f.get<size_t>()])
                throw std::runtime_error(order_error);
            seen[f.get<size_t>()] = true;
            order.push_back(f.get<int>());
        }
        seqs.push_back({{from, to}, order});
    }
    std::stable_sort(seqs.begin(), seqs.end(), [](const LayerPrintSequence& a, const LayerPrintSequence& b) { return a.first.first < b.first.first; });
    for (size_t i = 1; i < seqs.size(); ++i)
        if (seqs[i - 1].first.second >= seqs[i].first.first)
            throw std::runtime_error("other_layers_sequence: layer ranges " + sw_slicer_range_text(seqs[i - 1]) + " and " +
                                     sw_slicer_range_text(seqs[i]) + " overlap");
    return seqs;
}

// Read the settings of a plate (default: the current plate). Read-only.
void SSWCP_SliceProject_Instance::sw_SlicerGetPlateSettings()
{
    try {
        if (wxGetApp().plater() == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        m_res_data = sw_slicer_plate_settings_json(sw_slicer_resolve_plate(m_param_data));
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Change the settings of a plate (default: the current plate), like the plate settings dialog, but
// on plate n without selecting it. Only the given fields change; all or nothing. One undo step
// "API: Plate settings" (the undo stack stores the plate config); a no-op takes no snapshot.
void SSWCP_SliceProject_Instance::sw_SlicerSetPlateSettings()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        // 1. Guards, plate, at least one field.
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        sw_slicer_require_not_slicing();
        const int  n  = sw_slicer_resolve_plate(m_param_data);
        PartPlate* pp = plater->get_partplate_list().get_plate(n);
        // Fields that are present (null counts as given for the two sequences: it clears them).
        auto given = [this](const char* key) { return m_param_data.count(key) != 0 && (!m_param_data.at(key).is_null() ||
                                                    std::string(key) == "first_layer_sequence" ||
                                                    std::string(key) == "other_layers_sequence"); };
        const bool has_bed = given("bed_type"), has_seq = given("print_sequence"), has_spiral = given("spiral_vase");
        const bool has_first = given("first_layer_sequence"), has_other = given("other_layers_sequence");
        if (!has_bed && !has_seq && !has_spiral && !has_first && !has_other)
            throw std::runtime_error("specify at least one plate setting");

        // 2. Validate everything first.
        auto string_param = [this](const char* key) {
            const json& j = m_param_data.at(key);
            return j.is_string() ? j.get<std::string>() : std::string("?");
        };
        BedType bed_type = btDefault;
        if (has_bed) {
            const std::string v = string_param("bed_type");
            if (v != "default" && (v == "Default Plate" || !ConfigOptionEnum<BedType>::from_string(v, bed_type) || bed_type == btDefault))
                throw std::runtime_error("unknown bed_type '" + v + "'");
            if (v == "default")
                bed_type = btDefault;
        }
        PrintSequence print_seq = PrintSequence::ByDefault;
        if (has_seq) {
            const std::string v = string_param("print_sequence");
            if (v != "default" && !ConfigOptionEnum<PrintSequence>::from_string(v, print_seq))
                throw std::runtime_error("print_sequence must be 'default', 'by layer' or 'by object'");
            if (v == "default")
                print_seq = PrintSequence::ByDefault;
        }
        std::string spiral;
        if (has_spiral) {
            spiral = string_param("spiral_vase");
            if (spiral != "default" && spiral != "on" && spiral != "off")
                throw std::runtime_error("spiral_vase must be 'default', 'on' or 'off'");
        }
        // The same filament count as the dialog (including mixed filaments).
        const int                       filaments = static_cast<int>(plater->get_extruder_colors_from_plater_config().size());
        std::vector<int>                first_seq;
        std::vector<LayerPrintSequence> other_seqs;
        if (has_first)
            first_seq = sw_slicer_parse_first_layer_sequence(m_param_data.at("first_layer_sequence"), filaments);
        if (has_other)
            other_seqs = sw_slicer_parse_other_layers_sequence(m_param_data.at("other_layers_sequence"), filaments);

        // What actually changes.
        const bool change_bed    = has_bed && pp->get_bed_type(false) != bed_type;
        const bool change_first  = has_first && pp->get_first_layer_print_sequence() != first_seq;
        const bool change_other  = has_other && pp->get_other_layers_print_sequence() != other_seqs;
        const bool change_seq    = has_seq && pp->get_print_seq() != print_seq;
        const bool change_spiral = has_spiral && sw_slicer_spiral_name(pp) != spiral;

        std::vector<std::string> notes;
        auto reply = [&]() {
            m_res_data          = sw_slicer_plate_settings_json(n);
            m_res_data["notes"] = notes;
            send_to_js();
            finish_job();
        };
        if (!change_bed && !change_first && !change_other && !change_seq && !change_spiral) {
            notes.push_back("plate settings unchanged; nothing changed");
            reply();
            return;
        }

        // 3. Apply to plate n (not necessarily the current plate), in the order of the callback of
        // the plate settings dialog (Plater::open_platesettings_dialog). The plate is not selected.
        {
            sswcp_api::Scope     api_scope;
            Plater::TakeSnapshot snapshot(plater, "API: Plate settings");
            if (change_bed) {
                pp->set_bed_type(bed_type);
                plater->update_project_dirty_from_presets();
                plater->set_plater_dirty(true);
            }
            if (change_first)
                pp->set_first_layer_print_sequence(first_seq);
            if (change_other)
                pp->set_other_layers_print_sequence(other_seqs);
            if (change_seq) {
                pp->set_print_seq(print_seq);
                plater->sync_print_seq_warning_notification();
            }
            if (change_spiral) {
                if (spiral == "on")
                    pp->set_spiral_vase_mode(true, false);
                else if (spiral == "off")
                    pp->set_spiral_vase_mode(false, false);
                else
                    pp->set_spiral_vase_mode(false, true);
            }
            plater->update_project_dirty_from_presets();
            plater->set_plater_dirty(true);
            // The plate config is applied to the print of the plate when it is sliced; the sequences
            // and spiral mode do not invalidate the slice result themselves.
            pp->update_slice_result_valid_state(false);
            plater->update();
            wxGetApp().obj_list()->update_selections();
            const std::vector<std::string>& api_notes = sswcp_api::notes();
            notes.insert(notes.end(), api_notes.begin(), api_notes.end());
        }
        if (change_spiral && sw_slicer_spiral_name(pp) != spiral)
            notes.push_back("spiral_vase is '" + sw_slicer_spiral_name(pp) + "': the plate follows the global setting, which already uses spiral mode");
        reply();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- Printer connection --------------------------------------------------------------------
// Connecting is done by the app's own device page (Flutter): only that page holds the credentials
// of a printer. The API prepares it (checks, app in front, Device tab open) and follows the progress
// through DeviceInfo::connected. Disconnecting is done by the API, like sw_Disconnect.
// Never return or accept credentials: printer JSON is built from a white list.

// A known printer, without any credentials (white list).
static json sw_slicer_printer_json(const DeviceInfo& d)
{
    json j;
    j["dev_id"]      = d.dev_id;
    j["dev_name"]    = d.dev_name;
    j["model"]       = d.model_name;
    j["preset_name"] = d.preset_name;
    j["link_mode"]   = d.link_mode.empty() ? json(nullptr) : json(d.link_mode);
    j["connected"]   = d.connected;
    // Over the cloud (wan) the address is the cloud broker, not the printer.
    if (d.link_mode == "lan")
        j["ip"] = d.ip;
    return j;
}

// The connected device, or nullptr.
static const DeviceInfo* sw_slicer_connected_device(const std::vector<DeviceInfo>& devices)
{
    for (const DeviceInfo& d : devices)
        if (d.connected)
            return &d;
    return nullptr;
}

static const DeviceInfo* sw_slicer_find_device(const std::vector<DeviceInfo>& devices, const std::string& dev_id)
{
    for (const DeviceInfo& d : devices)
        if (d.dev_id == dev_id)
            return &d;
    return nullptr;
}

// Last connect request made through the API. Only touched on the GUI thread (all sw_Slicer*
// handlers run there), so no mutex.
namespace {
struct SwSlicerConnectRequest
{
    std::string                           dev_id; // empty = none
    std::chrono::steady_clock::time_point since;
};
} // namespace

static SwSlicerConnectRequest& sw_slicer_connect_request()
{
    static SwSlicerConnectRequest request;
    return request;
}
static constexpr std::chrono::seconds SW_SLICER_CONNECT_TIMEOUT{180};

// {dev_id, state, seconds} of the connect request, or null if there is none. state: "connected"
// (the requested printer is connected), "other_connected", "timeout" or "waiting". With consume,
// a finished request is reported once and then cleared.
static json sw_slicer_connect_request_json(bool consume)
{
    SwSlicerConnectRequest& request = sw_slicer_connect_request();
    if (request.dev_id.empty())
        return json();
    const std::vector<DeviceInfo> devices   = wxGetApp().app_config->get_devices();
    const DeviceInfo*             connected = sw_slicer_connected_device(devices);
    const auto                    elapsed   = std::chrono::steady_clock::now() - request.since;
    std::string                   state     = "waiting";
    if (connected != nullptr && connected->dev_id == request.dev_id)
        state = "connected";
    else if (connected != nullptr)
        state = "other_connected";
    else if (elapsed > SW_SLICER_CONNECT_TIMEOUT)
        state = "timeout";
    json j;
    j["dev_id"]  = request.dev_id;
    j["state"]   = state;
    j["seconds"] = static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
    if (consume && state != "waiting")
        request.dev_id.clear();
    return j;
}

// For sw_SlicerGetStatus: {connected: dev_id|null, connect_request?}. Does not clear a finished
// request (sw_SlicerListPrinters reports it).
static json sw_slicer_printer_status_json()
{
    json              j;
    const auto        devices   = wxGetApp().app_config->get_devices();
    const DeviceInfo* connected = sw_slicer_connected_device(devices);
    j["connected"]              = connected != nullptr ? json(connected->dev_id) : json(nullptr);
    const json request          = sw_slicer_connect_request_json(/*consume=*/false);
    if (!request.is_null())
        j["connect_request"] = request;
    return j;
}

// The device page of the app (the "My Devices" list), as the app loads it after a connection.
static void sw_slicer_load_device_page(MainFrame* frame)
{
    const wxString url = wxString::FromUTF8(LOCALHOST_URL + std::to_string(wxGetApp().get_page_http_port()) +
                                            "/web/flutter_web/index.html?path=2");
    frame->load_printer_url(wxGetApp().get_international_url(url));
}

// Guards shared by connect and disconnect: no upload and no send dialog of the app.
static void sw_slicer_require_no_printer_traffic()
{
    {
        const std::shared_ptr<SwSlicerUploadState>& state = sw_slicer_upload_state();
        std::lock_guard<std::mutex>                 lock(state->mutex);
        if (state->state == "uploading")
            throw std::runtime_error("an upload is still running");
    }
    if (wxGetApp().get_web_preprint_dialog() != nullptr)
        throw std::runtime_error("the app's send dialog is open");
}

// Only 'allowed' parameters, and the confirmation flag first.
static void sw_slicer_require_printer_params(const json& params, const char* confirm_key, const std::string& confirm_error)
{
    if (!sw_slicer_flag_true(params, confirm_key))
        throw std::runtime_error(confirm_error);
    for (auto it = params.begin(); it != params.end(); ++it)
        if (it.key() != "dev_id" && it.key() != confirm_key)
            throw std::runtime_error("unknown parameter '" + it.key() + "'");
}

// Known printers (without credentials), the connected one, the login state and the progress of a
// connect request. Read-only (a finished connect request is reported once).
void SSWCP_SliceProject_Instance::sw_SlicerListPrinters()
{
    try {
        const std::vector<DeviceInfo> devices   = wxGetApp().app_config->get_devices();
        const DeviceInfo*             connected = sw_slicer_connected_device(devices);
        const bool                    logged_in = wxGetApp().sm_get_userinfo()->is_user_login();
        json                          printers  = json::array();
        bool                          any_wan   = false;
        for (const DeviceInfo& d : devices) {
            printers.push_back(sw_slicer_printer_json(d));
            any_wan = any_wan || d.link_mode == "wan";
        }
        std::vector<std::string> notes;
        if (logged_in && !any_wan)
            notes.push_back("cloud printers are not stored by the app between sessions; they appear here after the app's Device tab has shown them");
        m_res_data["printers"]  = printers;
        m_res_data["connected"] = connected != nullptr ? json(connected->dev_id) : json(nullptr);
        m_res_data["logged_in"] = logged_in;
        const json request      = sw_slicer_connect_request_json(/*consume=*/true);
        if (!request.is_null())
            m_res_data["connect_request"] = request;
        m_res_data["notes"] = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Prepare connecting a printer through the app's device page: the app comes to the front with the
// Device tab open; the printer is then selected under "My Devices" (by the user or by an
// assistant that controls the desktop). Follow the progress with sw_SlicerListPrinters. Requires confirm_connect:true.
// Nothing is started on the printer.
void SSWCP_SliceProject_Instance::sw_SlicerConnectPrinter()
{
    try {
        // 1. Confirmation and parameters first.
        sw_slicer_require_printer_params(m_param_data, "confirm_connect", "connecting a printer requires confirm_connect:true");
        if (!sw_slicer_has_param(m_param_data, "dev_id") || !m_param_data.at("dev_id").is_string() ||
            m_param_data.at("dev_id").get<std::string>().empty())
            throw std::runtime_error("dev_id is required (see sw_SlicerListPrinters)");
        const std::string dev_id = m_param_data.at("dev_id").get<std::string>();

        // 2. Guards.
        sw_slicer_require_no_printer_traffic();
        if (wxGetApp().get_web_device_dialog() != nullptr)
            throw std::runtime_error("the app's add-device dialog is open");

        const std::vector<DeviceInfo> devices   = wxGetApp().app_config->get_devices();
        const DeviceInfo*             connected = sw_slicer_connected_device(devices);
        const DeviceInfo*             device    = sw_slicer_find_device(devices, dev_id);
        const bool                    logged_in = wxGetApp().sm_get_userinfo()->is_user_login();
        std::vector<std::string>      notes;
        auto reply = [&](const char* status) {
            m_res_data["status"] = status;
            m_res_data["dev_id"] = dev_id;
            if (device != nullptr) {
                m_res_data["dev_name"]  = device->dev_name;
                m_res_data["link_mode"] = device->link_mode.empty() ? json(nullptr) : json(device->link_mode);
            }
            m_res_data["notes"] = notes;
            send_to_js();
            finish_job();
        };

        // 3./4. Already connected, or another printer is.
        if (connected != nullptr && connected->dev_id == dev_id) {
            notes.push_back("printer already connected; nothing changed");
            reply("connected");
            return;
        }
        if (connected != nullptr)
            throw std::runtime_error("printer '" + connected->dev_name + "' is connected; disconnect it first (sw_SlicerDisconnectPrinter)");

        // 5./6. Cloud login; unknown printers.
        if (device != nullptr && device->link_mode == "wan" && !logged_in)
            throw std::runtime_error("printer '" + device->dev_name +
                                     "' is connected through the Snapmaker cloud; log in to your Snapmaker account in the app first");
        if (device == nullptr) {
            if (!logged_in)
                throw std::runtime_error("unknown printer '" + dev_id + "' (see sw_SlicerListPrinters)");
            notes.push_back("printer '" + dev_id + "' is not known to the app yet (cloud printers appear in the Device tab); select it there");
        }

        MainFrame* frame = wxGetApp().mainframe;
        if (frame == nullptr || frame->m_printer_view == nullptr)
            throw std::runtime_error("the app's device page is not available");

        // 7. App to the front (the device page only works in the active window).
        sw_slicer_bring_app_to_front(notes, "the device page");
        // 8. Device tab with the device page (the same URL the app loads after a connection).
        if (!frame->m_printer_view->isSnapmakerPage())
            sw_slicer_load_device_page(frame);
        frame->select_tab(size_t(MainFrame::tpMonitor));

        // 9. Remember the request; the app's own threads do the connecting.
        SwSlicerConnectRequest& request = sw_slicer_connect_request();
        request.dev_id                  = dev_id;
        request.since                   = std::chrono::steady_clock::now();

        // 10.
        const std::string shown = device != nullptr && !device->dev_name.empty() ? device->dev_name : dev_id;
        notes.push_back("the app's Device tab is open; select '" + shown + "' under My Devices to connect");
        notes.push_back("keep the app window in front until the printer is connected");
        notes.push_back("connecting only links the app to the printer; nothing is started on the printer");
        reply("waiting_for_app");
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Disconnect the app from the connected printer, like "Disconnect" on the Device tab. A print that
// runs on the printer continues. Requires confirm_disconnect:true. The network part runs on a worker
// thread; the reply is sent from the GUI thread.
void SSWCP_SliceProject_Instance::sw_SlicerDisconnectPrinter()
{
    try {
        // 1. Confirmation and parameters first.
        sw_slicer_require_printer_params(m_param_data, "confirm_disconnect", "disconnecting a printer requires confirm_disconnect:true");
        std::string requested;
        if (sw_slicer_has_param(m_param_data, "dev_id")) {
            const json& j = m_param_data.at("dev_id");
            if (!j.is_string() || j.get<std::string>().empty())
                throw std::runtime_error("dev_id must be a non-empty string (see sw_SlicerListPrinters)");
            requested = j.get<std::string>();
        }

        // 2. Guards.
        sw_slicer_require_no_printer_traffic();

        const std::vector<DeviceInfo> devices   = wxGetApp().app_config->get_devices();
        const DeviceInfo*             connected = sw_slicer_connected_device(devices);
        // 3. Nothing connected (a pending connect request ends here as well).
        if (connected == nullptr) {
            sw_slicer_connect_request().dev_id.clear();
            m_res_data["status"] = "disconnected";
            if (!requested.empty())
                m_res_data["dev_id"] = requested;
            m_res_data["notes"] = std::vector<std::string>{"no printer connected; nothing changed"};
            send_to_js();
            finish_job();
            return;
        }
        // 4. Another printer than the requested one.
        if (!requested.empty() && requested != connected->dev_id)
            throw std::runtime_error("printer '" + requested + "' is not the connected printer ('" + connected->dev_id + "')");
        // 5. A pending connect request ends here.
        sw_slicer_connect_request().dev_id.clear();

        // 6. The worker thread only disconnects (PrintHost, AppConfig through CallAfter inside
        // sswcp_disconnect_current_machine); it touches neither the GUI, the Plater nor the model.
        const std::string             dev_id    = connected->dev_id;
        const std::string             dev_name  = connected->dev_name;
        std::weak_ptr<SSWCP_Instance> weak_self = shared_from_this();
        if (m_work_thread.joinable())
            m_work_thread.join();
        m_work_thread = std::thread([weak_self, dev_id, dev_name]() {
            bool ok = false;
            try {
                ok = sswcp_disconnect_current_machine(true);
            } catch (...) {
                ok = false;
            }
            // Queued after the CallAfter calls of the disconnect itself (AppConfig, presets).
            wxGetApp().CallAfter([weak_self, ok, dev_id, dev_name]() {
                std::shared_ptr<SSWCP_Instance> self = weak_self.lock();
                if (!ok) {
                    if (self)
                        self->handle_general_fail(-1, "the app reported a failure while disconnecting");
                    return;
                }
                // Reload the U1 device page, so that it lets go of its "current printer" and does not
                // connect it again when the app comes back to the foreground.
                MainFrame* frame = wxGetApp().mainframe;
                if (frame != nullptr && frame->m_printer_view != nullptr && frame->m_printer_view->is_u1_device_page())
                    sw_slicer_load_device_page(frame);
                if (!self)
                    return;
                self->m_res_data["status"]   = "disconnected";
                self->m_res_data["dev_id"]   = dev_id;
                self->m_res_data["dev_name"] = dev_name;
                self->m_res_data["notes"]    = std::vector<std::string>{"a print that is running on the printer continues", "not part of undo"};
                self->send_to_js();
                self->finish_job();
            });
        });
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- Parts, object actions and plates ---------------------------------------------------
// The actions of the right-click menus of the object list and the 3D view. Where the app has a
// function without a dialog, the handler calls it inside one "API: ..." snapshot; where the menu opens
// a choice or input dialog (change part type, rename, clone), the handler does the work after that
// dialog itself, with the same app calls.

// Changing the model while a project is being loaded is refused (the loading progress dialog lets
// other requests through).
static void sw_slicer_require_not_loading()
{
    sw_slicer_require_no_calibration_running();
    if (wxGetApp().plater()->is_loading_project())
        throw std::runtime_error("not possible while a project is being loaded");
}

// Volume type of the API ("part", "negative", ...); the reverse of sw_slicer_volume_type().
static ModelVolumeType sw_slicer_parse_volume_type(const std::string& type)
{
    if (type == "part")             return ModelVolumeType::MODEL_PART;
    if (type == "negative")         return ModelVolumeType::NEGATIVE_VOLUME;
    if (type == "modifier")         return ModelVolumeType::PARAMETER_MODIFIER;
    if (type == "support_blocker")  return ModelVolumeType::SUPPORT_BLOCKER;
    if (type == "support_enforcer") return ModelVolumeType::SUPPORT_ENFORCER;
    throw std::runtime_error("unknown type '" + type + "' (expected part|negative|modifier|support_blocker|support_enforcer)");
}

// Primitive of the API -> the shape name of create_mesh() in GUI_ObjectList.cpp.
static const char* sw_slicer_primitive_shape(const std::string& primitive)
{
    static const std::pair<const char*, const char*> shapes[] = {
        {"cube", "Cube"}, {"cylinder", "Cylinder"}, {"sphere", "Sphere"}, {"slab", "Slab"},
        {"cone", "Cone"}, {"disc", "Disc"},         {"torus", "Torus"},
    };
    for (const auto& s : shapes)
        if (primitive == s.first)
            return s.second;
    throw std::runtime_error("unknown primitive '" + primitive + "' (expected cube|cylinder|sphere|slab|cone|disc|torus)");
}

// Required string parameter that names an object or part: trimmed, not empty, no characters that
// the app refuses in names (same check as ObjectList::rename_item), no line breaks.
static std::string sw_slicer_get_name(const json& params, const char* key)
{
    const std::string k = key;
    if (!sw_slicer_has_param(params, key) || !params.at(key).is_string())
        throw std::runtime_error("'" + k + "' must be a non-empty string");
    std::string  name = params.at(key).get<std::string>();
    const char*  ws   = " \t\r\n\f\v";
    const size_t b    = name.find_first_not_of(ws);
    name              = (b == std::string::npos) ? std::string() : name.substr(b, name.find_last_not_of(ws) - b + 1);
    if (name.empty())
        throw std::runtime_error("'" + k + "' must be a non-empty string");
    if (Plater::has_illegal_filename_characters(name))
        throw std::runtime_error("the name contains characters that are not allowed: <>:/\\|?*\"");
    for (const unsigned char c : name)
        if (c < 0x20 || c == 0x7f)
            throw std::runtime_error("the name must not contain line breaks or control characters");
    if (wxString::FromUTF8(name).empty())
        throw std::runtime_error("'" + k + "' is not valid UTF-8");
    return name;
}

// Show the model name of an object (vol < 0) or part in the object list. Returns false if the list
// has no item for it (a single-part object has no part item).
static bool sw_slicer_refresh_list_name(int obj, int vol)
{
    ObjectDataViewModel* list_model = wxGetApp().obj_list()->GetModel();
    const ModelObject*   mo         = wxGetApp().plater()->model().objects[obj];
    if (list_model == nullptr)
        return false;
    wxDataViewItem item = list_model->GetObjectItem(mo);
    if (vol >= 0 && item.IsOk())
        item = list_model->GetVolumeItem(item, vol);
    if (!item.IsOk())
        return false;
    list_model->SetName(from_u8(vol >= 0 ? mo->volumes[vol]->name : mo->name), item);
    return true;
}

// Required 'object' parameter (no fallback to the selection).
static int sw_slicer_required_object(const json& params)
{
    if (!sw_slicer_has_param(params, "object"))
        throw std::runtime_error("specify 'object' (0-based)");
    sw_slicer_get_index(params, "object"); // type check
    return sw_slicer_resolve_object(params);
}

// Optional index parameter of object obj ('instance' or 'volume'); def if missing.
static int sw_slicer_optional_sub_index(const json& params, const char* key, int obj, int def)
{
    if (!sw_slicer_has_param(params, key))
        return def;
    const int idx = sw_slicer_get_index(params, key);
    if (std::string(key) == "instance")
        sw_slicer_check_indices(obj, idx, -1);
    else
        sw_slicer_check_indices(obj, -1, idx);
    return idx;
}

// The plate an instance belongs to (-1 if it is on no plate).
static int sw_slicer_instance_plate(int obj, int inst)
{
    PartPlateList& plates = wxGetApp().plater()->get_partplate_list();
    const int      plate  = plates.find_instance_belongs(obj, inst);
    return (plate >= 0) ? plate : plates.find_instance(obj, inst);
}

// Add a primitive: as a part, negative part, modifier or support blocker/enforcer of an object
// (ObjectList::load_generic_subobject, like "Add part/modifier/... > Cube"), or without 'object' as
// a new object (ObjectList::load_shape_object, like "Add primitive"). The primitive gets the app's
// default size. One undo step "API: Add part" / "API: Add primitive".
void SSWCP_SliceProject_Instance::sw_SlicerAddVolume()
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
        sw_slicer_require_3d_view();
        if (!sw_slicer_has_param(m_param_data, "primitive") || !m_param_data.at("primitive").is_string())
            throw std::runtime_error("specify 'primitive' (cube|cylinder|sphere|slab|cone|disc|torus)");
        const std::string primitive = m_param_data.at("primitive").get<std::string>();
        const std::string shape     = sw_slicer_primitive_shape(primitive);
        const bool        has_type  = sw_slicer_has_param(m_param_data, "type");
        if (has_type && !m_param_data.at("type").is_string())
            throw std::runtime_error("'type' must be a string (part|negative|modifier|support_blocker|support_enforcer)");
        const std::string     type_name = has_type ? m_param_data.at("type").get<std::string>() : std::string("part");
        const ModelVolumeType type      = sw_slicer_parse_volume_type(type_name);
        const bool            has_name  = sw_slicer_has_param(m_param_data, "name");
        const std::string     name      = has_name ? sw_slicer_get_name(m_param_data, "name") : std::string();
        Model&                   model  = plater->model();
        GLCanvas3D*              canvas = sw_slicer_canvas();
        ObjectList*              list   = wxGetApp().obj_list();
        std::vector<std::string> notes;

        if (!sw_slicer_has_param(m_param_data, "object")) {
            // A primitive as a new object.
            if (type != ModelVolumeType::MODEL_PART)
                throw std::runtime_error("specify 'object' to add a " + type_name);
            if (primitive == "slab")
                throw std::runtime_error("a slab can only be added to an object");
            const size_t n_before = model.objects.size();
            {
                sswcp_api::Scope     api_scope;
                Plater::TakeSnapshot snapshot(plater, "API: Add primitive");
                // load_shape_object() adds nothing while something is selected. Cleared after the
                // snapshot, so undo restores the user's selection.
                canvas->get_selection().clear();
                list->update_selections();
                canvas->update_gizmos_on_off_state();
                canvas->set_as_dirty();
                // The app's own snapshot "Add Primitive" is suppressed.
                list->load_shape_object(shape);
                if (has_name && model.objects.size() == n_before + 1) {
                    // Like ObjectList::update_name_in_model(): a single-part object names its part too.
                    ModelObject* mo = model.objects.back();
                    mo->name        = name;
                    if (mo->volumes.size() == 1)
                        mo->volumes.front()->name = name;
                    sw_slicer_refresh_list_name(static_cast<int>(n_before), -1);
                }
                notes = sswcp_api::notes();
            }
            if (model.objects.size() != n_before + 1)
                throw std::runtime_error(sw_slicer_join_notes("the primitive could not be added", notes));
            const int obj = static_cast<int>(n_before);
            m_res_data["object"] = obj;
            m_res_data["name"]   = model.objects[obj]->name;
            m_res_data["type"]   = "part";
            m_res_data["notes"]  = notes;
            send_to_js();
            finish_job();
            return;
        }

        // A primitive as a part of an object.
        if (!has_type)
            throw std::runtime_error("specify 'type' (part|negative|modifier|support_blocker|support_enforcer)");
        const int    obj = sw_slicer_required_object(m_param_data);
        ModelObject* mo  = model.objects[obj];
        // Stricter than the context menu of this fork, which offers these on cut objects too: a new
        // solid or negative part changes the geometry of one half of the cut without the other.
        if (mo->is_cut() && (type == ModelVolumeType::MODEL_PART || type == ModelVolumeType::NEGATIVE_VOLUME))
            throw std::runtime_error("parts cannot be added to a cut object");
        // load_generic_subobject() reads the object from the object list and the instance from the
        // canvas selection.
        sw_slicer_select(obj, 0, -1);
        if (list->get_selected_obj_idx() != obj || canvas->get_selection().get_object_idx() != obj)
            throw std::runtime_error("could not select object " + std::to_string(obj));
        std::set<size_t>             ids_before;
        std::set<const ModelVolume*> ptrs_before;
        for (const ModelVolume* mv : mo->volumes) {
            ids_before.insert(mv->id().id);
            ptrs_before.insert(mv);
        }
        ModelVolume* added = nullptr;
        {
            sswcp_api::Scope     api_scope;
            Plater::TakeSnapshot snapshot(plater, "API: Add part");
            // The app's own snapshot "Add primitive" is suppressed, and in API mode the
            // "Add Modifier" tip is not shown.
            list->load_generic_subobject(shape, type);
            for (ModelVolume* mv : mo->volumes)
                if (ids_before.count(mv->id().id) == 0 && ptrs_before.count(mv) == 0) {
                    added = mv;
                    break;
                }
            if (added != nullptr && has_name)
                added->name = name;
            notes = sswcp_api::notes();
        }
        if (added == nullptr)
            throw std::runtime_error(sw_slicer_join_notes("the part could not be added", notes));
        // The index after the app has reordered the volumes (reorder_volumes_and_get_selection).
        int vol = -1;
        for (size_t v = 0; v < mo->volumes.size(); ++v)
            if (mo->volumes[v] == added)
                vol = static_cast<int>(v);
        if (has_name)
            sw_slicer_refresh_list_name(obj, vol);
        m_res_data["object"] = obj;
        m_res_data["volume"] = vol;
        m_res_data["name"]   = added->name;
        m_res_data["type"]   = sw_slicer_volume_type(added->type());
        m_res_data["notes"]  = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Change the type of a part, the work of ObjectList::change_part_type() after its choice dialog. The
// app reorders the volumes afterwards: the reply holds the new index. One undo step
// "API: Change part type"; the same type takes no snapshot.
void SSWCP_SliceProject_Instance::sw_SlicerChangeVolumeType()
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
        const SwSlicerModelTarget t = sw_slicer_model_target(m_param_data);
        if (t.vol < 0)
            throw std::runtime_error("specify 'volume' (0-based)");
        if (!sw_slicer_has_param(m_param_data, "type") || !m_param_data.at("type").is_string())
            throw std::runtime_error("specify 'type' (part|negative|modifier|support_blocker|support_enforcer)");
        const ModelVolumeType new_type = sw_slicer_parse_volume_type(m_param_data.at("type").get<std::string>());
        ModelVolume*          mv       = t.mv;
        const ModelVolumeType old_type = mv->type();

        std::vector<std::string> notes;
        auto reply = [&](int vol) {
            m_res_data["object"] = t.obj;
            m_res_data["volume"] = vol;
            m_res_data["type"]   = sw_slicer_volume_type(mv->type());
            m_res_data["notes"]  = notes;
            send_to_js();
            finish_job();
        };
        if (new_type == old_type) {
            notes.push_back("type unchanged; nothing changed");
            reply(t.vol);
            return;
        }
        // The checks of change_part_type() (it shows an error dialog for the first one, and does not
        // offer the support types for text and SVG parts).
        if (old_type == ModelVolumeType::MODEL_PART && sw_slicer_count_model_parts(t.mo) == 1)
            throw std::runtime_error("the type of the last solid part cannot be changed");
        if ((mv->is_text() || mv->is_svg()) &&
            (new_type == ModelVolumeType::SUPPORT_BLOCKER || new_type == ModelVolumeType::SUPPORT_ENFORCER))
            throw std::runtime_error("a text or SVG part can only be a part, negative volume or modifier");
        // The object list does not show cut connectors as parts, so the GUI cannot change their type.
        if (mv->is_cut_connector())
            throw std::runtime_error("the type of a cut connector cannot be changed");
        // Same rule as sw_SlicerAddVolume: no new solid or negative geometry on one half of a cut.
        auto is_geometry = [](ModelVolumeType ty) {
            return ty == ModelVolumeType::MODEL_PART || ty == ModelVolumeType::NEGATIVE_VOLUME;
        };
        if (t.mo->is_cut() && is_geometry(new_type) && !is_geometry(old_type))
            throw std::runtime_error("a cut object cannot get a new part or negative part");

        sw_slicer_select(t.obj, -1, t.vol);
        ObjectList* list = wxGetApp().obj_list();
        list->select_item(ObjectVolumeID{t.mo, mv});
        {
            sswcp_api::Scope     api_scope;
            Plater::TakeSnapshot snapshot(plater, "API: Change part type");
            mv->set_type(new_type);
            // Reorders the volumes, rebuilds the object's items in the list and schedules the
            // reslice (ObjectList::changed_object), like change_part_type().
            wxDataViewItemArray sel = list->reorder_volumes_and_get_selection(t.obj, [mv](const ModelVolume* v) { return v == mv; });
            if (!sel.IsEmpty())
                list->select_item(sel.front());
            else
                plater->changed_object(t.obj);
            const std::vector<std::string>& api_notes = sswcp_api::notes();
            notes.insert(notes.end(), api_notes.begin(), api_notes.end());
        }
        int vol = -1;
        for (size_t v = 0; v < t.mo->volumes.size(); ++v)
            if (t.mo->volumes[v] == mv)
                vol = static_cast<int>(v);
        if (vol != t.vol)
            notes.push_back("the part is now volume " + std::to_string(vol) + " (the app reorders parts by type)");
        sw_slicer_canvas()->set_as_dirty();
        reply(vol);
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Rename an object or part, the work of ObjectList::rename_item() after its input dialog (the model
// part as in ObjectList::update_name_in_model()). One undo step "API: Rename"; the same name takes no
// snapshot.
void SSWCP_SliceProject_Instance::sw_SlicerRename()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        sw_slicer_require_not_loading();
        const SwSlicerModelTarget t        = sw_slicer_model_target(m_param_data);
        const bool                is_part  = t.vol >= 0;
        const std::string         name     = sw_slicer_get_name(m_param_data, "name");
        const std::string         old_name = is_part ? t.mv->name : t.mo->name;

        std::vector<std::string> notes;
        auto reply = [&]() {
            m_res_data["object"] = t.obj;
            if (is_part)
                m_res_data["volume"] = t.vol;
            m_res_data["name"]     = is_part ? t.mv->name : t.mo->name;
            m_res_data["old_name"] = old_name;
            m_res_data["notes"]    = notes;
            send_to_js();
            finish_job();
        };
        if (name == old_name) {
            notes.push_back("name unchanged; nothing changed");
            reply();
            return;
        }
        sw_slicer_select(t.obj, -1, t.vol);
        wxGetApp().obj_list()->select_item(ObjectVolumeID{t.mo, is_part ? t.mv : nullptr});
        {
            sswcp_api::Scope     api_scope;
            Plater::TakeSnapshot snapshot(plater, "API: Rename");
            if (is_part) {
                t.mv->name = name;
            } else {
                t.mo->name = name;
                // Like update_name_in_model(): a single-part object names its part too.
                if (t.mo->volumes.size() == 1)
                    t.mo->volumes.front()->name = name;
                Slic3r::save_object_mesh(*t.mo);
            }
            if (!sw_slicer_refresh_list_name(t.obj, t.vol))
                notes.push_back("the part is not shown separately in the object list");
            const std::vector<std::string>& api_notes = sswcp_api::notes();
            notes.insert(notes.end(), api_notes.begin(), api_notes.end());
        }
        sw_slicer_canvas()->set_as_dirty();
        reply();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Clone one instance of an object (default 0) 'count' times, like the clone dialog (Selection::clone(): copy to the app's internal
// clipboard, then paste 'count' times), without its progress bar and without arranging (that is a
// separate step, sw_SlicerArrange). One undo step "API: Clone".
void SSWCP_SliceProject_Instance::sw_SlicerClone()
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
        sw_slicer_require_3d_view();
        const int   obj = sw_slicer_required_object(m_param_data);
        const json* jc  = sw_slicer_has_param(m_param_data, "count") ? &m_param_data.at("count") : nullptr;
        // Same limit as the spin control of the clone dialog (CloneDialog.cpp).
        if (jc == nullptr || !jc->is_number_integer() || jc->get<long long>() < 1 || jc->get<long long>() > 1000)
            throw std::runtime_error("count must be between 1 and 1000");
        const int count = jc->get<int>();
        // One instance (default 0) is cloned: paste_objects_from_clipboard() puts every instance of a
        // copy at the same position, so cloning all instances would stack them.
        const int inst = sw_slicer_optional_sub_index(m_param_data, "instance", obj, 0);

        sw_slicer_select(obj, inst, -1);
        GLCanvas3D* canvas = sw_slicer_canvas();
        Selection&  sel    = canvas->get_selection();
        if (sel.is_empty() || sel.get_mode() != Selection::Instance || sel.get_object_idx() != obj ||
            sel.get_instance_idx() != inst)
            throw std::runtime_error("could not select instance " + std::to_string(inst) + " of object " + std::to_string(obj));
        Model&                   model    = plater->model();
        const size_t             n_before = model.objects.size();
        const size_t             n_inst   = model.objects[obj]->instances.size();
        std::vector<std::string> notes;
        {
            sswcp_api::Scope     api_scope;
            Plater::TakeSnapshot snapshot(plater, "API: Clone");
            // The app's own snapshot "Selection-clone" is suppressed.
            sel.clone(count);
            notes = sswcp_api::notes();
        }
        const size_t n_after = model.objects.size();
        if (n_after <= n_before)
            throw std::runtime_error(sw_slicer_join_notes("cloning produced nothing", notes));
        json added = json::array();
        for (size_t i = n_before; i < n_after; ++i)
            added.push_back(static_cast<int>(i));
        // Selection::copy_to_clipboard() fills the selection's own clipboard (a Model), not the
        // clipboard of the operating system.
        notes.push_back("the app's internal copy/paste clipboard now holds this object");
        if (n_inst > 1)
            notes.push_back("the object has " + std::to_string(n_inst) + " instances; only instance " + std::to_string(inst) +
                            " was cloned (each clone has one instance)");
        m_res_data["instance"]      = inst;
        m_res_data["objects_added"] = added;
        m_res_data["objects"]       = sw_slicer_objects_json();
        m_res_data["notes"]         = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Mirror an instance (default 0) or, with 'volume', a part along x, y or z, like the "Mirror" menu
// (GLCanvas3D::mirror_selection). Mirroring always changes something. One undo step "API: Mirror".
void SSWCP_SliceProject_Instance::sw_SlicerMirror()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        sw_slicer_require_not_loading();
        sw_slicer_require_3d_view();
        const std::string a = (sw_slicer_has_param(m_param_data, "axis") && m_param_data.at("axis").is_string()) ?
                                  m_param_data.at("axis").get<std::string>() : std::string();
        if (a != "x" && a != "y" && a != "z")
            throw std::runtime_error("axis must be 'x', 'y' or 'z'");
        const Axis axis = (a == "x") ? X : (a == "y") ? Y : Z;
        const int  obj  = sw_slicer_required_object(m_param_data);
        const int  inst = sw_slicer_optional_sub_index(m_param_data, "instance", obj, 0);
        const int  vol  = sw_slicer_optional_sub_index(m_param_data, "volume", obj, -1);
        sw_slicer_select(obj, inst, vol);
        std::vector<std::string> notes;
        // Same condition as the menu item (no cut objects, a single instance).
        if (!plater->can_mirror())
            throw std::runtime_error("this cannot be mirrored (cut objects cannot be mirrored)");
        {
            sswcp_api::Scope     api_scope;
            Plater::TakeSnapshot snapshot(plater, "API: Mirror");
            // The app's own snapshot "Mirror Object" is suppressed.
            sw_slicer_canvas()->mirror_selection(axis);
            const std::vector<std::string>& api_notes = sswcp_api::notes();
            notes.insert(notes.end(), api_notes.begin(), api_notes.end());
        }
        const ModelObject* mo = plater->model().objects[obj];
        m_res_data["object"]   = obj;
        m_res_data["instance"] = inst;
        if (vol >= 0)
            m_res_data["volume"] = vol;
        m_res_data["mirror"] = sw_slicer_vec3(vol >= 0 ? mo->volumes[vol]->get_mirror() : mo->instances[inst]->get_mirror());
        m_res_data["notes"]  = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Drop an instance (default 0) onto the bed, like the "Drop" menu (Plater::drop_selection). The menu
// item is disabled when the selection already touches the bed; then no snapshot is taken.
// One undo step "API: Drop".
void SSWCP_SliceProject_Instance::sw_SlicerDrop()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        sw_slicer_require_not_loading();
        sw_slicer_require_3d_view();
        const int obj  = sw_slicer_required_object(m_param_data);
        const int inst = sw_slicer_optional_sub_index(m_param_data, "instance", obj, 0);
        sw_slicer_select(obj, inst, -1);
        // Selection::drop() moves by the min z of the selection's bounding box (all volumes of the
        // instance): the no-op check uses the same value.
        const double             min_z = sw_slicer_canvas()->get_selection().get_bounding_box().min.z();
        std::vector<std::string> notes;
        if (std::abs(min_z) <= 1e-4) {
            notes.push_back("already on the bed; nothing changed");
        } else {
            sswcp_api::Scope     api_scope;
            Plater::TakeSnapshot snapshot(plater, "API: Drop");
            // The app's own snapshot "Move Object" is suppressed.
            plater->drop_selection();
            const std::vector<std::string>& api_notes = sswcp_api::notes();
            notes.insert(notes.end(), api_notes.begin(), api_notes.end());
        }
        m_res_data["object"]   = obj;
        m_res_data["instance"] = inst;
        m_res_data["position"] = sw_slicer_vec3(plater->model().objects[obj]->instances[inst]->get_offset());
        m_res_data["notes"]    = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Center an instance (default 0) on its plate in x/y, like the "Center" menu. The menu centers on the
// current plate (Plater::center_selection); the API centers on the plate the instance is on
// (GLCanvas3D::do_center_plate, the same move), so an object never jumps to another plate. An
// instance on no plate is centered on the current plate. One undo step "API: Center".
void SSWCP_SliceProject_Instance::sw_SlicerCenter()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        sw_slicer_require_not_loading();
        sw_slicer_require_3d_view();
        const int obj  = sw_slicer_required_object(m_param_data);
        const int inst = sw_slicer_optional_sub_index(m_param_data, "instance", obj, 0);
        sw_slicer_select(obj, inst, -1);
        PartPlateList& plates = plater->get_partplate_list();
        int            plate  = sw_slicer_instance_plate(obj, inst);
        if (plate < 0)
            plate = plates.get_curr_plate_index();
        // Same values as Selection::center_plate().
        GLCanvas3D*              canvas = sw_slicer_canvas();
        const Vec3d              src    = canvas->get_selection().get_bounding_box().center();
        const Vec3d              tar    = plates.get_plate(plate)->get_center_origin();
        std::vector<std::string> notes;
        if (std::abs(tar.x() - src.x()) <= 1e-3 && std::abs(tar.y() - src.y()) <= 1e-3) {
            notes.push_back("already centered on the plate; nothing changed");
        } else {
            sswcp_api::Scope     api_scope;
            Plater::TakeSnapshot snapshot(plater, "API: Center");
            // The app's own snapshot "Move Object" is suppressed.
            canvas->do_center_plate(plate);
            const std::vector<std::string>& api_notes = sswcp_api::notes();
            notes.insert(notes.end(), api_notes.begin(), api_notes.end());
        }
        m_res_data["object"]   = obj;
        m_res_data["instance"] = inst;
        m_res_data["plate"]    = plate;
        m_res_data["position"] = sw_slicer_vec3(plater->model().objects[obj]->instances[inst]->get_offset());
        m_res_data["notes"]    = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Make all instances of an object, or one instance, printable or not, the work of
// ObjectList::toggle_printable_state() (with an explicit state instead of a toggle). Unprintable
// instances are not sliced. One undo step "API: Set printable"; no change takes no snapshot.
void SSWCP_SliceProject_Instance::sw_SlicerSetPrintable()
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
        const int  obj      = sw_slicer_required_object(m_param_data);
        const bool has_inst = sw_slicer_has_param(m_param_data, "instance");
        const int  inst     = sw_slicer_optional_sub_index(m_param_data, "instance", obj, -1);
        if (!m_param_data.count("printable") || !m_param_data.at("printable").is_boolean())
            throw std::runtime_error("'printable' must be true or false");
        const bool   printable = m_param_data.at("printable").get<bool>();
        ModelObject* mo        = plater->model().objects[obj];
        std::vector<int> targets;
        if (has_inst)
            targets.push_back(inst);
        else
            for (size_t i = 0; i < mo->instances.size(); ++i)
                targets.push_back(static_cast<int>(i));
        bool changes = false;
        for (int i : targets)
            changes = changes || mo->instances[i]->printable != printable;

        sw_slicer_select(obj, has_inst ? inst : -1, -1);
        std::vector<std::string> notes;
        if (!changes) {
            notes.push_back("printable state unchanged; nothing changed");
        } else {
            sswcp_api::Scope     api_scope;
            Plater::TakeSnapshot snapshot(plater, "API: Set printable");
            ObjectList* list = wxGetApp().obj_list();
            for (int i : targets) {
                mo->instances[i]->printable = printable;
                list->update_printable_state(obj, i);
            }
            sw_slicer_canvas()->update_instance_printable_state_for_objects({static_cast<size_t>(obj)});
            plater->update();
            plater->reload_paint_after_background_process_apply();
            const std::vector<std::string>& api_notes = sswcp_api::notes();
            notes.insert(notes.end(), api_notes.begin(), api_notes.end());
        }
        m_res_data["object"] = obj;
        if (has_inst)
            m_res_data["instance"] = inst;
        m_res_data["printable"] = printable;
        m_res_data["notes"]     = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Scale an instance (default 0) uniformly to fill the build volume and center it, like "Scale to build
// volume" (Plater::scale_selection_to_fit_print_volume). One undo step "API: Scale to fit".
// No-op: for a rectangular bed the handler repeats the app's computation (Selection.cpp,
// fit_rectangle) beforehand; if the app would change nothing, or only by rounding noise, no snapshot is
// taken. For a circular bed that check is not done: if nothing changed, an empty undo step remains and
// the reply says so.
void SSWCP_SliceProject_Instance::sw_SlicerScaleToFit()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        sw_slicer_require_not_loading();
        sw_slicer_require_3d_view();
        const int obj  = sw_slicer_required_object(m_param_data);
        const int inst = sw_slicer_optional_sub_index(m_param_data, "instance", obj, 0);
        sw_slicer_select(obj, inst, -1);
        // Same condition as the menu item (no cut objects, a rectangular or circular bed).
#if ENABLE_ENHANCED_PRINT_VOLUME_FIT
        if (!plater->can_scale_to_print_volume())
            throw std::runtime_error("this object cannot be scaled to the build volume (not for cut objects or this bed shape)");
#else
        // The no-op check below follows the ENABLE_ENHANCED_PRINT_VOLUME_FIT version of the app.
        throw std::runtime_error("scale to fit is not available in this build");
#endif

        const Selection&   sel = sw_slicer_canvas()->get_selection();
        const BuildVolume& bv  = plater->build_volume();
        bool               noop = false;
        if (bv.type() == BuildVolume_Type::Rectangle) {
            const BoundingBoxf3 pv       = bv.bounding_volume();
            const Vec3d         pv_size  = pv.size();
            const BoundingBoxf3 bb       = sel.get_bounding_box();
            const Vec3d         box_size = bb.size() + 0.02 * Vec3d::Ones();
            const double        sx       = (box_size.x() != 0.0) ? pv_size.x() / box_size.x() : 0.0;
            const double        sy       = (box_size.y() != 0.0) ? pv_size.y() / box_size.y() : 0.0;
            const double        sz       = (box_size.z() != 0.0) ? pv_size.z() / box_size.z() : 0.0;
            if (sx == 0.0 || sy == 0.0 || sz == 0.0) {
                noop = true; // the app does nothing either
            } else {
                const double s      = std::min(sx, std::min(sy, sz));
                const Vec3d  offset = pv.center() - bb.center();
                noop = s <= 0.0 || s == 1.0 ||
                       (std::abs(s - 1.0) <= 1e-6 && std::abs(offset.x()) <= 1e-3 && std::abs(offset.y()) <= 1e-3 &&
                        std::abs(bb.min.z()) <= 1e-4);
            }
        }
        const ModelInstance*     mi           = plater->model().objects[obj]->instances[inst];
        const Vec3d              scale_before = mi->get_scaling_factor();
        const Vec3d              pos_before   = mi->get_offset();
        const int                plate_before = sw_slicer_instance_plate(obj, inst);
        std::vector<std::string> notes;
        if (noop) {
            notes.push_back("already fits; nothing changed");
        } else {
            {
                sswcp_api::Scope     api_scope;
                Plater::TakeSnapshot snapshot(plater, "API: Scale to fit");
                // The app's own snapshot "Scale To Fit" is suppressed.
                plater->scale_selection_to_fit_print_volume();
                const std::vector<std::string>& api_notes = sswcp_api::notes();
                notes.insert(notes.end(), api_notes.begin(), api_notes.end());
            }
            if (mi->get_scaling_factor() == scale_before && mi->get_offset() == pos_before)
                notes.push_back("already fits; nothing changed (an empty undo step was recorded)");
            const int plate_after = sw_slicer_instance_plate(obj, inst);
            if (plate_after != plate_before)
                notes.push_back("the object is now on plate " + std::to_string(plate_after) +
                                " (the app fits it into the build volume of the first plate)");
        }
        const ModelObject* mo = plater->model().objects[obj];
        m_res_data["object"]   = obj;
        m_res_data["instance"] = inst;
        // Same keys and units as sw_SlicerScale.
        m_res_data["scale_percent"] = sw_slicer_vec3(mo->instances[inst]->get_scaling_factor() * 100.0);
        m_res_data["size_mm"]       = sw_slicer_vec3(mo->instance_bounding_box(static_cast<size_t>(inst)).size());
        m_res_data["notes"]    = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// All plates: name, lock, the objects with at least one instance on the plate, and the slice state.
// Read-only. (PartPlate::get_objects() is not used: it returns all model objects.)
void SSWCP_SliceProject_Instance::sw_SlicerListPlates()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        PartPlateList& plates = plater->get_partplate_list();
        const Model&   model  = plater->model();
        json           list   = json::array();
        for (int i = 0; i < plates.get_plate_count(); ++i) {
            PartPlate* p       = plates.get_plate(i);
            json       objects = json::array();
            for (size_t o = 0; o < model.objects.size(); ++o)
                for (size_t j = 0; j < model.objects[o]->instances.size(); ++j)
                    if (p->contain_instance(static_cast<int>(o), static_cast<int>(j))) {
                        objects.push_back(static_cast<int>(o));
                        break;
                    }
            json e;
            e["plate"]   = i;
            e["name"]    = p->get_plate_name();
            e["locked"]  = p->is_locked();
            e["empty"]   = p->empty();
            e["objects"] = objects;
            e["sliced"]  = p->is_slice_result_valid();
            e["ready"]   = p->is_slice_result_ready_for_print();
            list.push_back(e);
        }
        m_res_data["plates"]        = list;
        m_res_data["current_plate"] = plates.get_curr_plate_index();
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Make a plate the current plate (Slice and the exports work on it), like clicking it. The model does
// not change: no "API: " step (the app's own snapshot "select partplate!" is skipped by undo).
void SSWCP_SliceProject_Instance::sw_SlicerSelectPlate()
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
        sw_slicer_get_index(m_param_data, "plate"); // required here
        const int                n = sw_slicer_resolve_plate(m_param_data);
        std::vector<std::string> notes;
        if (plater->get_partplate_list().get_curr_plate_index() == n)
            notes.push_back("already the current plate");
        else
            sw_slicer_select_plate(n);
        m_res_data["current_plate"] = plater->get_partplate_list().get_curr_plate_index();
        m_res_data["notes"]         = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Duplicate a plate (default: the current plate) with copies of its objects, like "Duplicate Current
// Plate" (Plater::duplicate_plate). The copies are new objects at the end of the model. An empty plate
// gives an empty copy. One undo step "API: Duplicate plate".
void SSWCP_SliceProject_Instance::sw_SlicerDuplicatePlate()
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
        const int n = sw_slicer_resolve_plate(m_param_data);
        // Checked first: PartPlateList::duplicate_plate() does not handle a failed create_plate().
        if (!plater->can_add_plate())
            throw std::runtime_error("cannot add a plate (maximum reached)");
        PartPlateList& plates = plater->get_partplate_list();
        PartPlate*     src    = plates.get_plate(n);
        Model&         model  = plater->model();
        // PartPlateList::duplicate_plate() copies an object once per instance on the plate, each copy
        // with all instances: an object with several instances would end up on the new plate several
        // times (and instances on other plates would be copied too). Refused instead.
        std::string multi;
        for (size_t o = 0; o < model.objects.size(); ++o) {
            const ModelObject* mo = model.objects[o];
            if (mo->instances.size() < 2)
                continue;
            for (size_t j = 0; j < mo->instances.size(); ++j)
                if (src->contain_instance(static_cast<int>(o), static_cast<int>(j))) {
                    multi += (multi.empty() ? "" : ", ") + std::to_string(o) + " '" + mo->name + "'";
                    break;
                }
        }
        if (!multi.empty())
            throw std::runtime_error("plate " + std::to_string(n) + " holds objects with more than one instance (" + multi +
                                     "); the app cannot duplicate such a plate correctly");
        const bool               was_empty = src->empty();
        const size_t             n_before  = model.objects.size();
        int                      idx       = -1;
        std::vector<std::string> notes;
        {
            sswcp_api::Scope     api_scope;
            Plater::TakeSnapshot snapshot(plater, "API: Duplicate plate");
            idx   = plater->duplicate_plate(n);
            notes = sswcp_api::notes();
        }
        if (idx < 0)
            throw std::runtime_error(sw_slicer_join_notes("the plate could not be duplicated", notes));
        json added = json::array();
        for (size_t i = n_before; i < model.objects.size(); ++i)
            added.push_back(static_cast<int>(i));
        if (was_empty)
            notes.push_back("plate " + std::to_string(n) + " was empty; the new plate is empty too");
        sw_slicer_canvas()->set_as_dirty();
        m_res_data["plate"]         = idx;
        m_res_data["plate_count"]   = plates.get_plate_count();
        m_res_data["objects_added"] = added;
        m_res_data["objects"]       = sw_slicer_objects_json();
        m_res_data["notes"]         = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Name and/or lock a plate (default: the current plate), like the plate name dialog and the lock icon
// on the bed, without selecting the plate. An empty name removes the name. One undo step
// "API: Plate settings" (the undo stack stores the plate's name and lock state, PartPlate::save/load);
// no change takes no snapshot.
void SSWCP_SliceProject_Instance::sw_SlicerSetPlate()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        sw_slicer_require_not_loading();
        const int      n          = sw_slicer_resolve_plate(m_param_data);
        PartPlateList& plates     = plater->get_partplate_list();
        PartPlate*     pp         = plates.get_plate(n);
        const bool     has_name   = sw_slicer_has_param(m_param_data, "name");
        const bool     has_locked = sw_slicer_has_param(m_param_data, "locked");
        if (!has_name && !has_locked)
            throw std::runtime_error("specify 'name' and/or 'locked'");
        std::string name;
        if (has_name) {
            if (!m_param_data.at("name").is_string())
                throw std::runtime_error("'name' must be a string");
            name = m_param_data.at("name").get<std::string>();
            for (const unsigned char c : name)
                if (c < 0x20 || c == 0x7f)
                    throw std::runtime_error("the plate name must not contain line breaks or control characters");
            const wxString wname = wxString::FromUTF8(name);
            if (!name.empty() && wname.empty())
                throw std::runtime_error("'name' is not valid UTF-8");
            // Same limit as the plate name dialog (PlateNameEditDialog, SetMaxLength(250)).
            if (wname.length() > 250)
                throw std::runtime_error("the plate name may be at most 250 characters");
        }
        if (has_locked && !m_param_data.at("locked").is_boolean())
            throw std::runtime_error("'locked' must be true or false");
        const bool locked      = has_locked && m_param_data.at("locked").get<bool>();
        const bool change_name = has_name && pp->get_plate_name() != name;
        const bool change_lock = has_locked && pp->is_locked() != locked;

        std::vector<std::string> notes;
        auto reply = [&]() {
            m_res_data["plate"]  = n;
            m_res_data["name"]   = pp->get_plate_name();
            m_res_data["locked"] = pp->is_locked();
            m_res_data["notes"]  = notes;
            send_to_js();
            finish_job();
        };
        if (!change_name && !change_lock) {
            notes.push_back("plate settings unchanged; nothing changed");
            reply();
            return;
        }
        {
            sswcp_api::Scope     api_scope;
            Plater::TakeSnapshot snapshot(plater, "API: Plate settings");
            if (change_name) {
                // Also regenerates the name texture on the bed and passes the name to the plate's print.
                pp->set_plate_name(name);
                // The plate item of the object list ("Plate n (name)"), like update_name_in_model().
                if (ObjectDataViewModel* list_model = wxGetApp().obj_list()->GetModel()) {
                    list_model->SetCurSelectedPlateFullName(n, name);
                    const wxDataViewItem item = list_model->GetItemByPlateId(n);
                    if (item.IsOk())
                        list_model->ItemChanged(item);
                }
            }
            if (change_lock && plates.lock_plate(n, locked) != 0)
                throw std::runtime_error("plate " + std::to_string(n) + " could not be locked or unlocked");
            plater->set_plater_dirty(true);
            plater->update();
            const std::vector<std::string>& api_notes = sswcp_api::notes();
            notes.insert(notes.end(), api_notes.begin(), api_notes.end());
        }
        sw_slicer_canvas()->set_as_dirty();
        reply();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- Preview controls and canvas screenshot ---------------------------------------
// View state only: none of these commands takes an undo snapshot or marks the project dirty.

// The preview canvas. Throws if it is not available (yet).
static GLCanvas3D* sw_slicer_preview_canvas()
{
    GLCanvas3D* canvas = wxGetApp().plater()->get_preview_canvas3D();
    if (canvas == nullptr)
        throw std::runtime_error("no preview canvas available");
    return canvas;
}

// The preview must hold a loaded G-code result.
static void sw_slicer_require_preview_data()
{
    if (!sw_slicer_preview_canvas()->get_gcode_viewer().has_data())
        throw std::runtime_error("no preview loaded (slice the plate and open the Preview first)");
}

template<class T> struct SwSlicerNamed
{
    const char* name;
    T           value;
};

// View types in the order of the legend's combo box.
static const SwSlicerNamed<GCodeViewer::EViewType> sw_slicer_view_type_names[] = {
    { "feature_type", GCodeViewer::EViewType::FeatureType },
    { "filament", GCodeViewer::EViewType::ColorPrint },
    { "speed", GCodeViewer::EViewType::Feedrate },
    { "layer_height", GCodeViewer::EViewType::Height },
    { "line_width", GCodeViewer::EViewType::Width },
    { "flow", GCodeViewer::EViewType::VolumetricRate },
    { "layer_time", GCodeViewer::EViewType::LayerTime },
    { "layer_time_log", GCodeViewer::EViewType::LayerTimeLog },
    { "fan_speed", GCodeViewer::EViewType::FanSpeed },
    { "temperature", GCodeViewer::EViewType::Temperature },
};

// Extrusion roles shown as line types in the legend.
static const SwSlicerNamed<ExtrusionRole> sw_slicer_role_names[] = {
    { "inner_wall", erPerimeter },
    { "outer_wall", erExternalPerimeter },
    { "overhang_wall", erOverhangPerimeter },
    { "sparse_infill", erInternalInfill },
    { "internal_solid_infill", erSolidInfill },
    { "top_surface", erTopSolidInfill },
    { "bottom_surface", erBottomSurface },
    { "ironing", erIroning },
    { "bridge", erBridgeInfill },
    { "internal_bridge", erInternalBridgeInfill },
    { "gap_infill", erGapFill },
    { "skirt", erSkirt },
    { "brim", erBrim },
    { "support", erSupportMaterial },
    { "support_interface", erSupportMaterialInterface },
    { "support_transition", erSupportTransition },
    { "prime_tower", erWipeTower },
    { "custom", erCustom },
};

// Preview options (bits of Preview::OptionType). Tool changes, color changes, pause prints and
// custom G-code are not offered: the legend does not show them either.
static const SwSlicerNamed<Preview::OptionType> sw_slicer_option_names[] = {
    { "travel", Preview::OptionType::Travel },
    { "retract", Preview::OptionType::Retractions },
    { "unretract", Preview::OptionType::Unretractions },
    { "wipe", Preview::OptionType::Wipe },
    { "seams", Preview::OptionType::Seams },
    { "shells", Preview::OptionType::Shells },
    { "tool_marker", Preview::OptionType::ToolMarker },
    { "legend", Preview::OptionType::Legend },
};

template<class T, size_t N> static std::string sw_slicer_named_list(const SwSlicerNamed<T> (&table)[N])
{
    std::string out;
    for (const SwSlicerNamed<T>& e : table)
        out += (out.empty() ? "" : "|") + std::string(e.name);
    return out;
}

// Value for a name; throws "unknown <what> '<name>' (expected a|b|...)".
template<class T, size_t N>
static T sw_slicer_named_value(const SwSlicerNamed<T> (&table)[N], const std::string& name, const char* what)
{
    for (const SwSlicerNamed<T>& e : table)
        if (name == e.name)
            return e.value;
    throw std::runtime_error("unknown " + std::string(what) + " '" + name + "' (expected " + sw_slicer_named_list(table) + ")");
}

// Name for a value, nullptr if the value has no name.
template<class T, size_t N> static const char* sw_slicer_value_name(const SwSlicerNamed<T> (&table)[N], T value)
{
    for (const SwSlicerNamed<T>& e : table)
        if (e.value == value)
            return e.name;
    return nullptr;
}

static unsigned int sw_slicer_option_bit(Preview::OptionType option) { return 1u << static_cast<unsigned int>(option); }

// "3D", "Preview" or "Assemble".
static std::string sw_slicer_canvas_view_name(GLCanvas3D* canvas)
{
    if (canvas == nullptr)
        return "";
    switch (canvas->get_canvas_type()) {
    case GLCanvas3D::ECanvasType::CanvasPreview: return "Preview";
    case GLCanvas3D::ECanvasType::CanvasAssembleView: return "Assemble";
    default: return "3D";
    }
}

// 0-based layer tick for params[layer_key] (1-based integer) or params[z_key] (closest layer,
// in mm); -1 if neither is given. Out-of-range layers are clamped with a note.
static int sw_slicer_layer_tick(const json& params, const char* layer_key, const char* z_key, const std::vector<double>& zs,
                                int max_tick, std::vector<std::string>& notes)
{
    const bool has_layer = sw_slicer_has_param(params, layer_key);
    const bool has_z     = sw_slicer_has_param(params, z_key);
    if (has_layer && has_z)
        throw std::runtime_error("pass either '" + std::string(layer_key) + "' or '" + std::string(z_key) + "', not both");
    if (!has_layer && !has_z)
        return -1;
    if (has_layer) {
        const json& j = params.at(layer_key);
        if (!j.is_number_integer())
            throw std::runtime_error("'" + std::string(layer_key) + "' must be an integer (1-based layer)");
        const long long n    = j.get<long long>();
        const long long tick = std::max(0LL, std::min(n - 1, static_cast<long long>(max_tick)));
        if (tick != n - 1)
            notes.push_back("layer " + std::to_string(n) + " clamped to " + std::to_string(tick + 1));
        return static_cast<int>(tick);
    }
    const json& j = params.at(z_key);
    if (!j.is_number() || !std::isfinite(j.get<double>()))
        throw std::runtime_error("'" + std::string(z_key) + "' must be a finite number (mm)");
    const double z    = j.get<double>();
    int          tick = 0;
    double       best = std::numeric_limits<double>::max();
    for (size_t i = 0; i < zs.size(); ++i) {
        const double d = std::abs(zs[i] - z);
        if (d < best) {
            best = d;
            tick = static_cast<int>(i);
        }
    }
    return std::max(0, std::min(tick, max_tick));
}

static json sw_slicer_layer_z(const std::vector<double>& zs, int tick)
{
    return (tick >= 0 && tick < static_cast<int>(zs.size())) ? json(zs[tick]) : json(nullptr);
}

static json sw_slicer_camera_json()
{
    const Camera& camera = wxGetApp().plater()->get_camera();
    json          j;
    j["type"] = camera.get_type_as_string();
    j["zoom"] = camera.get_zoom();
    return j;
}

// The current state of the preview (shared reply part of all preview commands).
static json sw_slicer_preview_state_json()
{
    Plater*      plater = wxGetApp().plater();
    GLCanvas3D*  canvas = sw_slicer_preview_canvas();
    GCodeViewer& gv     = canvas->get_gcode_viewer();
    const bool   loaded = gv.has_data();
    PartPlate*   plate  = plater->get_partplate_list().get_curr_plate();

    json j;
    j["view"]           = sw_slicer_canvas_view_name(plater->get_current_canvas3D());
    j["preview_loaded"] = loaded;
    j["sliced"]         = plate != nullptr && plate->is_slice_result_valid();
    j["plate"]          = plater->get_partplate_list().get_curr_plate_index();

    const char* view_type = sw_slicer_value_name(sw_slicer_view_type_names, gv.get_view_type());
    j["view_type"]        = view_type != nullptr ? view_type : "other";
    json view_types       = json::array();
    for (GCodeViewer::EViewType t : gv.get_selectable_view_types())
        if (const char* name = sw_slicer_value_name(sw_slicer_view_type_names, t))
            view_types.push_back(name);
    j["view_types"] = view_types;

    if (loaded) {
        const unsigned int role_flags = gv.get_toolpath_role_visibility_flags();
        json               roles      = json::array();
        for (ExtrusionRole role : gv.get_roles())
            if (const char* name = sw_slicer_value_name(sw_slicer_role_names, role))
                roles.push_back({ { "role", name }, { "visible", ((role_flags >> static_cast<unsigned int>(role)) & 1u) != 0 } });
        j["roles"] = roles;

        json filaments = json::array();
        for (unsigned char id : gv.get_extruder_ids())
            filaments.push_back({ { "filament", static_cast<int>(id) + 1 }, { "visible", gv.is_filament_visible(id) } });
        j["filaments"] = filaments;
    } else {
        j["roles"]     = nullptr;
        j["filaments"] = nullptr;
    }

    const unsigned int option_flags = gv.get_options_visibility_flags();
    json               options      = json::object();
    for (const SwSlicerNamed<Preview::OptionType>& e : sw_slicer_option_names)
        options[e.name] = (option_flags & sw_slicer_option_bit(e.value)) != 0;
    j["options"] = options;

    if (loaded) {
        IMSlider*                  ls = gv.get_layers_slider();
        const std::vector<double>& zs = canvas->get_gcode_layers_zs();
        j["layers"] = { { "count", ls->GetMaxValue() + 1 },
                        { "bottom", ls->GetLowerValue() + 1 },
                        { "top", ls->GetHigherValue() + 1 },
                        { "z_bottom", sw_slicer_layer_z(zs, ls->GetLowerValue()) },
                        { "z_top", sw_slicer_layer_z(zs, ls->GetHigherValue()) },
                        { "one_layer", ls->is_one_layer() } };

        IMSlider*                           ms = gv.get_moves_slider();
        const GCodeViewer::SequentialView& sv = gv.get_sequential_view();
        const size_t                        last = sv.current.last;
        j["moves"] = { { "count", ms->GetMaxValue() + 1 },
                       { "current", ms->GetHigherValue() + 1 },
                       { "gcode_line", last < sv.gcode_ids.size() ? json(sv.gcode_ids[last]) : json(nullptr) },
                       { "position", { sv.current_position.x(), sv.current_position.y(), sv.current_position.z() } } };
    } else {
        j["layers"] = nullptr;
        j["moves"]  = nullptr;
    }

    j["camera"] = sw_slicer_camera_json();
    return j;
}

// Read-only state of the preview: view type, line types, filaments, options, sliders, camera.
void SSWCP_SliceProject_Instance::sw_SlicerGetPreviewState()
{
    try {
        m_res_data = sw_slicer_preview_state_json();
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Exactly true or false, otherwise throws "<what> '<name>' must be true or false".
static bool sw_slicer_bool_value(const json& j, const std::string& what, const std::string& name)
{
    if (!j.is_boolean())
        throw std::runtime_error(what + " '" + name + "' must be true or false");
    return j.get<bool>();
}

// Preview settings like the legend: view type, line types, filaments and options. Everything is
// validated before anything is changed.
void SSWCP_SliceProject_Instance::sw_SlicerSetPreview()
{
    try {
        const json& p              = m_param_data;
        const bool  has_view_type  = sw_slicer_has_param(p, "view_type");
        const bool  has_roles      = sw_slicer_has_param(p, "roles");
        const bool  has_only_roles = sw_slicer_has_param(p, "only_roles");
        const bool  has_filaments  = sw_slicer_has_param(p, "filaments");
        const bool  has_options    = sw_slicer_has_param(p, "options");
        if (!has_view_type && !has_roles && !has_only_roles && !has_filaments && !has_options)
            throw std::runtime_error("specify at least one of view_type, roles, only_roles, filaments, options");
        if (has_roles && has_only_roles)
            throw std::runtime_error("pass either 'roles' or 'only_roles', not both");
        sw_slicer_require_preview_data();
        sw_slicer_require_not_slicing();

        GLCanvas3D*  canvas = sw_slicer_preview_canvas();
        GCodeViewer& gv     = canvas->get_gcode_viewer();

        // --- Validate everything first. ---
        GCodeViewer::EViewType view_type = gv.get_view_type();
        if (has_view_type) {
            if (!p.at("view_type").is_string())
                throw std::runtime_error("'view_type' must be a string (expected " + sw_slicer_named_list(sw_slicer_view_type_names) + ")");
            const std::string name = p.at("view_type").get<std::string>();
            view_type              = sw_slicer_named_value(sw_slicer_view_type_names, name, "view_type");
            const std::vector<GCodeViewer::EViewType> selectable = gv.get_selectable_view_types();
            if (std::find(selectable.begin(), selectable.end(), view_type) == selectable.end())
                throw std::runtime_error("view_type '" + name + "' is not available");
        }

        std::vector<std::pair<ExtrusionRole, bool>> role_changes;
        std::vector<std::string>                    role_names;
        if (has_roles) {
            const json& j = p.at("roles");
            if (!j.is_object() || j.empty())
                throw std::runtime_error("'roles' must be a non-empty object {line type: true|false}");
            for (auto it = j.begin(); it != j.end(); ++it) {
                const ExtrusionRole role = sw_slicer_named_value(sw_slicer_role_names, it.key(), "role");
                role_changes.emplace_back(role, sw_slicer_bool_value(it.value(), "role", it.key()));
                role_names.push_back(it.key());
            }
        }
        if (has_only_roles) {
            const json& j = p.at("only_roles");
            if (!j.is_array() || j.empty())
                throw std::runtime_error("'only_roles' must be a non-empty list of line types");
            std::set<std::string> seen;
            for (const json& e : j) {
                if (!e.is_string())
                    throw std::runtime_error("'only_roles' must contain line type names (expected " + sw_slicer_named_list(sw_slicer_role_names) + ")");
                const std::string name = e.get<std::string>();
                const ExtrusionRole role = sw_slicer_named_value(sw_slicer_role_names, name, "role");
                if (!seen.insert(name).second)
                    throw std::runtime_error("role '" + name + "' is listed twice in 'only_roles'");
                role_changes.emplace_back(role, true);
                role_names.push_back(name);
            }
        }

        std::vector<std::pair<int, bool>> filament_changes; // 1-based filament, visible
        if (has_filaments) {
            const json& j = p.at("filaments");
            const int   n = wxGetApp().filaments_cnt();
            if (!j.is_object() || j.empty())
                throw std::runtime_error("'filaments' must be a non-empty object {\"<filament 1.." + std::to_string(n) + ">\": true|false}");
            for (auto it = j.begin(); it != j.end(); ++it) {
                const std::string& key   = it.key();
                const bool         digits = !key.empty() && key.size() <= 4 &&
                                    std::all_of(key.begin(), key.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
                const int          f      = digits ? std::stoi(key) : 0;
                if (f < 1 || f > n)
                    throw std::runtime_error("filament '" + key + "' does not exist (1.." + std::to_string(n) + ")");
                filament_changes.emplace_back(f, sw_slicer_bool_value(it.value(), "filament", key));
            }
        }

        std::vector<std::pair<Preview::OptionType, bool>> option_changes;
        if (has_options) {
            const json& j = p.at("options");
            if (!j.is_object() || j.empty())
                throw std::runtime_error("'options' must be a non-empty object {option: true|false} (options: " + sw_slicer_named_list(sw_slicer_option_names) + ")");
            for (auto it = j.begin(); it != j.end(); ++it) {
                const Preview::OptionType option = sw_slicer_named_value(sw_slicer_option_names, it.key(), "option");
                option_changes.emplace_back(option, sw_slicer_bool_value(it.value(), "option", it.key()));
            }
        }

        // --- Apply. ---
        json                     changed = json::array();
        std::vector<std::string> notes;

        // 1. View type first: choosing a type in the app makes all line types (and, for the
        //    filament view, all filaments) visible again; roles/filaments are applied after it.
        if (has_view_type && view_type != gv.get_view_type()) {
            gv.select_view_type(view_type);
            changed.push_back("view_type");
            if (view_type == GCodeViewer::EViewType::FeatureType && !has_roles && !has_only_roles)
                notes.push_back("all line types are visible again (as when choosing the type in the app)");
            else if (view_type == GCodeViewer::EViewType::ColorPrint && !has_roles && !has_only_roles && !has_filaments)
                notes.push_back("all line types and filaments are visible again (as when choosing the type in the app)");
        }
        const GCodeViewer::EViewType final_type = gv.get_view_type();

        // 2. Line types.
        if (has_roles || has_only_roles) {
            const unsigned int old_flags = gv.get_toolpath_role_visibility_flags();
            unsigned int       flags     = old_flags;
            if (has_only_roles)
                for (const SwSlicerNamed<ExtrusionRole>& e : sw_slicer_role_names)
                    flags &= ~(1u << static_cast<unsigned int>(e.value));
            for (const std::pair<ExtrusionRole, bool>& rc : role_changes) {
                const unsigned int bit = 1u << static_cast<unsigned int>(rc.first);
                flags                  = rc.second ? (flags | bit) : (flags & ~bit);
            }
            const std::vector<ExtrusionRole>& present = gv.get_roles();
            for (size_t i = 0; i < role_changes.size(); ++i)
                if (std::find(present.begin(), present.end(), role_changes[i].first) == present.end())
                    notes.push_back("line type '" + role_names[i] + "' does not occur in this slice");
            if (flags != old_flags) {
                gv.set_toolpath_role_visibility_flags(flags);
                changed.push_back("roles");
            }
            if (final_type != GCodeViewer::EViewType::FeatureType)
                notes.push_back("line type visibility applies in every view; the legend shows it only in feature_type");
        }

        // 3. Filaments.
        if (has_filaments) {
            bool                              any = false;
            const std::vector<unsigned char>& used = gv.get_extruder_ids();
            for (const std::pair<int, bool>& fc : filament_changes) {
                const size_t id     = static_cast<size_t>(fc.first - 1);
                const bool   before = gv.is_filament_visible(id);
                if (before != fc.second) {
                    gv.set_filament_visible(id, fc.second);
                    any = any || gv.is_filament_visible(id) != before;
                }
                if (std::find(used.begin(), used.end(), static_cast<unsigned char>(id)) == used.end())
                    notes.push_back("filament " + std::to_string(fc.first) + " is not used in this slice");
            }
            if (any)
                changed.push_back("filaments");
            if (final_type != GCodeViewer::EViewType::ColorPrint)
                notes.push_back("filament visibility applies in the filament view");
        }

        // 4. Options (only the offered bits change).
        bool travel_changed = false;
        if (has_options) {
            const unsigned int old_flags = gv.get_options_visibility_flags();
            unsigned int       flags     = old_flags;
            for (const std::pair<Preview::OptionType, bool>& oc : option_changes) {
                const unsigned int bit = sw_slicer_option_bit(oc.first);
                flags                  = oc.second ? (flags | bit) : (flags & ~bit);
                if (oc.first == Preview::OptionType::Legend && !oc.second)
                    notes.push_back("the legend stays hidden until you show it again with options.legend:true");
            }
            if (flags != old_flags) {
                canvas->set_gcode_options_visibility_from_flags(flags);
                travel_changed = ((flags ^ old_flags) & sw_slicer_option_bit(Preview::OptionType::Travel)) != 0;
                changed.push_back("options");
            }
        }

        if (!changed.empty()) {
            gv.refresh_after_visibility_change(travel_changed);
            canvas->set_as_dirty();
            canvas->request_extra_frame();
        } else {
            notes.push_back("preview settings unchanged; nothing changed");
        }

        m_res_data            = sw_slicer_preview_state_json();
        m_res_data["changed"] = changed;
        m_res_data["notes"]   = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Layer range of the preview (the vertical slider): bottom and top (1-based) or z in mm, and the
// one-layer mode. Applied right away so a following command sees the new range.
void SSWCP_SliceProject_Instance::sw_SlicerSetLayerRange()
{
    try {
        const json& p          = m_param_data;
        const bool  has_top    = sw_slicer_has_param(p, "top") || sw_slicer_has_param(p, "z_top");
        const bool  has_bottom = sw_slicer_has_param(p, "bottom") || sw_slicer_has_param(p, "z_bottom");
        const bool  has_one    = sw_slicer_has_param(p, "one_layer");
        if (!has_top && !has_bottom && !has_one)
            throw std::runtime_error("specify at least one of top, z_top, bottom, z_bottom, one_layer");
        sw_slicer_require_preview_data();
        sw_slicer_require_not_slicing();

        GLCanvas3D*  canvas = sw_slicer_preview_canvas();
        GCodeViewer& gv     = canvas->get_gcode_viewer();
        IMSlider*    s      = gv.get_layers_slider();
        if (s == nullptr)
            throw std::runtime_error("no layer slider available");
        const int                  max_tick = s->GetMaxValue();
        const std::vector<double>& zs       = canvas->get_gcode_layers_zs();

        std::vector<std::string> notes;
        int top_tick    = sw_slicer_layer_tick(p, "top", "z_top", zs, max_tick, notes);
        int bottom_tick = sw_slicer_layer_tick(p, "bottom", "z_bottom", zs, max_tick, notes);
        if (has_one && !p.at("one_layer").is_boolean())
            throw std::runtime_error("'one_layer' must be true or false");
        const bool cur_one = s->is_one_layer();
        const bool one     = has_one ? p.at("one_layer").get<bool>() : cur_one;
        if (one && has_bottom)
            throw std::runtime_error("one_layer shows a single layer; pass only 'top' or 'z_top'");
        // Leaving the one-layer mode shows the whole range again, like the slider's button;
        // a given top or bottom still applies.
        const bool leaving_one = cur_one && !one;
        if (top_tick < 0)
            top_tick = leaving_one ? max_tick : s->GetHigherValue();
        if (bottom_tick < 0)
            bottom_tick = leaving_one ? s->GetMinValue() : s->GetLowerValue();
        if (!one && bottom_tick > top_tick)
            throw std::runtime_error("bottom must not be above top");

        auto reply = [&]() {
            m_res_data["bottom"]      = s->GetLowerValue() + 1;
            m_res_data["top"]         = s->GetHigherValue() + 1;
            m_res_data["layer_count"] = s->GetMaxValue() + 1;
            m_res_data["z_bottom"]    = sw_slicer_layer_z(zs, s->GetLowerValue());
            m_res_data["z_top"]       = sw_slicer_layer_z(zs, s->GetHigherValue());
            m_res_data["one_layer"]   = s->is_one_layer();
            m_res_data["notes"]       = notes;
            send_to_js();
            finish_job();
        };

        if (one == cur_one && top_tick == s->GetHigherValue() && (one || bottom_tick == s->GetLowerValue())) {
            notes.push_back("layer range unchanged; nothing changed");
            reply();
            return;
        }

        if (one != cur_one && !s->switch_one_layer_mode())
            throw std::runtime_error("the layer slider is busy (close the custom G-code window in the app first)");
        if (one)
            s->SetHigherValue(top_tick); // the slider keeps bottom = top in one-layer mode
        else
            s->SetSelectionSpan(bottom_tick, top_tick);
        canvas->apply_gcode_slider_changes();
        canvas->set_as_dirty();
        canvas->request_extra_frame();
        reply();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Horizontal slider of the preview: the move within the top visible layer.
void SSWCP_SliceProject_Instance::sw_SlicerSetMove()
{
    try {
        const json& p        = m_param_data;
        const bool  has_move = sw_slicer_has_param(p, "move");
        const bool  has_pos  = sw_slicer_has_param(p, "position");
        if (has_move == has_pos)
            throw std::runtime_error("specify either 'move' (1-based) or position:'first'|'last'");
        long long   move = 0;
        std::string position;
        if (has_move) {
            const json& j = p.at("move");
            if (!j.is_number_integer() || j.get<long long>() < 1)
                throw std::runtime_error("'move' must be an integer >= 1 (1-based)");
            move = j.get<long long>();
        } else {
            const json& j = p.at("position");
            position      = j.is_string() ? j.get<std::string>() : std::string();
            if (position != "first" && position != "last")
                throw std::runtime_error("position must be 'first' or 'last'");
        }
        sw_slicer_require_preview_data();
        sw_slicer_require_not_slicing();

        GLCanvas3D*  canvas = sw_slicer_preview_canvas();
        GCodeViewer& gv     = canvas->get_gcode_viewer();
        IMSlider*    ms     = gv.get_moves_slider();
        if (ms == nullptr)
            throw std::runtime_error("no move slider available");
        const int max_tick = ms->GetMaxValue();

        std::vector<std::string> notes;
        int                      idx = 0;
        if (has_move) {
            idx = static_cast<int>(std::min(move - 1, static_cast<long long>(max_tick)));
            if (idx != move - 1)
                notes.push_back("move " + std::to_string(move) + " clamped to " + std::to_string(max_tick + 1));
        } else {
            idx = position == "first" ? ms->GetMinValue() : max_tick;
        }

        if (idx == ms->GetHigherValue()) {
            notes.push_back("move unchanged; nothing changed");
        } else {
            ms->SetHigherValue(idx);
            // Updates the sequential view and the tool marker right away.
            canvas->apply_gcode_slider_changes();
            canvas->set_as_dirty();
            canvas->request_extra_frame();
            if (ms->GetHigherValue() != idx)
                notes.push_back("moved to the nearest visible move");
        }

        const GCodeViewer::SequentialView& sv   = gv.get_sequential_view();
        const size_t                        last = sv.current.last;
        m_res_data["move"]       = ms->GetHigherValue() + 1;
        m_res_data["move_count"] = ms->GetMaxValue() + 1;
        m_res_data["gcode_line"] = last < sv.gcode_ids.size() ? json(sv.gcode_ids[last]) : json(nullptr);
        m_res_data["position"]   = { sv.current_position.x(), sv.current_position.y(), sv.current_position.z() };
        m_res_data["notes"]      = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Camera presets like the View menu, and zoom. The camera is shared by the 3D view and the Preview.
void SSWCP_SliceProject_Instance::sw_SlicerSetCamera()
{
    try {
        static const char* const presets[] = { "default", "iso", "top", "bottom", "front", "rear", "left", "right", "topfront" };
        static const char* const zooms[]   = { "fit", "bed", "plate", "objects", "selection", "gcode" };
        auto                     join      = [](const char* const* b, const char* const* e) {
            std::string out;
            for (; b != e; ++b)
                out += (out.empty() ? "" : "|") + std::string(*b);
            return out;
        };
        auto contains = [](const char* const* b, const char* const* e, const std::string& v) {
            return std::any_of(b, e, [&v](const char* s) { return v == s; });
        };

        const json& p          = m_param_data;
        const bool  has_preset = sw_slicer_has_param(p, "preset");
        const bool  has_zoom   = sw_slicer_has_param(p, "zoom");
        if (!has_preset && !has_zoom)
            throw std::runtime_error("specify at least one of preset, zoom");
        std::string preset, zoom;
        if (has_preset) {
            preset = p.at("preset").is_string() ? p.at("preset").get<std::string>() : p.at("preset").dump();
            if (!p.at("preset").is_string() || !contains(std::begin(presets), std::end(presets), preset))
                throw std::runtime_error("unknown preset '" + preset + "' (expected " + join(std::begin(presets), std::end(presets)) + ")");
        }
        if (has_zoom) {
            zoom = p.at("zoom").is_string() ? p.at("zoom").get<std::string>() : p.at("zoom").dump();
            if (!p.at("zoom").is_string() || !contains(std::begin(zooms), std::end(zooms), zoom))
                throw std::runtime_error("unknown zoom '" + zoom + "' (expected " + join(std::begin(zooms), std::end(zooms)) + ")");
        }

        Plater*     plater = wxGetApp().plater();
        GLCanvas3D* c      = plater->get_current_canvas3D();
        if (c == nullptr || c->get_wxglcanvas() == nullptr || !c->get_wxglcanvas()->IsShownOnScreen())
            throw std::runtime_error("the 3D view is not shown (select the Prepare or Preview tab first)");
        const bool in_preview_with_data = c->get_canvas_type() == GLCanvas3D::ECanvasType::CanvasPreview &&
                                          c->get_gcode_viewer().has_data();
        const bool has_objects = !plater->model().objects.empty();

        std::vector<std::string> notes;
        std::string              zoom_target = zoom;
        if (zoom == "fit") {
            zoom_target = in_preview_with_data ? "gcode" : (has_objects ? "objects" : "plate");
            notes.push_back("fit zoomed to '" + zoom_target + "'");
        }
        if (zoom_target == "selection" && c->get_selection().is_empty())
            throw std::runtime_error("nothing is selected");
        if (zoom_target == "gcode" && !in_preview_with_data)
            throw std::runtime_error("zoom 'gcode' needs the Preview with a sliced plate");
        if (zoom_target == "objects" && !has_objects)
            throw std::runtime_error("no objects to zoom to");

        Camera&                 camera      = plater->get_camera();
        const Camera::EType     type_before = camera.get_type();
        const double            zoom_before = camera.get_zoom();
        const Transform3d       view_before = camera.get_view_matrix();

        if (preset == "default") {
            // Like View > Default View.
            plater->select_view("plate");
            c->zoom_to_bed();
        } else if (!preset.empty()) {
            plater->select_view(preset);
        }

        if (zoom_target == "bed")
            c->zoom_to_bed();
        else if (zoom_target == "plate")
            c->zoom_to_plate(REQUIRES_ZOOM_TO_CUR_PLATE);
        else if (zoom_target == "objects")
            c->zoom_to_volumes();
        else if (zoom_target == "selection")
            c->zoom_to_selection();
        else if (zoom_target == "gcode")
            c->zoom_to_gcode();

        c->set_as_dirty();
        c->request_extra_frame();

        const double matrix_diff = (camera.get_view_matrix().matrix() - view_before.matrix()).cwiseAbs().maxCoeff();
        if (camera.get_type() != type_before)
            notes.push_back("projection switched to " + camera.get_type_as_string() +
                            " (auto perspective is on in the app preferences)");
        else if (matrix_diff < 1e-9 && std::abs(camera.get_zoom() - zoom_before) < 1e-9)
            notes.push_back("camera unchanged; nothing changed");

        m_res_data["view"] = sw_slicer_canvas_view_name(c);
        if (has_preset)
            m_res_data["preset"] = preset;
        if (has_zoom)
            m_res_data["zoom"] = zoom;
        m_res_data["camera"] = sw_slicer_camera_json();
        m_res_data["notes"]  = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// PNG of the visible canvas (3D view, Preview or assembly view), written to 'path'. The reply holds
// only metadata, never image data.
void SSWCP_SliceProject_Instance::sw_SlicerScreenshot()
{
    try {
        const json&                   p      = m_param_data;
        const boost::filesystem::path target = sw_slicer_output_path(p, "path", { ".png" });
        sw_slicer_require_overwrite_allowed(target, p);
        int max_size = 1280;
        if (sw_slicer_has_param(p, "max_size")) {
            const json& j = p.at("max_size");
            if (!j.is_number_integer() || j.get<long long>() < 64 || j.get<long long>() > 4096)
                throw std::runtime_error("max_size must be an integer between 64 and 4096");
            max_size = j.get<int>();
        }
        if (wxGetApp().mainframe != nullptr && wxGetApp().mainframe->IsIconized())
            throw std::runtime_error("the app window is minimized; restore it first");

        GLCanvas3D* c = wxGetApp().plater()->get_current_canvas3D();
        if (c == nullptr || c->get_wxglcanvas() == nullptr || !c->get_wxglcanvas()->IsShownOnScreen())
            throw std::runtime_error("the 3D view is not shown (select the Prepare or Preview tab first)");
        std::vector<unsigned char> rgba;
        unsigned int               w = 0, h = 0;
        if (!c->capture_frame(rgba, w, h))
            throw std::runtime_error("the 3D view could not be rendered (is the app window visible?)");

        // Convert on the GUI thread; no GL needed from here on.
        wxImage        img(static_cast<int>(w), static_cast<int>(h), false);
        unsigned char* dst     = img.GetData();
        bool           uniform = true;
        for (unsigned int y = 0; y < h; ++y) {
            // OpenGL rows start at the bottom.
            const unsigned char* src = rgba.data() + static_cast<size_t>(h - 1 - y) * w * 4;
            for (unsigned int x = 0; x < w; ++x, src += 4, dst += 3) {
                dst[0]  = src[0];
                dst[1]  = src[1];
                dst[2]  = src[2];
                uniform = uniform && src[0] == rgba[0] && src[1] == rgba[1] && src[2] == rgba[2];
            }
        }
        rgba.clear();
        rgba.shrink_to_fit();
        const unsigned int longest = std::max(w, h);
        if (longest > static_cast<unsigned int>(max_size)) {
            const double f = double(max_size) / double(longest);
            img.Rescale(std::max(1, int(std::lround(w * f))), std::max(1, int(std::lround(h * f))), wxIMAGE_QUALITY_HIGH);
        }

        boost::system::error_code ec;
        const bool                existed = boost::filesystem::exists(target, ec);
        sw_slicer_write_via_temp(target, [&img](const boost::filesystem::path& tmp) {
            wxLogNull no_log_dialog; // a write error is reported as an API error, not a log dialog
            return img.SaveFile(from_path(tmp), wxBITMAP_TYPE_PNG);
        });

        std::vector<std::string> notes;
        if (uniform)
            notes.push_back("the image is uniform; the window may be covered or not drawn yet");
        if (existed)
            notes.push_back("existing file replaced");

        const uintmax_t bytes = boost::filesystem::file_size(target, ec);
        m_res_data["path"]          = sw_slicer_path_to_utf8(target);
        m_res_data["width"]         = img.GetWidth();
        m_res_data["height"]        = img.GetHeight();
        m_res_data["canvas_width"]  = w;
        m_res_data["canvas_height"] = h;
        m_res_data["bytes"]         = ec ? json(nullptr) : json(static_cast<unsigned long long>(bytes));
        m_res_data["view"]          = sw_slicer_canvas_view_name(c);
        m_res_data["notes"]         = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- Object clipboard, visibility, instances, plates and model export ---------------------
// Copy/paste through the app's internal clipboard (the selection's clipboard, not the clipboard of
// the operating system), hide/show in the assembly view, the number of instances, instance to
// object, moving to another plate, and exporting STL / generic 3MF without a file dialog.

// 'objects' parameter: a non-empty list of existing object indices without duplicates.
static std::vector<int> sw_slicer_parse_objects(const json& params, const char* key)
{
    const std::string k = key;
    const int         n = static_cast<int>(wxGetApp().plater()->model().objects.size());
    if (!sw_slicer_has_param(params, key) || !params.at(key).is_array() || params.at(key).empty())
        throw std::runtime_error("'" + k + "' must be a non-empty list of object indices");
    std::vector<int> objs;
    for (const json& j : params.at(key)) {
        if (!j.is_number_integer() || j.get<long long>() < 0 || j.get<long long>() >= n)
            throw std::runtime_error("object index " + j.dump() + " in '" + k + "' does not exist (there are " +
                                     std::to_string(n) + " objects, 0-based)");
        const int obj = j.get<int>();
        if (std::find(objs.begin(), objs.end(), obj) != objs.end())
            throw std::runtime_error("object index " + std::to_string(obj) + " appears twice in '" + k + "'");
        objs.push_back(obj);
    }
    return objs;
}

// After changing the canvas selection: the object list and the gizmos follow it (as in sw_slicer_select).
static void sw_slicer_selection_changed(GLCanvas3D* canvas)
{
    wxGetApp().obj_list()->update_selections();
    canvas->update_gizmos_on_off_state();
    canvas->set_as_dirty();
}

// Select the instances (object, instance) in the 3D canvas and the object list.
static void sw_slicer_select_instances(const std::vector<std::pair<int, int>>& items)
{
    GLCanvas3D* canvas = sw_slicer_canvas();
    Selection&  s      = canvas->get_selection();
    s.clear();
    for (size_t i = 0; i < items.size(); ++i)
        s.add_instance(static_cast<unsigned int>(items[i].first), static_cast<unsigned int>(items[i].second), i == 0);
    sw_slicer_selection_changed(canvas);
}

// Select whole objects (all instances) in the 3D canvas and the object list.
static void sw_slicer_select_whole_objects(const std::vector<int>& objs)
{
    GLCanvas3D* canvas = sw_slicer_canvas();
    Selection&  s      = canvas->get_selection();
    s.clear();
    for (size_t i = 0; i < objs.size(); ++i)
        s.add_object(static_cast<unsigned int>(objs[i]), i == 0);
    sw_slicer_selection_changed(canvas);
}

// ObjectIDs of the volumes of an object, to find the volumes a paste added.
static std::set<size_t> sw_slicer_volume_ids(const ModelObject* mo)
{
    std::set<size_t> ids;
    for (const ModelVolume* mv : mo->volumes)
        ids.insert(mv->id().id);
    return ids;
}

// The objects of a selection (the wipe tower, object_idx >= 1000, is skipped).
static std::vector<int> sw_slicer_selected_objects(const Selection& s)
{
    std::vector<int> objs;
    for (const auto& content : s.get_content())
        if (content.first >= 0 && content.first < 1000)
            objs.push_back(content.first);
    return objs;
}

// Copy objects or parts to the app's internal clipboard, like Ctrl+C: exactly one of {object,
// instance?, volumes?}, {objects:[...]} or {scope:'selection'}. Of an object one instance (default 0)
// is copied: pasting puts every instance of a copy at the same spot. The model does not change: no
// undo step. The clipboard of the operating system is not touched.
void SSWCP_SliceProject_Instance::sw_SlicerCopy()
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
        const bool scope_ok    = has_scope && m_param_data.at("scope").is_string() &&
                              m_param_data.at("scope").get<std::string>() == "selection";
        if (int(has_object) + int(has_objects) + int(has_scope) != 1 || (has_scope && !scope_ok))
            throw std::runtime_error("specify exactly one of: object, objects, scope:'selection'");
        const bool has_inst = sw_slicer_has_param(m_param_data, "instance");
        const bool has_vols = sw_slicer_has_param(m_param_data, "volumes");
        if (!has_object && (has_inst || has_vols))
            throw std::runtime_error("'instance' and 'volumes' can only be used with 'object'");

        sw_slicer_require_free_gizmo();
        sw_slicer_require_not_loading();
        // Plater::can_copy_to_clipboard() requires the 3D view.
        sw_slicer_require_3d_view();
        Model&                   model  = plater->model();
        GLCanvas3D*              canvas = sw_slicer_canvas();
        Selection&               sel    = canvas->get_selection();
        std::vector<std::string> notes;
        // Copying changes no model data: the selection changes take no (selection) snapshot, which would
        // discard the redo branch.
        Plater::SuppressSnapshots no_snapshots(plater);

        if (has_object) {
            const int          obj  = sw_slicer_required_object(m_param_data);
            const int          inst = sw_slicer_optional_sub_index(m_param_data, "instance", obj, 0);
            const ModelObject* mo   = model.objects[obj];
            if (has_vols) {
                const json& arr = m_param_data.at("volumes");
                if (!arr.is_array() || arr.empty())
                    throw std::runtime_error("'volumes' must be a non-empty list of volume indices");
                std::vector<int> vols;
                for (const json& j : arr) {
                    if (!j.is_number_integer() || j.get<long long>() < 0 || j.get<long long>() >= (long long) mo->volumes.size())
                        throw std::runtime_error("volume " + j.dump() + " does not exist (object " + std::to_string(obj) + " has " +
                                                 std::to_string(mo->volumes.size()) + " volume(s))");
                    const int vol = j.get<int>();
                    if (std::find(vols.begin(), vols.end(), vol) != vols.end())
                        throw std::runtime_error("volume index " + std::to_string(vol) + " appears twice in 'volumes'");
                    vols.push_back(vol);
                }
                // As the user would click the parts in the object list (all solid parts allowed: the copy
                // is a parts copy, not an object copy).
                sel.clear();
                for (size_t i = 0; i < vols.size(); ++i)
                    sel.add_volume(static_cast<unsigned int>(obj), static_cast<unsigned int>(vols[i]), inst, i == 0);
                sw_slicer_selection_changed(canvas);
            } else {
                // One instance: paste_objects_from_clipboard() puts all instances of a copy at the same spot.
                sw_slicer_select(obj, inst, -1);
                if (mo->instances.size() > 1)
                    notes.push_back("the object has " + std::to_string(mo->instances.size()) + " instances; only instance " +
                                    std::to_string(inst) + " was copied");
            }
        } else if (has_objects) {
            const std::vector<int>           objs = sw_slicer_parse_objects(m_param_data, "objects");
            std::vector<std::pair<int, int>> items;
            for (int obj : objs) {
                items.emplace_back(obj, 0);
                if (model.objects[obj]->instances.size() > 1)
                    notes.push_back("object " + std::to_string(obj) + " has " + std::to_string(model.objects[obj]->instances.size()) +
                                    " instances; only instance 0 was copied");
            }
            sw_slicer_select_instances(items);
        } else if (sel.is_empty()) {
            throw std::runtime_error("nothing is selected");
        }
        // Selection::copy_to_clipboard() indexes model.objects with every selected object index; the wipe
        // tower (object_idx >= 1000) would read past the end (Plater::can_copy_to_clipboard() allows it).
        if (sel.is_wipe_tower() || sw_slicer_selected_objects(sel).size() != sel.get_content().size())
            throw std::runtime_error("the wipe tower cannot be copied; select objects or parts only");

        if (!plater->can_copy_to_clipboard())
            throw std::runtime_error("the selection cannot be copied");
        {
            sswcp_api::Scope api_scope;
            // The app route of Ctrl+C: the object list's clipboard is reset (the list selection is now an
            // object or part), then the selection's clipboard is filled. No snapshot (the app takes none).
            plater->copy_selection_to_clipboard();
            const std::vector<std::string>& api_notes = sswcp_api::notes();
            notes.insert(notes.end(), api_notes.begin(), api_notes.end());
        }
        const Selection::Clipboard& cb = sel.get_clipboard();
        if (cb.is_empty())
            throw std::runtime_error(sw_slicer_join_notes("nothing was copied", notes));
        const bool parts = cb.get_mode() == Selection::Volume;
        if (parts) {
            std::set<int> vols;
            for (unsigned int idx : sel.get_volume_idxs()) {
                const GLVolume* v = sel.get_volume(idx);
                if (v != nullptr && v->volume_idx() >= 0 && v->object_idx() == sel.get_object_idx())
                    vols.insert(v->volume_idx());
            }
            m_res_data["mode"]     = "parts";
            m_res_data["object"]   = sel.get_object_idx();
            m_res_data["instance"] = sel.get_instance_idx();
            m_res_data["volumes"]  = std::vector<int>(vols.begin(), vols.end());
            notes.insert(notes.begin(), "the app's internal copy/paste clipboard now holds " +
                                            std::to_string(cb.get_objects().front()->volumes.size()) +
                                            " part(s); the Windows clipboard is not changed");
        } else {
            json objs      = json::array();
            json instances = json::array();
            for (const auto& content : sel.get_content()) {
                if (content.first < 0 || content.first >= 1000 || content.second.empty())
                    continue;
                objs.push_back(content.first);
                instances.push_back(*content.second.begin());
                if (content.second.size() > 1)
                    notes.push_back("object " + std::to_string(content.first) + ": " + std::to_string(content.second.size()) +
                                    " instances were copied; pasting puts them at the same spot");
            }
            m_res_data["mode"]      = "objects";
            m_res_data["objects"]   = objs;
            m_res_data["instances"] = instances;
            notes.insert(notes.begin(), "the app's internal copy/paste clipboard now holds " + std::to_string(cb.get_objects().size()) +
                                            " object(s); the Windows clipboard is not changed");
        }
        m_res_data["notes"] = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Paste the app's internal clipboard, like Ctrl+V. Objects go to the nearest free spot of the current
// plate, or of 'plate' (which then becomes the current plate); parts go into 'object' (instance
// 'instance', default 0). One undo step "API: Paste" per call.
void SSWCP_SliceProject_Instance::sw_SlicerPaste()
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
        // The paste code uses plater()->canvas3D(), the current canvas.
        sw_slicer_require_3d_view();
        Model&                      model  = plater->model();
        PartPlateList&              plates = plater->get_partplate_list();
        GLCanvas3D*                 canvas = sw_slicer_canvas();
        Selection&                  sel    = canvas->get_selection();
        const Selection::Clipboard& cb     = sel.get_clipboard();
        if (cb.is_empty())
            throw std::runtime_error("the clipboard is empty (copy first with sw_SlicerCopy, or Ctrl+C in the app)");
        const bool               has_object = sw_slicer_has_param(m_param_data, "object");
        const bool               has_inst   = sw_slicer_has_param(m_param_data, "instance");
        std::vector<std::string> notes;

        if (cb.get_mode() == Selection::Instance) {
            if (has_object || has_inst)
                throw std::runtime_error("the clipboard holds objects; 'object' and 'instance' are only for pasting parts");
            const int plate = sw_slicer_resolve_plate(m_param_data);
            if (plates.get_plate(plate)->is_locked())
                throw std::runtime_error("plate " + std::to_string(plate) + " is locked");
            // The free-spot search of the paste works on the current plate.
            sw_slicer_select_plate(plate);
            const size_t n_before = model.objects.size();
            {
                sswcp_api::Scope     api_scope;
                Plater::TakeSnapshot snapshot(plater, "API: Paste");
                // Objects are only pasted in instance mode (Selection::paste_from_clipboard). Cleared after
                // the snapshot, so undo restores the user's selection; clear() keeps the mode.
                sel.clear();
                wxGetApp().obj_list()->update_selections();
                sel.set_mode(Selection::Instance);
                // The selection's clipboard only: Plater::paste_from_clipboard() would first paste settings or
                // layer ranges that were copied in the object list.
                sel.paste_from_clipboard();
                const std::vector<std::string>& api_notes = sswcp_api::notes();
                notes.insert(notes.end(), api_notes.begin(), api_notes.end());
            }
            const size_t n_after = model.objects.size();
            if (n_after <= n_before)
                throw std::runtime_error(sw_slicer_join_notes("pasting produced nothing", notes));
            json added = json::array();
            for (size_t i = n_before; i < n_after; ++i)
                added.push_back(static_cast<int>(i));
            const int on_plate = sw_slicer_instance_plate(static_cast<int>(n_before), 0);
            if (on_plate < 0)
                notes.push_back("no free spot on the plate; the copy was placed off the plate (use sw_SlicerArrange)");
            else
                notes.push_back("placed in the nearest free spot of plate " + std::to_string(on_plate));
            m_res_data["mode"]          = "objects";
            m_res_data["plate"]         = on_plate;
            m_res_data["objects_added"] = added;
            m_res_data["objects"]       = sw_slicer_objects_json();
            m_res_data["notes"]         = notes;
            send_to_js();
            finish_job();
            return;
        }

        // A parts copy.
        if (!has_object)
            throw std::runtime_error("the clipboard holds parts; pass 'object' to paste them into");
        if (sw_slicer_has_param(m_param_data, "plate"))
            throw std::runtime_error("'plate' is only for pasting objects");
        const int    obj  = sw_slicer_required_object(m_param_data);
        const int    inst = sw_slicer_optional_sub_index(m_param_data, "instance", obj, 0);
        ModelObject* mo   = model.objects[obj];
        // The app allows pasting parts into a cut object; the API, like sw_SlicerAddVolume, refuses solid
        // and negative parts there (they change one half of the cut without the other).
        if (mo->is_cut())
            for (const ModelObject* src : cb.get_objects())
                for (const ModelVolume* mv : src->volumes)
                    if (mv->is_model_part() || mv->is_negative_volume())
                        throw std::runtime_error("solid or negative parts cannot be pasted into a cut object");
        sw_slicer_select(obj, inst, -1);
        if (!sel.is_from_single_instance() || sel.get_object_idx() != obj || sel.get_instance_idx() != inst)
            throw std::runtime_error("could not select instance " + std::to_string(inst) + " of object " + std::to_string(obj));
        const std::set<size_t> ids_before = sw_slicer_volume_ids(mo);
        {
            sswcp_api::Scope     api_scope;
            Plater::TakeSnapshot snapshot(plater, "API: Paste");
            // The selection's clipboard only: Plater::paste_from_clipboard() would first paste settings or
            // layer ranges that were copied in the object list.
            sel.paste_from_clipboard();
            const std::vector<std::string>& api_notes = sswcp_api::notes();
            notes.insert(notes.end(), api_notes.begin(), api_notes.end());
        }
        // The indices after the app has reordered the volumes (reorder_volumes_and_get_selection).
        json added = json::array();
        for (size_t v = 0; v < mo->volumes.size(); ++v)
            if (ids_before.count(mo->volumes[v]->id().id) == 0)
                added.push_back(static_cast<int>(v));
        if (added.empty())
            throw std::runtime_error(sw_slicer_join_notes("pasting produced nothing", notes));
        m_res_data["mode"]          = "parts";
        m_res_data["object"]        = obj;
        m_res_data["instance"]      = inst;
        m_res_data["volumes_added"] = added;
        m_res_data["objects"]       = sw_slicer_objects_json();
        m_res_data["notes"]         = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Hide or show an object, instance or solid part, like "Hide"/"Show" in the right-click menu of the
// assembly view (the only place where the app offers it). It is a view setting of the assembly
// canvas (GLVolume::visible): slicing and printing do not change, and the undo stack does not store
// it, so there is no undo step.
void SSWCP_SliceProject_Instance::sw_SlicerSetVisible()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        if (!sw_slicer_has_param(m_param_data, "visible") || !m_param_data.at("visible").is_boolean())
            throw std::runtime_error("'visible' must be true or false");
        const bool visible = m_param_data.at("visible").get<bool>();
        const int  obj     = sw_slicer_required_object(m_param_data);
        const int  inst    = sw_slicer_optional_sub_index(m_param_data, "instance", obj, -1);
        const int  vol     = sw_slicer_optional_sub_index(m_param_data, "volume", obj, -1);
        sw_slicer_require_free_gizmo();
        // The menu cancels running jobs (Plater::set_selected_visible); the API refuses instead.
        sw_slicer_require_idle();
        sw_slicer_require_not_loading();
        GLCanvas3D* ac = plater->get_assmeble_canvas3D();
        if (ac == nullptr || plater->get_current_canvas3D() != ac || ac->get_canvas_type() != GLCanvas3D::CanvasAssembleView)
            throw std::runtime_error("hide/show only exists in the assembly view (sw_SlicerShowView Assemble)");
        // The assembly view only loads solid parts.
        if (vol >= 0 && !plater->model().objects[obj]->volumes[vol]->is_model_part())
            throw std::runtime_error("only solid parts are shown in the assembly view");

        // A view setting: the selection changes take no (selection) snapshot, which would discard the redo branch.
        Plater::SuppressSnapshots no_snapshots(plater);
        // Select on the assembly canvas; the object list follows that selection there (scene_selection).
        Selection& s = ac->get_selection();
        s.clear();
        if (vol >= 0)
            s.add_volume(static_cast<unsigned int>(obj), static_cast<unsigned int>(vol), std::max(inst, 0), true);
        else if (inst >= 0)
            s.add_instance(static_cast<unsigned int>(obj), static_cast<unsigned int>(inst), true);
        else
            s.add_object(static_cast<unsigned int>(obj), true);
        wxGetApp().obj_list()->update_selections();
        ac->set_as_dirty();
        if (s.is_empty())
            throw std::runtime_error("could not select object " + std::to_string(obj) + " in the assembly view");

        // Same reading as the menu (append_menu_item_set_visible).
        int n_visible = 0;
        int n_total   = 0;
        for (unsigned int i : s.get_volume_idxs()) {
            const GLVolume* v = s.get_volume(i);
            if (v == nullptr)
                continue;
            ++n_total;
            n_visible += v->visible ? 1 : 0;
        }
        const json previous = (n_visible == n_total) ? json(true) : (n_visible == 0) ? json(false) : json("mixed");

        std::vector<std::string> notes;
        if ((visible && n_visible == n_total) || (!visible && n_visible == 0)) {
            notes.push_back("visibility unchanged; nothing changed");
        } else {
            sswcp_api::Scope api_scope;
            // GLCanvas3D::set_selected_visible() without the snapshot of Plater::set_selected_visible(): the
            // undo stack does not store GLVolume::visible, so that snapshot would be an empty undo step.
            ac->set_selected_visible(visible);
            ac->set_as_dirty();
            ac->request_extra_frame();
            const std::vector<std::string>& api_notes = sswcp_api::notes();
            notes.insert(notes.end(), api_notes.begin(), api_notes.end());
        }
        notes.push_back("visibility is a view setting of the assembly view: it does not change printing and is not an undo step");

        m_res_data["object"] = obj;
        if (vol >= 0)
            m_res_data["instance"] = std::max(inst, 0);
        else if (inst >= 0)
            m_res_data["instance"] = inst;
        if (vol >= 0)
            m_res_data["volume"] = vol;
        m_res_data["visible"]  = visible;
        m_res_data["previous"] = previous;
        m_res_data["view"]     = sw_slicer_canvas_view_name(ac);
        m_res_data["notes"]    = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Set the number of instances of an object. Only lowering is possible: the app makes copies as
// separate objects (its "set number of instances" is disabled), see sw_SlicerClone. The highest
// instances are deleted (the route of sw_SlicerDelete with 'instance'). One undo step
// "API: Set instances"; the same count takes no snapshot.
void SSWCP_SliceProject_Instance::sw_SlicerSetInstanceCount()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        const json* jc = sw_slicer_has_param(m_param_data, "count") ? &m_param_data.at("count") : nullptr;
        // Same limit as the clone dialog (sw_SlicerClone).
        if (jc == nullptr || !jc->is_number_integer() || jc->get<long long>() < 1 || jc->get<long long>() > 1000)
            throw std::runtime_error("count must be between 1 and 1000");
        const int count = jc->get<int>();
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        sw_slicer_require_not_slicing();
        sw_slicer_require_3d_view();
        Model&    model = plater->model();
        const int obj   = sw_slicer_required_object(m_param_data);
        const int n     = static_cast<int>(model.objects[obj]->instances.size());
        // Same rule as can_increase_instances()/can_decrease_instances().
        if (model.objects[obj]->is_cut())
            throw std::runtime_error("the instances of a cut object cannot be changed");

        std::vector<std::string> notes;
        if (count == n) {
            notes.push_back("instance count unchanged; nothing changed");
        } else if (count > n) {
            throw std::runtime_error("the app makes copies as separate objects; use sw_SlicerClone (count) to add copies");
        } else {
            // Never the last instance (count >= 1): ObjectList::del_subobject_from_object() would show an
            // error dialog for it.
            std::vector<std::pair<int, int>> items;
            for (int i = count; i < n; ++i)
                items.emplace_back(obj, i);
            sw_slicer_select_instances(items);
            if (!plater->can_delete())
                throw std::runtime_error("these instances cannot be deleted");
            {
                sswcp_api::Scope     api_scope;
                Plater::TakeSnapshot snapshot(plater, "API: Set instances");
                // The app's own snapshot "Delete Selected Objects" is suppressed.
                plater->remove_selected();
                const std::vector<std::string>& api_notes = sswcp_api::notes();
                notes.insert(notes.end(), api_notes.begin(), api_notes.end());
            }
            if (obj >= static_cast<int>(model.objects.size()) || static_cast<int>(model.objects[obj]->instances.size()) != count)
                throw std::runtime_error(sw_slicer_join_notes("the instance count could not be set", notes));
        }
        m_res_data["object"]   = obj;
        m_res_data["count"]    = static_cast<int>(model.objects[obj]->instances.size());
        m_res_data["previous"] = n;
        m_res_data["objects"]  = sw_slicer_objects_json();
        m_res_data["notes"]    = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Make an instance a separate object, like "Set as an individual object" (ObjectList::split_instances);
// without 'instance' every instance becomes its own object ("Set as individual objects"). The new
// objects are appended to the model. One undo step "API: Instance to object".
void SSWCP_SliceProject_Instance::sw_SlicerInstanceToObject()
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
        // split_instances() reads scene_selection(): the 3D canvas must be the current one.
        sw_slicer_require_3d_view();
        Model&    model = plater->model();
        const int obj   = sw_slicer_required_object(m_param_data);
        const int inst  = sw_slicer_optional_sub_index(m_param_data, "instance", obj, -1);
        const int n     = static_cast<int>(model.objects[obj]->instances.size());
        // Checked up front (the menu is disabled then, can_set_instance_to_object()): otherwise
        // del_subobject_from_object() would show an error dialog for the last instance.
        if (n < 2)
            throw std::runtime_error("object " + std::to_string(obj) + " has only one instance; nothing to separate");
        // The app allows it, but the copy would share the cut link of the original.
        if (model.objects[obj]->is_cut())
            throw std::runtime_error("instances of a cut object cannot be separated through the API");

        sw_slicer_select(obj, inst, -1);
        const Selection& sel = sw_slicer_canvas()->get_selection();
        if (sel.get_object_idx() != obj || (inst >= 0 ? sel.get_instance_idx() != inst : !sel.is_single_full_object()))
            throw std::runtime_error("could not select object " + std::to_string(obj));
        const size_t             n_before = model.objects.size();
        std::vector<std::string> notes;
        {
            sswcp_api::Scope     api_scope;
            Plater::TakeSnapshot snapshot(plater, "API: Instance to object");
            // The app's own snapshot "Instances to Separated Objects" is suppressed.
            wxGetApp().obj_list()->split_instances();
            const std::vector<std::string>& api_notes = sswcp_api::notes();
            notes.insert(notes.end(), api_notes.begin(), api_notes.end());
        }
        const size_t n_after = model.objects.size();
        if (n_after <= n_before)
            throw std::runtime_error(sw_slicer_join_notes("no instance was separated", notes));
        json added = json::array();
        for (size_t i = n_before; i < n_after; ++i)
            added.push_back(static_cast<int>(i));
        if (inst >= 0)
            notes.push_back("instance " + std::to_string(inst) + " is now object " + std::to_string(n_before) +
                            "; the remaining instances keep their indices shifted down");
        else
            // instances_to_separated_objects() works from the last instance down.
            notes.push_back("instance 0 stays object " + std::to_string(obj) + "; instance i is now object " + std::to_string(n_before) +
                            " + " + std::to_string(n - 1) + " - i");
        m_res_data["objects_added"] = added;
        m_res_data["objects"]       = sw_slicer_objects_json();
        m_res_data["notes"]         = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Move an instance (default 0) to another plate. place:'free' (default) puts it in the nearest free
// spot of the target plate, like pasting (that search works on the current plate, so the target plate
// becomes the current plate); place:'center' puts it in the middle, like Center (it may overlap).
// Locked plates are refused. One undo step "API: Move to plate"; already on the plate takes no snapshot.
void SSWCP_SliceProject_Instance::sw_SlicerMoveToPlate()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        if (!sw_slicer_has_param(m_param_data, "plate"))
            throw std::runtime_error("specify 'plate' (0-based)");
        const int         plate = sw_slicer_resolve_plate(m_param_data);
        const std::string place = !sw_slicer_has_param(m_param_data, "place") ? std::string("free") :
                                  m_param_data.at("place").is_string()        ? m_param_data.at("place").get<std::string>() :
                                                                                std::string("?");
        if (place != "free" && place != "center")
            throw std::runtime_error("place must be 'free' or 'center'");
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        sw_slicer_require_not_slicing();
        sw_slicer_require_3d_view();
        const int      obj    = sw_slicer_required_object(m_param_data);
        const int      inst   = sw_slicer_optional_sub_index(m_param_data, "instance", obj, 0);
        PartPlateList& plates = plater->get_partplate_list();
        GLCanvas3D*    canvas = sw_slicer_canvas();
        const int      from   = sw_slicer_instance_plate(obj, inst);

        std::vector<std::string> notes;
        auto reply = [&]() {
            m_res_data["object"]        = obj;
            m_res_data["instance"]      = inst;
            m_res_data["plate"]         = plate;
            m_res_data["from_plate"]    = from;
            m_res_data["position"]      = sw_slicer_vec3(plater->model().objects[obj]->instances[inst]->get_offset());
            m_res_data["current_plate"] = plates.get_curr_plate_index();
            m_res_data["notes"]         = notes;
            send_to_js();
            finish_job();
        };
        if (from == plate) {
            notes.push_back("already on plate " + std::to_string(plate) + "; nothing changed");
            reply();
            return;
        }
        // Dragging in the app ignores the lock; the API does not touch a locked plate.
        if (plates.get_plate(plate)->is_locked())
            throw std::runtime_error("plate " + std::to_string(plate) + " is locked");
        if (from >= 0 && plates.get_plate(from)->is_locked())
            throw std::runtime_error("plate " + std::to_string(from) + " is locked");

        // Everything is checked before the current plate changes.
        Selection& sel           = canvas->get_selection();
        auto       instance_only = [&]() {
            return !sel.is_empty() && sel.get_object_idx() == obj && sel.get_instance_idx() == inst;
        };
        sw_slicer_select(obj, inst, -1);
        if (!instance_only())
            throw std::runtime_error("could not select instance " + std::to_string(inst) + " of object " + std::to_string(obj));
        if (place == "free") {
            // get_nearest_empty_cell() only searches the current plate.
            sw_slicer_select_plate(plate);
            notes.push_back("plate " + std::to_string(plate) + " is now the current plate (the free-spot search works on the current plate)");
            // Selecting a plate may change the selection: select the instance again.
            sw_slicer_select(obj, inst, -1);
            if (!instance_only())
                throw std::runtime_error("could not select instance " + std::to_string(inst) + " of object " + std::to_string(obj) +
                                         " after making plate " + std::to_string(plate) + " the current plate");
        }
        const BoundingBoxf3 bbox   = sel.get_bounding_box();
        const BoundingBoxf3 volume = plates.get_plate(plate)->get_build_volume();
        if (bbox.size().x() > volume.size().x() || bbox.size().y() > volume.size().y())
            notes.push_back("the object is larger than the plate");
        {
            sswcp_api::Scope     api_scope;
            Plater::TakeSnapshot snapshot(plater, "API: Move to plate");
            if (place == "free") {
                // Same call as Selection::paste_objects_from_clipboard(), then the move of Selection::center_plate().
                const Vec3d center = plates.get_plate(plate)->get_center_origin();
                const Vec2f cell   = canvas->get_nearest_empty_cell({float(center.x()), float(center.y())},
                                                                    {float(bbox.size().x() + 1), float(bbox.size().y() + 1)});
                sel.move_to_center(Vec3d(cell.x() - bbox.center().x(), cell.y() - bbox.center().y(), 0.0));
                // do_move() updates the plate assignment of the instance; no snapshot of its own.
                canvas->do_move("");
            } else {
                // The route of sw_SlicerCenter; the app's own snapshot "Move Object" is suppressed.
                canvas->do_center_plate(plate);
            }
            const std::vector<std::string>& api_notes = sswcp_api::notes();
            notes.insert(notes.end(), api_notes.begin(), api_notes.end());
        }
        if (place == "center")
            notes.push_back("centered on plate " + std::to_string(plate) + "; it may overlap other objects (use sw_SlicerArrange with scope 'plate')");
        const int after = sw_slicer_instance_plate(obj, inst);
        if (after != plate)
            notes.push_back(after >= 0 ? "the app placed the object on plate " + std::to_string(after) :
                                         std::string("the app placed the object on no plate"));
        reply();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Output folder parameter: an absolute path to an existing folder.
static boost::filesystem::path sw_slicer_output_dir(const json& params, const char* key)
{
    const std::string k   = key;
    const std::string msg = "'" + k + "' must be an absolute path to an existing folder";
    if (!sw_slicer_has_param(params, key) || !params.at(key).is_string() || params.at(key).get<std::string>().empty())
        throw std::runtime_error(msg);
    const boost::filesystem::path p = sw_slicer_path_from_utf8(params.at(key).get<std::string>());
    boost::system::error_code     ec;
    if (!p.is_absolute() || !boost::filesystem::is_directory(p, ec))
        throw std::runtime_error(msg);
    return p;
}

// A file name stem from an object name: no folder or forbidden characters, trimmed, without a
// trailing ".stl", never empty or a reserved Windows device name.
static std::string sw_slicer_safe_file_stem(const std::string& name)
{
    std::string s = name;
    for (char& c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f || std::string("<>:/\\|?*\"").find(c) != std::string::npos)
            c = '_';
    }
    if (boost::algorithm::iends_with(s, ".stl"))
        s.resize(s.size() - 4);
    // Windows drops trailing dots and spaces from file names.
    const size_t b = s.find_first_not_of(' ');
    const size_t e = s.find_last_not_of(" .");
    s              = (b == std::string::npos || e == std::string::npos || e < b) ? std::string() : s.substr(b, e - b + 1);
    if (s.empty())
        return "object";
    static const char* const reserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
                                           "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    const std::string base = s.substr(0, s.find('.'));
    for (const char* r : reserved)
        if (boost::algorithm::iequals(base, r))
            return "_" + s;
    return s;
}

// stem.stl in dir, or stem(1).stl, stem(2).stl, ... if that exists (the scheme of Plater::export_stl).
static boost::filesystem::path sw_slicer_unique_file(const boost::filesystem::path& dir, const std::string& stem)
{
    boost::filesystem::path p = dir / sw_slicer_path_from_utf8(stem + ".stl");
    for (int n = 1;; ++n) {
        // exists(p, ec) also sets ec for a file that is not there, so look at the status type instead.
        boost::system::error_code          ec;
        const boost::filesystem::file_status st = boost::filesystem::status(p, ec);
        if (st.type() == boost::filesystem::file_not_found)
            return p;
        if (ec || st.type() == boost::filesystem::status_error)
            throw std::runtime_error("cannot check whether the file exists: " + sw_slicer_path_to_utf8(p));
        if (n > 100000)
            throw std::runtime_error("no free file name for " + stem + ".stl");
        p = dir / sw_slicer_path_from_utf8(stem + "(" + std::to_string(n) + ").stl");
    }
}

// Export objects as STL without a file dialog, the work of "Export all objects as one STL / as STLs"
// (MainFrame) and "Export as one STL / as STLs" (object menu): scope 'all' (default), 'plate',
// 'objects' or 'selection' (whole objects); one file, or with split one file per object in a folder,
// never replacing a file (an existing name gets "(n)"). Without split another existing file is only
// replaced with overwrite:true. Writes through temporary files. No undo step.
void SSWCP_SliceProject_Instance::sw_SlicerExportStl()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        const std::string scope = !sw_slicer_has_param(m_param_data, "scope") ? std::string("all") :
                                  m_param_data.at("scope").is_string()        ? m_param_data.at("scope").get<std::string>() :
                                                                                std::string("?");
        if (scope != "all" && scope != "plate" && scope != "objects" && scope != "selection")
            throw std::runtime_error("scope must be 'all', 'plate', 'objects' or 'selection'");
        if (sw_slicer_has_param(m_param_data, "plate") && scope != "plate")
            throw std::runtime_error("'plate' can only be used with scope:'plate'");
        if (sw_slicer_has_param(m_param_data, "objects") && scope != "objects")
            throw std::runtime_error("'objects' can only be used with scope:'objects'");
        if (sw_slicer_has_param(m_param_data, "split") && !m_param_data.at("split").is_boolean())
            throw std::runtime_error("'split' must be true or false");
        const bool split = sw_slicer_flag_true(m_param_data, "split");
        if (split && sw_slicer_flag_true(m_param_data, "overwrite"))
            throw std::runtime_error("split never replaces files; 'overwrite' cannot be used with split");

        // The handler changes the selection.
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        sw_slicer_require_not_loading();
        Model& model = plater->model();
        if (model.objects.empty())
            throw std::runtime_error("the project is empty; nothing to export");
        // The SLA branch of export_stl() reads slice results.
        if (plater->printer_technology() != ptFFF)
            throw std::runtime_error("only FFF printers are supported");
        // Exporting changes no model data: the selection changes take no (selection) snapshot, which would
        // discard the redo branch.
        Plater::SuppressSnapshots no_snapshots(plater);

        std::vector<std::string> notes;
        GLCanvas3D*              canvas = sw_slicer_canvas();
        const Selection&         sel    = canvas->get_selection();
        // The condition of export_stl(): whole objects only.
        auto whole_objects = [&sel]() { return !sel.is_wipe_tower() && (sel.is_single_full_object() || sel.is_multiple_full_object()); };

        std::vector<int> objs;
        if (scope == "all") {
            for (size_t i = 0; i < model.objects.size(); ++i)
                objs.push_back(static_cast<int>(i));
        } else if (scope == "plate") {
            const int  n = sw_slicer_resolve_plate(m_param_data);
            PartPlate* p = plater->get_partplate_list().get_plate(n);
            for (size_t o = 0; o < model.objects.size(); ++o) {
                const ModelObject* mo = model.objects[o];
                int                on = 0;
                for (size_t j = 0; j < mo->instances.size(); ++j)
                    on += p->contain_instance(static_cast<int>(o), static_cast<int>(j)) ? 1 : 0;
                if (on == 0)
                    continue;
                objs.push_back(static_cast<int>(o));
                if (on < static_cast<int>(mo->instances.size()))
                    notes.push_back("object " + std::to_string(o) + " also has instances on other plates; all its instances are exported");
            }
            if (objs.empty())
                throw std::runtime_error("plate " + std::to_string(n) + " has no objects");
        } else if (scope == "objects") {
            objs = sw_slicer_parse_objects(m_param_data, "objects");
        } else {
            if (sel.is_empty())
                throw std::runtime_error("nothing is selected");
            if (!whole_objects())
                throw std::runtime_error("select whole objects in the app first (parts or single instances are not exported as STL)");
            objs = sw_slicer_selected_objects(sel);
        }
        std::sort(objs.begin(), objs.end());

        const bool any_negative = std::any_of(objs.begin(), objs.end(), [&model](int o) {
            const ModelVolumePtrs& vols = model.objects[o]->volumes;
            return std::any_of(vols.begin(), vols.end(), [](const ModelVolume* mv) { return mv->is_negative_volume(); });
        });
        if (any_negative)
            notes.push_back("negative parts were subtracted; if that fails the app shows a notification and exports only the positive parts");

        // export_stl_to() always reports success (store_stl() does not check the write): check the file.
        auto write_stl = [plater](const boost::filesystem::path& tmp, bool selection_only) {
            const bool                ok = plater->export_stl_to(sw_slicer_path_to_utf8(tmp), false, selection_only, false);
            boost::system::error_code ec;
            return ok && boost::filesystem::is_regular_file(tmp, ec) && boost::filesystem::file_size(tmp, ec) > 0 && !ec;
        };
        auto file_json = [](const boost::filesystem::path& p, const std::vector<int>& file_objs) {
            boost::system::error_code ec;
            const uintmax_t           bytes = boost::filesystem::file_size(p, ec);
            json                      f;
            f["path"]    = sw_slicer_path_to_utf8(p);
            f["bytes"]   = ec ? json(nullptr) : json(static_cast<unsigned long long>(bytes));
            f["objects"] = file_objs;
            return f;
        };
        json files = json::array();

        if (!split) {
            const boost::filesystem::path target = sw_slicer_output_path(m_param_data, "path", {".stl"});
            sw_slicer_require_overwrite_allowed(target, m_param_data);
            boost::system::error_code ec;
            const bool                replaces       = boost::filesystem::exists(target, ec);
            const bool                selection_only = scope != "all";
            if (selection_only) {
                // Selected as the user would before the menu item; the selection stays visible.
                sw_slicer_select_whole_objects(objs);
                if (!whole_objects() || sw_slicer_selected_objects(sel).size() != objs.size())
                    throw std::runtime_error("the objects could not be selected for export");
            }
            {
                sswcp_api::Scope api_scope;
                sw_slicer_write_via_temp(target, [&](const boost::filesystem::path& tmp) { return write_stl(tmp, selection_only); });
                const std::vector<std::string>& api_notes = sswcp_api::notes();
                notes.insert(notes.end(), api_notes.begin(), api_notes.end());
            }
            files.push_back(file_json(target, objs));
            // export_stl() moves a single object with one instance by -origin_translation.
            if (selection_only && objs.size() == 1 && model.objects[objs.front()]->instances.size() == 1)
                notes.push_back("coordinates are relative to the object (as the app's STL export does)");
            if (replaces)
                notes.push_back("existing file replaced");
        } else {
            const boost::filesystem::path dir = sw_slicer_output_dir(m_param_data, "path");
            // One object at a time, each through a temporary file: the app route (multi_stls) writes
            // directly, the API never replaces a file.
            std::vector<std::string> written;
            bool                     relative = false;
            for (int o : objs) {
                try {
                    sw_slicer_select_whole_objects({o});
                    if (!sel.is_single_full_object() || sel.get_object_idx() != o)
                        throw std::runtime_error("object " + std::to_string(o) + " could not be selected for export");
                    const boost::filesystem::path file = sw_slicer_unique_file(dir, sw_slicer_safe_file_stem(model.objects[o]->name));
                    {
                        sswcp_api::Scope api_scope;
                        sw_slicer_write_via_temp(file, [&](const boost::filesystem::path& tmp) { return write_stl(tmp, true); });
                        const std::vector<std::string>& api_notes = sswcp_api::notes();
                        notes.insert(notes.end(), api_notes.begin(), api_notes.end());
                    }
                    files.push_back(file_json(file, {o}));
                    written.push_back(sw_slicer_path_to_utf8(file));
                    relative = relative || model.objects[o]->instances.size() == 1;
                } catch (std::exception& e) {
                    sw_slicer_select_whole_objects(objs);
                    std::string msg = "exported " + std::to_string(written.size()) + " of " + std::to_string(objs.size()) +
                                      " objects before the error: " + e.what();
                    if (!written.empty()) {
                        msg += "; written:";
                        for (const std::string& w : written)
                            msg += " " + w;
                    }
                    throw std::runtime_error(msg);
                }
            }
            // Show what was exported.
            sw_slicer_select_whole_objects(objs);
            if (relative)
                notes.push_back("coordinates of objects with one instance are relative to the object (as the app's STL export does)");
        }

        m_res_data["scope"] = scope;
        m_res_data["files"] = files;
        m_res_data["notes"] = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Export the whole project as a generic 3MF without the app's extensions, like "Export Generic 3MF"
// (Plater::export_core_3mf: export_3mf with SaveStrategy::Silence). The project file, title and saved
// state do not change. Another existing file is only replaced with overwrite:true. No undo step.
void SSWCP_SliceProject_Instance::sw_SlicerExport3mf()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        // The thumbnails are rendered on the GUI thread, as in sw_SlicerSaveProject.
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        sw_slicer_require_not_slicing();
        if (plater->model().objects.empty())
            throw std::runtime_error("the project is empty; nothing to export");
        if (plater->using_exported_file() || plater->only_gcode_mode())
            throw std::runtime_error("a G-code or sliced file is open; it cannot be exported as a model");

        const boost::filesystem::path target = sw_slicer_output_path(m_param_data, "path", {".3mf"});
        if (boost::algorithm::iends_with(m_param_data.at("path").get<std::string>(), ".gcode.3mf"))
            throw std::runtime_error("use a .3mf name without .gcode (sw_SlicerExportSliced writes sliced files)");
        const boost::filesystem::path own = into_path(plater->get_project_filename(".3mf"));
        boost::system::error_code     ec;
        if (!own.empty() && boost::filesystem::exists(target, ec) && boost::filesystem::equivalent(target, own, ec) && !ec)
            throw std::runtime_error("this is the project's own file; use sw_SlicerSaveProject to save it");
        sw_slicer_require_overwrite_allowed(target, m_param_data);
        const bool replaces = boost::filesystem::exists(target, ec);
        const bool new_svg  = sw_slicer_has_new_svg_files(plater->model());

        std::vector<std::string> notes;
        {
            sswcp_api::Scope api_scope;
            // Exactly Plater::export_core_3mf() after its file dialog. Silence: no project file name is
            // recorded and publish() skips the SVG question. None of the follow-up steps of saving a project.
            sw_slicer_write_via_temp(target, [plater](const boost::filesystem::path& tmp) {
                return plater->export_3mf(tmp, SaveStrategy::Silence) >= 0;
            });
            notes = sswcp_api::notes();
        }
        if (new_svg)
            notes.push_back("original SVG files stored in the file with their local paths (dialog skipped)");
        if (replaces)
            notes.push_back("existing file replaced");

        const uintmax_t bytes = boost::filesystem::file_size(target, ec);
        m_res_data["path"]    = sw_slicer_path_to_utf8(target);
        m_res_data["bytes"]   = ec ? json(nullptr) : json(static_cast<unsigned long long>(bytes));
        m_res_data["objects"] = static_cast<int>(plater->model().objects.size());
        m_res_data["plates"]  = plater->get_partplate_list().get_plate_count();
        m_res_data["notes"]   = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- Preset management: changes, compare, save, delete, discard ---------------------------------
// Read the unsaved changes of the printer/print/filament presets, compare presets, save the edited
// settings as a user preset, delete a user preset and discard unsaved changes, without dialogs.
// Presets are not part of the undo stack: no command here takes a snapshot. Nothing here talks to the
// cloud: saving and deleting are refused while the user preset sync can run, and presets linked to a
// cloud copy are neither deleted nor replaced by another preset.

// Values longer than this are cut off in diff replies, unless the key was asked for in 'keys'.
static const size_t SW_SLICER_MAX_VALUE_CHARS = 2000;

// Only FFF printers have the print/filament tabs and the option lists used here.
static void sw_slicer_require_fff()
{
    if (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() != ptFFF)
        throw std::runtime_error("only FFF printers are supported");
}

static Tab* sw_slicer_preset_tab(Preset::Type type)
{
    Tab* tab = wxGetApp().get_tab(type);
    if (tab == nullptr || tab->get_presets() == nullptr)
        throw std::runtime_error("no settings tab for this preset type");
    return tab;
}

// The API name of a preset type; the reverse of sw_slicer_preset_type().
static std::string sw_slicer_preset_type_name(Preset::Type type)
{
    if (type == Preset::TYPE_PRINTER)
        return "printer";
    if (type == Preset::TYPE_FILAMENT)
        return "filament";
    return "print";
}

// Add a note once (several checks can produce the same text).
static void sw_slicer_add_note(std::vector<std::string>& notes, const std::string& text)
{
    if (std::find(notes.begin(), notes.end(), text) == notes.end())
        notes.push_back(text);
}

static void sw_slicer_add_notes(std::vector<std::string>& notes, const std::vector<std::string>& more)
{
    for (const std::string& n : more)
        sw_slicer_add_note(notes, n);
}

static std::string sw_slicer_join_ints(const std::vector<int>& values)
{
    std::string out;
    for (int v : values)
        out += (out.empty() ? "" : ", ") + std::to_string(v);
    return out;
}

// Optional parameter 'keys': a non-empty list of distinct option names (or the pseudo key
// "extruders_count" of the printer diff). Missing = no filter.
static std::optional<std::vector<std::string>> sw_slicer_parse_keys(const json& params)
{
    if (!sw_slicer_has_param(params, "keys"))
        return std::nullopt;
    const json& j = params.at("keys");
    if (!j.is_array() || j.empty())
        throw std::runtime_error("'keys' must be a non-empty list of option names");
    std::vector<std::string> keys;
    for (const json& k : j) {
        if (!k.is_string() || k.get<std::string>().empty())
            throw std::runtime_error("'keys' must be a non-empty list of option names");
        const std::string key = k.get<std::string>();
        if (std::find(keys.begin(), keys.end(), key) != keys.end())
            throw std::runtime_error("'" + key + "' appears twice in 'keys'");
        if (key != "extruders_count" && print_config_def.get(key) == nullptr)
            throw std::runtime_error("unknown option '" + key + "'");
        keys.push_back(key);
    }
    return keys;
}

// Can the app's user preset sync run? The conditions of GUI_App::start_sync_user_preset() and its
// callers: not in stealth mode, "Auto sync user presets" on and a cloud account logged in. While it
// runs, the sync thread uploads created/updated presets and deletes cloud copies within seconds.
static bool sw_slicer_cloud_sync_active()
{
    GUI_App& app = wxGetApp();
    return !app.app_config->get_stealth_mode() && app.app_config->get("sync_user_preset") == "true" && app.is_user_login();
}

static void sw_slicer_require_no_cloud_sync(const char* what)
{
    if (sw_slicer_cloud_sync_active())
        throw std::runtime_error(std::string("user preset sync with the cloud is on (Preferences); ") + what +
                                 " is refused so that nothing is uploaded or deleted in the cloud");
}

// Preset summary for replies. No cloud ids: only whether a cloud copy is linked.
static json sw_slicer_preset_info_json(const Preset& p)
{
    json j;
    j["name"]         = p.name;
    j["system"]       = p.is_system;
    j["user"]         = p.is_user();
    j["project"]      = p.is_project_embedded;
    j["inherits"]     = p.inherits();
    j["compatible"]   = p.is_compatible;
    j["cloud_linked"] = !p.setting_id.empty();
    if (p.is_user() && !p.file.empty())
        j["file"] = p.file;
    return j;
}

// Value of an option as text in the form of sw_SlicerGetConfig (opt_serialize); with index >= 0 one
// element of a vector option. null if the option is missing, nil or the index is out of range. A
// nullable vector is nil only if all its elements are; a single nil element is returned as "nil".
// Without 'full', values longer than SW_SLICER_MAX_VALUE_CHARS are cut off and 'truncated' is set.
static json sw_slicer_option_value(const DynamicPrintConfig& cfg, const std::string& key, int index, bool full, bool& truncated)
{
    const ConfigOption* opt = cfg.option(key);
    if (opt == nullptr || opt->is_nil())
        return nullptr;
    std::string s;
    if (index < 0 || !opt->is_vector()) {
        s = cfg.opt_serialize(key);
    } else {
        const std::vector<std::string> values = static_cast<const ConfigOptionVectorBase*>(opt)->vserialize();
        if (index >= static_cast<int>(values.size()))
            return nullptr;
        s = values[static_cast<size_t>(index)];
    }
    if (!full && s.size() > SW_SLICER_MAX_VALUE_CHARS) {
        size_t cut = SW_SLICER_MAX_VALUE_CHARS;
        // Do not cut a UTF-8 sequence in half.
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
            --cut;
        s = s.substr(0, cut) + "\xE2\x80\xA6"; // U+2026 horizontal ellipsis
        truncated = true;
    }
    return s;
}

// "key#3" -> ("key", 3); otherwise index -1 (like get_pure_opt_key() in UnsavedChangesDialog.cpp).
static void sw_slicer_split_key(const std::string& k, std::string& pure, int& index)
{
    const size_t pos = k.find('#');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= k.size() ||
        !std::all_of(k.begin() + pos + 1, k.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
        pure  = k;
        index = -1;
        return;
    }
    pure  = k.substr(0, pos);
    index = std::atoi(k.c_str() + pos + 1);
}

// Flow-variant option lists of a preset type (print, filament); nullptr for printers.
static const std::vector<std::string>* sw_slicer_flow_options(Preset::Type type, ConfigFlowDomain& domain)
{
    if (type == Preset::TYPE_FILAMENT) {
        domain = ConfigFlowDomain::Filament;
        return &filament_flow_variant_options();
    }
    if (type == Preset::TYPE_PRINT) {
        domain = ConfigFlowDomain::Process;
        return &process_flow_variant_options();
    }
    return nullptr;
}

// Index in the reference config of element 'cur_index' of a flow-variant option of the current config:
// the element with the same flow mode, like get_flow_variant_string_value() in UnsavedChangesDialog.cpp.
// -1 = no such element (shown as null).
static int sw_slicer_flow_ref_index(const DynamicPrintConfig& ref, const DynamicPrintConfig& cur, ConfigFlowDomain domain, int cur_index)
{
    const auto* cur_modes = cur.option<ConfigOptionStrings>(flow_support_key(domain));
    if (cur_modes == nullptr || cur_index >= static_cast<int>(cur_modes->values.size()))
        return cur_index;
    const auto* ref_modes = ref.option<ConfigOptionStrings>(flow_support_key(domain));
    if (ref_modes == nullptr)
        return -1;
    const auto it = std::find(ref_modes->values.begin(), ref_modes->values.end(), cur_modes->values[static_cast<size_t>(cur_index)]);
    return it == ref_modes->values.end() ? -1 : static_cast<int>(std::distance(ref_modes->values.begin(), it));
}

// Sort diff items by key, then index (items without index first).
static void sw_slicer_sort_diff(json& items)
{
    std::sort(items.begin(), items.end(), [](const json& l, const json& r) {
        const std::string& lk = l.at("key").get_ref<const std::string&>();
        const std::string& rk = r.at("key").get_ref<const std::string&>();
        if (lk != rk)
            return lk < rk;
        const int li = l.count("index") ? l.at("index").get<int>() : -1;
        const int ri = r.count("index") ? r.at("index").get<int>() : -1;
        return li < ri;
    });
}

// One item per changed key {key, index?, label?, <ref_name>, <cur_name>, truncated?}, filtered on the
// pure key by 'only', sorted by key and index. 'flow_options' (print/filament): elements of these options
// are matched by flow mode in the reference config.
static json sw_slicer_diff_json(const std::vector<std::string>&               keys,
                                const DynamicPrintConfig&                     ref,
                                const DynamicPrintConfig&                     cur,
                                const char*                                   ref_name,
                                const char*                                   cur_name,
                                const std::optional<std::vector<std::string>>& only,
                                const std::vector<std::string>*               flow_options = nullptr,
                                ConfigFlowDomain                              flow_domain  = ConfigFlowDomain::Process)
{
    json items = json::array();
    for (const std::string& k : keys) {
        std::string pure;
        int         index = -1;
        sw_slicer_split_key(k, pure, index);
        const bool asked = only && std::find(only->begin(), only->end(), pure) != only->end();
        if (only && !asked)
            continue;
        int ref_index = index;
        if (index >= 0 && flow_options != nullptr &&
            std::find(flow_options->begin(), flow_options->end(), pure) != flow_options->end())
            ref_index = sw_slicer_flow_ref_index(ref, cur, flow_domain, index);
        bool truncated = false;
        json item;
        item["key"] = pure;
        if (index >= 0)
            item["index"] = index;
        if (const ConfigOptionDef* def = print_config_def.get(pure); def != nullptr && !def->label.empty())
            item["label"] = def->label;
        item[ref_name] = (index >= 0 && ref_index < 0) ? json(nullptr) : sw_slicer_option_value(ref, pure, ref_index, asked, truncated);
        item[cur_name] = sw_slicer_option_value(cur, pure, index, asked, truncated);
        if (truncated)
            item["truncated"] = true;
        items.push_back(item);
    }
    sw_slicer_sort_diff(items);
    return items;
}

// The changed keys of the edited preset of a type, chosen like UnsavedChangesDialog::update_tree():
// deep compare (per extruder, "key#i") for printers; for print/filament the flow-variant options per
// element ("key#i"). Against the saved preset or (against_parent) the parent preset. Read-only.
static std::vector<std::string> sw_slicer_changed_keys(const PresetCollection& c, Preset::Type type, bool against_parent)
{
    const bool               deep = (type == Preset::TYPE_PRINTER);
    std::vector<std::string> keys = against_parent ? c.current_different_from_parent_options(deep) : c.current_dirty_options(deep);
    ConfigFlowDomain                domain = ConfigFlowDomain::Process;
    const std::vector<std::string>* flow   = sw_slicer_flow_options(type, domain);
    if (flow != nullptr) {
        keys.erase(std::remove_if(keys.begin(), keys.end(),
                                  [flow](const std::string& k) { return std::find(flow->begin(), flow->end(), k) != flow->end(); }),
                   keys.end());
        const std::vector<std::string> flow_keys = against_parent ? c.current_flow_variant_different_from_parent_options(domain, *flow) :
                                                                    c.current_flow_variant_dirty_options(domain, *flow);
        keys.insert(keys.end(), flow_keys.begin(), flow_keys.end());
    }
    // Keys that PresetCollection::is_dirty() ignores; keep equal to skipped_in_dirty in Preset.cpp.
    static const std::set<std::string> skipped = {"printer_settings_id", "print_settings_id", "filament_settings_id",
                                                  "mixed_filament_definitions"};
    keys.erase(std::remove_if(keys.begin(), keys.end(),
                              [](const std::string& k) {
                                  std::string pure;
                                  int         index = -1;
                                  sw_slicer_split_key(k, pure, index);
                                  return skipped.count(pure) > 0;
                              }),
               keys.end());
    return keys;
}

// The pure option names of a list of changed keys, unique and sorted.
static std::vector<std::string> sw_slicer_pure_keys(const std::vector<std::string>& keys)
{
    std::set<std::string> out;
    for (const std::string& k : keys) {
        std::string pure;
        int         index = -1;
        sw_slicer_split_key(k, pure, index);
        out.insert(pure);
    }
    return std::vector<std::string>(out.begin(), out.end());
}

// The filament slots (1-based) that use a filament preset.
static std::vector<int> sw_slicer_slots_using(const std::string& name)
{
    std::vector<int>                slots;
    const std::vector<std::string>& presets = wxGetApp().preset_bundle->filament_presets;
    for (size_t i = 0; i < presets.size(); ++i)
        if (presets[i] == name)
            slots.push_back(static_cast<int>(i) + 1);
    return slots;
}

// Number of extruders of a printer config (length of the option), or -1.
static int sw_slicer_extruder_count(const DynamicPrintConfig& cfg, const char* key)
{
    const ConfigOption* opt = cfg.option(key);
    if (opt == nullptr || !opt->is_vector())
        return -1;
    return static_cast<int>(static_cast<const ConfigOptionVectorBase*>(opt)->size());
}

// Does any other preset of the collection inherit from 'name'?
static std::vector<std::string> sw_slicer_presets_inheriting(const PresetCollection& c, const std::string& name)
{
    std::vector<std::string> out;
    for (const Preset& p : c)
        if (p.inherits() == name)
            out.push_back(p.name);
    return out;
}

// Which settings of the selected presets are changed: not saved yet (against "saved", like the
// unsaved-changes dialog) or different from the parent preset (against "parent", like the markers in
// the settings tab). Read-only: nothing is selected, marked or saved.
void SSWCP_SliceProject_Instance::sw_SlicerGetPresetChanges()
{
    try {
        sw_slicer_require_fff();
        const bool                has_type = sw_slicer_has_param(m_param_data, "type");
        std::vector<Preset::Type> types;
        if (has_type)
            types.push_back(sw_slicer_preset_type(sw_slicer_type_param(m_param_data)));
        else
            types = {Preset::TYPE_PRINTER, Preset::TYPE_PRINT, Preset::TYPE_FILAMENT};
        std::string against = "saved";
        if (sw_slicer_has_param(m_param_data, "against")) {
            const json& j = m_param_data.at("against");
            against       = j.is_string() ? j.get<std::string>() : std::string();
            if (against != "saved" && against != "parent")
                throw std::runtime_error("against must be 'saved' or 'parent'");
        }
        const bool parent = against == "parent";
        if (sw_slicer_has_param(m_param_data, "keys") && !has_type)
            throw std::runtime_error("'keys' needs 'type'");
        const std::optional<std::vector<std::string>> only = sw_slicer_parse_keys(m_param_data);

        std::vector<std::string> notes;
        json                     presets   = json::array();
        const char*              ref_field = parent ? "parent" : "saved";
        for (Preset::Type t : types) {
            const PresetCollection& c        = sw_slicer_preset_collection(t);
            const Preset&           edited   = c.get_edited_preset();
            const Preset&           selected = c.get_selected_preset();
            const std::string       tname    = sw_slicer_preset_type_name(t);
            json                    e;
            e["type"]    = tname;
            e["name"]    = edited.name;
            e["dirty"]   = c.current_is_dirty();
            e["against"] = against;
            const Preset* ref = parent ? c.get_selected_preset_parent() : &selected;
            json          changes = json::array();
            if (ref == nullptr) {
                e["reference"] = nullptr;
                sw_slicer_add_note(notes, tname + " preset '" + edited.name + "' has no parent preset");
            } else {
                e["reference"] = ref->name;
                ConfigFlowDomain                domain = ConfigFlowDomain::Process;
                const std::vector<std::string>* flow   = sw_slicer_flow_options(t, domain);
                changes = sw_slicer_diff_json(sw_slicer_changed_keys(c, t, parent), ref->config, edited.config, ref_field, "current",
                                              only, flow, domain);
                // A changed number of extruders, as the unsaved-changes dialog shows it.
                if (t == Preset::TYPE_PRINTER) {
                    const int n = sw_slicer_extruder_count(ref->config, "nozzle_diameter");
                    const int m = sw_slicer_extruder_count(edited.config, "nozzle_diameter");
                    if (n >= 0 && m >= 0 && n != m &&
                        (!only || std::find(only->begin(), only->end(), "extruders_count") != only->end())) {
                        json item;
                        item["key"]     = "extruders_count";
                        item["label"]   = "Extruders count";
                        item[ref_field] = n;
                        item["current"] = m;
                        changes.push_back(item);
                        sw_slicer_sort_diff(changes);
                    }
                }
            }
            e["preset"]  = sw_slicer_preset_info_json(selected);
            e["changes"] = changes;
            if (t == Preset::TYPE_FILAMENT) {
                const std::vector<int> slots = sw_slicer_slots_using(edited.name);
                e["slots"]                   = slots;
                if (slots.empty())
                    sw_slicer_add_note(notes, "the filament settings being edited ('" + edited.name + "') are not used by any slot");
            }
            presets.push_back(e);
        }
        m_res_data["presets"]       = presets;
        m_res_data["presets_dirty"] = sw_slicer_presets_dirty();
        m_res_data["notes"]         = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Compare two presets of one type, like "Compare presets" (DiffPresetDialog::update_tree): names mean
// the saved presets, also for the selected one. Without 'b', 'a' is compared with the edited settings
// (including unsaved changes). Read-only.
void SSWCP_SliceProject_Instance::sw_SlicerComparePresets()
{
    try {
        sw_slicer_require_fff();
        const std::string  type = sw_slicer_type_param(m_param_data);
        const Preset::Type t    = sw_slicer_preset_type(type);
        if (!sw_slicer_has_param(m_param_data, "a") || !m_param_data.at("a").is_string() || m_param_data.at("a").get<std::string>().empty())
            throw std::runtime_error("specify 'a' (see sw_SlicerListPresets)");
        const std::string a     = m_param_data.at("a").get<std::string>();
        const bool        has_b = sw_slicer_has_param(m_param_data, "b");
        if (has_b && (!m_param_data.at("b").is_string() || m_param_data.at("b").get<std::string>().empty()))
            throw std::runtime_error("'b' must be a preset name; omit it to compare with the unsaved changes");
        const std::string b = has_b ? m_param_data.at("b").get<std::string>() : std::string();
        if (has_b && a == b)
            throw std::runtime_error("'a' and 'b' are the same preset; omit 'b' to compare with the unsaved changes");
        const std::optional<std::vector<std::string>> only = sw_slicer_parse_keys(m_param_data);

        // find_preset(..., real = true): the saved preset, not the edited copy of the selected one.
        PresetCollection& c  = sw_slicer_preset_collection(t);
        const Preset*     pa = c.find_preset(a, false, true);
        if (pa == nullptr || pa->is_default)
            throw std::runtime_error("preset '" + a + "' does not exist");
        const Preset* pb = nullptr;
        if (has_b) {
            pb = c.find_preset(b, false, true);
            if (pb == nullptr || pb->is_default)
                throw std::runtime_error("preset '" + b + "' does not exist");
        } else {
            pb = &c.get_edited_preset();
        }
        if (pa->printer_technology() != pb->printer_technology())
            throw std::runtime_error("the presets are for different printer technologies");

        // The keys as in DiffPresetDialog::update_tree(); for print/filament the flow-variant options per
        // element (b is the "edited" side, as in sw_slicer_changed_keys). The *_settings_id, inherits and
        // compatible_* differences are kept: the dialog shows them too.
        const bool deep         = (t == Preset::TYPE_PRINTER);
        const int  a_extruders  = sw_slicer_extruder_count(pa->config, "extruder_colour");
        const int  b_extruders  = sw_slicer_extruder_count(pb->config, "extruder_colour");
        std::vector<std::string> keys = (t == Preset::TYPE_PRINTER && a_extruders < b_extruders) ?
                                            PresetCollection::dirty_options(pb, pa, deep) :
                                            PresetCollection::dirty_options(pa, pb, deep);
        ConfigFlowDomain                domain = ConfigFlowDomain::Process;
        const std::vector<std::string>* flow   = sw_slicer_flow_options(t, domain);
        // Without the flow-support option flow_variant_dirty_options() reports nothing: keep the plain keys.
        if (flow != nullptr && pb->config.option(flow_support_key(domain)) != nullptr) {
            keys.erase(std::remove_if(keys.begin(), keys.end(),
                                      [flow](const std::string& k) { return std::find(flow->begin(), flow->end(), k) != flow->end(); }),
                       keys.end());
            const std::vector<std::string> flow_keys = PresetCollection::flow_variant_dirty_options(pb, pa, domain, *flow);
            keys.insert(keys.end(), flow_keys.begin(), flow_keys.end());
        }
        json differences = sw_slicer_diff_json(keys, pa->config, pb->config, "a", "b", only, flow, domain);
        size_t count = keys.size();
        if (t == Preset::TYPE_PRINTER && a_extruders >= 0 && b_extruders >= 0 && a_extruders != b_extruders) {
            ++count;
            if (!only || std::find(only->begin(), only->end(), "extruders_count") != only->end()) {
                json item;
                item["key"]   = "extruders_count";
                item["label"] = "Extruders count";
                item["a"]     = a_extruders;
                item["b"]     = b_extruders;
                differences.push_back(item);
                sw_slicer_sort_diff(differences);
            }
        }

        std::vector<std::string> notes;
        const std::string        selected = c.get_selected_preset_name();
        const bool               dirty    = c.current_is_dirty();
        json                     ja       = sw_slicer_preset_info_json(*pa);
        json                     jb       = sw_slicer_preset_info_json(*pb);
        if (!has_b) {
            jb["edited"] = true;
            jb["dirty"]  = dirty;
            if (a == selected && !dirty)
                sw_slicer_add_note(notes, "no unsaved changes; the edited state equals '" + a + "'");
        }
        if (t != Preset::TYPE_PRINTER) {
            if (!pa->is_compatible)
                sw_slicer_add_note(notes, "'" + a + "' is not compatible with the selected printer (compared anyway)");
            if (has_b && !pb->is_compatible)
                sw_slicer_add_note(notes, "'" + b + "' is not compatible with the selected printer (compared anyway)");
        }
        if (has_b && dirty) {
            if (a == selected)
                sw_slicer_add_note(notes, "'" + a + "' is compared as saved; it has unsaved changes (omit 'b' to compare with them)");
            if (b == selected)
                sw_slicer_add_note(notes, "'" + b + "' is compared as saved; it has unsaved changes (pass it as 'a' and omit 'b' to compare with them)");
        }

        m_res_data["type"]        = type;
        m_res_data["a"]           = ja;
        m_res_data["b"]           = jb;
        m_res_data["equal"]       = count == 0;
        m_res_data["count"]       = count;
        m_res_data["differences"] = differences;
        m_res_data["notes"]       = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Why a name cannot be used to save a preset ("" = valid): the rules of the save dialog, in its order
// (SavePresetDialog::Item::update()), plus API rules for the file name (path_from_name()): no ".json"
// suffix, no control characters, no Windows device name. An existing user preset is not an error here;
// the caller asks for overwrite:true.
static std::string sw_slicer_preset_name_error(const PresetCollection& c, const std::string& name)
{
    if (name.find_first_of("<>[]:/\\|?*\"") != std::string::npos)
        return "the name contains an illegal character (one of <>[]:/\\|?*\")";
    if (name.find(PresetCollection::get_suffix_modified()) != std::string::npos || name.find("(modified)") != std::string::npos)
        return "the name must not contain '(modified)'";
    if (name == "Default Setting" || name == "Default Filament" || name == "Default Printer")
        return "this name is reserved";
    const Preset* existing = c.find_preset(name, false);
    if (existing != nullptr && (existing->is_default || existing->is_system))
        return "overwriting a system preset is not allowed";
    if (name.empty())
        return "the name must not be empty";
    if (name.front() == ' ' || name.back() == ' ')
        return "the name must not start or end with a space";
    if (c.get_preset_name_by_alias(name) != name)
        return "the name equals the alias of a system preset";
    if (boost::algorithm::iends_with(name, ".json"))
        return "the name must not end in .json";
    if (std::any_of(name.begin(), name.end(), [](char ch) { return static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F; }))
        return "the name must not contain control characters";
    // The file is "<name>.json": Windows reserves these names, also with any extension.
    static const char* const devices[] = {"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
                                          "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    const std::string stem = boost::algorithm::trim_right_copy(name.substr(0, name.find('.')));
    for (const char* d : devices)
        if (boost::algorithm::iequals(stem, d))
            return "the name is reserved by Windows (" + std::string(d) + ")";
    return std::string();
}

// Save the edited settings of a type as a user preset, like the save icon of the settings tab with a
// name typed in the save dialog (Tab::save_preset with a name: no dialog). An existing user preset is
// replaced only with overwrite:true; system presets never. Not part of undo.
void SSWCP_SliceProject_Instance::sw_SlicerSavePreset()
{
    try {
        // 1. Guards.
        sw_slicer_require_idle();
        sw_slicer_require_not_slicing();
        sw_slicer_require_not_loading();
        sw_slicer_require_fff();

        // 2. Parameters.
        const std::string  type = sw_slicer_type_param(m_param_data);
        const Preset::Type t    = sw_slicer_preset_type(type);
        if (!sw_slicer_has_param(m_param_data, "name") || !m_param_data.at("name").is_string() ||
            m_param_data.at("name").get<std::string>().empty())
            throw std::runtime_error("specify 'name'");
        const std::string name      = m_param_data.at("name").get<std::string>();
        const bool        overwrite = sw_slicer_flag_true(m_param_data, "overwrite");
        PresetBundle*     bundle    = wxGetApp().preset_bundle;
        int               slot      = 0;
        if (sw_slicer_has_param(m_param_data, "filament")) {
            if (t != Preset::TYPE_FILAMENT)
                throw std::runtime_error("'filament' only applies to type 'filament'");
            const json&   j     = m_param_data.at("filament");
            const size_t  count = bundle->filament_presets.size();
            if (!j.is_number_integer() || j.get<long long>() < 1 || j.get<long long>() > static_cast<long long>(count))
                throw std::runtime_error("filament slot must be between 1 and " + std::to_string(count));
            slot = j.get<int>();
        }

        // 3. Name rules of the save dialog.
        PresetCollection& c   = sw_slicer_preset_collection(t);
        const std::string err = sw_slicer_preset_name_error(c, name);
        if (!err.empty())
            throw std::runtime_error(err);

        // 4. Physical printers are not supported (the save dialog would update them).
        if (t == Preset::TYPE_PRINTER && bundle->physical_printers.has_selection())
            throw std::runtime_error("a physical printer is selected; save printer presets in the app");

        // 5. No cloud traffic.
        sw_slicer_require_no_cloud_sync("saving a preset");

        // 6. Filament slot: only a check, nothing is selected.
        std::vector<std::string> notes;
        const std::string        edited_name = c.get_edited_preset().name;
        if (slot > 0) {
            const std::string& in_slot = bundle->filament_presets[static_cast<size_t>(slot - 1)];
            if (in_slot != edited_name)
                throw std::runtime_error("slot " + std::to_string(slot) + " uses '" + in_slot + "'; the filament settings being edited belong to '" +
                                         edited_name + "' (select that preset in slot " + std::to_string(slot) + " first)");
        } else if (t == Preset::TYPE_FILAMENT) {
            const std::vector<int> used = sw_slicer_slots_using(edited_name);
            if (used.size() > 1)
                sw_slicer_add_note(notes, "the edited filament preset is used by slots " + sw_slicer_join_ints(used) +
                                              "; all of them follow the new name");
        }

        // 7. An existing preset with this name. File names ignore case on Windows: a preset whose name
        // differs only in case, or whose file is the file of the new preset, counts as the same preset
        // (saving would overwrite its file). A preset file that is not loaded is only replaced with overwrite.
        const std::string selected    = c.get_selected_preset_name();
        Preset*           existing    = c.find_preset(name, false, /*real=*/true);
        bool              orphan_file = false;
        if (existing == nullptr) {
            const std::string path = c.path_from_name(name, /*detach=*/false);
            for (Preset& p : c)
                if (boost::algorithm::iequals(p.name, name) || (!p.file.empty() && boost::algorithm::iequals(p.file, path))) {
                    existing = &p;
                    break;
                }
            if (existing != nullptr) {
                if (existing->is_default || existing->is_system)
                    throw std::runtime_error("overwriting a system preset is not allowed (names differ only in case: '" + existing->name + "')");
                if (existing->name == selected)
                    throw std::runtime_error("the name differs only in case from the selected preset '" + selected +
                                             "'; use that exact name to save it");
            } else {
                boost::system::error_code ec;
                orphan_file = boost::filesystem::exists(sw_slicer_path_from_utf8(path), ec);
            }
        }
        const std::string existing_name = existing != nullptr ? existing->name : std::string();
        const bool        case_variant  = existing != nullptr && existing_name != name;
        if (existing != nullptr && existing->is_project_embedded)
            throw std::runtime_error("'" + existing_name + "' is a preset stored in the project; the API only saves user presets");
        if (existing != nullptr && !overwrite)
            throw std::runtime_error(case_variant ? "a preset named '" + existing_name +
                                                        "' already exists (names differ only in case); pass overwrite:true to replace it" :
                                                    "preset '" + name + "' already exists; pass overwrite:true to replace it");
        if (orphan_file && !overwrite)
            throw std::runtime_error("a preset file with this name already exists in the preset folder; pass overwrite:true to replace it");
        if (existing != nullptr && existing_name != selected) {
            if (!sw_slicer_presets_inheriting(c, existing_name).empty())
                throw std::runtime_error("other presets inherit from '" + existing_name + "'; it cannot be replaced");
            if (!existing->setting_id.empty())
                throw std::runtime_error("'" + existing_name + "' is linked to a cloud copy; replace it in the app");
            if (t == Preset::TYPE_FILAMENT) {
                const std::vector<int> used = sw_slicer_slots_using(existing_name);
                // The slots keep the old name: with another spelling they would point to a deleted preset.
                if (!used.empty() && case_variant)
                    throw std::runtime_error("'" + existing_name + "' is used by filament slot(s) " + sw_slicer_join_ints(used) +
                                             "; save it under its exact name");
                if (!used.empty())
                    sw_slicer_add_note(notes, "slot(s) " + sw_slicer_join_ints(used) + " use '" + existing_name + "' and now get the new values");
            }
            if (!existing->is_compatible)
                sw_slicer_add_note(notes, "'" + existing_name + "' was not compatible with the selected printer; it is replaced by a compatible preset");
        }

        auto reply = [&](bool created, bool replaced, const std::vector<std::string>& saved_changes) {
            m_res_data["type"]          = type;
            m_res_data["name"]          = name;
            m_res_data["previous"]      = selected;
            m_res_data["created"]       = created;
            m_res_data["replaced"]      = replaced;
            m_res_data["preset"]        = sw_slicer_preset_info_json(c.get_selected_preset());
            m_res_data["saved_changes"] = saved_changes;
            if (t == Preset::TYPE_FILAMENT)
                m_res_data["slots"] = sw_slicer_slots_using(c.get_selected_preset_name());
            m_res_data["project_dirty"] = sw_slicer_project_dirty();
            m_res_data["notes"]         = notes;
            send_to_js();
            finish_job();
        };

        // 8. Nothing to save.
        if (name == selected && !c.current_is_dirty()) {
            sw_slicer_add_note(notes, "no unsaved changes; nothing changed");
            reply(false, false, {});
            return;
        }

        // 9. Before saving. 'existing' is not used after the save: deleting and saving change the deque.
        const bool                     had_existing       = existing != nullptr;
        const std::vector<std::string> saved_changes      = sw_slicer_pure_keys(sw_slicer_changed_keys(c, t, false));
        const bool                     edited_from_system = c.get_edited_preset().is_system;
        Tab*                           tab                = sw_slicer_preset_tab(t);

        // 10. Save.
        std::vector<std::string> api_notes;
        {
            sswcp_api::Scope api_scope;
            // Replacing another preset works like the save dialog: delete it first, then save the edited
            // settings under its name (SavePresetDialog::Item::accept). No cloud copy exists (checked above).
            if (existing != nullptr && existing_name != selected) {
                existing->sync_info = "delete";
                c.delete_preset(existing_name);
            }
            existing = nullptr;
            tab->save_preset(name, /*detach=*/false, /*save_to_project=*/false);
            api_notes = sswcp_api::notes();
        }

        // 11. Check. Settings checks during the refresh can adjust values after the file was written: then
        // the save succeeded, but the preset shows unsaved changes.
        boost::system::error_code ec;
        const Preset&             saved = c.get_selected_preset();
        if (c.get_selected_preset_name() != name ||
            (saved.is_user() && !boost::filesystem::exists(sw_slicer_path_from_utf8(saved.file), ec)))
            throw std::runtime_error(sw_slicer_join_notes("the preset could not be saved", api_notes));
        if (c.current_is_dirty())
            sw_slicer_add_note(notes, "values were adjusted after saving; the preset shows unsaved changes");

        // 12./13. Reply.
        const bool created = !had_existing || name != selected;
        if (orphan_file)
            sw_slicer_add_note(notes, "an existing preset file with this name (not loaded in the app) was replaced");
        if (!had_existing)
            sw_slicer_add_note(notes, "saved as a new user preset and selected; '" + selected + "' is unchanged");
        else if (name == selected)
            sw_slicer_add_note(notes, "the stored values of '" + name + "' were replaced; the old values cannot be restored (presets are not part of undo)");
        else if (case_variant)
            sw_slicer_add_note(notes, "preset '" + existing_name + "' was replaced by '" + name + "' and selected; its old values cannot be restored");
        else
            sw_slicer_add_note(notes, "preset '" + name + "' was replaced and selected; its old values cannot be restored");
        if (name != selected)
            sw_slicer_add_note(notes, "presets are not part of undo; select 'previous' to switch back");
        if (t == Preset::TYPE_FILAMENT && edited_from_system) {
            const std::string printer = bundle->printers.get_selected_preset_base().name;
            const auto*       compat  = saved.config.option<ConfigOptionStrings>("compatible_printers");
            if (compat != nullptr && compat->values.size() == 1 && compat->values.front() == printer)
                sw_slicer_add_note(notes, "the new filament preset is only compatible with printer '" + printer + "' (as in the app)");
        }
        sw_slicer_add_note(notes, "saved locally only (user preset cloud sync is off)");
        if (wxGetApp().is_user_login())
            sw_slicer_add_note(notes, "a cloud account is logged in; the preset is marked for upload if preset sync is turned on later");
        sw_slicer_add_notes(notes, api_notes);
        reply(created, had_existing || orphan_file, saved_changes);
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Delete a user preset that is not selected, like deleting a preset in the filament manager
// (delete_filament_preset_by_name): the preset files are removed. System presets, the selected preset,
// presets used by a filament slot or inherited by other presets, custom printer roots, presets of a
// physical printer and presets linked to a cloud copy are refused. Not part of undo.
void SSWCP_SliceProject_Instance::sw_SlicerDeletePreset()
{
    try {
        // 1. Guards.
        sw_slicer_require_idle();
        sw_slicer_require_not_slicing();
        sw_slicer_require_not_loading();
        sw_slicer_require_fff();

        // 2. Explicit confirmation.
        if (!sw_slicer_flag_true(m_param_data, "confirm_delete"))
            throw std::runtime_error("pass confirm_delete:true to delete a preset (this cannot be undone)");

        // 3. The preset.
        const std::string  type = sw_slicer_type_param(m_param_data);
        const Preset::Type t    = sw_slicer_preset_type(type);
        if (!sw_slicer_has_param(m_param_data, "name") || !m_param_data.at("name").is_string() ||
            m_param_data.at("name").get<std::string>().empty())
            throw std::runtime_error("specify 'name'");
        const std::string name   = m_param_data.at("name").get<std::string>();
        PresetBundle*     bundle = wxGetApp().preset_bundle;
        PresetCollection& c      = sw_slicer_preset_collection(t);
        const Preset*     p      = c.find_preset(name, false, /*real=*/true);
        if (p == nullptr)
            throw std::runtime_error("preset '" + name + "' does not exist");

        // 4. Refusals.
        if (p->is_default || p->is_system)
            throw std::runtime_error("system presets cannot be deleted");
        if (p->is_project_embedded)
            throw std::runtime_error("presets stored in the project cannot be deleted through the API");
        if (name == c.get_selected_preset_name())
            throw std::runtime_error("'" + name + "' is the selected preset; select another preset first (sw_SlicerSelectPreset)");
        if (t == Preset::TYPE_FILAMENT) {
            const std::vector<int> used = sw_slicer_slots_using(name);
            if (!used.empty())
                throw std::runtime_error("'" + name + "' is used by filament slot(s) " + sw_slicer_join_ints(used) +
                                         "; select another filament there first");
        }
        const std::vector<std::string> heirs = sw_slicer_presets_inheriting(c, name);
        if (!heirs.empty()) {
            std::string list;
            for (const std::string& h : heirs)
                list += (list.empty() ? "" : ", ") + h;
            throw std::runtime_error("other presets inherit from '" + name + "': " + list);
        }
        if (t == Preset::TYPE_PRINTER && c.get_preset_base(*p) == p)
            throw std::runtime_error("'" + name +
                                     "' is a custom printer; deleting it would also delete its filament and process presets; delete it in the app");
        if (t == Preset::TYPE_PRINTER && !bundle->physical_printers.get_printers_with_preset(name).empty())
            throw std::runtime_error("'" + name + "' is used by a physical printer; delete it in the app");
        if (!p->setting_id.empty())
            throw std::runtime_error("'" + name + "' is linked to a cloud copy; delete it in the app");
        sw_slicer_require_no_cloud_sync("deleting a preset");
        if (c.current_is_dirty())
            throw std::runtime_error("the " + type + " settings have unsaved changes; save or discard them first");
        // select_preset_by_name() would pick another preset if the selected one is hidden.
        if (!c.get_selected_preset().is_visible)
            throw std::runtime_error("the selected " + type + " preset is hidden; select a visible preset first");

        // 5.
        const std::string selected = c.get_selected_preset_name();
        const std::string file     = p->file;
        Tab*              tab      = sw_slicer_preset_tab(t);
        p                          = nullptr; // the deque changes below

        // 6. Delete.
        std::vector<std::string> notes;
        {
            sswcp_api::Scope api_scope;
            // Like deleting a preset from the filament manager (delete_filament_preset_by_name): remove the
            // files and the entry, then re-select the selected preset by name, because delete_preset() does
            // not adjust the selected index. There are no unsaved edits (checked above).
            c.delete_preset(name);
            c.select_preset_by_name(selected, /*force=*/true);
            bundle->update_compatible(PresetSelectCompatibleType::Never);
            tab->update_tab_ui();
            wxGetApp().plater()->sidebar().update_presets(t);
            wxGetApp().mainframe->diff_dialog.update_presets(t);
            bundle->export_selections(*wxGetApp().app_config);
            notes = sswcp_api::notes();
        }

        // 7. Check.
        boost::system::error_code ec;
        if (c.find_preset(name, false, true) != nullptr || c.get_selected_preset_name() != selected ||
            (!file.empty() && boost::filesystem::exists(sw_slicer_path_from_utf8(file), ec)))
            throw std::runtime_error(sw_slicer_join_notes("the preset could not be deleted", notes));

        // 8. Reply.
        std::vector<std::string> out;
        sw_slicer_add_note(out, "the preset files were removed; this cannot be undone (presets are not part of undo)");
        sw_slicer_add_notes(out, notes);
        m_res_data["type"]     = type;
        m_res_data["name"]     = name;
        m_res_data["deleted"]  = true;
        m_res_data["file"]     = file;
        m_res_data["selected"] = selected;
        m_res_data["notes"]    = out;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Discard unsaved preset changes: all (the "Discard" button of the unsaved-changes dialog), of one type,
// or single options of one type (back to the saved preset). Not part of undo.
void SSWCP_SliceProject_Instance::sw_SlicerDiscardPresetChanges()
{
    try {
        // 1. Guards. No cloud check: discarding writes nothing to disk.
        sw_slicer_require_idle();
        sw_slicer_require_not_slicing();
        sw_slicer_require_not_loading();
        sw_slicer_require_fff();

        // 2. Parameters.
        const bool has_type = sw_slicer_has_param(m_param_data, "type");
        if (sw_slicer_has_param(m_param_data, "keys") && !has_type)
            throw std::runtime_error("'keys' needs 'type'");
        std::vector<Preset::Type> types;
        if (has_type)
            types.push_back(sw_slicer_preset_type(sw_slicer_type_param(m_param_data)));
        else
            types = {Preset::TYPE_PRINTER, Preset::TYPE_PRINT, Preset::TYPE_FILAMENT};
        const std::optional<std::vector<std::string>> keys = sw_slicer_parse_keys(m_param_data);

        // 3. What is changed now, per type.
        json discarded = json::array();
        for (Preset::Type t : types) {
            const PresetCollection& c = sw_slicer_preset_collection(t);
            if (!c.current_is_dirty())
                continue;
            json e;
            e["type"] = sw_slicer_preset_type_name(t);
            e["name"] = c.get_edited_preset().name;
            e["keys"] = sw_slicer_pure_keys(sw_slicer_changed_keys(c, t, false));
            discarded.push_back(e);
        }

        std::vector<std::string> notes;
        auto reply = [&]() {
            m_res_data["discarded"]     = discarded;
            m_res_data["presets_dirty"] = sw_slicer_presets_dirty();
            m_res_data["notes"]         = notes;
            send_to_js();
            finish_job();
        };

        // 4. Nothing to discard.
        if (discarded.empty()) {
            sw_slicer_add_note(notes, "no unsaved preset changes; nothing changed");
            reply();
            return;
        }

        std::vector<std::string> api_notes;
        if (!has_type) {
            // 5. All types, exactly the "Discard" button.
            sswcp_api::Scope api_scope;
            sw_slicer_discard_preset_changes();
            api_notes = sswcp_api::notes();
        } else if (!keys) {
            // 6. One type, like GUI_App::check_and_keep_current_preset_changes() for that type only. The
            // other tabs reload their edited presets, so their unsaved changes stay.
            Tab*             tab = sw_slicer_preset_tab(types.front());
            sswcp_api::Scope api_scope;
            tab->get_presets()->discard_current_changes();
            wxGetApp().load_current_presets(false);
            api_notes = sswcp_api::notes();
        } else {
            // 7. Single options of one type.
            const Preset::Type       t       = types.front();
            PresetCollection&        c       = sw_slicer_preset_collection(t);
            const std::vector<std::string> changed = discarded.front().at("keys").get<std::vector<std::string>>();
            if (std::find(keys->begin(), keys->end(), "extruders_count") != keys->end())
                throw std::runtime_error("'extruders_count' cannot be discarded on its own; discard all printer changes instead (omit 'keys')");
            for (const std::string& k : *keys)
                if (std::find(changed.begin(), changed.end(), k) == changed.end())
                    throw std::runtime_error("'" + k + "' has no unsaved change");
            if (t == Preset::TYPE_PRINTER && sw_slicer_extruder_count(c.get_selected_preset().config, "nozzle_diameter") !=
                                                 sw_slicer_extruder_count(c.get_edited_preset().config, "nozzle_diameter"))
                throw std::runtime_error("the number of extruders was changed; discard all printer changes instead (omit 'keys')");
            // The flow-variant vectors follow the *_flow_support list: reverting one of them without the
            // others would leave vectors that do not match the declared variants.
            ConfigFlowDomain                domain = ConfigFlowDomain::Process;
            const std::vector<std::string>* flow   = sw_slicer_flow_options(t, domain);
            if (flow != nullptr) {
                const std::string   support_key = flow_support_key(domain);
                const ConfigOption* saved_sup   = c.get_selected_preset().config.option(support_key);
                const ConfigOption* edited_sup  = c.get_edited_preset().config.option(support_key);
                const bool          changed_sup = (saved_sup == nullptr) != (edited_sup == nullptr) ||
                                         (saved_sup != nullptr && edited_sup != nullptr && !(*saved_sup == *edited_sup));
                const bool touches_flow = std::any_of(keys->begin(), keys->end(), [&](const std::string& k) {
                    return k == support_key || std::find(flow->begin(), flow->end(), k) != flow->end();
                });
                if (changed_sup && touches_flow)
                    throw std::runtime_error("the flow variants were changed; discard all " + sw_slicer_preset_type_name(t) +
                                             " changes instead (omit 'keys')");
            }
            Tab*             tab = sw_slicer_preset_tab(t);
            sswcp_api::Scope api_scope;
            // Revert single options to the saved preset, like the per-option arrow in the settings tab,
            // without Tab::load_config (which runs the full config update).
            DynamicPrintConfig&       edited = c.get_edited_preset().config;
            const DynamicPrintConfig& saved  = c.get_selected_preset().config;
            for (const std::string& k : *keys) {
                if (const ConfigOption* o = saved.option(k))
                    edited.set_key_value(k, o->clone());
                else
                    edited.erase(k);
            }
            tab->update_dirty();
            tab->reload_config();
            api_notes = sswcp_api::notes();
            discarded.front()["keys"] = *keys;
            sw_slicer_add_note(notes, "options reverted without the full settings update (like sw_SlicerSetConfig); dependent options are re-checked at the next slice");
        }

        // 8. Reply.
        sw_slicer_add_note(notes, "the discarded values cannot be restored (presets are not part of undo)");
        sw_slicer_add_notes(notes, api_notes);
        reply();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// --- Calibration: list, create, mapping, apply ---------------------------------
// The test projects of the Calibration menu without its dialogs. The API fills Calib_Params itself, with
// the defaults and input rules of the dialogs (calib_dlg.cpp), and calls the same Plater::calib_* functions.
// A test changes the edited presets (not saved); the API records these changes, so that
// sw_SlicerApplyCalibration can restore them before it sets the chosen value. Nothing here sends anything
// to a printer: slicing, sending and printing stay separate commands.

namespace {
// One parameter of a test. kind: 'n' number, 'i' list of whole numbers, 'b' bool, 'e' choice.
struct SwCalibParamDef
{
    const char*              name;
    char                     kind;
    double                   def;
    std::vector<const char*> choices;
    const char*              unit;
};

// One test of the Calibration menu. result_type == nullptr: there is no slicer setting to apply.
struct SwCalibTestDef
{
    const char*                  id;
    const char*                  menu;
    CalibMode                    mode;
    std::vector<SwCalibParamDef> params;
    const char*                  rules;
    const char*                  result_type;
    std::vector<const char*>     result_keys;
};

// What the last sw_SlicerCalibrate did (memory only). Only names, values and the model id are kept, no
// pointers into the model or the presets.
struct SwCalibRecord
{
    struct Change
    {
        Preset::Type                  type;
        std::string                   key;
        std::unique_ptr<ConfigOption> before; // nullptr: the key was missing
        std::unique_ptr<ConfigOption> after;  // nullptr: the key was removed
    };
    std::string         test;
    json                params;
    std::string         project_name;
    std::vector<size_t> object_ids; // ids of the test objects right after creation
    std::string         presets[3]; // printer, print, filament
    double              flow_ratio = 0; // filament_flow_ratio[0] before the test
    std::vector<Change> changes;
};
} // namespace

static const Preset::Type sw_slicer_calib_types[3] = {Preset::TYPE_PRINTER, Preset::TYPE_PRINT, Preset::TYPE_FILAMENT};

// API limits that the dialogs do not have.
static const double SW_SLICER_CALIB_MAX_STEPS = 100;
static const size_t SW_SLICER_CALIB_MAX_LIST  = 10;

// The tests of the Calibration menu (MainFrame.cpp), with the defaults of their dialogs (calib_dlg.cpp).
// Defaults that depend on the extruder type or the material: sw_slicer_calib_default().
static const std::vector<SwCalibTestDef>& sw_slicer_calib_tests()
{
    static const std::vector<const char*> extruders = {"dde", "bowden"};
    static const std::vector<const char*> materials = {"pla", "abs_asa", "petg", "pctg", "tpu", "pa_cf", "pet_cf"};
    static const std::vector<const char*> models    = {"ringing_tower", "fast_tower"};
    static const std::vector<const char*> pa_keys   = {"pressure_advance", "enable_pressure_advance"};
    static const std::vector<const char*> flow_keys = {"filament_flow_ratio"};
    static const char* const              flow_rules = "no parameters";
    static const std::vector<SwCalibTestDef> tests = {
        {"pa_tower", "Pressure advance > PA Tower", CalibMode::Calib_PA_Tower,
         {{"extruder", 'e', 0, extruders, ""}, {"start", 'n', 0.0, {}, ""}, {"end", 'n', 0.1, {}, ""}, {"step", 'n', 0.002, {}, ""}},
         "start >= 0, end >= start + step, step >= 0.0001, at most 100 steps", "filament", pa_keys},
        {"pa_line", "Pressure advance > PA Line", CalibMode::Calib_PA_Line,
         {{"extruder", 'e', 0, extruders, ""},
          {"start", 'n', 0.0, {}, ""},
          {"end", 'n', 0.1, {}, ""},
          {"step", 'n', 0.002, {}, ""},
          {"print_numbers", 'b', 1, {}, ""}},
         "start >= 0, end >= start + step, step >= 0.0001, at most 100 lines", "filament", pa_keys},
        {"pa_pattern", "Pressure advance > PA Pattern", CalibMode::Calib_PA_Pattern,
         {{"extruder", 'e', 0, extruders, ""},
          {"start", 'n', 0.0, {}, ""},
          {"end", 'n', 0.08, {}, ""},
          {"step", 'n', 0.005, {}, ""},
          {"accelerations", 'i', 0, {}, "mm/s²"},
          {"speeds", 'i', 0, {}, "mm/s"}},
         "start >= 0, end >= start + step, step >= 0.0001, at most 100 patterns; accelerations and speeds: at most 10 "
         "positive whole numbers each (empty = the app's default)",
         "filament", pa_keys},
        {"flow_pass1", "Flow rate > Pass 1", CalibMode::Calib_None, {}, flow_rules, "filament", flow_keys},
        {"flow_pass2", "Flow rate > Pass 2", CalibMode::Calib_None, {}, flow_rules, "filament", flow_keys},
        {"flow_yolo", "Flow rate > YOLO (Recommended)", CalibMode::Calib_None, {}, flow_rules, "filament", flow_keys},
        {"flow_yolo_perfectionist", "Flow rate > YOLO (perfectionist version)", CalibMode::Calib_None, {}, flow_rules, "filament", flow_keys},
        {"temp_tower", "Temperature", CalibMode::Calib_Temp_Tower,
         {{"material", 'e', 0, materials, ""}, {"start", 'n', 230, {}, "°C"}, {"end", 'n', 190, {}, "°C"}, {"step", 'n', 5, {}, "°C"}},
         "start <= 350, end >= 170, end <= start - 5, multiples of 5, step is always 5", "filament", {"nozzle_temperature"}},
        {"max_flowrate", "Max flowrate", CalibMode::Calib_Vol_speed_Tower,
         {{"start", 'n', 5, {}, "mm³/s"}, {"end", 'n', 20, {}, "mm³/s"}, {"step", 'n', 0.5, {}, "mm³/s"}},
         "start > 0, step > 0, end >= start + step", "filament", {"filament_max_volumetric_speed"}},
        {"retraction", "Retraction test", CalibMode::Calib_Retraction_tower,
         {{"start", 'n', 0, {}, "mm"}, {"end", 'n', 2, {}, "mm"}, {"step", 'n', 0.1, {}, "mm"}},
         "start >= 0, step > 0, end >= start + step", "filament", {"filament_retraction_length"}},
        {"vfa", "VFA", CalibMode::Calib_VFA_Tower,
         {{"start", 'n', 40, {}, "mm/s"}, {"end", 'n', 200, {}, "mm/s"}, {"step", 'n', 10, {}, "mm/s"}},
         "start > 10, step > 0, end >= start + step", nullptr, {}},
        {"input_shaping_freq", "Input Shaping > Frequency", CalibMode::Calib_Input_shaping_freq,
         {{"model", 'e', 0, models, ""},
          {"freq_start_x", 'n', 15, {}, "Hz"},
          {"freq_end_x", 'n', 110, {}, "Hz"},
          {"freq_start_y", 'n', 15, {}, "Hz"},
          {"freq_end_y", 'n', 110, {}, "Hz"},
          {"damping", 'n', 0.15, {}, ""}},
         "0 < freq_start < freq_end <= 500 per axis, 0 <= damping < 1", nullptr, {}},
        {"input_shaping_damp", "Input Shaping > Damping/zeta factor", CalibMode::Calib_Input_shaping_damp,
         {{"model", 'e', 0, models, ""},
          {"freq_x", 'n', 30, {}, "Hz"},
          {"freq_y", 'n', 30, {}, "Hz"},
          {"damping_start", 'n', 0.0, {}, ""},
          {"damping_end", 'n', 0.4, {}, ""}},
         "0 <= freq_x, freq_y <= 500, 0 <= damping_start < damping_end <= 1", nullptr, {}},
        {"junction_deviation", "Cornering > Junction Deviation", CalibMode::Calib_Junction_Deviation,
         {{"model", 'e', 0, models, ""}, {"start", 'n', 0.0, {}, "mm"}, {"end", 'n', 0.25, {}, "mm"}},
         "0 <= start < end < 1; end > 0.3 may cause layer shift", "printer", {"machine_max_junction_deviation"}},
    };
    return tests;
}

static std::string sw_slicer_calib_mode_name(CalibMode mode)
{
    switch (mode) {
    case CalibMode::Calib_PA_Line:            return "pa_line";
    case CalibMode::Calib_PA_Pattern:         return "pa_pattern";
    case CalibMode::Calib_PA_Tower:           return "pa_tower";
    case CalibMode::Calib_Flow_Rate:          return "flow_rate";
    case CalibMode::Calib_Temp_Tower:         return "temp_tower";
    case CalibMode::Calib_Vol_speed_Tower:    return "max_flowrate";
    case CalibMode::Calib_VFA_Tower:          return "vfa";
    case CalibMode::Calib_Retraction_tower:   return "retraction";
    case CalibMode::Calib_Input_shaping_freq: return "input_shaping_freq";
    case CalibMode::Calib_Input_shaping_damp: return "input_shaping_damp";
    case CalibMode::Calib_Junction_Deviation: return "junction_deviation";
    default:                                  return "none";
    }
}

static bool sw_slicer_calib_is_pa(const SwCalibTestDef& t)
{
    return t.mode == CalibMode::Calib_PA_Tower || t.mode == CalibMode::Calib_PA_Line || t.mode == CalibMode::Calib_PA_Pattern;
}

static bool sw_slicer_calib_is_flow(const SwCalibTestDef& t) { return std::string(t.id).rfind("flow_", 0) == 0; }

static const SwCalibTestDef& sw_slicer_calib_test_by_id(const std::string& id)
{
    for (const SwCalibTestDef& t : sw_slicer_calib_tests())
        if (id == t.id)
            return t;
    throw std::runtime_error("unknown calibration test '" + id + "'");
}

// The test of a calibration mode (flow tests have no mode); nullptr if none.
static const SwCalibTestDef* sw_slicer_calib_test_by_mode(CalibMode mode)
{
    if (mode == CalibMode::Calib_None)
        return nullptr;
    for (const SwCalibTestDef& t : sw_slicer_calib_tests())
        if (t.mode == mode)
            return &t;
    return nullptr;
}

// Required parameter 'test'.
static const SwCalibTestDef& sw_slicer_calib_test(const json& params)
{
    if (!sw_slicer_has_param(params, "test") || !params.at("test").is_string())
        throw std::runtime_error("specify 'test' (see sw_SlicerListCalibrations)");
    return sw_slicer_calib_test_by_id(params.at("test").get<std::string>());
}

// A number as text for replies and messages (independent of the app's locale).
static std::string sw_slicer_calib_num(double v)
{
    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    ss << std::setprecision(10) << v;
    return ss.str();
}

static double sw_slicer_calib_round(double v, int decimals)
{
    const double f = std::pow(10.0, decimals);
    return std::round(v * f) / f;
}

// Start and end temperature of a material, as the temperature dialog sets them (on_filament_type_changed()).
static std::pair<int, int> sw_slicer_calib_material_temps(const std::string& material)
{
    if (material == "abs_asa")
        return {270, 230};
    if (material == "petg")
        return {250, 230};
    if (material == "pctg")
        return {280, 240};
    if (material == "tpu")
        return {240, 210};
    if (material == "pa_cf" || material == "pet_cf")
        return {320, 280};
    return {230, 190};
}

// Default of a parameter. 'resolved' holds the parameters before it (extruder and material come first):
// the PA dialog changes end and step for a Bowden extruder (its reset_params()), the
// temperature dialog start and end per material.
static json sw_slicer_calib_default(const SwCalibTestDef& t, const SwCalibParamDef& p, const json& resolved)
{
    const std::string name = p.name;
    if (sw_slicer_calib_is_pa(t) && resolved.is_object() && resolved.value("extruder", std::string("dde")) == "bowden") {
        if (name == "end")
            return 1.0;
        if (name == "step")
            return t.mode == CalibMode::Calib_PA_Pattern ? 0.05 : 0.02;
    }
    if (t.mode == CalibMode::Calib_Temp_Tower) {
        const std::pair<int, int> temps = sw_slicer_calib_material_temps(resolved.is_object() ? resolved.value("material", std::string("pla")) : "pla");
        if (name == "start")
            return temps.first;
        if (name == "end")
            return temps.second;
        if (name == "step")
            return 5;
    }
    switch (p.kind) {
    case 'e': return std::string(p.choices.front());
    case 'b': return p.def != 0;
    case 'i': return json::array();
    default:  return p.def;
    }
}

// A list of positive whole numbers, at most SW_SLICER_CALIB_MAX_LIST values. The dialog drops other
// values silently (ParseStringValues()); the API refuses them.
static json sw_slicer_calib_int_list(const json& j, const std::string& key)
{
    const std::string err = "'" + key + "' must be a list of positive whole numbers";
    if (!j.is_array())
        throw std::runtime_error(err);
    if (j.size() > SW_SLICER_CALIB_MAX_LIST)
        throw std::runtime_error("'" + key + "' has more than 10 values");
    json out = json::array();
    for (const json& v : j) {
        if (!v.is_number())
            throw std::runtime_error(err);
        const double d = v.get<double>();
        if (!std::isfinite(d) || d <= 0 || d != std::floor(d) || d > 1e6)
            throw std::runtime_error(err);
        out.push_back(static_cast<int>(d));
    }
    return out;
}

// Refuse more than SW_SLICER_CALIB_MAX_STEPS steps, lines or patterns.
static void sw_slicer_calib_require_steps(double n, const char* what)
{
    if (n > SW_SLICER_CALIB_MAX_STEPS)
        throw std::runtime_error(std::string("too many ") + what + " (" + sw_slicer_calib_num(std::min(n, 1e15)) + "); at most 100");
}

// The input rules of the dialogs (the on_start() handlers in calib_dlg.cpp), plus the API limits.
static void sw_slicer_calib_check_rules(const SwCalibTestDef& t, json& r)
{
    auto num = [&r](const char* k) { return r.at(k).get<double>(); };
    switch (t.mode) {
    case CalibMode::Calib_PA_Tower:
    case CalibMode::Calib_PA_Line:
    case CalibMode::Calib_PA_Pattern: {
        const double s = num("start"), e = num("end"), st = num("step");
        if (s < 0 || st < EPSILON || e < s + st)
            throw std::runtime_error("invalid values: start >= 0, end >= start + step, step >= 0.0001");
        // The counts of the app: tower height in mm (_calib_pa_tower), lines (GCode.cpp), patterns (calib.hpp).
        if (t.mode == CalibMode::Calib_PA_Tower)
            sw_slicer_calib_require_steps(std::ceil((e - s) / st) + 1, "steps");
        else if (t.mode == CalibMode::Calib_PA_Line)
            sw_slicer_calib_require_steps(std::ceil((e - s) / st) + 1, "lines");
        else
            sw_slicer_calib_require_steps(std::ceil((e - s) / st + 1), "patterns");
        break;
    }
    case CalibMode::Calib_Temp_Tower: {
        const double s = num("start"), e = num("end"), st = num("step");
        if (s > 350 || e < 170 || e > s - 5)
            throw std::runtime_error("invalid values: start <= 350, end >= 170, end <= start - 5");
        if (std::fmod(s, 5.0) != 0 || std::fmod(e, 5.0) != 0)
            throw std::runtime_error("temperatures must be multiples of 5");
        if (st != 5)
            throw std::runtime_error("'step' can only be 5 (the tower has blocks of 5 degrees)");
        r["start"] = static_cast<int>(s);
        r["end"]   = static_cast<int>(e);
        r["step"]  = 5;
        break;
    }
    case CalibMode::Calib_Vol_speed_Tower: {
        const double s = num("start"), e = num("end"), st = num("step");
        if (s <= 0 || st <= 0 || e < s + st)
            throw std::runtime_error("invalid values: start > 0, step > 0, end >= start + step");
        break;
    }
    case CalibMode::Calib_Retraction_tower: {
        const double s = num("start"), e = num("end"), st = num("step");
        if (s < 0 || st <= 0 || e < s + st)
            throw std::runtime_error("invalid values: start >= 0, step > 0, end >= start + step");
        break;
    }
    case CalibMode::Calib_VFA_Tower: {
        const double s = num("start"), e = num("end"), st = num("step");
        if (s <= 10 || st <= 0 || e < s + st)
            throw std::runtime_error("invalid values: start > 10, step > 0, end >= start + step");
        break;
    }
    case CalibMode::Calib_Input_shaping_freq: {
        // Both axes are checked (the dialog checks freq_end_x twice and freq_end_y not at all).
        const double sx = num("freq_start_x"), ex = num("freq_end_x"), sy = num("freq_start_y"), ey = num("freq_end_y");
        if (sx < 0 || sx >= ex || ex > 500 || sy < 0 || sy >= ey || ey > 500)
            throw std::runtime_error("invalid values: 0 <= freq_start < freq_end <= 500");
        // API rule: the app also writes a frequency for layer 1, below freq_start (GCode.cpp), and a
        // negative frequency stops slicing (GCodeWriter::set_input_shaping()).
        if (sx <= 0 || sy <= 0)
            throw std::runtime_error("invalid values: freq_start_x and freq_start_y must be > 0");
        const double d = num("damping");
        if (d < 0 || d >= 1)
            throw std::runtime_error("invalid damping factor: 0 <= damping < 1");
        break;
    }
    case CalibMode::Calib_Input_shaping_damp: {
        const double fx = num("freq_x"), fy = num("freq_y");
        if (fx < 0 || fx > 500 || fy < 0 || fy > 500)
            throw std::runtime_error("invalid values: 0 <= freq <= 500");
        const double ds = num("damping_start"), de = num("damping_end");
        if (ds < 0 || de > 1 || ds >= de)
            throw std::runtime_error("invalid damping factors: 0 <= damping_start < damping_end <= 1");
        break;
    }
    case CalibMode::Calib_Junction_Deviation: {
        const double s = num("start"), e = num("end");
        if (s < 0 || e >= 1 || s >= e)
            throw std::runtime_error("invalid values: 0 <= start < end < 1");
        break;
    }
    default: break;
    }
}

// The full parameter set of a test: the given values (checked) plus the defaults of the dialog.
// 'given' is the 'params' object of the request (null = none).
static json sw_slicer_calib_resolve_params(const SwCalibTestDef& t, const json& given_in)
{
    const json given = given_in.is_null() ? json::object() : given_in;
    if (!given.is_object())
        throw std::runtime_error("'params' must be an object");
    for (auto it = given.begin(); it != given.end(); ++it)
        if (std::none_of(t.params.begin(), t.params.end(), [&it](const SwCalibParamDef& p) { return it.key() == p.name; }))
            throw std::runtime_error("unknown parameter '" + it.key() + "' for test '" + t.id + "'");
    json r = json::object();
    for (const SwCalibParamDef& p : t.params) {
        const std::string k = p.name;
        if (!sw_slicer_has_param(given, k.c_str())) {
            r[k] = sw_slicer_calib_default(t, p, r);
            continue;
        }
        const json& v = given.at(k);
        switch (p.kind) {
        case 'n':
            if (!v.is_number() || !std::isfinite(v.get<double>()))
                throw std::runtime_error("'" + k + "' must be a number");
            r[k] = v.get<double>();
            break;
        case 'b':
            if (!v.is_boolean())
                throw std::runtime_error("'" + k + "' must be true or false");
            r[k] = v.get<bool>();
            break;
        case 'e': {
            const std::string s = v.is_string() ? v.get<std::string>() : std::string();
            if (std::none_of(p.choices.begin(), p.choices.end(), [&s](const char* c) { return s == c; })) {
                std::string list;
                for (const char* c : p.choices)
                    list += (list.empty() ? "" : ", ") + std::string(c);
                throw std::runtime_error("'" + k + "' must be one of: " + list);
            }
            r[k] = s;
            break;
        }
        default: r[k] = sw_slicer_calib_int_list(v, k); break;
        }
    }
    sw_slicer_calib_check_rules(t, r);
    return r;
}

// Calib_Params for a test. Every field is set: Calib_Params initializes only mode and extruder_id
// (calib.hpp), the dialogs reuse their member.
static Calib_Params sw_slicer_calib_params(const SwCalibTestDef& t, const json& r)
{
    Calib_Params cp;
    cp.extruder_id   = 0;
    cp.start         = 0;
    cp.end           = 0;
    cp.step          = 0;
    cp.print_numbers = false;
    cp.freqStartX    = 0;
    cp.freqEndX      = 0;
    cp.freqStartY    = 0;
    cp.freqEndY      = 0;
    cp.test_model    = 0;
    cp.accelerations.clear();
    cp.speeds.clear();
    cp.mode = t.mode;
    auto num   = [&r](const char* k) { return r.at(k).get<double>(); };
    auto model = [&r]() { return r.at("model").get<std::string>() == "fast_tower" ? 1 : 0; };
    switch (t.mode) {
    case CalibMode::Calib_PA_Tower:
    case CalibMode::Calib_PA_Line:
    case CalibMode::Calib_PA_Pattern:
        cp.start = num("start");
        cp.end   = num("end");
        cp.step  = num("step");
        // The dialog: off for the tower, the checkbox for lines, always on for the pattern.
        cp.print_numbers = t.mode == CalibMode::Calib_PA_Line ? r.at("print_numbers").get<bool>() : t.mode == CalibMode::Calib_PA_Pattern;
        if (t.mode == CalibMode::Calib_PA_Pattern) {
            for (const json& v : r.at("accelerations"))
                cp.accelerations.push_back(v.get<double>());
            for (const json& v : r.at("speeds"))
                cp.speeds.push_back(v.get<double>());
        }
        break;
    case CalibMode::Calib_Temp_Tower:
        cp.start = num("start");
        cp.end   = num("end");
        cp.step  = 5;
        break;
    case CalibMode::Calib_Vol_speed_Tower:
    case CalibMode::Calib_Retraction_tower:
    case CalibMode::Calib_VFA_Tower:
        cp.start = num("start");
        cp.end   = num("end");
        cp.step  = num("step");
        break;
    case CalibMode::Calib_Input_shaping_freq:
        cp.freqStartX = num("freq_start_x");
        cp.freqEndX   = num("freq_end_x");
        cp.freqStartY = num("freq_start_y");
        cp.freqEndY   = num("freq_end_y");
        cp.start      = num("damping");
        cp.test_model = model();
        break;
    case CalibMode::Calib_Input_shaping_damp:
        cp.freqStartX = num("freq_x");
        cp.freqStartY = num("freq_y");
        cp.start      = num("damping_start");
        cp.end        = num("damping_end");
        cp.test_model = model();
        break;
    case CalibMode::Calib_Junction_Deviation:
        cp.start      = num("start");
        cp.end        = num("end");
        cp.test_model = model();
        break;
    default: break;
    }
    return cp;
}

// The parameters of a test made in the app, in the form of the API (only the fields the test uses).
static json sw_slicer_calib_params_json(const SwCalibTestDef& t, const Calib_Params& cp)
{
    json r = json::object();
    const char* model = cp.test_model == 1 ? "fast_tower" : "ringing_tower";
    switch (t.mode) {
    case CalibMode::Calib_PA_Tower:
    case CalibMode::Calib_PA_Line:
    case CalibMode::Calib_PA_Pattern:
        r["start"] = cp.start;
        r["end"]   = cp.end;
        r["step"]  = cp.step;
        if (t.mode == CalibMode::Calib_PA_Line)
            r["print_numbers"] = cp.print_numbers;
        if (t.mode == CalibMode::Calib_PA_Pattern) {
            r["accelerations"] = cp.accelerations;
            r["speeds"]        = cp.speeds;
        }
        break;
    case CalibMode::Calib_Temp_Tower:
        r["start"] = static_cast<int>(std::lround(cp.start));
        r["end"]   = static_cast<int>(std::lround(cp.end));
        r["step"]  = 5;
        break;
    case CalibMode::Calib_Vol_speed_Tower:
    case CalibMode::Calib_Retraction_tower:
    case CalibMode::Calib_VFA_Tower:
        r["start"] = cp.start;
        r["end"]   = cp.end;
        r["step"]  = cp.step;
        break;
    case CalibMode::Calib_Input_shaping_freq:
        r["model"]        = model;
        r["freq_start_x"] = cp.freqStartX;
        r["freq_end_x"]   = cp.freqEndX;
        r["freq_start_y"] = cp.freqStartY;
        r["freq_end_y"]   = cp.freqEndY;
        r["damping"]      = cp.start;
        break;
    case CalibMode::Calib_Input_shaping_damp:
        r["model"]         = model;
        r["freq_x"]        = cp.freqStartX;
        r["freq_y"]        = cp.freqStartY;
        r["damping_start"] = cp.start;
        r["damping_end"]   = cp.end;
        break;
    case CalibMode::Calib_Junction_Deviation:
        r["model"] = model;
        r["start"] = cp.start;
        r["end"]   = cp.end;
        break;
    default: break;
    }
    return r;
}

static std::string sw_slicer_calib_flavor()
{
    const DynamicPrintConfig& cfg = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    return cfg.has("gcode_flavor") ? cfg.opt_serialize("gcode_flavor") : std::string();
}

// Does the test work with the selected printer? GCodeWriter::set_junction_deviation() writes M205 J only
// for Marlin 2; input shaping is SET_INPUT_SHAPER for Klipper and M593 otherwise.
static json sw_slicer_calib_support(const SwCalibTestDef& t)
{
    json              j;
    const std::string flavor = sw_slicer_calib_flavor();
    j["supported"]           = true;
    if (t.mode == CalibMode::Calib_Junction_Deviation && flavor != "marlin2") {
        j["supported"] = false;
        j["reason"]    = "junction deviation is only written for Marlin 2 firmware (gcode_flavor is '" + flavor + "')";
    } else if ((t.mode == CalibMode::Calib_Input_shaping_freq || t.mode == CalibMode::Calib_Input_shaping_damp) &&
               flavor != "klipper" && flavor != "marlin2" && flavor != "reprapfirmware") {
        j["note"] = "the app writes M593 for this firmware (gcode_flavor is '" + flavor + "'); check that the firmware supports it";
    }
    return j;
}

// The test changes the edited filament preset but prints with filament 1 (a loaded test model has no
// 'extruder' setting): refuse if they differ. Empty = no problem.
static std::string sw_slicer_calib_filament_problem()
{
    PresetBundle*     bundle = wxGetApp().preset_bundle;
    const std::string edited = bundle->filaments.get_edited_preset().name;
    if (bundle->filament_presets.empty())
        return "no filament is selected for slot 1";
    const std::string& slot1 = bundle->filament_presets.front();
    if (slot1 == edited)
        return "";
    return "the test prints with filament 1 ('" + slot1 + "'), but the filament settings being edited belong to '" + edited +
           "'; select '" + slot1 + "' for editing first";
}

// Lives as long as the app: the PA pattern keeps a reference to these parameters (CalibPressureAdvancePattern::m_params) and uses it at every reslice, like the dialog's own member.
static Calib_Params& sw_slicer_calib_storage()
{
    static Calib_Params s;
    return s;
}

static std::optional<SwCalibRecord>& sw_slicer_calib_record()
{
    static std::optional<SwCalibRecord> r;
    return r;
}

// The record of the last API calibration, if it still belongs to this project and these presets: at
// least one test object is still in the model (object ids are unique and kept by undo/redo; the id of
// the plater's Model never changes, also not on a new project) and the presets have the same names.
static SwCalibRecord* sw_slicer_calib_valid_record()
{
    std::optional<SwCalibRecord>& r = sw_slicer_calib_record();
    if (!r)
        return nullptr;
    const ModelObjectPtrs& objects = wxGetApp().plater()->model().objects;
    if (std::none_of(objects.begin(), objects.end(), [&r](const ModelObject* mo) {
            return std::find(r->object_ids.begin(), r->object_ids.end(), mo->id().id) != r->object_ids.end();
        }))
        return nullptr;
    for (int i = 0; i < 3; ++i)
        if (sw_slicer_preset_collection(sw_slicer_calib_types[i]).get_edited_preset().name != r->presets[i])
            return nullptr;
    return &*r;
}

// The Print of plate 1, which carries the calibration settings: the calib_* functions set them on the
// Print of the current plate right after their new project, which is plate 1. Plater::fff_print() is
// not used: that is the plater's own Print object, not the one of a plate.
static Print* sw_slicer_calib_print()
{
    PartPlate* pp = wxGetApp().plater()->get_partplate_list().get_plate(0);
    return pp != nullptr ? pp->fff_print() : nullptr;
}

// Leave calibration mode on every plate (and on the plater's own Print), like load_files() does for the
// current plate. Returns whether a mode was active.
static bool sw_slicer_calib_reset_mode()
{
    Plater* plater = wxGetApp().plater();
    bool    was    = plater->fff_print().calib_mode() != CalibMode::Calib_None;
    plater->fff_print().calib_mode() = CalibMode::Calib_None;
    PartPlateList& plates = plater->get_partplate_list();
    for (int i = 0; i < plates.get_plate_count(); ++i)
        if (PartPlate* pp = plates.get_plate(i); pp != nullptr && pp->fff_print() != nullptr) {
            was = was || pp->fff_print()->calib_mode() != CalibMode::Calib_None;
            pp->fff_print()->calib_mode() = CalibMode::Calib_None;
        }
    return was;
}

// Value of a (cloned) option as text, like sw_slicer_option_value(): null if missing or nil, cut off
// above SW_SLICER_MAX_VALUE_CHARS.
static json sw_slicer_calib_option_text(const ConfigOption* opt, bool& truncated)
{
    if (opt == nullptr || opt->is_nil())
        return nullptr;
    std::string s = opt->serialize();
    if (s.size() > SW_SLICER_MAX_VALUE_CHARS) {
        size_t cut = SW_SLICER_MAX_VALUE_CHARS;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
            --cut;
        s         = s.substr(0, cut) + "\xE2\x80\xA6"; // U+2026 horizontal ellipsis
        truncated = true;
    }
    return s;
}

// The keys whose options differ between two snapshots of the edited presets (printer, print, filament),
// with cloned options. Vector elements set directly (max_layer_height[0]) are included.
static std::vector<SwCalibRecord::Change> sw_slicer_calib_diff(const DynamicPrintConfig before[3], const DynamicPrintConfig after[3])
{
    std::vector<SwCalibRecord::Change> out;
    for (int i = 0; i < 3; ++i) {
        std::set<std::string> keys;
        for (const std::string& k : before[i].keys())
            keys.insert(k);
        for (const std::string& k : after[i].keys())
            keys.insert(k);
        for (const std::string& k : keys) {
            const ConfigOption* a = before[i].option(k);
            const ConfigOption* b = after[i].option(k);
            if (a != nullptr && b != nullptr && *a == *b)
                continue;
            SwCalibRecord::Change c;
            c.type = sw_slicer_calib_types[i];
            c.key  = k;
            if (a != nullptr)
                c.before.reset(a->clone());
            if (b != nullptr)
                c.after.reset(b->clone());
            out.push_back(std::move(c));
        }
    }
    return out;
}

static int sw_slicer_calib_type_order(Preset::Type t) { return t == Preset::TYPE_PRINTER ? 0 : t == Preset::TYPE_PRINT ? 1 : 2; }

// [{type, key, label?, before, after, truncated?}], sorted by type (printer, print, filament) and key.
static json sw_slicer_calib_changes_json(const std::vector<SwCalibRecord::Change>& changes)
{
    std::vector<const SwCalibRecord::Change*> sorted;
    for (const SwCalibRecord::Change& c : changes)
        sorted.push_back(&c);
    std::sort(sorted.begin(), sorted.end(), [](const SwCalibRecord::Change* l, const SwCalibRecord::Change* r) {
        const int lo = sw_slicer_calib_type_order(l->type), ro = sw_slicer_calib_type_order(r->type);
        return lo != ro ? lo < ro : l->key < r->key;
    });
    json items = json::array();
    for (const SwCalibRecord::Change* c : sorted) {
        bool truncated = false;
        json item;
        item["type"] = sw_slicer_preset_type_name(c->type);
        item["key"]  = c->key;
        if (const ConfigOptionDef* def = print_config_def.get(c->key); def != nullptr && !def->label.empty())
            item["label"] = def->label;
        item["before"] = sw_slicer_calib_option_text(c->before.get(), truncated);
        item["after"]  = sw_slicer_calib_option_text(c->after.get(), truncated);
        if (truncated)
            item["truncated"] = true;
        items.push_back(item);
    }
    return items;
}

// Per object the keys of its own settings (names only; sw_SlicerGetObjectConfig reads the values).
static json sw_slicer_calib_object_settings()
{
    const Model& model = wxGetApp().plater()->model();
    json         out   = json::array();
    for (size_t i = 0; i < model.objects.size(); ++i) {
        const ModelObject* mo   = model.objects[i];
        t_config_option_keys keys = mo->config.keys();
        std::sort(keys.begin(), keys.end());
        out.push_back({{"object", static_cast<int>(i)}, {"name", mo->name}, {"keys", keys}});
    }
    return out;
}

// The undo steps of the app after the last project separator (new project): add_model's
// "Import Object: ...", the primitive of the PA pattern. They are not "API: " steps.
static std::vector<std::string> sw_slicer_calib_app_undo_steps()
{
    const std::vector<UndoRedo::Snapshot>& snaps = wxGetApp().plater()->undo_redo_stack_main().snapshots();
    size_t                                 first = 0;
    for (size_t i = 0; i < snaps.size(); ++i)
        if (snaps[i].snapshot_data.snapshot_type == UndoRedo::SnapshotType::ProjectSeparator)
            first = i + 1;
    std::vector<std::string> names;
    for (size_t i = first; i < snaps.size(); ++i)
        if (!snaps[i].is_topmost() && UndoRedo::snapshot_modifies_project(snaps[i]))
            names.push_back(snaps[i].name);
    return names;
}

// Height of the model above the bed: the sliced range of the (cut) test model.
static double sw_slicer_calib_model_height()
{
    double h = 0;
    for (const ModelObject* mo : wxGetApp().plater()->model().objects)
        if (!mo->instances.empty()) {
            const BoundingBoxf3 bb = mo->instance_bounding_box(0);
            h                      = std::max(h, bb.max.z() - std::max(0.0, bb.min.z()));
        }
    return h;
}

// The layer heights (print_z) the G-code goes through, counted like m_layer_count in GCode::_do_export()
// for printing by layer: all object and support layers, merged when closer than EPSILON. Empty if the
// plate is not sliced (or slicing runs).
static std::vector<double> sw_slicer_calib_layer_zs(const Print* print)
{
    std::vector<double> out;
    if (print == nullptr || wxGetApp().plater()->is_background_process_slicing() || !print->is_step_done(psSlicingFinished))
        return out;
    std::vector<double> zs;
    for (const PrintObject* o : print->objects()) {
        for (const Layer* l : o->layers())
            zs.push_back(l->print_z);
        for (const SupportLayer* l : o->support_layers())
            zs.push_back(l->print_z);
    }
    std::sort(zs.begin(), zs.end());
    for (double z : zs)
        if (out.empty() || std::abs(z - out.back()) >= EPSILON)
            out.push_back(z);
    return out;
}

// The mapping height/line/pattern/object/layer -> value as the app uses it when slicing (GCode.cpp,
// calib.cpp, the object settings of the flow tests). 'p' are the parameters in the form of the API;
// app_units: a max flowrate test made in the app (its parameters are stored in mm/s). Read-only.
static json sw_slicer_calib_mapping(const SwCalibTestDef& t, const json& p, const Print* print, double flow_ratio, bool app_units)
{
    json        m;
    json        table = json::array();
    json        notes = json::array();
    const double h    = sw_slicer_calib_model_height();
    auto        d     = [&p](const char* k) { return p.at(k).get<double>(); };
    const size_t max_rows = 2000;
    m["value_key"] = t.result_type != nullptr ? json{{"type", t.result_type}, {"key", t.result_keys.front()}} : json(nullptr);
    m["unit"]      = "";
    // Rows per height band: [z_from, z_to) of the layer top (print_z), cut at the model height.
    auto band = [&table, h](double z_from, double z_to, double value) {
        table.push_back({{"z_from", sw_slicer_calib_round(z_from, 3)}, {"z_to", sw_slicer_calib_round(std::min(z_to, h), 3)}, {"value", value}});
    };
    auto covered_up_to = [&notes, &table](double end, bool descending) {
        if (table.empty())
            return;
        const double last = table.back().at("value").get<double>();
        if (!descending && last < end - 1e-9)
            notes.push_back("the model covers values up to " + sw_slicer_calib_num(last) + "; higher values are not printed");
        if (descending && last > end + 1e-9)
            notes.push_back("the model covers values down to " + sw_slicer_calib_num(last) + "; lower values are not printed");
    };
    switch (t.mode) {
    case CalibMode::Calib_PA_Tower: {
        m["axis"]    = "z";
        m["formula"] = "start + floor(z) * step";
        for (int k = 0; k < h && table.size() < max_rows; ++k)
            band(k, k + 1, sw_slicer_calib_round(d("start") + k * d("step"), 6));
        covered_up_to(d("end"), false);
        break;
    }
    case CalibMode::Calib_Temp_Tower: {
        m["axis"]    = "z";
        m["unit"]    = "°C";
        m["formula"] = "start - 5 * floor(z / 10.001)";
        for (int b = 0; b * 10.001 < h && table.size() < max_rows; ++b)
            band(b * 10.001, (b + 1) * 10.001, d("start") - 5.0 * b);
        covered_up_to(d("end"), true);
        break;
    }
    case CalibMode::Calib_VFA_Tower: {
        m["axis"]    = "z";
        m["unit"]    = "mm/s";
        m["formula"] = "round(start + floor(z / 5) * step) (outer wall speed)";
        for (int b = 0; b * 5.0 < h && table.size() < max_rows; ++b)
            band(b * 5.0, (b + 1) * 5.0, std::round(d("start") + b * d("step")));
        covered_up_to(d("end"), false);
        break;
    }
    case CalibMode::Calib_Vol_speed_Tower: {
        m["axis"] = "z";
        m["unit"] = app_units ? "mm/s" : "mm³/s";
        m["formula"] = app_units ? "start + z * step (outer wall speed, rounded to whole mm/s)" :
                                   "start + z * step (outer wall speed = this / mm³ per mm, rounded to whole mm/s)";
        for (int k = 0; k < h && table.size() < max_rows; ++k)
            band(k, k + 1, sw_slicer_calib_round(d("start") + k * d("step"), 4));
        notes.push_back("the value is the one at the bottom of each band; it rises continuously with the height");
        if (app_units)
            notes.push_back("this test was made in the app, which stores its values as outer wall speed in mm/s");
        covered_up_to(d("end"), false);
        break;
    }
    case CalibMode::Calib_Retraction_tower: {
        m["axis"]    = "z";
        m["unit"]    = "mm";
        m["formula"] = "start + floor(max(0, z - 0.4)) * step";
        if (h > 0)
            band(0, 1.4, sw_slicer_calib_round(d("start"), 4));
        for (int k = 1; 0.4 + k < h && table.size() < max_rows; ++k)
            band(0.4 + k, 0.4 + k + 1, sw_slicer_calib_round(d("start") + k * d("step"), 4));
        covered_up_to(d("end"), false);
        break;
    }
    case CalibMode::Calib_PA_Line: {
        // GCode.cpp: count = llround(ceil((end - start) / step)) + 1, limited by the bed depth in
        // CalibPressureAdvanceLine::generate_test(); line i at start_y + i * 3.5 mm (front to back).
        m["axis"]    = "line";
        m["formula"] = "start + i * step, i = 0 ... count-1, count = ceil((end - start) / step) + 1; a number at every 2nd line";
        long long count = std::llround(std::ceil((d("end") - d("start")) / d("step"))) + 1;
        const ConfigOptionPoints* area = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionPoints>("printable_area");
        if (area != nullptr && area->values.size() <= 4 && !area->values.empty()) {
            const long long fit = static_cast<long long>((get_extents(area->values).size().y() - 10) / 3.5);
            if (fit < count) {
                notes.push_back("the bed fits only " + std::to_string(fit) + " lines; the app prints only the first " + std::to_string(fit));
                count = fit;
            }
        } else if (area != nullptr && area->values.size() > 4) {
            notes.push_back("on a round bed the app may print fewer lines");
        }
        const bool numbers = p.value("print_numbers", true);
        for (long long i = 0; i < count && table.size() < max_rows; ++i)
            table.push_back({{"index", i}, {"value", sw_slicer_calib_round(d("start") + i * d("step"), 6)}, {"labeled", numbers && i % 2 == 0}});
        m["count"] = count;
        notes.push_back("line 0 is the front line; the lines go to the back of the bed");
        break;
    }
    case CalibMode::Calib_PA_Pattern: {
        m["axis"]    = "pattern";
        m["formula"] = "start + j * step, j = 0 ... count-1, count = ceil((end - start) / step + 1), left to right; a number at every 2nd pattern";
        const int count = static_cast<int>(std::ceil((d("end") - d("start")) / d("step") + 1));
        for (int j = 0; j < count && table.size() < max_rows; ++j)
            table.push_back({{"index", j}, {"value", sw_slicer_calib_round(d("start") + j * d("step"), 6)}, {"labeled", j % 2 == 0}});
        m["count"] = count;
        // Speed and acceleration of each test from the object name pa_pattern_<speed>_<accel> (_calib_pa_pattern()).
        json         objects = json::array();
        const Model& model   = wxGetApp().plater()->model();
        for (size_t i = 0; i < model.objects.size(); ++i) {
            const std::string& name = model.objects[i]->name;
            int                speed = 0, accel = 0;
            if (std::sscanf(name.c_str(), "pa_pattern_%d_%d", &speed, &accel) == 2)
                objects.push_back({{"object", static_cast<int>(i)}, {"name", name}, {"speed", speed}, {"acceleration", accel}});
        }
        m["objects"] = objects;
        break;
    }
    case CalibMode::Calib_Input_shaping_freq:
    case CalibMode::Calib_Input_shaping_damp:
    case CalibMode::Calib_Junction_Deviation: {
        // GCode.cpp: the layer index i counts from 0 (layer 1 in the preview); 'layer' below is i + 1.
        m["axis"] = "layer";
        if (t.mode == CalibMode::Calib_Input_shaping_freq) {
            m["unit"]    = "Hz";
            m["formula"] = "layer 1 and from layer 3: freq_start + (freq_end - freq_start) * (layer - 3) / (layer_count - 3) per axis; "
                           "layer 2: damping = damping (layer 1 gives a frequency below freq_start)";
        } else if (t.mode == CalibMode::Calib_Input_shaping_damp) {
            m["formula"] = "layer 2: frequencies freq_x, freq_y; other layers: damping_start + (damping_end - damping_start) * (layer - 1) / layer_count";
        } else {
            m["unit"]    = "mm";
            m["formula"] = "start + (end - start) * (layer - 1) / layer_count";
        }
        const std::vector<double> zs = sw_slicer_calib_layer_zs(print);
        if (zs.empty()) {
            table = nullptr;
            notes.push_back("slice first for a per-layer table");
            break;
        }
        const unsigned int count = static_cast<unsigned int>(zs.size());
        m["layer_count"]         = count;
        // Same arithmetic as GCode.cpp (m_layer_count is unsigned).
        const double freq_div = static_cast<double>(count - 3u);
        if (t.mode == CalibMode::Calib_Input_shaping_freq) {
            if (count <= 3)
                notes.push_back("only " + std::to_string(count) + " layers: the frequency formula divides by layer_count - 3, so the values are not usable");
            const double low = std::min(d("freq_start_x") - 2 * (d("freq_end_x") - d("freq_start_x")) / freq_div,
                                        d("freq_start_y") - 2 * (d("freq_end_y") - d("freq_start_y")) / freq_div);
            if (count > 3 && low < 0)
                notes.push_back("slicing fails: the frequency for layer 1 is below 0 (" + sw_slicer_calib_num(sw_slicer_calib_round(low, 2)) +
                                " Hz); use a higher freq_start or a smaller range");
        }
        for (unsigned int i = 0; i < count && table.size() < max_rows; ++i) {
            json row;
            row["layer"] = i + 1;
            row["z"]     = sw_slicer_calib_round(zs[i], 3);
            if (t.mode == CalibMode::Calib_Input_shaping_freq) {
                row["freq_x"]  = nullptr;
                row["freq_y"]  = nullptr;
                row["damping"] = nullptr;
                if (i == 1)
                    row["damping"] = d("damping");
                else {
                    // Layer 1 (i = 0) takes this branch too in GCode.cpp: (0 - 2) gives a value below freq_start.
                    row["freq_x"] = sw_slicer_calib_round(d("freq_start_x") + (d("freq_end_x") - d("freq_start_x")) * (static_cast<int>(i) - 2) / freq_div, 2);
                    row["freq_y"] = sw_slicer_calib_round(d("freq_start_y") + (d("freq_end_y") - d("freq_start_y")) * (static_cast<int>(i) - 2) / freq_div, 2);
                }
            } else if (t.mode == CalibMode::Calib_Input_shaping_damp) {
                row["freq_x"]  = nullptr;
                row["freq_y"]  = nullptr;
                row["damping"] = nullptr;
                if (i == 1) {
                    row["freq_x"] = d("freq_x");
                    row["freq_y"] = d("freq_y");
                } else
                    row["damping"] = sw_slicer_calib_round(d("damping_start") + (d("damping_end") - d("damping_start")) * i / count, 3);
            } else {
                row["value"] = sw_slicer_calib_round(d("start") + (d("end") - d("start")) * i / count, 3);
            }
            table.push_back(row);
        }
        if (print != nullptr && print->config().print_sequence == PrintSequence::ByObject)
            notes.push_back("the table assumes printing by layer; printing by object counts the layers of every object");
        break;
    }
    default: {
        if (!sw_slicer_calib_is_flow(t))
            break;
        // Flow tests: print_flow_ratio per object, set from the object name flowrate_<modifier>
        // (adjust_settings_for_flowrate_calib(), 'm' = minus).
        const bool linear = std::string(t.id).rfind("flow_yolo", 0) == 0;
        m["axis"]         = "object";
        m["formula"]      = linear ? "flow_ratio + modifier" : "flow_ratio * (1 + modifier / 100)";
        const Model& model = wxGetApp().plater()->model();
        for (size_t i = 0; i < model.objects.size(); ++i) {
            const ModelObject* mo = model.objects[i];
            if (mo->name.rfind("flowrate_", 0) != 0)
                continue;
            json row;
            row["object"] = static_cast<int>(i);
            row["name"]   = mo->name;
            std::string text = mo->name.substr(9);
            if (!text.empty() && text[0] == 'm')
                text[0] = '-';
            std::istringstream ss(text);
            ss.imbue(std::locale::classic());
            double modifier = 0;
            row["modifier"] = (ss >> modifier) ? json(modifier) : json(nullptr);
            const double pfr = mo->config.has("print_flow_ratio") ? mo->config.opt_float("print_flow_ratio") : 1.0;
            row["print_flow_ratio"] = sw_slicer_calib_round(pfr, 6);
            row["flow_ratio"]       = flow_ratio > 0 ? json(sw_slicer_calib_round(flow_ratio * pfr, 4)) : json(nullptr);
            table.push_back(row);
        }
        if (flow_ratio > 0)
            m["flow_ratio_before"] = flow_ratio;
        break;
    }
    }
    if (table.is_array() && table.size() >= max_rows)
        notes.push_back("the table was cut off after " + std::to_string(max_rows) + " rows");
    m["table"] = table;
    m["notes"] = notes;
    return m;
}

// List the calibration tests of the Calibration menu with parameters, defaults, rules, result key and
// whether the test works with the selected printer. Read-only.
void SSWCP_SliceProject_Instance::sw_SlicerListCalibrations()
{
    try {
        sw_slicer_require_fff();
        json tests = json::array();
        for (const SwCalibTestDef& t : sw_slicer_calib_tests()) {
            json jt;
            jt["test"] = t.id;
            jt["menu"] = t.menu;
            jt["mode"] = sw_slicer_calib_mode_name(t.mode);
            json params   = json::array();
            json defaults = json::object();
            for (const SwCalibParamDef& p : t.params) {
                json jp;
                jp["name"] = p.name;
                jp["type"] = p.kind == 'n' ? "number" : p.kind == 'i' ? "int_list" : p.kind == 'b' ? "bool" : "enum";
                jp["default"]       = sw_slicer_calib_default(t, p, defaults);
                defaults[p.name]    = jp["default"];
                if (p.kind == 'e') {
                    json choices = json::array();
                    for (const char* c : p.choices)
                        choices.push_back(c);
                    jp["choices"] = choices;
                }
                if (p.unit != nullptr && *p.unit != '\0')
                    jp["unit"] = p.unit;
                const std::string name = p.name;
                if (sw_slicer_calib_is_pa(t) && (name == "end" || name == "step"))
                    jp["default_bowden"] = sw_slicer_calib_default(t, p, json{{"extruder", "bowden"}});
                if (t.mode == CalibMode::Calib_Temp_Tower && (name == "start" || name == "end")) {
                    json by = json::object();
                    for (const char* mat : t.params.front().choices)
                        by[mat] = sw_slicer_calib_default(t, p, json{{"material", mat}});
                    jp["defaults_by_material"] = by;
                }
                params.push_back(jp);
            }
            jt["params"] = params;
            jt["rules"]  = t.rules;
            if (t.result_type != nullptr) {
                json keys = json::array();
                for (const char* k : t.result_keys)
                    keys.push_back(k);
                jt["result"] = {{"type", t.result_type}, {"keys", keys}};
            } else
                jt["result"] = nullptr;
            jt.update(sw_slicer_calib_support(t));
            tests.push_back(jt);
        }

        const DynamicPrintConfig& printer = wxGetApp().preset_bundle->printers.get_edited_preset().config;
        const ConfigOptionFloats* nozzle  = printer.option<ConfigOptionFloats>("nozzle_diameter");
        json                      jprinter;
        jprinter["name"]            = wxGetApp().preset_bundle->printers.get_edited_preset().name;
        jprinter["gcode_flavor"]    = sw_slicer_calib_flavor();
        jprinter["nozzle_diameter"] = (nozzle != nullptr && !nozzle->values.empty()) ? json(nozzle->get_at(0)) : json(nullptr);

        const std::vector<std::string>& slots   = wxGetApp().preset_bundle->filament_presets;
        const std::string               problem = sw_slicer_calib_filament_problem();
        json                            jfilament;
        jfilament["edited"] = wxGetApp().preset_bundle->filaments.get_edited_preset().name;
        jfilament["slot_1"] = slots.empty() ? json(nullptr) : json(slots.front());
        jfilament["ok"]     = problem.empty();
        if (!problem.empty())
            jfilament["problem"] = problem;

        m_res_data["tests"]    = tests;
        m_res_data["printer"]  = jprinter;
        m_res_data["filament"] = jfilament;
        m_res_data["notes"]    = json::array();
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Create a calibration test project like the Calibration menu, without its dialogs. Closes the current
// project (F1 contract: discard_unsaved, preset_changes). The test changes preset values (not saved);
// the reply lists them. Never prints.
void SSWCP_SliceProject_Instance::sw_SlicerCalibrate()
{
    try {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            handle_general_fail(-1, "no active plater");
            return;
        }
        // 1. Guards.
        sw_slicer_require_free_gizmo();
        sw_slicer_require_idle();
        sw_slicer_require_not_slicing();
        sw_slicer_require_not_loading();
        sw_slicer_require_no_export();
        sw_slicer_require_fff();

        // 2.-3. Test, parameters, printer and filament: all checked before anything changes.
        const SwCalibTestDef& def      = sw_slicer_calib_test(m_param_data);
        const json            resolved = sw_slicer_calib_resolve_params(def, m_param_data.count("params") ? m_param_data.at("params") : json());
        const json            support  = sw_slicer_calib_support(def);
        if (!support.at("supported").get<bool>())
            throw std::runtime_error(support.at("reason").get<std::string>());
        const std::string problem = sw_slicer_calib_filament_problem();
        if (!problem.empty())
            throw std::runtime_error(problem);

        // 4. Project switch (as sw_SlicerNewProject).
        sw_slicer_require_discard(m_param_data);
        const std::string presets   = sw_slicer_preset_choice(m_param_data, /*allow_keep=*/true);
        const bool        was_dirty = sw_slicer_project_dirty();

        // 5. Create the test.
        DynamicPrintConfig       before[3];
        DynamicPrintConfig       after[3];
        std::vector<std::string> api_notes;
        {
            // RAII: refuse other model and preset changes until the test project is complete.
            struct InProgress
            {
                InProgress() { sw_slicer_calib_in_progress() = true; }
                ~InProgress() { sw_slicer_calib_in_progress() = false; }
            } in_progress;
            sswcp_api::Scope api_scope;
            // Start from a clean project first (same steps as sw_SlicerNewProject), so the values captured
            // below are the ones the test starts from; the calib_* function then opens its own new project
            // on a clean plater, which asks nothing (close_with_confirm).
            if (presets == "discard")
                sw_slicer_discard_preset_changes();
            plater->new_project(/*skip_confirm=*/true, /*silent=*/false);
            for (int i = 0; i < 3; ++i)
                before[i] = sw_slicer_preset_collection(sw_slicer_calib_types[i]).get_edited_preset().config;
            Calib_Params& cp = sw_slicer_calib_storage();
            cp               = sw_slicer_calib_params(def, resolved);
            const std::string id = def.id;
            switch (def.mode) {
            case CalibMode::Calib_PA_Tower:
            case CalibMode::Calib_PA_Line:
            case CalibMode::Calib_PA_Pattern:         plater->calib_pa(sw_slicer_calib_storage()); break;
            case CalibMode::Calib_Temp_Tower:         plater->calib_temp(cp); break;
            case CalibMode::Calib_Vol_speed_Tower:    plater->calib_max_vol_speed(cp); break;
            case CalibMode::Calib_Retraction_tower:   plater->calib_retraction(cp); break;
            case CalibMode::Calib_VFA_Tower:          plater->calib_VFA(cp); break;
            case CalibMode::Calib_Input_shaping_freq: plater->calib_input_shaping_freq(cp); break;
            case CalibMode::Calib_Input_shaping_damp: plater->calib_input_shaping_damp(cp); break;
            case CalibMode::Calib_Junction_Deviation: plater->calib_junction_deviation(cp); break;
            default:
                plater->calib_flowrate(/*is_linear=*/id == "flow_yolo" || id == "flow_yolo_perfectionist",
                                       /*pass=*/(id == "flow_pass2" || id == "flow_yolo_perfectionist") ? 2 : 1);
                break;
            }
            // calib_retraction and calib_pa do not refresh every tab.
            for (Preset::Type t : sw_slicer_calib_types) {
                Tab* tab = sw_slicer_preset_tab(t);
                tab->update_dirty();
                tab->reload_config();
            }
            api_notes = sswcp_api::notes();
        }

        // 6. Check the result.
        Print* print = sw_slicer_calib_print();
        if (plater->model().objects.empty() || (def.mode != CalibMode::Calib_None && (print == nullptr || print->calib_mode() != def.mode)))
            throw std::runtime_error(sw_slicer_join_notes("the calibration project could not be created", api_notes));

        // 7. Record what the test changed.
        for (int i = 0; i < 3; ++i)
            after[i] = sw_slicer_preset_collection(sw_slicer_calib_types[i]).get_edited_preset().config;
        SwCalibRecord rec;
        rec.test         = def.id;
        rec.params       = resolved;
        rec.project_name = into_u8(plater->get_project_name());
        for (const ModelObject* mo : plater->model().objects)
            rec.object_ids.push_back(mo->id().id);
        for (int i = 0; i < 3; ++i)
            rec.presets[i] = sw_slicer_preset_collection(sw_slicer_calib_types[i]).get_edited_preset().name;
        if (const ConfigOptionFloats* fr = before[2].option<ConfigOptionFloats>("filament_flow_ratio"); fr != nullptr && !fr->values.empty())
            rec.flow_ratio = fr->get_at(0);
        rec.changes = sw_slicer_calib_diff(before, after);
        const json changes = sw_slicer_calib_changes_json(rec.changes);
        const size_t n_changes = rec.changes.size();
        const std::string project_name = rec.project_name;
        const double flow_ratio = rec.flow_ratio;
        sw_slicer_calib_record() = std::move(rec);

        // 8. Reply.
        json objects = json::array();
        for (const json& o : sw_slicer_objects_json())
            objects.push_back({{"object", o.at("object")}, {"name", o.at("name")}, {"plate", o.at("plate")}});
        const int  plates  = plater->get_partplate_list().get_plate_count();
        const json mapping = sw_slicer_calib_mapping(def, resolved, print, flow_ratio, false);

        // 9. Notes.
        std::vector<std::string> notes;
        sw_slicer_add_note(notes, "new project '" + project_name + "'; the previous project was closed (this cannot be undone)");
        if (was_dirty)
            sw_slicer_add_note(notes, "unsaved changes discarded");
        if (presets == "keep")
            sw_slicer_add_note(notes, "preset changes kept for the test");
        else if (presets == "discard")
            sw_slicer_add_note(notes, "preset changes discarded");
        if (n_changes > 0)
            sw_slicer_add_note(notes, "the test changed " + std::to_string(n_changes) +
                                          " preset values; they are unsaved and are restored by sw_SlicerApplyCalibration");
        else
            sw_slicer_add_note(notes, "the test changed no preset values");
        const std::vector<std::string> steps = sw_slicer_calib_app_undo_steps();
        if (!steps.empty()) {
            std::string list;
            for (const std::string& s : steps)
                list += (list.empty() ? "'" : ", '") + s + "'";
            sw_slicer_add_note(notes, steps.size() == 1 ? "the app added its own undo step " + list + " (sw_SlicerUndo cannot revert it)" :
                                                          "the app added its own undo steps " + list + " (sw_SlicerUndo cannot revert them)");
        }
        if (def.mode == CalibMode::Calib_PA_Pattern && plates > 1)
            sw_slicer_add_note(notes, "the test patterns did not fit on one plate; only plate 1 carries the calibration settings (as in the "
                                      "app); use fewer speeds or accelerations");
        for (const json& n : mapping.at("notes"))
            sw_slicer_add_note(notes, n.get<std::string>());
        if (def.mode == CalibMode::Calib_Junction_Deviation && resolved.at("end").get<double>() > 0.3)
            sw_slicer_add_note(notes, "high values may cause layer shift");
        if (support.count("note"))
            sw_slicer_add_note(notes, support.at("note").get<std::string>());
        sw_slicer_add_note(notes, "slice with sw_SlicerSlice; the API never starts a print");
        sw_slicer_add_notes(notes, api_notes);

        m_res_data["test"]            = def.id;
        m_res_data["params"]          = resolved;
        m_res_data["project_name"]    = project_name;
        m_res_data["objects"]         = objects;
        m_res_data["plates"]          = plates;
        m_res_data["preset_changes"]  = changes;
        m_res_data["object_settings"] = sw_slicer_calib_object_settings();
        m_res_data["calibration"]     = mapping;
        m_res_data["project_dirty"]   = sw_slicer_project_dirty();
        m_res_data["presets_dirty"]   = sw_slicer_presets_dirty();
        m_res_data["notes"]           = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Which calibration test is active and the mapping height/line/pattern/object/layer -> value, as the app
// uses it when slicing. Read-only.
void SSWCP_SliceProject_Instance::sw_SlicerGetCalibration()
{
    try {
        sw_slicer_require_fff();
        Plater*              plater = wxGetApp().plater();
        const Print*         print  = sw_slicer_calib_print();
        const CalibMode      mode   = print != nullptr ? print->calib_mode() : CalibMode::Calib_None;
        const SwCalibRecord* rec    = sw_slicer_calib_valid_record();
        std::vector<std::string> notes;

        const SwCalibTestDef* def = nullptr;
        json                  params;
        std::string           source;
        if (rec != nullptr) {
            def    = &sw_slicer_calib_test_by_id(rec->test);
            params = rec->params;
            source = "api";
        } else if ((def = sw_slicer_calib_test_by_mode(mode)) != nullptr) {
            params = sw_slicer_calib_params_json(*def, print->calib_params());
            source = "app";
        }
        bool active = false;
        if (def != nullptr) {
            active = sw_slicer_calib_is_flow(*def) || mode == def->mode;
            if (rec != nullptr && !active)
                sw_slicer_add_note(notes, "the calibration mode was reset (for example by a model import); the test model no longer slices with the calibration steps");
        }
        if (mode != CalibMode::Calib_None && plater->model().objects.empty())
            sw_slicer_add_note(notes, "the calibration mode is still active from an earlier test; it is reset by the next model import (app behaviour)");
        if (active && plater->get_partplate_list().get_curr_plate_index() != 0)
            sw_slicer_add_note(notes, "the calibration settings belong to plate 1");

        json mapping = nullptr;
        if (def != nullptr) {
            const bool app_units = source == "app" && def->mode == CalibMode::Calib_Vol_speed_Tower;
            mapping = sw_slicer_calib_mapping(*def, params, print, rec != nullptr ? rec->flow_ratio : 0, app_units);
        }

        m_res_data["active"]        = active;
        m_res_data["test"]          = def != nullptr ? json(def->id) : json(nullptr);
        m_res_data["mode"]          = sw_slicer_calib_mode_name(mode);
        m_res_data["source"]        = source.empty() ? json(nullptr) : json(source);
        m_res_data["params"]        = def != nullptr ? params : json(nullptr);
        m_res_data["project_name"]  = rec != nullptr ? rec->project_name : into_u8(plater->get_project_name());
        m_res_data["record_valid"]  = rec != nullptr;
        m_res_data["mapping"]       = mapping;
        m_res_data["notes"]         = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

// Restore the preset values the last API calibration changed and set the chosen value in the preset
// that holds the result. Nothing is saved (sw_SlicerSavePreset does that); presets are not part of undo.
void SSWCP_SliceProject_Instance::sw_SlicerApplyCalibration()
{
    try {
        // 1. Guards.
        sw_slicer_require_idle();
        sw_slicer_require_not_slicing();
        sw_slicer_require_not_loading();
        sw_slicer_require_fff();

        // 2. Which test.
        SwCalibRecord*           rec      = sw_slicer_calib_valid_record();
        const bool               has_test = sw_slicer_has_param(m_param_data, "test");
        std::vector<std::string> notes;
        if (has_test && !m_param_data.at("test").is_string())
            throw std::runtime_error("'test' must be a test name (see sw_SlicerListCalibrations)");
        if (rec == nullptr && !has_test)
            throw std::runtime_error("no calibration made through the API in this project; pass 'test'");
        const SwCalibTestDef& def = sw_slicer_calib_test_by_id(has_test ? m_param_data.at("test").get<std::string>() : rec->test);
        if (rec != nullptr && rec->test != def.id)
            throw std::runtime_error("the last calibration was '" + rec->test + "'");
        bool restore = true;
        if (sw_slicer_has_param(m_param_data, "restore_test_settings")) {
            if (!m_param_data.at("restore_test_settings").is_boolean())
                throw std::runtime_error("'restore_test_settings' must be true or false");
            restore = m_param_data.at("restore_test_settings").get<bool>();
        }

        // 3. Result key.
        if (def.result_type == nullptr)
            throw std::runtime_error("'" + std::string(def.id) + "' has no slicer setting to apply; use the mapping (sw_SlicerGetCalibration)");
        const Preset::Type result_type = sw_slicer_preset_type(def.result_type);

        // 4. Value.
        const bool flow       = sw_slicer_calib_is_flow(def);
        const bool has_value  = sw_slicer_has_param(m_param_data, "value");
        const bool has_object = sw_slicer_has_param(m_param_data, "object");
        if (has_object && !flow)
            throw std::runtime_error("'object' is only for the flow rate tests");
        if (has_value && has_object)
            throw std::runtime_error("pass either 'value' or 'object', not both");
        double value = 0;
        if (has_object) {
            if (rec == nullptr)
                throw std::runtime_error("'object' needs the calibration of sw_SlicerCalibrate in this project; pass 'value' instead");
            if (!m_param_data.at("object").is_number_integer())
                throw std::runtime_error("'object' must be an object index");
            const int  obj     = m_param_data.at("object").get<int>();
            const json mapping = sw_slicer_calib_mapping(def, rec->params, sw_slicer_calib_print(), rec->flow_ratio, false);
            bool       found   = false;
            for (const json& row : mapping.at("table"))
                if (row.at("object").get<int>() == obj && row.at("flow_ratio").is_number()) {
                    value = row.at("flow_ratio").get<double>();
                    found = true;
                }
            if (!found)
                throw std::runtime_error("object " + std::to_string(obj) + " is not a flow test object (see the mapping of sw_SlicerGetCalibration)");
        } else if (has_value) {
            const json& v = m_param_data.at("value");
            if (!v.is_number() || !std::isfinite(v.get<double>()))
                throw std::runtime_error("'value' must be a number");
            value = v.get<double>();
        } else {
            throw std::runtime_error("specify 'value'");
        }
        const bool temp = def.mode == CalibMode::Calib_Temp_Tower;
        if (flow && (value <= 0 || value > 2))
            throw std::runtime_error("'value' must be > 0 and <= 2");
        if (temp && (value != std::floor(value) || value < 150 || value > 350))
            throw std::runtime_error("'value' must be a whole number from 150 to 350");
        if (sw_slicer_calib_is_pa(def) && (value < 0 || value > 2))
            throw std::runtime_error("'value' must be from 0 to 2");
        if (def.mode == CalibMode::Calib_Vol_speed_Tower && (value <= 0 || value > 200))
            throw std::runtime_error("'value' must be > 0 and <= 200");
        if (def.mode == CalibMode::Calib_Retraction_tower && (value < 0 || value > 10))
            throw std::runtime_error("'value' must be from 0 to 10");
        if (def.mode == CalibMode::Calib_Junction_Deviation && (value < 0 || value >= 1))
            throw std::runtime_error("'value' must be >= 0 and < 1");
        if (rec != nullptr && !flow && rec->params.count("start") && rec->params.count("end")) {
            const double a = rec->params.at("start").get<double>(), b = rec->params.at("end").get<double>();
            if (value < std::min(a, b) - 1e-9 || value > std::max(a, b) + 1e-9)
                sw_slicer_add_note(notes, sw_slicer_calib_num(value) + " is outside the tested range " + sw_slicer_calib_num(a) + "\xE2\x80\x93" +
                                              sw_slicer_calib_num(b));
        }

        // 5. Restore the test settings: every recorded change whose value is still the test value.
        json                   restored     = json::array();
        json                   not_restored = json::array();
        std::set<Preset::Type> touched;
        auto is_result_key = [&](const SwCalibRecord::Change& c) {
            return c.type == result_type && std::any_of(def.result_keys.begin(), def.result_keys.end(), [&c](const char* k) { return c.key == k; });
        };
        auto same = [](const ConfigOption* a, const ConfigOption* b) { return a == nullptr ? b == nullptr : (b != nullptr && *a == *b); };
        if (restore && rec == nullptr)
            sw_slicer_add_note(notes, "test settings not restored: no API calibration record (check sw_SlicerGetPresetChanges)");
        if (restore && rec != nullptr) {
            // The flow-variant vectors follow the *_flow_support list (as in sw_SlicerDiscardPresetChanges):
            // when the test changed that list, all changes of that type must be restorable together.
            for (Preset::Type t : {Preset::TYPE_PRINT, Preset::TYPE_FILAMENT}) {
                ConfigFlowDomain domain = ConfigFlowDomain::Process;
                if (sw_slicer_flow_options(t, domain) == nullptr)
                    continue;
                const std::string         support_key = flow_support_key(domain);
                const DynamicPrintConfig& cfg         = sw_slicer_preset_collection(t).get_edited_preset().config;
                bool                      support_changed = false, blocked = false;
                for (const SwCalibRecord::Change& c : rec->changes) {
                    if (c.type != t)
                        continue;
                    support_changed = support_changed || c.key == support_key;
                    const ConfigOption* cur = cfg.option(c.key);
                    blocked = blocked || (!same(cur, c.after.get()) && !same(cur, c.before.get()) && !is_result_key(c));
                }
                if (support_changed && blocked)
                    throw std::runtime_error("the test changed the flow variants and they were changed again afterwards; discard all " +
                                             sw_slicer_preset_type_name(t) + " changes instead (sw_SlicerDiscardPresetChanges)");
            }
        }

        std::vector<std::string> api_notes;
        json                     applied = json::array();
        {
            sswcp_api::Scope api_scope;
            if (restore && rec != nullptr) {
                for (const SwCalibRecord::Change& c : rec->changes) {
                    DynamicPrintConfig& cfg = sw_slicer_preset_collection(c.type).get_edited_preset().config;
                    const ConfigOption* cur = cfg.option(c.key);
                    if (same(cur, c.before.get()))
                        continue; // already restored (an earlier apply)
                    if (!same(cur, c.after.get())) {
                        if (!is_result_key(c)) // the result key is set below anyway
                            not_restored.push_back({{"type", sw_slicer_preset_type_name(c.type)}, {"key", c.key}, {"reason", "changed after the calibration"}});
                        continue;
                    }
                    if (c.before == nullptr) {
                        // Not erased: a preset without the key would be incomplete. Left as it is.
                        not_restored.push_back({{"type", sw_slicer_preset_type_name(c.type)}, {"key", c.key}, {"reason", "missing before the test; left as it is"}});
                        continue;
                    }
                    cfg.set_key_value(c.key, c.before->clone());
                    bool truncated = false;
                    json item      = {{"type", sw_slicer_preset_type_name(c.type)}, {"key", c.key}, {"value", sw_slicer_calib_option_text(c.before.get(), truncated)}};
                    if (truncated)
                        item["truncated"] = true;
                    restored.push_back(item);
                    touched.insert(c.type);
                }
            }

            // 6. Set the result in every element of the option (flow variants and extruders stay consistent).
            DynamicPrintConfig& cfg = sw_slicer_preset_collection(result_type).get_edited_preset().config;
            for (const char* key : def.result_keys) {
                const std::string   k    = key;
                const std::string   elem = k == "enable_pressure_advance" ? std::string("1") :
                                           temp                          ? std::to_string(static_cast<int>(value)) :
                                                                           sw_slicer_calib_num(value);
                const ConfigOption* cur  = cfg.option(k);
                size_t              n    = 1;
                if (cur != nullptr && cur->is_vector())
                    n = std::max<size_t>(1, static_cast<const ConfigOptionVectorBase*>(cur)->size());
                std::string text;
                for (size_t i = 0; i < n; ++i)
                    text += (i == 0 ? "" : ",") + elem;
                bool              truncated = false;
                const json        before_v  = sw_slicer_calib_option_text(cur, truncated);
                DynamicPrintConfig probe     = cfg;
                probe.set_deserialize_strict(k, text); // throws for an invalid value, before anything is set
                cfg.set_key_value(k, probe.option(k)->clone());
                applied.push_back({{"type", def.result_type}, {"key", k}, {"before", before_v}, {"after", sw_slicer_calib_option_text(cfg.option(k), truncated)}});
            }
            touched.insert(result_type);
            for (Preset::Type t : touched) {
                Tab* tab = sw_slicer_preset_tab(t);
                tab->update_dirty();
                tab->reload_config();
            }
            api_notes = sswcp_api::notes();
        }

        // 7. Reply.
        if (!restored.empty())
            sw_slicer_add_note(notes, "the test's preset settings were restored; the test model itself still slices with the calibration steps "
                                      "(calibration mode and object settings stay); start a new project to leave the test");
        if (temp)
            sw_slicer_add_note(notes, "only nozzle_temperature was set; nozzle_temperature_initial_layer is unchanged (set it with sw_SlicerSetConfig if wanted)");
        sw_slicer_add_note(notes, "the values are unsaved preset changes; save them with sw_SlicerSavePreset (type '" +
                                      std::string(def.result_type) + "')");
        sw_slicer_add_note(notes, "presets are not part of undo");
        sw_slicer_add_notes(notes, api_notes);
        m_res_data["test"]          = def.id;
        m_res_data["applied"]       = applied;
        m_res_data["restored"]      = restored;
        m_res_data["not_restored"]  = not_restored;
        m_res_data["presets_dirty"] = sw_slicer_presets_dirty();
        m_res_data["notes"]         = notes;
        send_to_js();
        finish_job();
    } catch (std::exception& e) {
        handle_general_fail(-1, wxString::FromUTF8(e.what()));
    }
}

}} // namespace Slic3r::GUI
