/*
 * "Add to Steam": the game as a non-Steam shortcut in the Steam library.
 *
 * Steam keeps non-Steam games in userdata/<account>/config/shortcuts.vdf, a
 * binary KeyValues file: a map is 0x00 name\0 ... 0x08, a string 0x01
 * name\0 value\0, a 32-bit int 0x02 name\0 <4 bytes>. The root holds one
 * map, "shortcuts", whose entries are maps named "0", "1", ... -- so adding
 * one is appending a map before the last two 0x08s.
 *
 * Steam reads the file when it starts and writes it back when it exits, so
 * a running Steam would neither show the entry nor keep it. This refuses to
 * touch the file while Steam runs and says so.
 *
 * The shortcut goes to the account that logged in last (loginusers.vdf
 * "MostRecent"), or to every account when that is not recorded. The old
 * file is kept as shortcuts.vdf.bak.
 */
#include "steam_shortcut.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#define PATHSEP '\\'
#else
#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>
#define PATHSEP '/'
#endif

#define APP_NAME "RalliSport Challenge 2"

static int file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

/* ── where Steam is, and whether it is running ───────────── */

static int steam_root(char *out, size_t n)
{
#ifdef _WIN32
    char v[MAX_PATH];
    DWORD sz = sizeof v;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath",
                     RRF_RT_REG_SZ, NULL, v, &sz) == ERROR_SUCCESS) {
        char *c;
        for (c = v; *c; c++)
            if (*c == '/')
                *c = '\\';
        snprintf(out, n, "%s", v);
        return 1;
    }
    snprintf(out, n, "C:\\Program Files (x86)\\Steam");
    return file_exists(out);
#else
    /* ~/.steam/steam is the link Steam itself keeps; then the usual homes
     * of a native and a Flatpak install. */
    static const char *const cand[] = {
        "/.steam/steam", "/.local/share/Steam",
        "/.var/app/com.valvesoftware.Steam/.local/share/Steam",
    };
    const char *h = getenv("HOME");
    size_t i;
    for (i = 0; h && i < sizeof cand / sizeof cand[0]; i++) {
        char p[PATH_MAX], ud[PATH_MAX + 16];
        snprintf(p, sizeof p, "%s%s", h, cand[i]);
        snprintf(ud, sizeof ud, "%s/userdata", p);
        if (file_exists(ud)) {
            char real[PATH_MAX];
            snprintf(out, n, "%s", realpath(p, real) ? real : p);
            return 1;
        }
    }
    return 0;
#endif
}

static int steam_running(void)
{
#ifdef _WIN32
    DWORD pid = 0, sz = sizeof pid;
    HANDLE h;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam\\ActiveProcess", "pid",
                     RRF_RT_REG_DWORD, NULL, &pid, &sz) != ERROR_SUCCESS || !pid)
        return 0;
    h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h)
        return 0;
    {
        int alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
        CloseHandle(h);
        return alive;
    }
#else
    /* Steam writes its pid here and leaves the file behind when it quits,
     * so the pid has to be alive and still be Steam. */
    const char *h = getenv("HOME");
    char p[PATH_MAX], comm[64] = "";
    FILE *f;
    long pid = 0;
    if (!h)
        return 0;
    snprintf(p, sizeof p, "%s/.steam/steam.pid", h);
    f = fopen(p, "r");
    if (!f)
        return 0;
    if (fscanf(f, "%ld", &pid) != 1)
        pid = 0;
    fclose(f);
    if (pid <= 0 || kill((pid_t)pid, 0) != 0)
        return 0;
    snprintf(p, sizeof p, "/proc/%ld/comm", pid);
    f = fopen(p, "r");
    if (f) {
        if (!fgets(comm, sizeof comm, f))
            comm[0] = 0;
        fclose(f);
    }
    return strncmp(comm, "steam", 5) == 0;
#endif
}

/* ── what to launch ───────────────────────────────────────── */

/* Linux: rsc2/run.sh beside the build directory when there is one -- it
 * picks the discrete GPU -- otherwise the executable itself. */
static int game_exe(char *exe, size_t n, char *dir, size_t dn)
{
    char self[4096], *sl;
#ifdef _WIN32
    DWORD k = GetModuleFileNameA(NULL, self, sizeof self);
    if (!k || k >= sizeof self)
        return 0;
#else
    ssize_t k = readlink("/proc/self/exe", self, sizeof self - 1);
    if (k <= 0)
        return 0;
    self[k] = 0;
#endif
    snprintf(exe, n, "%s", self);
    sl = strrchr(self, PATHSEP);
    if (!sl)
        return 0;
    *sl = 0;
    snprintf(dir, dn, "%s", self);
#ifndef _WIN32
    {
        char run[4200], *up;
        snprintf(run, sizeof run, "%s/run.sh", self);
        if (!file_exists(run)) {
            up = strrchr(self, '/');
            if (up) {
                *up = 0;
                snprintf(run, sizeof run, "%s/run.sh", self);
                if (!file_exists(run))
                    *up = '/';
            }
        }
        if (file_exists(run)) {
            snprintf(exe, n, "%s", run);
            snprintf(dir, dn, "%s", self);
        }
    }
#endif
    return 1;
}

/* ── shortcuts.vdf ────────────────────────────────────────── */

typedef struct { unsigned char *p; size_t n, cap; } Buf;

static void put(Buf *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) {
        size_t cap = (b->n + n) * 2 + 256;
        unsigned char *q = (unsigned char *)realloc(b->p, cap);
        if (!q)
            return;
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}

static void put_str(Buf *b, const char *key, const char *val)
{
    put(b, "\x01", 1);
    put(b, key, strlen(key) + 1);
    put(b, val, strlen(val) + 1);
}

static void put_int(Buf *b, const char *key, uint32_t v)
{
    unsigned char le[4] = { (unsigned char)v, (unsigned char)(v >> 8),
                            (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    put(b, "\x02", 1);
    put(b, key, strlen(key) + 1);
    put(b, le, 4);
}

static uint32_t crc32(const unsigned char *d, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= d[i];
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* One value at *pos (type byte first); 0 when the file is not what it
 * should be. For a map, `fn` sees each string inside it, at any depth. */
static int vdf_skip(const unsigned char *d, size_t n, size_t *pos,
                    void (*fn)(void *, const char *, const char *), void *ctx)
{
    unsigned char t;
    const char *key;
    if (*pos >= n)
        return 0;
    t = d[(*pos)++];
    key = (const char *)d + *pos;
    *pos += strnlen(key, n - *pos) + 1;
    if (*pos > n)
        return 0;
    switch (t) {
    case 0x00:
        while (*pos < n && d[*pos] != 0x08)
            if (!vdf_skip(d, n, pos, fn, ctx))
                return 0;
        if (*pos >= n)
            return 0;
        (*pos)++;
        return 1;
    case 0x01: {
        const char *v = (const char *)d + *pos;
        *pos += strnlen(v, n - *pos) + 1;
        if (*pos > n)
            return 0;
        if (fn)
            fn(ctx, key, v);
        return 1;
    }
    case 0x02: case 0x03: case 0x04: case 0x06:
        *pos += 4;
        return *pos <= n;
    case 0x07: case 0x0A:
        *pos += 8;
        return *pos <= n;
    default:
        return 0;
    }
}

typedef struct { const char *exe_q; int found; } Dup;

static int ieq(const char *a, const char *b)
{
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b))
        a++, b++;
    return *a == *b;
}

static void dup_check(void *ctx, const char *key, const char *val)
{
    Dup *d = (Dup *)ctx;
    if ((ieq(key, "Exe") && strcmp(val, d->exe_q) == 0)
            || (ieq(key, "AppName") && strcmp(val, APP_NAME) == 0))
        d->found = 1;
}

static unsigned char *read_all(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *d = NULL;
    long sz;
    *n = 0;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (sz = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        d = (unsigned char *)malloc((size_t)sz + 1);
        if (d && fread(d, 1, (size_t)sz, f) == (size_t)sz)
            *n = (size_t)sz;
    }
    fclose(f);
    return d;
}

static int write_all(const char *path, const unsigned char *d, size_t n)
{
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f)
        return 0;
    ok = fwrite(d, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

/* 1 added, 2 already there, 0 failed. */
static int add_to(const char *cfgdir, const char *exe, const char *dir)
{
    static const unsigned char empty[] = "\x00shortcuts\x00\x08\x08";
    char path[4200], bak[4300], exe_q[4200], dir_q[4200], idx[16];
    unsigned char *d;
    size_t n, pos, count = 0;
    Buf b = { 0 };
    Dup dup;
    int ok;

    snprintf(path, sizeof path, "%s%cshortcuts.vdf", cfgdir, PATHSEP);
    snprintf(exe_q, sizeof exe_q, "\"%s\"", exe);
    snprintf(dir_q, sizeof dir_q, "\"%s\"", dir);
    d = read_all(path, &n);
    if (!d || n == 0) {
        free(d);
        d = (unsigned char *)malloc(sizeof empty - 1);
        if (!d)
            return 0;
        memcpy(d, empty, sizeof empty - 1);
        n = sizeof empty - 1;
    }
    /* 00 "shortcuts" 00 { entries } 08, then the root's 08. */
    if (n < 13 || memcmp(d, empty, 11) != 0 || d[n - 1] != 0x08 || d[n - 2] != 0x08) {
        free(d);
        return 0;
    }
    dup.exe_q = exe_q;
    dup.found = 0;
    pos = 11;
    while (pos < n && d[pos] != 0x08) {
        if (!vdf_skip(d, n, &pos, dup_check, &dup)) {
            free(d);
            return 0;
        }
        count++;
    }
    if (pos != n - 2) {
        free(d);
        return 0;
    }
    if (dup.found) {
        free(d);
        return 2;
    }

    put(&b, d, n - 2);
    snprintf(idx, sizeof idx, "%u", (unsigned)count);
    put(&b, "\x00", 1);
    put(&b, idx, strlen(idx) + 1);
    {
        /* The id Steam derives for a shortcut, which also names its artwork. */
        char key[8400];
        snprintf(key, sizeof key, "%s%s", exe_q, APP_NAME);
        put_int(&b, "appid", crc32((const unsigned char *)key, strlen(key)) | 0x80000000u);
    }
    put_str(&b, "appname", APP_NAME);    /* the case Steam writes */
    put_str(&b, "Exe", exe_q);
    put_str(&b, "StartDir", dir_q);
    put_str(&b, "icon", "");
    put_str(&b, "ShortcutPath", "");
    put_str(&b, "LaunchOptions", "");
    put_int(&b, "IsHidden", 0);
    put_int(&b, "AllowDesktopConfig", 1);
    put_int(&b, "AllowOverlay", 1);
    put_int(&b, "OpenVR", 0);
    put_int(&b, "Devkit", 0);
    put_str(&b, "DevkitGameID", "");
    put_int(&b, "DevkitOverrideAppID", 0);
    put_int(&b, "LastPlayTime", 0);
    put_str(&b, "FlatpakAppID", "");
    put(&b, "\x00tags\x00\x08", 7);
    put(&b, "\x08\x08\x08", 3);   /* the entry, "shortcuts", the root */

    snprintf(bak, sizeof bak, "%s.bak", path);
    if (file_exists(path)) {
        size_t on;
        unsigned char *old = read_all(path, &on);
        if (old)
            write_all(bak, old, on);
        free(old);
    }
    ok = b.p && write_all(path, b.p, b.n);
    free(b.p);
    free(d);
    return ok ? 1 : 0;
}

/* The account folders in userdata: numeric names, "0" being no account. */
static int list_accounts(const char *ud, char (*names)[32], int max)
{
    int count = 0;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    char pat[4300];
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\*", ud);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        const char *name = fd.cFileName;
#else
    DIR *dd = opendir(ud);
    struct dirent *e;
    if (!dd)
        return 0;
    while ((e = readdir(dd)) != NULL) {
        const char *name = e->d_name;
#endif
        size_t len = strlen(name);
        if (count < max && isdigit((unsigned char)name[0]) && strcmp(name, "0") != 0
                && len < 32) {
            memcpy(names[count], name, len + 1);
            count++;
        }
#ifdef _WIN32
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    }
    closedir(dd);
#endif
    return count;
}

/* The account id (userdata folder name) Steam last logged in with, or 0:
 * the user marked "MostRecent", or, in files without that key, the one
 * with the latest "timestamp". */
static unsigned long most_recent_account(const char *root)
{
    char path[4200], line[512], id[32] = "";
    unsigned long long best = 0, best_ts = 0;
    FILE *f;
    snprintf(path, sizeof path, "%s%cconfig%cloginusers.vdf", root, PATHSEP, PATHSEP);
    f = fopen(path, "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        char *k = strchr(line, '"'), *ke, *v = NULL;
        if (!k || !(ke = strchr(k + 1, '"')))
            continue;
        *ke = 0;
        k++;
        if ((v = strchr(ke + 1, '"')) != NULL) {
            char *ve = strchr(v + 1, '"');
            if (ve)
                *ve = 0;
            v++;
        }
        if (strncmp(k, "7656", 4) == 0 && strlen(k) == 17) {
            snprintf(id, sizeof id, "%s", k);
        } else if (id[0] && v && ieq(k, "MostRecent") && strcmp(v, "1") == 0) {
            best = strtoull(id, NULL, 10);
            break;
        } else if (id[0] && v && ieq(k, "timestamp")) {
            unsigned long long ts = strtoull(v, NULL, 10);
            if (ts > best_ts) {
                best_ts = ts;
                best = strtoull(id, NULL, 10);
            }
        }
    }
    fclose(f);
    /* SteamID64 minus the individual-account base. */
    return best > 76561197960265728ULL ? (unsigned long)(best - 76561197960265728ULL) : 0;
}

int rsc2_steam_add(char *msg, size_t n)
{
    char root[4096], exe[4096], dir[4096], cfg[4300], ud[4200];
    unsigned long acct;
    int added = 0, already = 0, failed = 0;

    if (!steam_root(root, sizeof root)) {
        snprintf(msg, n, "Steam was not found on this computer.");
        return 0;
    }
    if (steam_running()) {
        snprintf(msg, n, "Steam is running. Quit Steam completely, then press "
                 "Add to Steam again: Steam rewrites its list of games when it closes.");
        return 0;
    }
    if (!game_exe(exe, sizeof exe, dir, sizeof dir)) {
        snprintf(msg, n, "Could not work out where the game is installed.");
        return 0;
    }
    snprintf(ud, sizeof ud, "%s%cuserdata", root, PATHSEP);
    acct = most_recent_account(root);
    if (acct) {
        char acct_dir[4300];
        snprintf(acct_dir, sizeof acct_dir, "%s%c%lu", ud, PATHSEP, acct);
        if (!file_exists(acct_dir))
            acct = 0;
    }

    {
        char names[64][32];
        int k, count = list_accounts(ud, names, 64);
        for (k = 0; k < count; k++) {
            int r;
            if (acct && strtoul(names[k], NULL, 10) != acct)
                continue;
            snprintf(cfg, sizeof cfg, "%s%c%s%cconfig", ud, PATHSEP, names[k], PATHSEP);
#ifdef _WIN32
            CreateDirectoryA(cfg, NULL);
#else
            mkdir(cfg, 0755);
#endif
            r = add_to(cfg, exe, dir);
            if (r == 1) added++;
            else if (r == 2) already++;
            else failed++;
        }
    }

    if (added)
        snprintf(msg, n, "Added to Steam. Start Steam and it is in your library "
                 "as \"" APP_NAME "\"%s.", failed ? " (one account's list could not be read)" : "");
    else if (already)
        snprintf(msg, n, APP_NAME " is already in your Steam library.");
    else if (failed)
        snprintf(msg, n, "Steam's list of non-Steam games could not be read or written; "
                 "nothing was changed.");
    else
        snprintf(msg, n, "No Steam account was found. Log in to Steam once, then try again.");
    return added > 0;
}
