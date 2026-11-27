/*
 * scrcpy directory launcher.
 *
 * The real executable and all runtime DLLs live together in the hard-coded
 * "dll" subdirectory of the installation root. Because Windows resolves a
 * process' DLLs from the executable's own directory first, no PATH changes,
 * environment variables or registry lookups are needed.
 *
 * This launcher only derives its own directory, starts ../dll/scrcpy-core.exe
 * (hard-coded relative path), forwards arguments and returns its exit code.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fatal(const char *message)
{
    MessageBoxA(NULL, message, "scrcpy", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

/* Return the part of the command line after this program's name. */
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
    char exe_dir[MAX_PATH];
    char child_path[MAX_PATH];
    char child_cmd[8192];
    DWORD exe_len;
    const char *args;
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    char *mutable_cmd;
    DWORD exit_code = 0;
    char *slash;

    exe_len = GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    if (exe_len == 0 || exe_len >= MAX_PATH) {
        fatal("Cannot locate the scrcpy executable.");
    }

    slash = strrchr(exe_dir, '\\');
    if (!slash) {
        fatal("Cannot locate the scrcpy installation directory.");
    }
    *slash = '\0';

    /* Hard-coded layout: <install-root>\dll\scrcpy-core.exe */
    if (snprintf(child_path, sizeof(child_path), "%s\\dll\\scrcpy-core.exe",
                 exe_dir) >= (int) sizeof(child_path)) {
        fatal("Installation path is too long.");
    }

    args = skip_program_name(GetCommandLineA());
    if (snprintf(child_cmd, sizeof(child_cmd), "\"%s\"%s%s", child_path,
                 *args ? " " : "", args) >= (int) sizeof(child_cmd)) {
        fatal("Command line is too long.");
    }

    mutable_cmd = (char *) malloc(strlen(child_cmd) + 1);
    if (!mutable_cmd) {
        fatal("Out of memory.");
    }
    strcpy(mutable_cmd, child_cmd);

    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    startup.cb = sizeof(startup);

    if (!CreateProcessA(child_path, mutable_cmd, NULL, NULL, TRUE, 0, NULL,
                        exe_dir, &startup, &process)) {
        free(mutable_cmd);
        fatal("Cannot start dll\\scrcpy-core.exe.");
    }
    free(mutable_cmd);

    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    return (int) exit_code;
}
