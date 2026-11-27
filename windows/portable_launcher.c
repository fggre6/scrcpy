/*
 * Single-file launcher for scrcpy Windows builds.
 *
 * The build workflow embeds a zip of the complete runtime directory into this
 * executable as a binary object (objcopy). On first run the payload is
 * extracted to %LOCALAPPDATA%\scrcpy-portable\<payload-hash>, then the real
 * scrcpy.exe is started from there. Subsequent runs reuse the same directory,
 * so templates and scrcpy_mapping_config.json edited by the user persist.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Provided by objcopy from payload.zip */
extern unsigned char _binary_payload_zip_start[];
extern unsigned char _binary_payload_zip_end[];

static void fatal(const char *message)
{
    MessageBoxA(NULL, message, "scrcpy portable", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

static uint64_t fnv1a(const unsigned char *data, size_t len)
{
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i) {
        hash ^= (uint64_t) data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static void ensure_dir(const char *path)
{
    char partial[MAX_PATH];
    size_t len = strlen(path);

    if (len >= sizeof(partial)) {
        fatal("Runtime path is too long.");
    }

    for (size_t i = 1; i < len; ++i) {
        if (path[i] == '\\' || path[i] == '/') {
            memcpy(partial, path, i);
            partial[i] = '\0';
            CreateDirectoryA(partial, NULL);
        }
    }
    CreateDirectoryA(path, NULL);
}

static void write_payload(const char *zip_path)
{
    FILE *file = fopen(zip_path, "wb");
    size_t len;

    if (!file) {
        fatal("Cannot create the runtime payload file.");
    }

    len = (size_t) (_binary_payload_zip_end - _binary_payload_zip_start);
    if (fwrite(_binary_payload_zip_start, 1, len, file) != len) {
        fclose(file);
        fatal("Cannot write the runtime payload file.");
    }
    fclose(file);
}

static int run_and_wait(const char *application, const char *command_line,
                        const char *working_dir)
{
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    char *mutable_cmd;
    BOOL ok;
    DWORD exit_code = 0;

    mutable_cmd = (char *) malloc(strlen(command_line) + 1);
    if (!mutable_cmd) {
        return -1;
    }
    strcpy(mutable_cmd, command_line);

    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    startup.cb = sizeof(startup);

    ok = CreateProcessA(application, mutable_cmd, NULL, NULL, TRUE, 0, NULL,
                        working_dir, &startup, &process);
    free(mutable_cmd);

    if (!ok) {
        return -1;
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    return (int) exit_code;
}

/* Copy the arguments passed to this launcher, skipping its own program name. */
static const char *skip_program_name(const char *command_line)
{
    const char *p = command_line;

    if (*p == '"') {
        ++p;
        while (*p && *p != '"') {
            ++p;
        }
        if (*p == '"') {
            ++p;
        }
    } else {
        while (*p && *p != ' ' && *p != '\t') {
            ++p;
        }
    }

    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    return p;
}

int main(void)
{
    char appdata[MAX_PATH];
    char system_dir[MAX_PATH];
    char tar_exe[MAX_PATH];
    char runtime_dir[MAX_PATH];
    char zip_path[MAX_PATH];
    char marker_path[MAX_PATH];
    char child_path[MAX_PATH];
    char extract_cmd[8192];
    char child_cmd[8192];
    UINT system_dir_len;
    size_t payload_len;
    uint64_t hash;
    FILE *marker;
    int result;
    const char *args;

    if (FAILED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appdata))) {
        fatal("Cannot locate the local application data folder.");
    }

    payload_len = (size_t) (_binary_payload_zip_end - _binary_payload_zip_start);
    hash = fnv1a(_binary_payload_zip_start, payload_len);

    if (snprintf(runtime_dir, sizeof(runtime_dir),
                 "%s\\scrcpy-portable\\%016llx", appdata,
                 (unsigned long long) hash) >= (int) sizeof(runtime_dir)) {
        fatal("Runtime path is too long.");
    }
    ensure_dir(runtime_dir);

    if (snprintf(marker_path, sizeof(marker_path), "%s\\.extracted",
                 runtime_dir) >= (int) sizeof(marker_path)) {
        fatal("Runtime path is too long.");
    }

    marker = fopen(marker_path, "rb");
    if (!marker) {
        if (snprintf(zip_path, sizeof(zip_path), "%s\\payload.zip",
                     runtime_dir) >= (int) sizeof(zip_path)) {
            fatal("Runtime path is too long.");
        }
        write_payload(zip_path);

        system_dir_len = GetSystemDirectoryA(system_dir, MAX_PATH);
        if (system_dir_len == 0 || system_dir_len >= MAX_PATH) {
            fatal("Cannot locate the Windows system directory.");
        }
        if (snprintf(tar_exe, sizeof(tar_exe), "%s\\tar.exe", system_dir) >=
            (int) sizeof(tar_exe)) {
            fatal("Runtime path is too long.");
        }

        if (snprintf(extract_cmd, sizeof(extract_cmd),
                     "\"%s\" -xf \"%s\" -C \"%s\"", tar_exe, zip_path,
                     runtime_dir) >= (int) sizeof(extract_cmd)) {
            fatal("Extraction command is too long.");
        }

        result = run_and_wait(tar_exe, extract_cmd, runtime_dir);
        if (result != 0) {
            fatal("Cannot extract the bundled runtime files. "
                  "Windows 10 1809 or newer is required.");
        }

        remove(zip_path);
        marker = fopen(marker_path, "wb");
        if (marker) {
            fclose(marker);
        }
    } else {
        fclose(marker);
    }

    if (snprintf(child_path, sizeof(child_path), "%s\\scrcpy.exe",
                 runtime_dir) >= (int) sizeof(child_path)) {
        fatal("Runtime path is too long.");
    }

    args = skip_program_name(GetCommandLineA());
    if (snprintf(child_cmd, sizeof(child_cmd), "\"%s\"%s%s", child_path,
                 *args ? " " : "", args) >= (int) sizeof(child_cmd)) {
        fatal("Command line is too long.");
    }

    result = run_and_wait(child_path, child_cmd, runtime_dir);
    return result < 0 ? 1 : result;
}
