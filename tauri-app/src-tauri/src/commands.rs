//! Tauri IPC commands. All filesystem access is server-side; the webview only
//! sends/receives JSON. Process launch uses an argv array so Windows handles
//! quoting correctly (no launcher pass-through, see control-panel-plan.md).

use std::collections::BTreeMap;
use std::fs;
use std::io;
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::sync::Mutex;

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

use crate::paths;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct LaunchOptions {
    pub schema: u32,
    pub theme: String,
    pub device: String,
    pub variables: BTreeMap<String, String>,
    /// key = argument name without `--`; value = bool flag or string value.
    pub arguments: BTreeMap<String, Value>,
    pub extra_args: String,
    pub active_template: String,
}

impl Default for LaunchOptions {
    fn default() -> Self {
        let mut arguments = BTreeMap::new();
        arguments.insert("max-size".into(), json!("1280"));
        arguments.insert("bit-rate".into(), json!("8M"));
        arguments.insert("fps".into(), json!("60"));
        arguments.insert("fullscreen".into(), json!(false));
        arguments.insert("turn-screen-off".into(), json!(true));
        arguments.insert("no-audio".into(), json!(false));
        Self {
            schema: 1,
            theme: "hive-light".into(),
            device: String::new(),
            variables: {
                let mut v = BTreeMap::new();
                v.insert("size".into(), "1280x720".into());
                v
            },
            arguments,
            extra_args: String::new(),
            active_template: "default".into(),
        }
    }
}

#[derive(Debug, Serialize)]
pub struct TemplateInfo {
    pub name: String,
    pub file: String,
    pub count: usize,
}

#[derive(Debug, Serialize)]
pub struct AppInfo {
    pub version: String,
    pub install_dir: String,
    pub template_count: usize,
    pub core_exists: bool,
}

/// Handle of the currently running core process, kept so "stop" can kill it.
static RUNNING: Mutex<Option<std::process::Child>> = Mutex::new(None);

fn default_options_file() -> PathBuf {
    paths::launch_options_file()
}

fn read_json_or_default<T: serde::de::DeserializeOwned + Default>(path: &PathBuf) -> T {
    match fs::read_to_string(path) {
        Ok(s) => serde_json::from_str(&s).unwrap_or_default(),
        Err(_) => T::default(),
    }
}

fn write_json_pretty<T: Serialize>(path: &PathBuf, value: &T) -> Result<(), String> {
    let parent = path.parent().unwrap_or_else(|| std::path::Path::new("."));
    fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    let text = serde_json::to_string_pretty(value).map_err(|e| e.to_string())?;
    fs::write(path, text).map_err(|e| e.to_string())
}

#[tauri::command]
pub fn read_launch_options() -> Result<LaunchOptions, String> {
    Ok(read_json_or_default(&default_options_file()))
}

#[tauri::command]
pub fn write_launch_options(options: LaunchOptions) -> Result<(), String> {
    write_json_pretty(&default_options_file(), &options)
}

#[tauri::command]
pub fn read_mapping_config() -> Result<Value, String> {
    let path = paths::mapping_config_file();
    match fs::read_to_string(&path) {
        Ok(s) => serde_json::from_str(&s).map_err(|e| e.to_string()),
        Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(json!({
            "shortcuts": { "switch_template": "F2", "design_mode": "F3",
                           "toggle_mapping": "F4", "manager_panel": "F5" },
            "active_template": "default",
            "enabled": false
        })),
        Err(e) => Err(e.to_string()),
    }
}

#[tauri::command]
pub fn write_mapping_config(config: Value) -> Result<(), String> {
    write_json_pretty(&paths::mapping_config_file(), &config)
}

#[tauri::command]
pub fn list_templates() -> Result<Vec<TemplateInfo>, String> {
    let dir = paths::templates_dir();
    let mut out = Vec::new();
    let entries = match fs::read_dir(&dir) {
        Ok(e) => e,
        Err(_) => return Ok(out),
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if !path.is_file() {
            continue;
        }
        let ext = path.extension().and_then(|s| s.to_str()).unwrap_or("");
        if !ext.eq_ignore_ascii_case("json") {
            continue;
        }
        let file_name = path.file_name().and_then(|s| s.to_str()).unwrap_or("").to_string();
        let text = fs::read_to_string(&path).unwrap_or_default();
        let val: Value = serde_json::from_str(&text).unwrap_or(json!({}));
        let name = val.get("name").and_then(|v| v.as_str()).unwrap_or("").to_string();
        let count = val.get("mappings").and_then(|v| v.as_array()).map(|a| a.len()).unwrap_or(0);
        out.push(TemplateInfo { name, file: file_name, count });
    }
    out.sort_by(|a, b| a.file.cmp(&b.file));
    Ok(out)
}

#[tauri::command]
pub fn read_template(file: String) -> Result<Value, String> {
    let path = paths::templates_dir().join(&file);
    let text = fs::read_to_string(&path).map_err(|e| e.to_string())?;
    serde_json::from_str(&text).map_err(|e| e.to_string())
}

#[tauri::command]
pub fn save_template(file: String, content: Value) -> Result<(), String> {
    // Validate before touching disk: the C parser rejects anything but an
    // object with a mappings array.
    match content.get("mappings") {
        Some(Value::Array(_)) => {}
        _ => return Err("模板根节点缺少 'mappings' 数组".into()),
    }
    let path = paths::templates_dir().join(&file);
    write_json_pretty(&path, &content)
}

#[tauri::command]
pub fn duplicate_template(file: String, new_file: String) -> Result<(), String> {
    let src = paths::templates_dir().join(&file);
    let dst = paths::templates_dir().join(&new_file);
    let text = fs::read_to_string(&src).map_err(|e| e.to_string())?;
    fs::write(dst, text).map_err(|e| e.to_string())
}

#[tauri::command]
pub fn delete_template(file: String) -> Result<(), String> {
    let path = paths::templates_dir().join(&file);
    fs::remove_file(path).map_err(|e| e.to_string())
}

/// Expand ${name} variables (one level, no recursion) and build the argv array.
fn expand_variables(input: &str, vars: &BTreeMap<String, String>) -> Result<String, String> {
    let mut out = String::with_capacity(input.len());
    let bytes = input.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'$' && i + 1 < bytes.len() && bytes[i + 1] == b'{' {
            if let Some(end) = input[i + 2..].find('}') {
                let key = &input[i + 2..i + 2 + end];
                let value = vars.get(key).ok_or_else(|| format!("未定义变量: {key}"))?;
                if value.contains("${") {
                    return Err(format!("变量 {key} 的值含嵌套引用，仅支持一层展开"));
                }
                out.push_str(value);
                i = i + 2 + end + 1;
                continue;
            }
        }
        let ch = input[i..].chars().next().unwrap();
        out.push(ch);
        i += ch.len_utf8();
    }
    Ok(out)
}

/// Build the argv passed to scrcpy-core.exe. Deterministic order: arguments
/// (sorted by key) then extraArgs tokens.
fn build_argv(options: &LaunchOptions) -> Result<Vec<String>, String> {
    let mut argv = Vec::new();

    if !options.device.trim().is_empty() {
        let serial = expand_variables(options.device.trim(), &options.variables)?;
        argv.push("--serial".into());
        argv.push(serial);
    }

    for (key, value) in &options.arguments {
        match value {
            Value::Bool(true) => argv.push(format!("--{key}")),
            Value::Bool(false) => {}
            Value::String(s) => {
                let expanded = expand_variables(s, &options.variables)?;
                argv.push(format!("--{key}={expanded}"));
            }
            Value::Number(n) => argv.push(format!("--{key}={n}")),
            Value::Null => {}
            other => return Err(format!("参数 {key} 的值类型不受支持: {other}")),
        }
    }

    if !options.extra_args.trim().is_empty() {
        let expanded = expand_variables(options.extra_args.trim(), &options.variables)?;
        // Split on whitespace only; no shell involved, so this is safe argv.
        for token in expanded.split_whitespace() {
            argv.push(token.to_string());
        }
    }
    Ok(argv)
}

#[tauri::command]
pub fn build_launch_command(options: LaunchOptions) -> Result<Vec<String>, String> {
    build_argv(&options)
}

#[tauri::command]
pub fn launch_scrcpy(options: LaunchOptions) -> Result<String, String> {
    let core = paths::core_exe();
    if !core.exists() {
        return Err(format!("找不到 scrcpy-core.exe: {}", paths::to_string(&core)));
    }

    // Stop a previous instance first so STOP always has a single target.
    let _ = stop_scrcpy();

    let argv = build_argv(&options)?;
    let mut child = Command::new(&core)
        .args(&argv)
        .current_dir(paths::install_root())
        .stdin(Stdio::null())
        .spawn()
        .map_err(|e| e.to_string())?;

    let pid = child.id();
    let mut guard = RUNNING.lock().map_err(|e| e.to_string())?;
    *guard = Some(child);
    Ok(format!("started pid={pid}"))
}

#[tauri::command]
pub fn stop_scrcpy() -> Result<(), String> {
    let mut guard = RUNNING.lock().map_err(|e| e.to_string())?;
    if let Some(mut child) = guard.take() {
        let _ = child.kill();
        let _ = child.wait();
    }
    Ok(())
}

#[tauri::command]
pub fn open_install_dir() -> Result<(), String> {
    Command::new("explorer.exe")
        .arg(paths::install_root())
        .spawn()
        .map_err(|e| e.to_string())?;
    Ok(())
}

#[tauri::command]
pub fn list_devices() -> Result<Vec<String>, String> {
    // Prefer the adb bundled in dll\; fall back to PATH.
    let adb = paths::dll_dir().join("adb.exe");
    let program = if adb.exists() { adb } else { PathBuf::from("adb") };
    let output = Command::new(program)
        .arg("devices")
        .output()
        .map_err(|e| e.to_string())?;
    let text = String::from_utf8_lossy(&output.stdout);
    let mut devices = Vec::new();
    for line in text.lines().skip(1) {
        let mut parts = line.split_whitespace();
        if let (Some(serial), Some(state)) = (parts.next(), parts.next()) {
            if state == "device" {
                devices.push(serial.to_string());
            }
        }
    }
    Ok(devices)
}

#[tauri::command]
pub fn get_app_info() -> Result<AppInfo, String> {
    let version = fs::read_to_string(paths::version_file())
        .map(|s| s.trim().to_string())
        .unwrap_or_else(|_| "dev".into());
    let template_count = list_templates().map(|v| v.len()).unwrap_or(0);
    Ok(AppInfo {
        version,
        install_dir: paths::to_string(&paths::install_root()),
        template_count,
        core_exists: paths::core_exe().exists(),
    })
}
