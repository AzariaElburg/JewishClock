// =============================================================================
// sim_win.cpp - Windows calls the stand-ins need (simulator only, 2026-11-15).
// Kept apart from sim_platform.cpp: <windows.h> and the Arduino stand-ins don't mix.
//
// simRun(): a helper program (curl, for the clock's downloads) started WITHOUT the clock's
//   open files. system() hands every open file to the child; while curl ran (a download,
//   ~1 s) it held the picture the clock was just writing, and the flash stand-in could
//   neither rename nor remove it - "Could not save the picture" in a full run (resilience
//   pictures_fill, 2026-11-14 and -15, always at the moment the calendar's second download
//   started). Proven with a small program: a file open while system() starts a child can't
//   be renamed until the child ends. The clock's flash has no such thing.
// simWhoHolds(): which processes have a file open (Windows' Restart Manager, loaded at run
//   time - no extra library), said when a rename/remove of the flash stand-in fails.
// =============================================================================
#include <windows.h>
#include <restartmanager.h>
#include <string>
#include <vector>

// The command's exit code, -1 if it could not be started. No shell: no redirections in cmd.
int simRun(const std::string& cmd) {
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<char> line(cmd.begin(), cmd.end()); line.push_back(0);
    if (!CreateProcessA(nullptr, line.data(), nullptr, nullptr, FALSE /* no handles passed on */, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}

typedef DWORD (WINAPI *RmStartSession_t)(DWORD*, DWORD, WCHAR*);
typedef DWORD (WINAPI *RmRegisterResources_t)(DWORD, UINT, LPCWSTR*, UINT, RM_UNIQUE_PROCESS*, UINT, LPCWSTR*);
typedef DWORD (WINAPI *RmGetList_t)(DWORD, UINT*, UINT*, RM_PROCESS_INFO*, DWORD*);
typedef DWORD (WINAPI *RmEndSession_t)(DWORD);

static std::string narrow(const WCHAR* w) {
    char buf[600];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return n > 0 ? std::string(buf) : std::string("?");
}

// "name (pid N, this clock)" for each holder, "" = none found / not known.
std::string simWhoHolds(const std::string& path) {
    static HMODULE rm = LoadLibraryA("rstrtmgr.dll");
    if (!rm) return "restart manager not available";
    auto start = (RmStartSession_t)GetProcAddress(rm, "RmStartSession");
    auto reg   = (RmRegisterResources_t)GetProcAddress(rm, "RmRegisterResources");
    auto list  = (RmGetList_t)GetProcAddress(rm, "RmGetList");
    auto end   = (RmEndSession_t)GetProcAddress(rm, "RmEndSession");
    if (!start || !reg || !list || !end) return "restart manager not available";

    WCHAR wpath[MAX_PATH * 2];
    if (!MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath, MAX_PATH * 2)) return "?";
    DWORD session;
    WCHAR key[CCH_RM_SESSION_KEY + 1] = {0};
    if (start(&session, 0, key) != ERROR_SUCCESS) return "no session";
    std::string out;
    LPCWSTR files[] = { wpath };
    if (reg(session, 1, files, 0, nullptr, 0, nullptr) == ERROR_SUCCESS) {
        RM_PROCESS_INFO info[10];
        UINT needed = 0, got = 10;
        DWORD reasons = 0;
        DWORD r = list(session, &needed, &got, info, &reasons);
        if (r == ERROR_SUCCESS || r == ERROR_MORE_DATA) {
            for (UINT i = 0; i < got && i < 10; i++) {
                if (!out.empty()) out += ", ";
                out += narrow(info[i].strAppName) + " (pid " + std::to_string(info[i].Process.dwProcessId) +
                       (info[i].Process.dwProcessId == GetCurrentProcessId() ? ", THIS clock" : "") + ")";
            }
            if (got == 0) out = "no process holds it now";
        } else out = "list failed (" + std::to_string(r) + ")";
    }
    end(session);
    return out;
}
