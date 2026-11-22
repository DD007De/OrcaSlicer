#ifndef slic3r_CreatePresetsDialog_hpp_
#define slic3r_CreatePresetsDialog_hpp_

#include "libslic3r/Preset.hpp"
#include "wxExtensions.hpp"
#include "GUI_Utils.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/TextInput.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/RadioBox.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "miniz.h"
#include "ParamsDialog.hpp"

namespace Slic3r { 
namespace GUI {

class CreateFilamentPresetDialog : public DPIDialog
{
public:
    CreateFilamentPresetDialog(wxWindow *parent);
    ~CreateFilamentPresetDialog();

protected:
    enum FilamentOptionType { 
        VENDOR = 0,
        TYPE,
        SERIAL,
        FILAMENT_PRESET,
        PRESET_FOR_PRINTER,
        FILAMENT_NAME_COUNT
    };

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;
    bool        is_check_box_selected();
    wxBoxSizer *create_item(FilamentOptionType option_type);
    wxBoxSizer *create_vendor_item();
    wxBoxSizer *create_type_item();
    wxBoxSizer *create_serial_item();
    wxBoxSizer *create_filament_preset_item();
    wxBoxSizer *create_filament_preset_for_printer_item();
    wxWindow   *create_dialog_buttons();

private:
    void          clear_filament_preset_map();
    wxArrayString get_filament_preset_choices();
    wxBoxSizer *  create_radio_item(wxString title, wxWindow *parent, wxString tooltip, std::vector<std::pair<RadioBox *, wxString>> &radiobox_list);
    void          select_curr_radiobox(std::vector<std::pair<RadioBox *, wxString>> &radiobox_list, int btn_idx);
    wxString      curr_create_filament_type();
    void          get_filament_presets_by_machine();
    void          get_all_filament_presets();
    void          get_all_visible_printer_name();
    void          update_dialog_size();
    template<typename T>
    void          sort_printer_by_nozzle(std::vector<std::pair<std::string, T>> &printer_name_to_filament_preset);

private:
    struct CreateType
    {
        wxString base_filament;
        wxString base_filament_preset;
    };

private:
    std::vector<std::pair<RadioBox *, wxString>>                     m_create_type_btns;
    std::unordered_map<::CheckBox *, std::pair<std::string, Preset *>> m_filament_preset;
    std::unordered_map<::CheckBox *, std::pair<std::string, Preset *>> m_machint_filament_preset;
    std::unordered_map<std::string, std::vector<Preset *>>           m_filament_choice_map;
    std::unordered_map<wxString, std::string>                        m_public_name_to_filament_id_map;
    std::unordered_map<std::string, Preset *>                        m_all_presets_map;
    std::unordered_set<std::string>                                  m_system_filament_types_set;
    std::set<std::string>                                            m_visible_printers;
    CreateType                                                       m_create_type;
    Button *                                                         m_button_create                = nullptr;
    Button *                                                         m_button_cancel                = nullptr;
    ComboBox *                                                       m_filament_vendor_combobox     = nullptr;
    ::CheckBox *                                                     m_can_not_find_vendor_checkbox = nullptr;
    ComboBox *                                                       m_filament_type_combobox       = nullptr;
    ComboBox *                                                       m_exist_vendor_combobox        = nullptr;
    ComboBox *                                                       m_filament_preset_combobox     = nullptr;
    TextInput *                                                      m_filament_custom_vendor_input = nullptr;
    wxGridSizer *                                                    m_filament_presets_sizer       = nullptr;
    wxPanel *                                                        m_filament_preset_panel        = nullptr;
    wxScrolledWindow *                                               m_scrolled_preset_panel        = nullptr;
    TextInput *                                                      m_filament_serial_input        = nullptr;
    wxBoxSizer *                                                     m_scrolled_sizer               = nullptr;
    wxStaticText *                                                   m_filament_preset_text         = nullptr;

};

class CreatePrinterPresetDialog : public DPIDialog
{
public:
    CreatePrinterPresetDialog(wxWindow *parent);
    ~CreatePrinterPresetDialog();

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

/********************************************************    Control Construction    *****************************************************/
    wxBoxSizer *create_step_switch_item();
    //Create Printer Page1
    void        create_printer_page1(wxWindow *parent);
    wxBoxSizer *create_type_item(wxWindow *parent);
    wxBoxSizer *create_printer_item(wxWindow *parent);
    wxBoxSizer *create_nozzle_diameter_item(wxWindow *parent);
    wxBoxSizer *create_bed_shape_item(wxWindow *parent);
    wxBoxSizer *create_bed_size_item(wxWindow *parent);
    wxBoxSizer *create_origin_item(wxWindow *parent);
    wxBoxSizer *create_hot_bed_stl_item(wxWindow *parent);
    wxBoxSizer *create_hot_bed_svg_item(wxWindow *parent);
    wxBoxSizer *create_max_print_height_item(wxWindow *parent);
    wxWindow   *create_page1_dialog_buttons(wxWindow *parent);
    //Improt Presets Page2
    void create_printer_page2(wxWindow *parent);
    wxBoxSizer *create_printer_preset_item(wxWindow *parent);
    wxBoxSizer *create_presets_item(wxWindow *parent);
    wxBoxSizer *create_presets_template_item(wxWindow *parent);
    wxWindow   *create_page2_dialog_buttons(wxWindow *parent);

    void show_page1();
    void show_page2();

/**********************************************************    Data Interaction    *******************************************************/
    bool          data_init();
    void          set_current_visible_printer();
    void          select_curr_radiobox(std::vector<std::pair<RadioBox *, wxString>> &radiobox_list, int btn_idx);
    void          select_all_preset_template(std::vector<std::pair<::CheckBox *, Preset *>> &preset_templates);
    void          deselect_all_preset_template(std::vector<std::pair<::CheckBox *, Preset *>> &preset_templates);
    void          update_presets_list(bool jast_template = false);
    void          on_preset_model_value_change(wxCommandEvent &e);
    void          clear_preset_combobox();
    bool          save_printable_area_config(Preset *preset);
    bool          check_printable_area();
    bool          validate_input_valid();
    void          load_texture();
    void          load_model_stl();
    bool          load_system_and_user_presets_with_curr_model(PresetBundle &temp_preset_bundle, bool just_template = false);
    void          generate_process_presets_data(std::vector<Preset const *> presets, std::string nozzle);
    void          update_preset_list_size();
    wxArrayString printer_preset_sort_with_nozzle_diameter(const VendorProfile &vendor_profile, float nozzle_diameter);

    wxBoxSizer *create_radio_item(wxString title, wxWindow *parent, wxString tooltip, std::vector<std::pair<RadioBox *, wxString>> &radiobox_list);

    wxString    curr_create_preset_type();
    wxString    curr_create_printer_type();

private:
    struct CreatePrinterType
    {
        wxString create_printer;
        wxString create_nozzle;
        wxString base_template;
        wxString base_curr_printer;
    };

    CreatePrinterType                                  m_create_type;
    std::vector<std::pair<RadioBox *, wxString>>       m_create_type_btns;
    std::vector<std::pair<RadioBox *, wxString>>       m_create_presets_btns;
    std::vector<std::pair<::CheckBox *, Preset *>>           m_filament_preset;
    std::vector<std::pair<::CheckBox *, Preset *>>           m_process_preset;
    std::unordered_map<std::string, std::shared_ptr<Preset>> m_printer_name_to_preset;
    VendorProfile                                      m_printer_preset_vendor_selected;
    Slic3r::VendorProfile::PrinterModel                m_printer_preset_model_selected;
    bool                                               rewritten                        = false;
    Preset *                                           m_printer_preset                 = nullptr;
    wxStaticBitmap *                                   m_step_1                         = nullptr;
    wxStaticBitmap *                                   m_step_2                         = nullptr;
    Button *                                           m_button_bed_stl                 = nullptr;
    Button *                                           m_button_bed_svg                 = nullptr;
    wxScrolledWindow *                                 m_page1                          = nullptr;
    wxPanel *                                          m_page2                          = nullptr;
    wxScrolledWindow *                                 m_scrolled_preset_window         = nullptr;
    wxBoxSizer *                                       m_scrooled_preset_sizer          = nullptr;
    ComboBox *                                         m_select_vendor                  = nullptr;
    ComboBox *                                         m_select_model                   = nullptr;
    ComboBox *                                         m_select_printer                 = nullptr;
    ::CheckBox *                                             m_can_not_find_vendor_combox     = nullptr;
    wxStaticText *                                     m_can_not_find_vendor_text       = nullptr;
    wxTextCtrl *                                       m_custom_vendor_text_ctrl        = nullptr;
    wxTextCtrl *                                       m_custom_model_text_ctrl         = nullptr;
    ComboBox *                                         m_nozzle_diameter                = nullptr;
    ComboBox *                                         m_printer_vendor                 = nullptr;
    ComboBox *                                         m_printer_model                  = nullptr;
    TextInput *                                        m_bed_size_x_input               = nullptr;
    TextInput *                                        m_bed_size_y_input               = nullptr;
    TextInput *                                        m_bed_origin_x_input             = nullptr;
    TextInput *                                        m_bed_origin_y_input             = nullptr;
    TextInput *                                        m_print_height_input             = nullptr;
    wxGridSizer *                                      m_filament_preset_template_sizer = nullptr;
    wxGridSizer *                                      m_process_preset_template_sizer  = nullptr;
    wxPanel *                                          m_filament_preset_panel          = nullptr;
    wxPanel *                                          m_process_preset_panel           = nullptr;
    wxPanel *                                          m_preset_template_panel          = nullptr;
    wxBoxSizer *                                       m_filament_sizer                 = nullptr;
    wxPanel *                                          m_printer_info_panel             = nullptr;
    wxBoxSizer *                                       m_page1_sizer                    = nullptr;
    wxBoxSizer *                                       m_printer_info_sizer             = nullptr;
    wxBoxSizer *                                       m_page2_sizer                    = nullptr;
    wxStaticText *                                     m_upload_stl_tip_text            = nullptr;
    wxStaticText *                                     m_upload_svg_tip_text            = nullptr;
    std::string                                        m_custom_texture;
    std::string                                        m_custom_model;
};

enum SuccessType {
    PRINTER = 0,
    FILAMENT
};

class CreatePresetSuccessfulDialog : public DPIDialog
{
public:
    CreatePresetSuccessfulDialog(wxWindow *parent, const SuccessType &create_success_type);
    ~CreatePresetSuccessfulDialog();

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

};

// The work of File > Export > Export Preset Bundle without the dialog: the presets the dialog collects,
// the names it offers as check boxes per export type, and the archive writers. Shared by
// ExportConfigsDialog and the API (sw_SlicerExportPresets), so both follow the same rules.
class ExportPresetsCore
{
public:
    enum ExportCase {
        INITIALIZE_FAIL = 0,
        ADD_FILE_FAIL,
        ADD_BUNDLE_STRUCTURE_FAIL,
        FINALIZE_FAIL,
        OPEN_ZIP_WRITTEN_FILE,
        EXPORT_CANCEL,
        EXPORT_SUCCESS,
        CASE_COUNT,
    };

    // The five export types of the dialog, in its order.
    enum class Kind { PrinterBundle, FilamentBundle, PrinterPresets, FilamentPresets, ProcessPresets };

    ExportPresetsCore() { ++live_count(); }
    // Deletes the preset copies and the temporary folder (as closing the dialog does).
    ~ExportPresetsCore();
    ExportPresetsCore(const ExportPresetsCore &)            = delete;
    ExportPresetsCore &operator=(const ExportPresetsCore &) = delete;

    // Is an instance alive (the dialog is open)? All instances share the temporary folder, so a second
    // one must not be made while one lives. GUI thread only.
    static bool in_use() { return live_count() > 0; }

    // Collect the presets (re)creating the temporary folder. False if that folder could not be created.
    bool init();
    // The names the dialog offers as check boxes for a type, in the dialog's order: printer names for
    // PrinterBundle, PrinterPresets and ProcessPresets, filament names for the two filament types.
    std::vector<std::string> offered_names(Kind kind) const;
    // The collected copy of a printer preset (nullptr if there is none).
    Preset *printer_preset(const std::string &printer_name) const;
    // The file name the dialog writes in the chosen folder: "<name>.orca_printer", "<name>.orca_filament",
    // or the fixed zip name of a zip type ("Printer presets.zip", ...; name is not used).
    static std::string file_name(Kind kind, const std::string &name);

    // Write the bundle of one offered name to file (PrinterBundle, FilamentBundle). EXPORT_SUCCESS on success.
    ExportCase write_printer_bundle(const std::string &printer_name, const std::string &file);
    ExportCase write_filament_bundle(const std::string &filament_name, const std::string &file);
    // Write the zip of a zip type with the presets of all given offered names to file. EXPORT_SUCCESS on success.
    ExportCase write_presets_zip(Kind kind, const std::vector<std::string> &names, const std::string &file);

private:
    static int &live_count()
    {
        static int count = 0;
        return count;
    }
    static bool earse_preset_fields_for_safe(Preset *preset);
    static int  initial_zip_archive(mz_zip_archive &zip_archive, const std::string &file_path);
    static ExportCase save_zip_archive_to_file(mz_zip_archive &zip_archive);
    static ExportCase save_presets_to_zip(const std::string &export_file, const std::vector<std::pair<std::string, std::string>> &config_paths);

    std::unordered_map<std::string, Preset *>              m_printer_presets;//first: printer name, second: printer presets have same printer name
    std::unordered_map<std::string, std::vector<Preset *>> m_filament_presets;//first: printer name, second: filament presets have same printer name
    std::unordered_map<std::string, std::vector<Preset *>> m_process_presets;//first: printer name, second: filament presets have same printer name
    std::unordered_map<std::string, std::vector<std::pair<std::string, Preset *>>> m_filament_name_to_presets;//first: filament name, second presets have same filament name and printer name in vector
};

class ExportConfigsDialog : public DPIDialog
{
public:
    ExportConfigsDialog(wxWindow *parent);
    ~ExportConfigsDialog();//to do: delete preset

protected:

    struct ExportType
    {
        wxString preset_bundle;
        wxString filament_bundle;
        wxString printer_preset;
        wxString filament_preset;
        wxString process_preset;
    };

    using ExportCase = ExportPresetsCore::ExportCase;

private:
    void        data_init();
    void        select_curr_radiobox(std::vector<std::pair<RadioBox *, wxString>> &radiobox_list, int btn_idx);
    void        on_dpi_changed(const wxRect &suggested_rect) override;
    void        show_export_result(const ExportCase &export_case);
    bool        has_check_box_selected();
    std::string initial_file_path(const wxString &path, const std::string &sub_file_path);
    std::string initial_file_name(const wxString &path, const std::string file_name);
    wxBoxSizer *create_export_config_item(wxWindow *parent);
    wxWindow   *create_dialog_buttons(wxWindow *parent);
    wxBoxSizer *create_select_printer(wxWindow *parent);
    wxBoxSizer *create_radio_item(wxString title, wxWindow *parent, wxString tooltip, std::vector<std::pair<RadioBox *, wxString>> &radiobox_list);
    ExportCase  archive_preset_bundle_to_file(const wxString &path);
    ExportCase  archive_filament_bundle_to_file(const wxString &path);
    ExportCase  archive_printer_preset_to_file(const wxString &path);
    ExportCase  archive_filament_preset_to_file(const wxString &path);
    ExportCase  archive_process_preset_to_file(const wxString &path);

private:
    std::vector<std::pair<RadioBox *, wxString>>           m_export_type_btns;
    std::vector<std::pair<::CheckBox *, Preset *>>         m_preset;         // for printer preset bundle,printer preset, process preset export
    std::vector<std::pair<::CheckBox *, std::string>>      m_printer_name;    // for filament and peocess preset export, collaborate with m_filament_name_to_presets
    ExportPresetsCore                                      m_core;           // the presets and the archive writers
    ExportType                                             m_exprot_type;
    wxBoxSizer *                                           m_main_sizer             = nullptr;
    wxScrolledWindow *                                     m_scrolled_preset_window = nullptr;
    wxGridSizer *                                          m_preset_sizer   = nullptr;
    wxPanel *                                              m_presets_window = nullptr;
    wxStaticText *                                         m_serial_text    = nullptr;
};

class CreatePresetForPrinterDialog : public DPIDialog
{
public:
    CreatePresetForPrinterDialog(wxWindow *parent, std::string filament_type, std::string filament_id, std::string filament_vendor, std::string filament_name);
    ~CreatePresetForPrinterDialog();

private:
    void        on_dpi_changed(const wxRect &suggested_rect) override;
    void        get_visible_printer_and_compatible_filament_presets();
    wxBoxSizer *create_selected_printer_preset_sizer();
    wxBoxSizer *create_selected_filament_preset_sizer();
    wxWindow   *create_dialog_buttons();

private:
    std::string                                                                       m_filament_id;
    std::string                                                                       m_filament_name;
    std::string                                                                       m_filament_vendor;
    std::string                                                                       m_filament_type;
    std::shared_ptr<PresetBundle>                                                     m_preset_bundle;
    ComboBox *                                                                        m_selected_printer  = nullptr;
    ComboBox *                                                                        m_selected_filament = nullptr;
    std::unordered_map<wxString, std::shared_ptr<Preset>>                             filament_choice_to_filament_preset;
    std::unordered_map<std::string, std::vector<std::shared_ptr<Preset>>>             m_printer_compatible_filament_presets; // need be used when add presets
};

class EditFilamentPresetDialog;

class PresetTree
{
public:
    PresetTree(EditFilamentPresetDialog *dialog);
    
    wxPanel *get_preset_tree(std::pair<std::string, std::vector<std::shared_ptr<Preset>>> printer_and_presets);

private:
    wxPanel *get_root_item(wxPanel *parent, const std::string &printer_name);

    wxPanel *get_child_item(wxPanel *parent, std::shared_ptr<Preset> preset, std::string printer_name, int preset_index, bool is_last = false);

    void delete_preset(std::string printer_name, int need_delete_preset_index);

    void edit_preset(std::string printer_name, int need_edit_preset_index);

private:
    EditFilamentPresetDialog *                                   m_parent_dialog = nullptr;
    std::pair<std::string, std::vector<std::shared_ptr<Preset>>> m_printer_and_presets;

};

class EditFilamentPresetDialog : public DPIDialog
{
public:
    EditFilamentPresetDialog(wxWindow *parent, Filamentinformation *filament_info);
    ~EditFilamentPresetDialog();
    
    wxPanel *get_preset_tree_panel() { return m_preset_tree_panel; }
    std::shared_ptr<Preset> get_need_edit_preset() { return m_need_edit_preset; }
    void     set_printer_name(const std::string &printer_name) { m_selected_printer = printer_name; }
    void     set_need_delete_preset_index(int need_delete_preset_index) { m_need_delete_preset_index = need_delete_preset_index; }
    void     set_need_edit_preset_index(int need_edit_preset_index) { m_need_edit_preset_index = need_edit_preset_index; }
    void     delete_preset();
    void     edit_preset();

private:
    void        on_dpi_changed(const wxRect &suggested_rect) override;
    bool        get_same_filament_id_presets(std::string filament_id);
    void        update_preset_tree();
    wxBoxSizer *create_filament_basic_info();
    wxBoxSizer *create_add_filament_btn();
    wxBoxSizer *create_preset_tree_sizer();
    wxWindow   *create_dialog_buttons();

private:
    PresetTree *                                                          m_preset_tree_creater = nullptr;
    std::string                                                           m_filament_id;
    std::string                                                           m_filament_name;
    std::string                                                           m_vendor_name;
    std::string                                                           m_filament_type;
    std::string                                                           m_filament_serial;
    Button *                                                              m_add_filament_btn         = nullptr;
    wxBoxSizer *                                                          m_preset_tree_sizer        = nullptr;
    wxPanel *                                                             m_preset_tree_panel        = nullptr;
    wxScrolledWindow *                                                    m_preset_tree_window       = nullptr;
    wxBoxSizer *                                                          m_main_sizer               = nullptr;
    wxStaticText *                                                        m_note_text                = nullptr;
    int                                                                   m_need_delete_preset_index = -1;
    int                                                                   m_need_edit_preset_index   = -1;
    std::shared_ptr<Preset>                                               m_need_edit_preset;
    std::string                                                           m_selected_printer         = "";
    std::unordered_map<std::string, std::vector<std::shared_ptr<Preset>>> m_printer_compatible_presets;

};

}
}
#endif