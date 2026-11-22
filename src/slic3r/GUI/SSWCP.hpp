// Web communication protocol implementation for Slicer Studio
#ifndef SSWCP_HPP
#define SSWCP_HPP

#include <iostream>
#include <mutex>
#include <stack>
#include <wx/socket.h>

#include <wx/webview.h>
#include <unordered_set>
#include <unordered_map>
#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include "nlohmann/json.hpp"
#include "slic3r/Utils/Bonjour.hpp"
#include "slic3r/Utils/TimeoutMap.hpp"
#include "slic3r/Utils/PrintHost.hpp"
#include "slic3r/Utils/MQTT.hpp"
#include "libslic3r/SSWCPProtocol.hpp"


using namespace nlohmann;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;


//WCP Interface definition
#define UPDATE_PRIVACY_STATUS "sw_SubUserUpdatePrivacy"
#define GET_PRIVACY_STATUS "sw_GetUserUpdatePrivacy"
#define UPLOAD_CAMERA_TIMELAPSE "sw_UploadCameraTimelapse"
#define UPLOAD_ASYNC_TIMELAPSE_INSTANCE "sw_UploadAsyncTimelapseInstance"
#define DELETE_CAMERA_TIMELAPSE "sw_DeleteCameraTimelapse"
#define GET_DEVICEDATA_STORAGESPACE "sw_GetDeviceDataStorageSpace"
#define DOWNLOAD_FILE_AND_OPEN "sw_DownLoadFileAndOpen"
#define CANCEL_DOWNLOAD "sw_CancelDownload"
#define SUBSCRIBE_DOWNLOAD_STATE "sw_SubscribeDownloadState"
#define UNSUBSCRIBE_DOWNLOAD_STATE "sw_UnsubscribeDownloadState"
#define DOWN_LOAD_FILE "sw_DownLoadFile"
#define FILE_VIEW "sw_FileView"
#define OPEN_TIMELAPSE_FOLDER "sw_OpenTimelapseFolder"
#define GET_FILES_FROM_DIR "sw_GetFilesFromDir"
#define NOTIFY_UPLOAD_TIMELASPE "sw_NotifyUploadTimelaspe"

namespace Slic3r { namespace GUI {

class WCP_Logger
{
public:
    bool run();

    static WCP_Logger& getInstance();

    // Add a log message to the queue
    void add_log(const wxString& content, bool is_web, wxString time, wxString module, wxString level);

    void worker();

    bool set_level(wxString& level);

    int get_level() { return m_log_level; }

    std::unordered_map<wxString, int> m_log_level_map = {
        {"debug", 0},{"info", 1},{"warning", 2},{"error", 3},{"fatal", 4},
    };

private:
    WCP_Logger();
    ~WCP_Logger();
    std::mutex           m_log_mtx; // Mutex for thread-safe access
    std::queue<wxString> m_log_que;

    std::thread m_work_thread;

    std::mutex     m_end_mtx;
    bool m_end = false;
    wxSocketClient m_client;

    asio::io_context io_ctx;
    tcp::socket*      socket = nullptr;
    tcp::resolver*    resolver;

    bool inited = false;

    int                               m_log_level     = 0;

};

// Base class for handling web communication instances
class SSWCP_Instance : public std::enable_shared_from_this<SSWCP_Instance>
{
public:
    // Types of communication instances
    enum INSTANCE_TYPE {
        COMMON,             // Common instance type
        MACHINE_FIND,       // For machine discovery
        MACHINE_CONNECT,    // For machine connection
        MACHINE_OPTION,     // For machine options/settings
        SLICE_PROJECT,      // For homepage project business
        USER_LOGIN,         // For user login
        MACHINE_MANAGE,     // For homepage machine card
        MQTT_AGENT,         // For mqtt-agent
    };

public:
    // Constructor initializes instance with command and parameters
    SSWCP_Instance(std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview)
        : m_cmd(cmd), m_header(header), m_webview(webview), m_event_id(event_id), m_param_data(data)
    {}

    virtual ~SSWCP_Instance() {
        if (m_work_thread.joinable())
            m_work_thread.detach();
    }

    // Process the command
    virtual void process();

    // Send response back to JavaScript
    virtual void send_to_js();

    // Clean up after job completion
    virtual void finish_job();

    // General failure
    void handle_general_fail(int code = -1, const wxString& msg = "failure");

    // Get instance type
    virtual INSTANCE_TYPE getType() { return m_type; }

    // Check if instance is stopped
    virtual bool is_stop() { return false; }
    virtual void set_stop(bool stop) {}

    // Mark instance as illegal (invalid)
    virtual void set_Instance_illegal();
    virtual bool is_Instance_illegal();

    // Get associated webview
    wxWebView* get_web_view() const;
    void       set_web_view(wxWebView* view);

    // True for a request that came in through the local slicer API instead of a webview.
    virtual bool from_slicer_api() const { return false; }

public:
    // Handle timeout event
    virtual void on_timeout();

    static void on_mqtt_msg_arrived(std::shared_ptr<SSWCP_Instance> obj, const json& response);

    static void on_mqtt_status_msg_arrived(std::shared_ptr<SSWCP_Instance> obj, const json& response);

private:
    // Test methods
    void sync_test();
    void async_test();

    void test_mqtt_request();

    // select tab
    void sw_SwitchTab();

    // unsubscribe
    void sw_Webview_Unsubscribe();
    void sw_UnsubscribeAll();
    void sw_Unsubscribe_Filter();

    // get file stream
    void sw_GetFileStream();
    void sw_GetActiveFile();

    // orca console
    void sw_LaunchConsole();
    void sw_Log();
    void sw_SetLogLevel();

    // mac quit
    void sw_Exit();

    // orca log
    void sw_FileLog();

    // open browser
    void sw_OpenBrowser();
    void sw_OpenOrcaWebview();

    // Sentry
    void sw_UploadEvent();
    // SnapLog
    void sw_SnapLog();

    // Get software basic info
    void sw_GetSoftwareInfo();

    // open network dialog
    void sw_OpenNetworkDialog();


protected:
    std::thread m_work_thread; // Worker thread

public:
    std::string m_cmd;           // Command to execute
    json        m_header;        // Request header
    wxWebView*  m_webview;       // Associated webview
    std::string m_event_id;      // Event identifier
    json        m_param_data;    // Command parameters

    json        m_res_data;      // Response data
    int         m_status = 200;  // Response status code
    std::string m_msg = "OK";    // Response message

    static bool m_first_connected;

protected:
    INSTANCE_TYPE m_type = COMMON;  // Instance type

private:
    bool m_illegal = false;      // Invalid flag
    std::mutex m_illegal_mtx;    // Mutex for illegal flag
};

// Instance class for handling machine connection
class SSWCP_MachineConnect_Instance : public SSWCP_Instance
{
public:
    SSWCP_MachineConnect_Instance(std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview)
        : SSWCP_Instance(cmd, header, data, event_id, webview)
    {
        m_type = MACHINE_CONNECT;
    }

    ~SSWCP_MachineConnect_Instance()
    {
        if (m_work_thread.joinable())
            m_work_thread.detach();
    }

    void process() override;

private:
    // Connection test methods
    void sw_test_connect();
    void sw_connect();
    void sw_disconnect();

    void sw_connect_other_device();

    void sw_get_pin_code();

    // Subscribe to foreground/background change events (event_id=205890)
    void sw_SubscribeForegroundChange();

    // Owns the one-shot pin-code MQTT client (sw_get_pin_code). Released on
    // the UI thread after the response — never from a Paho callback thread,
    // where destroying a client is not allowed.
    static std::shared_ptr<MqttClient> m_pin_client;
    static void release_pin_client() { m_pin_client.reset(); }

};

// Instance class for handling machine discovery
class SSWCP_MachineFind_Instance : public SSWCP_Instance
{
public:
    SSWCP_MachineFind_Instance(std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview)
        : SSWCP_Instance(cmd, header, data, event_id, webview)
    {
        m_type = MACHINE_FIND;
    }

    void process() override;

    bool is_stop() { return m_stop; }
    void set_stop(bool stop);

private:
    // Machine discovery methods
    void sw_GetMachineFindSupportInfo();
    void sw_StartMachineFind();
    void sw_StopMachineFind();
    void sw_WakeupFind();

private:
    void add_machine_to_list(const json& machine_info);
    void onOneEngineEnd();

private:
    std::mutex m_machine_list_mtx;  // Mutex for machine list
    std::mutex m_stop_mtx;          // Mutex for stop flag

private:
    std::unordered_map<std::string, json> m_machine_list;  // Found machines

private:
    int m_engine_end_count = 0;                         // Counter for finished discovery engines
    std::vector<std::shared_ptr<Bonjour>> m_engines;    // Discovery engines
    bool m_stop = false;                                // Stop flag
};

// Instance class for handling machine options
class SSWCP_MachineOption_Instance : public SSWCP_Instance
{
public:
    SSWCP_MachineOption_Instance(std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview)
        : SSWCP_Instance(cmd, header, data, event_id, webview)
    {
        m_type = MACHINE_OPTION;
    }

    ~SSWCP_MachineOption_Instance()
    {
        if (m_work_thread.joinable())
            m_work_thread.detach();
    }

    void process() override;

private:

    // Machine option methods
    void sw_SendGCodes();
    void sw_FileGetStatus();
    void sw_MachinePrintStart();
    void sw_MachinePrintPause();
    void sw_MachinePrintResume();
    void sw_MachinePrintCancel();

    // SYSTEM
    void sw_MachineFilesRoots();
    void sw_MachineFilesMetadata();
    void sw_MachineFilesThumbnails();
    void sw_MachineFilesGetDirectory();
    void sw_CameraStartMonitor();
    void sw_CameraStopMonitor();
    void sw_DeleteMachineFile();

    // PrePrint
    void sw_GetFileFilamentMapping();
    void sw_SetFilamentMappingComplete();
    void sw_FinishFilamentMapping();
    // sw_FinishFilamentMapping post-close handlers, one per FinishFilamentMappingEvent.
    // Add a new handler here when a new event value is introduced.
    void on_finish_filament_mapping_custom_flow_regroup();

    // new
    void sw_SetDeviceName();
    void sw_ControlLed();
    void sw_ControlPrintSpeed();
    void sw_BedMesh_AbortProbeMesh();
    void sw_ControlPurifier();
    void sw_ControlMainFan();
    void sw_ControlGenericFan();
    void sw_ControlBedTemp();
    void sw_ControlExtruderTemp();
    void sw_FilesThumbnailsBase64();
    void sw_exception_query();
    void sw_GetFileListPage();
    void sw_UploadCameraTimelapse();
    void sw_UploadAsyncTimelapseInstance();
    void sw_DeleteCameraTimelapse();
    void sw_GetCameraTimelapseInstance();

    void sw_DefectDetactionConfig();    
    void sw_PrinterDefectDetection();

    void sw_GetDeviceDataStorageSpace();

    void CmdForwarding();

    // Download machine file
    void sw_DownloadMachineFile();

    // upload file to machine
    void sw_UploadFiletoMachine();

    // get is legal to send & print
    void sw_GetPrintLegal();
    
    void sw_GetPrintZip();    
    void sw_FinishPreprint();    
    void sw_ServerClientManagerSetUserinfo();

    // request device download file and print
    void sw_PullCloudFile();

    // request deivice cancel download file
    void sw_CancelPullCloudFile();

    // request device download file and print
    void sw_StartCloudPrint();

    // Request device to start local file print
    void sw_StartLocalPrint();

    // Request device heartbeat
    void sw_MachineHeartbeat();

};

// Instance class for page state change subscription
class SSWCP_PageStateChange_Instance : public SSWCP_Instance
{
public:
    SSWCP_PageStateChange_Instance(std::string cmd, const json& header, const json& data,
                                    std::string event_id, wxWebView* webview)
        : SSWCP_Instance(cmd, header, data, event_id, webview)
    {
        m_type = COMMON;
    }

    ~SSWCP_PageStateChange_Instance() {}

    void process() override;

private:
    void sw_SubscribePageStateChange();
    void sw_UnsubscribePageStateChange();
};

// Instance class for Snapmaker user login
class SSWCP_UserLogin_Instance : public SSWCP_Instance
{
public:
    SSWCP_UserLogin_Instance(std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview)
        : SSWCP_Instance(cmd, header, data, event_id, webview)
    {
        m_type = USER_LOGIN;
    }

    ~SSWCP_UserLogin_Instance() {}

    void process() override;

private:
    void sw_GetUserUpdatePrivacy();

    void sw_SubUserUpdatePrivacy();

    void sw_DownloadFileAndOpen();

    void sw_DownloadFileEx();

    void sw_DownLoadFile();

    void sw_CancelDownload();

    void sw_FileView();
    void sw_OpenTimelapseFolder();
    void sw_SubscribeDownloadState();
    void sw_UnsubscribeDownloadState();
    void sw_GetFilesFromDir();
    void sw_NotifyUploadTimelaspe();

public:
    // Passive subscription entry — sw_SubscribeDownloadState only registers,
    // downloads are triggered by sw_DownLoadFile which pushes "download_complete"
    // to matching subscribers on completion.
    struct SubscribeInfo {
        std::string event_id;
        std::string sn;
        std::string type;     // "timelapse"
        wxWebView*  webview;  // not owned — captured so we can push events after the caller instance is gone
    };
    static std::unordered_map<std::string, std::shared_ptr<SubscribeInfo>> m_subscribe_map;  // event_id -> info

    // Push a single file's download state to every subscriber whose sn matches.
    // Does NOT remove subscribers (can be called multiple times).
    // state: "success" | "failed" | "cancelled"
    static void push_timelapse_state(const std::string& sn,
                                     const std::string& file_name,
                                     const std::string& file_url,
                                     const std::string& date_index,
                                     const std::string& save_path,
                                     const std::string& state);
};

// Instance class for homepage business
class SSWCP_SliceProject_Instance : public SSWCP_Instance
{
public:
    SSWCP_SliceProject_Instance(std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview)
        : SSWCP_Instance(cmd, header, data, event_id, webview)
    {
        m_type = SLICE_PROJECT;
    }

    ~SSWCP_SliceProject_Instance()
    {

    }

    void process() override;

private:
    void sw_NewProject();

    void sw_OpenProject();

    void sw_GetRecentProjects(); 

    void sw_OpenRecentFile();

    void sw_DeleteRecentFiles();

    void sw_SubscribeRecentFiles();

    // sw_Slicer*: live control of the slicer (implemented in SSWCPSlicer.cpp).
    // Probe and preset config.
    void sw_SlicerPing();
    void sw_SlicerGetConfig();
    void sw_SlicerSetConfig();

    // Slicing, status polling, print time and material usage, notifications after slicing.
    void sw_SlicerSlice();
    void sw_SlicerCancelSlice();
    void sw_SlicerGetStatus();
    void sw_SlicerGetStats();
    void sw_SlicerGetLegend();
    void sw_SlicerGetSliceIssues();

    // View switching and layer navigation in the preview.
    void sw_SlicerShowView();
    void sw_SlicerGoToLayer();
    void sw_SlicerGetLayerCodes();
    void sw_SlicerAddLayerCode();
    void sw_SlicerEditLayerCode();
    void sw_SlicerDeleteLayerCode();

    // Color change from layer N (or height z) via MMU segmentation painting; an alias of
    // sw_SlicerPaint {type:"color", action:"height_band"}.
    void sw_SlicerSetColorFromLayer();

    // 3D toolbar: objects, selection, job status, undo/redo, open/close gizmo.
    void sw_SlicerListObjects();
    void sw_SlicerSelect();
    void sw_SlicerGetSelection();
    void sw_SlicerJobStatus();
    void sw_SlicerUndo();
    void sw_SlicerRedo();
    void sw_SlicerOpenGizmo();
    void sw_SlicerCloseGizmo();

    // Add model and plate (API mode without dialogs).
    void sw_SlicerAddModel();
    void sw_SlicerAddPlate();

    // Orient and arrange (asynchronous through the job worker).
    void sw_SlicerOrient();
    void sw_SlicerArrange();

    // Move, rotate, scale.
    void sw_SlicerMove();
    void sw_SlicerRotate();
    void sw_SlicerScale();

    // Split, lay on face, cut (plane + groove).
    void sw_SlicerSplitObjects();
    void sw_SlicerSplitParts();
    void sw_SlicerLayOnFace();
    void sw_SlicerCut();

    // Variable layer height and brim ears.
    void sw_SlicerLayerHeight();
    void sw_SlicerBrimEars();

    // Parametric painting (support / seam / fuzzy skin / color) and "Remap filaments" (color).
    void sw_SlicerPaint();

    // Text (Emboss), mesh boolean, measure and assemble.
    void sw_SlicerAddText();
    void sw_SlicerEditText();
    void sw_SlicerMeshBoolean();
    void sw_SlicerGetMeasurement();
    void sw_SlicerAssemble();
    void sw_SlicerResetMeasurement();

    // Project lifecycle: state, project info, delete, new/open/save project.
    void sw_SlicerGetProjectState();
    void sw_SlicerSetProjectInfo();
    void sw_SlicerDelete();
    void sw_SlicerDeletePlate();
    void sw_SlicerReorder();
    void sw_SlicerNewProject();
    void sw_SlicerOpenProject();
    void sw_SlicerSaveProject();
    void sw_SlicerQuit();

    // Slice all plates and export.
    void sw_SlicerSliceAll();
    void sw_SlicerExportGcode();
    void sw_SlicerExportSliced();

    // Object settings and filament, presets, filament slots, plate settings.
    void sw_SlicerGetObjectConfig();
    void sw_SlicerSetObjectConfig();
    void sw_SlicerSetFilament();
    void sw_SlicerListPresets();
    void sw_SlicerSelectPreset();
    void sw_SlicerSetFilamentCount();
    void sw_SlicerSetFilamentColor();
    void sw_SlicerGetPlateSettings();
    void sw_SlicerSetPlateSettings();
    void sw_SlicerSetBedType();
    void sw_SlicerGetNozzleSetup();
    void sw_SlicerSetNozzleDiameter();
    void sw_SlicerSetFilamentsVisible();
    void sw_SlicerSetNozzleFlow();
    void sw_SlicerGetFilamentGrouping();
    void sw_SlicerSetFilamentGrouping();
    void sw_SlicerGetFlushVolumes();
    void sw_SlicerSetFlushVolumes();
    void sw_SlicerGetWipeTower();
    void sw_SlicerSetWipeTower();
    void sw_SlicerListColorMixes();
    void sw_SlicerSuggestColorMix();
    void sw_SlicerAddColorMix();
    void sw_SlicerEditColorMix();
    void sw_SlicerDeleteColorMix();

    // Parts, object actions and plates.
    void sw_SlicerAddVolume();
    void sw_SlicerChangeVolumeType();
    void sw_SlicerRename();
    void sw_SlicerClone();
    void sw_SlicerMirror();
    void sw_SlicerReloadFromDisk();
    void sw_SlicerReplaceWithStl();
    void sw_SlicerSubdivisionMesh();
    void sw_SlicerConvertUnits();
    void sw_SlicerDrop();
    void sw_SlicerCenter();
    void sw_SlicerSetPrintable();
    void sw_SlicerScaleToFit();
    void sw_SlicerListPlates();
    void sw_SlicerSelectPlate();
    void sw_SlicerDuplicatePlate();
    void sw_SlicerSetPlate();

    // Preview controls and canvas screenshot.
    void sw_SlicerGetPreviewState();
    void sw_SlicerSetPreview();
    void sw_SlicerSetLayerRange();
    void sw_SlicerSetMove();
    void sw_SlicerGetToolState();
    void sw_SlicerGetGcodeLines();
    void sw_SlicerSetCamera();
    void sw_SlicerScreenshot();

    // Object clipboard, visibility, instances, plates and model export.
    void sw_SlicerCopy();
    void sw_SlicerPaste();
    void sw_SlicerSetVisible();
    void sw_SlicerSetInstanceCount();
    void sw_SlicerInstanceToObject();
    void sw_SlicerMoveToPlate();
    void sw_SlicerExportStl();
    void sw_SlicerExport3mf();

    // Preset management: changes, compare, save, delete, discard, Import Configs, Export Preset Bundle.
    void sw_SlicerGetPresetChanges();
    void sw_SlicerComparePresets();
    void sw_SlicerSavePreset();
    void sw_SlicerDeletePreset();
    void sw_SlicerDiscardPresetChanges();
    void sw_SlicerImportConfigs();
    void sw_SlicerExportPresets();

    // Calibration: list tests, create a test project, read the value mapping, apply a result.
    void sw_SlicerListCalibrations();
    void sw_SlicerCalibrate();
    void sw_SlicerGetCalibration();
    void sw_SlicerApplyCalibration();

    // Toolbar extras: text info, cut section, merge, simplify, layer ranges, SVG, repair.
    void sw_SlicerGetText();
    // Text styles (Style list of the text tool): list, save, save_as, rename, delete.
    void sw_SlicerTextStyle();
    void sw_SlicerCutSection();
    void sw_SlicerMergeObjects();
    void sw_SlicerSimplify();
    void sw_SlicerGetLayerRanges();
    void sw_SlicerAddLayerRange();
    void sw_SlicerEditLayerRange();
    void sw_SlicerDeleteLayerRange();
    void sw_SlicerAddSvg();
    // Edit an SVG part afterwards with the fields of the SVG tool.
    void sw_SlicerEditSvg();
    void sw_SlicerRepair();
    // Context: preferences, view preferences and view options.
    void sw_SlicerGetPreferences();
    void sw_SlicerSetViewPreferences();
    void sw_SlicerSetPreferences();
    void sw_SlicerSetViewOptions();
};

// Main SSWCP class for managing communication instances
class SSWCP
{
public:
    // Handle incoming web messages
    static void handle_web_message(std::string message, wxWebView* webview);

    // Create new SSWCP instance
    static std::shared_ptr<SSWCP_Instance> create_sswcp_instance(
        std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview);

    // Delete instance
    static void delete_target(SSWCP_Instance* target);

    // Extend a one-shot instance's timeout by the default timeout; used by
    // long-running modal commands so their pending response is not dropped
    // after the default 80 s.
    static void renew_instance_timeout(SSWCP_Instance* instance);

    // Stop machine discovery
    static void stop_machine_find();

    // Handle webview deletion
    static void on_webview_delete(wxWebView* webview);

    // update the active file name
    static void update_active_filename(const std::string& filename);

    // update the display name
    static void update_display_filename(const std::string& display_name);

    // get the active file name
    static std::string get_active_filename();

    static std::string get_display_filename();

    static nlohmann::json build_active_file_metadata();
    static nlohmann::json build_active_file_json(const std::string& file_path, const std::string& file_name, bool is_zip = false);
    static nlohmann::json build_filament_mapping_json(const std::string& filename, const std::string& display_name);

    static std::mutex m_file_size_mutex;
    static long long m_active_file_size;


    static std::unordered_map<std::string, int> m_tab_map; // for switching tab

    // Sorted list of all registered sw_Slicer* commands (reported by sw_SlicerPing).
    static std::vector<std::string> slicer_commands();

private:
    static std::unordered_set<std::string> m_machine_find_cmd_list;     // Machine find commands
    static std::unordered_set<std::string> m_machine_option_cmd_list;   // Machine option commands
    static std::unordered_set<std::string> m_machine_connect_cmd_list;  // Machine connect commands
    static std::unordered_set<std::string> m_project_cmd_list; // homepage project commands
    static std::unordered_set<std::string> m_login_cmd_list; // homepage login commands
    static std::unordered_set<std::string> m_page_state_cmd_list; // page state change commands;

    static TimeoutMap<SSWCP_Instance*, std::shared_ptr<SSWCP_Instance>> m_instance_list;  // Active instances
    static constexpr std::chrono::milliseconds DEFAULT_INSTANCE_TIMEOUT{80000}; // Default timeout (8s)

    static std::string m_active_gcode_filename; // name of the file which is pretend to be upload and print
    static std::string m_display_gcode_filename; // name for display

}; 

class MachineIPType
{
public:
    static MachineIPType* getInstance();

    void add_instance(const std::string& ip, const std::string& machine_type)
    {
        std::lock_guard<std::mutex> lock(m_map_mtx);
        m_ip_type_map[ip] = machine_type;
    }

    bool get_machine_type(const std::string& ip, std::string& output)
    {
        std::lock_guard<std::mutex> lock(m_map_mtx);
        auto it = m_ip_type_map.find(ip);
        if (it == m_ip_type_map.end()) return false;
        output = it->second;
        return true;
    }

private:
    std::mutex                                   m_map_mtx;
    std::unordered_map<std::string, std::string> m_ip_type_map;

};

std::string base64_encode(const char* data, size_t len);
std::string make_wcp_download_url(const std::string& file_path);

}};

#endif
