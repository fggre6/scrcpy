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

/// CLI option registry, derived from app/src/cli.c `options[]`.
/// (short, long, takes_value). Only options relevant to parsing user input
/// are listed; unknown long options are still preserved verbatim.
const CLI_OPTIONS: &[(Option<char>, &str, bool)] = &[
    (Some('b'), "video-bit-rate", true),
    (Some('d'), "select-usb", false),
    (Some('e'), "select-tcpip", false),
    (Some('f'), "fullscreen", false),
    (Some('h'), "help", false),
    (Some('K'), "keyboard", false), // shortcut for --keyboard=uhid
    (Some('m'), "max-size", true),
    (Some('M'), "mirror", false),
    (None, "max-fps", true),
    (Some('n'), "no-control", false),
    (Some('N'), "no-playback", false),
    (None, "no-audio", false),
    (Some('p'), "port", true),
    (Some('r'), "record", true),
    (Some('s'), "serial", true),
    (Some('S'), "turn-screen-off", false),
    (Some('t'), "show-touches", false),
    (None, "tcpip", true),
    (Some('v'), "version", false),
    (Some('V'), "verbosity", true),
    (None, "video-codec", true),
    (Some('w'), "stay-awake", false),
    (Some('x'), "flex-display", false),
    (None, "always-on-top", false),
    (None, "angle", true),
    (None, "audio-bit-rate", true),
    (None, "audio-buffer", true),
    (None, "audio-codec", true),
    (None, "audio-source", true),
    (None, "bit-rate", true),
    (None, "codec", true),
    (None, "crop", true),
    (None, "display-id", true),
    (None, "fps", true),
    (None, "gamepad", true),
    (None, "keyboard", true),
    (None, "mouse", true),
    (None, "no-video", false),
    (None, "no-window", false),
    (None, "rotation", true),
    (None, "window-size", true),
    (None, "video-source", true),
];

fn longopt_takes_value(name: &str) -> Option<bool> {
    CLI_OPTIONS.iter().find(|(_, l, _)| *l == name).map(|(_, _, v)| *v)
}
fn shortopt_info(c: char) -> Option<(&'static str, bool)> {
    CLI_OPTIONS.iter().find(|(s, _, _)| *s == Some(c)).map(|(_, l, v)| (*l, *v))
}

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
        // arguments starts EMPTY on purpose: anything not explicitly set is
        // left to scrcpy's own built-in defaults (the user's requirement).
        Self {
            schema: 1,
            theme: "hive-light".into(),
            device: String::new(),
            variables: BTreeMap::new(),
            arguments: BTreeMap::new(),
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

/// Build the argv passed to scrcpy-core.exe.
///
/// Rule: only explicitly configured options are emitted. Empty strings are
/// skipped entirely (never `--key=`), false booleans are skipped, and any
/// option left untouched falls back to scrcpy's own built-in default.
/// `device` and `arguments["serial"]` are the same setting: prefer `device`
/// and skip the duplicate from arguments.
fn build_argv(options: &LaunchOptions) -> Result<Vec<String>, String> {
    let mut argv = Vec::new();
    let mut has_serial = false;

    if !options.device.trim().is_empty() {
        let serial = expand_variables(options.device.trim(), &options.variables)?;
        argv.push("--serial".into());
        argv.push(serial);
        has_serial = true;
    }

    for (key, value) in &options.arguments {
        // device field already supplied --serial; do not emit it twice.
        if key == "serial" && has_serial {
            continue;
        }
        match value {
            Value::Bool(true) => argv.push(format!("--{key}")),
            Value::Bool(false) => {} // false = leave scrcpy default
            Value::String(s) => {
                let expanded = expand_variables(s, &options.variables)?;
                if expanded.trim().is_empty() {
                    continue; // empty = leave scrcpy default, never "--key="
                }
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

/// Parse a full scrcpy command line into LaunchOptions, so pasting a command
/// like `--tcpip=192.168.0.101:5555 --max-fps=60 --no-audio -Sw` fills every
/// form field automatically. Short options are expanded using CLI_OPTIONS;
/// combined flags such as `-Sw` are split into `-S -w`.
#[tauri::command]
pub fn parse_launch_command(command: String) -> Result<LaunchOptions, String> {
    let mut opts = LaunchOptions::default();
    // Parsing a command means the user wants exactly what they typed: drop
    // the form defaults so nothing is silently carried over.
    opts.arguments.clear();
    opts.extra_args.clear();
    opts.device.clear();

    let tokens = shell_split(&command)?;
    let mut i = 0;
    while i < tokens.len() {
        let tok = &tokens[i];
        if tok == "--" {
            // everything after -- is passed through verbatim
            opts.extra_args = tokens[i + 1..].join(" ");
            break;
        }
        if let Some(body) = tok.strip_prefix("--") {
            let (name, inline_val) = match body.split_once('=') {
                Some((n, v)) => (n, Some(v.to_string())),
                None => (body, None),
            };
            if name.is_empty() {
                i += 1;
                continue;
            }
            let takes = longopt_takes_value(name);
            match (takes, inline_val) {
                (Some(true), Some(v)) => {
                    // Keep each option under its own name. --tcpip is NOT
                    // --serial: it selects a TCP/IP device, and scrcpy accepts
                    // the address as the value of --tcpip itself.
                    opts.arguments.insert(name.to_string(), Value::String(v));
                }
                (Some(true), None) => {
                    // value-taking option with separate token
                    if i + 1 < tokens.len() {
                        let v = tokens[i + 1].clone();
                        opts.arguments.insert(name.to_string(), Value::String(v));
                        i += 1;
                    } else {
                        return Err(format!("选项 --{name} 缺少值"));
                    }
                }
                (Some(false), _) => {
                    opts.arguments.insert(name.to_string(), Value::Bool(true));
                }
                (None, Some(v)) => {
                    // Unknown long option: emit verbatim through extra args
                    push_extra(&mut opts.extra_args, &format!("--{name}={v}"));
                }
                (None, None) => {
                    push_extra(&mut opts.extra_args, &format!("--{name}"));
                }
            }
            i += 1;
            continue;
        }
        if tok.starts_with('-') && tok.len() > 1 {
            let chars: Vec<char> = tok[1..].chars().collect();
            for (idx, c) in chars.iter().enumerate() {
                match shortopt_info(*c) {
                    Some((long, true)) => {
                        // value option: rest of token, or next token
                        let rest: String = chars[idx + 1..].iter().collect();
                        let val = if !rest.is_empty() {
                            rest
                        } else if i + 1 < tokens.len() {
                            i += 1;
                            tokens[i].clone()
                        } else {
                            return Err(format!("选项 -{c} 缺少值"));
                        };
                        if long == "serial" {
                            opts.device = val;
                        } else {
                            opts.arguments.insert(long.to_string(), Value::String(val));
                        }
                        break;
                    }
                    Some((long, false)) => {
                        opts.arguments.insert(long.to_string(), Value::Bool(true));
                    }
                    None => {
                        push_extra(&mut opts.extra_args, &format!("-{c}"));
                    }
                }
            }
            i += 1;
            continue;
        }
        // bare token (serial, file path, etc.)
        push_extra(&mut opts.extra_args, tok);
        i += 1;
    }
    Ok(opts)
}

fn push_extra(extra: &mut String, token: &str) {
    if !extra.is_empty() {
        extra.push(' ');
    }
    extra.push_str(token);
}

/// Split a command line on whitespace, honoring single/double quotes.
fn shell_split(s: &str) -> Result<Vec<String>, String> {
    let mut out = Vec::new();
    let mut cur = String::new();
    let mut quote: Option<char> = None;
    let mut chars = s.chars().peekable();
    while let Some(c) = chars.next() {
        match quote {
            Some(q) => {
                if c == q {
                    quote = None;
                } else {
                    cur.push(c);
                }
            }
            None => match c {
                '\'' | '"' => quote = Some(c),
                c if c.is_whitespace() => {
                    if !cur.is_empty() {
                        out.push(std::mem::take(&mut cur));
                    }
                }
                '\\' => {
                    if let Some(&n) = chars.peek() {
                        cur.push(n);
                        chars.next();
                    }
                }
                _ => cur.push(c),
            },
        }
    }
    if quote.is_some() {
        return Err("引号未闭合".into());
    }
    if !cur.is_empty() {
        out.push(cur);
    }
    Ok(out)
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
