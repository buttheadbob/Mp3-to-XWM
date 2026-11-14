// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef _WIN32
#define _DEFAULT_SOURCE  /* readlink, sigaction and friends under strict C modes */
#endif
#include "platform.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#define SEP '\\'
#else
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#define SEP '/'
#endif

bool plat_join(char *out, size_t size, const char *dir, const char *name)
{
    int n = snprintf(out, size, "%s%c%s", dir, SEP, name);
    return n >= 0 && (size_t)n < size;
}

static int next_non_space(void)  /* piped input: the next non-space character */
{
    for (;;) {
        int c = getchar();
        if (c == EOF)
            return -1;
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t')
            return c;
    }
}

#ifdef _WIN32

static UINT g_old_cp;

static void restore_console(void)
{
    if (g_old_cp)
        SetConsoleOutputCP(g_old_cp);
}

static BOOL WINAPI on_console_event(DWORD type)
{
    (void)type;
    restore_console();
    return FALSE;  /* let the default handler end the program */
}

void plat_console_init(void)
{
    g_old_cp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    atexit(restore_console);
    SetConsoleCtrlHandler(on_console_event, TRUE);
}

bool plat_interactive(void)
{
    DWORD mode;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
}

/* UTF-8 path to UTF-16. Absolute paths get the \\?\ prefix so they may exceed MAX_PATH. */
static bool to_wide_path(const char *s, wchar_t *out, int count)
{
    int off = 0;
    if (((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) && s[1] == ':' && s[2] == '\\') {
        wcscpy(out, L"\\\\?\\");
        off = 4;
    } else if (s[0] == '\\' && s[1] == '\\' && s[2] != '?') {
        wcscpy(out, L"\\\\?\\UNC");
        off = 7;
        s += 1;  /* keep one backslash: \\?\UNC\server\share */
    }
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, out + off, count - off) > 0;
}

static bool to_utf8(const wchar_t *s, char *out, int size)
{
    return WideCharToMultiByte(CP_UTF8, 0, s, -1, out, size, nullptr, nullptr) > 0;
}

int plat_exe_dir(char *out, size_t size)
{
    wchar_t w[PATH_MAX_LEN];
    DWORD n = GetModuleFileNameW(nullptr, w, PATH_MAX_LEN);
    if (n == 0 || n >= PATH_MAX_LEN)
        return -1;
    const wchar_t *p = w;
    if (!wcsncmp(p, L"\\\\?\\", 4) && p[5] == L':')
        p += 4;  /* plain drive path; the prefix is added again where needed */
    if (!to_utf8(p, out, (int)size))
        return -1;
    char *cut = strrchr(out, SEP);
    if (!cut)
        return -1;
    *cut = '\0';
    return 0;
}

bool plat_dir_exists(const char *path)
{
    wchar_t w[PATH_MAX_LEN];
    if (!to_wide_path(path, w, PATH_MAX_LEN))
        return false;
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int plat_make_dir(const char *path)
{
    wchar_t w[PATH_MAX_LEN];
    return to_wide_path(path, w, PATH_MAX_LEN) && CreateDirectoryW(w, nullptr) ? 0 : -1;
}

FILE *plat_fopen(const char *path, const char *mode)
{
    wchar_t w[PATH_MAX_LEN], wm[8];
    if (!to_wide_path(path, w, PATH_MAX_LEN) || !MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, 8))
        return nullptr;
    return _wfopen(w, wm);
}

int plat_remove(const char *path)
{
    wchar_t w[PATH_MAX_LEN];
    return to_wide_path(path, w, PATH_MAX_LEN) && DeleteFileW(w) ? 0 : -1;
}

int plat_rename(const char *from, const char *to)
{
    wchar_t wf[PATH_MAX_LEN], wt[PATH_MAX_LEN];
    if (!to_wide_path(from, wf, PATH_MAX_LEN) || !to_wide_path(to, wt, PATH_MAX_LEN))
        return -1;
    return MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
}

int plat_list_files(const char *dir, plat_file_fn fn, void *ctx)
{
    char pattern[PATH_MAX_LEN];
    wchar_t w[PATH_MAX_LEN];
    if (!plat_join(pattern, sizeof pattern, dir, "*") || !to_wide_path(pattern, w, PATH_MAX_LEN))
        return -1;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
    int count = 0;
    do {
        char name[PATH_MAX_LEN];
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !to_utf8(fd.cFileName, name, sizeof name))
            continue;
        if (fn)
            fn(name, ctx);
        count++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return count;
}

int plat_read_key(void)
{
    if (!plat_interactive())
        return next_non_space();
    for (;;) {
        int c = _getch();
        if (c != 0 && c != 0xE0)
            return c;
        _getch();  /* arrows, Delete, F keys and the like send a second code: ignore both */
    }
}

#else

static struct termios g_saved, g_raw_mode;
static volatile sig_atomic_t g_raw;      /* the terminal is in raw mode now */
static volatile sig_atomic_t g_reading;  /* a key read is under way */

static void restore_terminal(void)
{
    if (g_raw) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
        g_raw = 0;
    }
}

static void on_signal(int sig);

/* Installs on_signal for sig unless the signal is ignored, as under nohup. */
static void catch_signal(int sig)
{
    struct sigaction old, sa = {0};
    if (sigaction(sig, nullptr, &old) == 0 && old.sa_handler == SIG_IGN)
        return;
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, nullptr);
}

static void on_signal(int sig)
{
    int saved_errno = errno;  /* the interrupted code may be about to read errno */
    restore_terminal();
    if (sig == SIGCONT) {  /* back from Ctrl+Z: raw mode again if a key read is waiting */
        if (g_reading) {
            tcsetattr(STDIN_FILENO, TCSANOW, &g_raw_mode);
            g_raw = 1;
        }
        catch_signal(SIGTSTP);
        errno = saved_errno;
        return;
    }
    signal(sig, SIG_DFL);
    raise(sig);  /* ends the program, or stops it for SIGTSTP */
}

void plat_console_init(void)
{
    atexit(restore_terminal);
    static constexpr int sigs[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGTSTP, SIGCONT};
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        catch_signal(sigs[i]);
}

bool plat_interactive(void)
{
    return isatty(STDIN_FILENO);
}

int plat_exe_dir(char *out, size_t size)
{
    ssize_t n = readlink("/proc/self/exe", out, size - 1);
    if (n <= 0 || (size_t)n >= size - 1)
        return -1;
    out[n] = '\0';
    char *cut = strrchr(out, SEP);
    if (!cut)
        return -1;
    *cut = '\0';
    return 0;
}

bool plat_dir_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int plat_make_dir(const char *path)
{
    return mkdir(path, 0755);
}

FILE *plat_fopen(const char *path, const char *mode)
{
    return fopen(path, mode);
}

int plat_remove(const char *path)
{
    return remove(path);
}

int plat_rename(const char *from, const char *to)
{
    return rename(from, to);
}

int plat_list_files(const char *dir, plat_file_fn fn, void *ctx)
{
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        char full[PATH_MAX_LEN];
        struct stat st;
        if (!plat_join(full, sizeof full, dir, e->d_name) || stat(full, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (fn)
            fn(e->d_name, ctx);
        count++;
    }
    closedir(d);
    return count;
}

int plat_read_key(void)
{
    if (!isatty(STDIN_FILENO))
        return next_non_space();
    if (tcgetattr(STDIN_FILENO, &g_saved) != 0)
        return next_non_space();
    g_raw_mode = g_saved;
    g_raw_mode.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    g_raw_mode.c_cc[VMIN] = 1;
    g_raw_mode.c_cc[VTIME] = 0;
    g_reading = 1;
    g_raw = 1;
    tcsetattr(STDIN_FILENO, TCSANOW, &g_raw_mode);
    unsigned char c;
    ssize_t n;
    do
        n = read(STDIN_FILENO, &c, 1);
    while (n < 0 && errno == EINTR);  /* interrupted by Ctrl+Z and resumed */
    g_reading = 0;
    restore_terminal();
    return n == 1 ? c : -1;
}

#endif
