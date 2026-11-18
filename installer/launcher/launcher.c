/* XCX-Setup.exe: starts python\pythonw.exe -m installer.gui with this exe's folder as the working directory, passes
   its own arguments on (XCX-Setup.exe --play --fullscreen: installer/gui.py main) and waits for the window, which waits
   for the game Play starts, so a launcher that tracks this process sees the game's lifetime; the exit code is the
   window's (the game's after Play).

   No console window. The two message strings come from the catalog (installer/text/<lang>.json, ids launcher.title and
   launcher.python_missing); tools/make_release.py writes them into launcher_text.h before it compiles this file.
   Build: llvm/bin/llvm-windres -I runtime -O coff launcher.rc -o launcher_rc.o, then
   llvm/bin/clang -O2 -mwindows launcher.c launcher_rc.o -o XCX-Setup.exe */
#include <stdlib.h>
#include <windows.h>
#include <wchar.h>

#include "launcher_text.h"

#define LANGID_RUSSIAN 0x19
#define PATH_CHARS (MAX_PATH * 2)

static void fail(void)
{
    int russian = (GetUserDefaultUILanguage() & 0x3ff) == LANGID_RUSSIAN;
    MessageBoxW(NULL, russian ? LAUNCHER_PYTHON_MISSING_RU : LAUNCHER_PYTHON_MISSING_EN,
                russian ? LAUNCHER_TITLE_RU : LAUNCHER_TITLE_EN, MB_OK | MB_ICONERROR);
}

/* The arguments after the program name in this process's command line, as typed (quoting kept). */
static const wchar_t *own_arguments(void)
{
    const wchar_t *p = GetCommandLineW();
    if (*p == L'"') {
        for (p++; *p && *p != L'"'; p++) {}
        if (*p) p++;
    } else {
        while (*p && *p != L' ' && *p != L'\t') p++;
    }
    while (*p == L' ' || *p == L'\t') p++;
    return p;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_line, int show)
{
    wchar_t folder[PATH_CHARS], python[PATH_CHARS];
    DWORD n = GetModuleFileNameW(NULL, folder, PATH_CHARS), code = 0;
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    const wchar_t *args = own_arguments();
    size_t cap = wcslen(args) + PATH_CHARS + 64;
    wchar_t *command = malloc(cap * sizeof *command);
    (void)instance; (void)previous; (void)command_line; (void)show;
    if (!command) { fail(); return 1; }
    if (n == 0 || n >= PATH_CHARS) { fail(); return 1; }
    while (n > 0 && folder[n - 1] != L'\\') n--;
    folder[n > 0 ? n - 1 : 0] = 0;
    if (swprintf(python, PATH_CHARS, L"%ls\\python\\pythonw.exe", folder) < 0 ||
        GetFileAttributesW(python) == INVALID_FILE_ATTRIBUTES) { fail(); return 1; }
    if (swprintf(command, cap, L"\"%ls\" -m installer.gui%ls%ls", python, *args ? L" " : L"", args) < 0) {
        fail();
        return 1;
    }
    if (!CreateProcessW(python, command, NULL, NULL, FALSE, 0, NULL, folder, &si, &pi)) { fail(); return 1; }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    free(command);
    return (int)code;
}
