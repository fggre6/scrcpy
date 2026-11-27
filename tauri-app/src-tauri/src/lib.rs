mod commands;
mod paths;

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            commands::read_launch_options,
            commands::write_launch_options,
            commands::read_mapping_config,
            commands::write_mapping_config,
            commands::list_templates,
            commands::read_template,
            commands::save_template,
            commands::duplicate_template,
            commands::delete_template,
            commands::build_launch_command,
            commands::parse_launch_command,
            commands::launch_scrcpy,
            commands::stop_scrcpy,
            commands::open_install_dir,
            commands::list_devices,
            commands::get_app_info,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
