//! Install-root resolution. All paths are hardcoded relative to the exe,
//! never from environment variables or the registry (see control-panel-plan).

use std::path::{Path, PathBuf};

/// The directory containing scrcpy.exe (the Tauri panel).
pub fn install_root() -> PathBuf {
    std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.to_path_buf()))
        .unwrap_or_else(|| PathBuf::from("."))
}

pub fn core_exe() -> PathBuf {
    install_root().join("dll").join("scrcpy-core.exe")
}

pub fn dll_dir() -> PathBuf {
    install_root().join("dll")
}

pub fn templates_dir() -> PathBuf {
    install_root().join("templates")
}

pub fn launch_options_file() -> PathBuf {
    install_root().join("launch_options.json")
}

pub fn mapping_config_file() -> PathBuf {
    install_root().join("scrcpy_mapping_config.json")
}

pub fn version_file() -> PathBuf {
    install_root().join("version.txt")
}

pub fn template_file(name: &str) -> PathBuf {
    // Only accept a plain file name, never a path component: the template
    // picker passes names coming from the UI, keep it inside templates\.
    let safe: String = name
        .chars()
        .filter(|c| c.is_alphanumeric() || *c == '_' || *c == '-' || *c == '.')
        .collect();
    templates_dir().join(format!("{safe}.json"))
}

pub fn to_string(path: &Path) -> String {
    path.to_string_lossy().into_owned()
}
