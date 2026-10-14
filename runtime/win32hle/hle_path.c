/*
 * hle_path.c - Windows paths on a POSIX file system.
 *
 * A guest names files the Windows way: `C:\Westwood\Sun\SCORES.MIX`,
 * `..\Movies`, `data\rules.ini`, in any case. The host's files are on a
 * case-sensitive file system in whatever case they were installed in. So:
 *
 *   - a drive letter maps to a host directory (hle_set_drive; Z: is "/", as
 *     under Wine), and a path without one is relative to the host's cwd;
 *   - '\\' and '/' both separate;
 *   - each component that does not exist as spelled is looked up in its
 *     directory without regard to case.
 *
 * The reverse (a host directory as the guest should see it) is
 * hle_guest_path, for GetCurrentDirectoryA and GetModuleFileNameA.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include "win32hle.h"

static char *g_drive[26];

void hle_set_drive(char letter, const char *host_root) {
    int i = (letter | 0x20) - 'a';
    if (i < 0 || i >= 26) return;
    free(g_drive[i]);
    g_drive[i] = host_root ? realpath(host_root, NULL) : NULL;
}

static const char *drive_root(int i) {
    if (g_drive[i]) return g_drive[i];
    return i == 'z' - 'a' ? "" : NULL;          /* Z: is the host's root */
}

/* Append one component to out (which holds an existing directory, or ""
 * for the cwd), matching it without case if it is not there as spelled.
 * Returns 1 if the result exists. */
/* The names of a directory, for the case-insensitive match: read once and
 * kept until the directory's mtime changes, because the game asks the disk
 * for thousands of files that are not there before it reads them from its
 * MIX files, and an asset pack's layer holds 5,000 files: a readdir per
 * miss cost 42 s at a skirmish's start. */
#define DIR_CACHE 64
typedef struct { char *path; time_t mtime; char **names; int n; } dir_cache_t;
static dir_cache_t g_dirs[DIR_CACHE];
static int g_ndirs, g_dirs_next;

static int name_cmp(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

static void dir_cache_free(dir_cache_t *c) {
    for (int i = 0; i < c->n; i++) free(c->names[i]);
    free(c->names), free(c->path);
    memset(c, 0, sizeof *c);
}

static int dir_cache_fill(dir_cache_t *c, const char *dir, time_t mtime) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int cap = 256;
    c->path = strdup(dir);
    c->names = (char **)malloc(sizeof(char *) * (size_t)cap);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (c->n == cap) cap *= 2, c->names = (char **)realloc(c->names, sizeof(char *) * (size_t)cap);
        c->names[c->n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(c->names, (size_t)c->n, sizeof(char *), name_cmp);
    c->mtime = mtime;
    return 1;
}

static dir_cache_t *dir_cache_get(const char *dir) {
    struct stat st;
    if (stat(dir, &st) != 0) return NULL;
    dir_cache_t *c = NULL;
    for (int i = 0; i < g_ndirs && !c; i++)
        if (g_dirs[i].path && !strcmp(g_dirs[i].path, dir)) c = &g_dirs[i];
    if (c && c->mtime == st.st_mtime) return c;
    if (!c) {                                            /* a new slot, or the oldest when full */
        if (g_ndirs < DIR_CACHE) c = &g_dirs[g_ndirs++];
        else c = &g_dirs[g_dirs_next], g_dirs_next = (g_dirs_next + 1) % DIR_CACHE;
    }
    dir_cache_free(c);
    return dir_cache_fill(c, dir, st.st_mtime) ? c : NULL;
}

/* The directory's entry that is want without case, or NULL. */
static const char *dir_find(const char *dir, const char *want) {
    dir_cache_t *c = dir_cache_get(dir);
    if (!c) return NULL;
    int lo = 0, hi = c->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, r = strcasecmp(c->names[mid], want);
        if (!r) return c->names[mid];
        if (r < 0) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static int join_component(char *out, size_t n, const char *comp, size_t clen) {
    size_t len = strlen(out);
    if (clen == 1 && comp[0] == '.') return 1;
    if (clen == 2 && comp[0] == '.' && comp[1] == '.') {
        if (!len || !strcmp(out, ".") ) { snprintf(out, n, ".."); return 1; }
        char *slash = strrchr(out, '/');
        if (slash && strcmp(slash + 1, "..")) { if (slash == out) slash[1] = 0; else *slash = 0; return 1; }
        snprintf(out + len, n - len, "%s..", len && out[len - 1] != '/' ? "/" : "");
        return 1;
    }
    char want[512];
    if (clen >= sizeof want) clen = sizeof want - 1;
    memcpy(want, comp, clen), want[clen] = 0;
    const char *sep = len && out[len - 1] != '/' ? "/" : "";
    snprintf(out + len, n - len, "%s%s", sep, want);
    struct stat st;
    if (lstat(out[0] ? out : ".", &st) == 0) return 1;
    out[len] = 0;
    const char *hit = dir_find(len ? out : ".", want);
    int found = hit != NULL;
    snprintf(out + len, n - len, "%s%s", sep, found ? hit : want);
    return found;
}

/* ---- overlays: a mod's folder laid over the game's, and read-only layers ----
 *
 * With hle_set_overlay(base, overlay), a file under base is looked for in
 * overlay first (at the same relative path, without case), so a mod's
 * rules.ini or map is read instead of the game's. Writes to files under base
 * go to the overlay (hle_host_path_for_write), so playing a mod never changes
 * the game's own files; FindFirstFileA lists both, the overlay's first.
 *
 * hle_add_overlay(dir) adds a read-only layer under the same base (an asset
 * pack's built sprites): reads look in the mod's layer, then each read-only
 * layer in the order added, then the game's folder. Writes never go to a
 * read-only layer: to the mod's when there is one, else the game's folder. */
#define MAX_LAYERS 16
static char *g_overlay_base;
static char *g_layers[MAX_LAYERS];     /* [0] the writable one (a mod), or NULL; then read-only */
static int g_nlayers;

static void layers_clear(void) {
    for (int i = 0; i < MAX_LAYERS; i++) free(g_layers[i]), g_layers[i] = NULL;
    free(g_overlay_base), g_overlay_base = NULL;
    g_nlayers = 0;
}

void hle_set_overlay(const char *base, const char *overlay) {
    layers_clear();
    if (!base) return;
    g_overlay_base = realpath(base, NULL);
    if (!g_overlay_base) return;
    g_layers[0] = overlay ? realpath(overlay, NULL) : NULL;
    g_nlayers = 1;
}

int hle_add_overlay(const char *dir) {
    if (!g_overlay_base || !dir || g_nlayers >= MAX_LAYERS) return 0;
    char *real = realpath(dir, NULL);
    if (!real) return 0;
    g_layers[g_nlayers++] = real;
    return 1;
}

/* host as an absolute path, if it is relative (to the cwd). */
/* host as an absolute path, if it is relative (to the cwd). */
static void absolute(const char *host, char *out, size_t n) {
    char cwd[768];
    if (host[0] == '/' || !getcwd(cwd, sizeof cwd)) snprintf(out, n, "%s", host);
    else snprintf(out, n, "%s/%s", cwd, strcmp(host, ".") ? host : "");
    size_t l = strlen(out);
    while (l > 1 && out[l - 1] == '/') out[--l] = 0;
}

static int under(const char *path, const char *dir) {
    size_t l = strlen(dir);
    return !strncmp(path, dir, l) && (path[l] == '/' || !path[l]);
}

/* The path of host inside layer k: -1 when host is not under base (or is in
 * a layer itself), else whether it exists there. */
static int layer_path(int k, const char *host, char *out, size_t n) {
    char abs[1024];
    out[0] = 0;
    if (!g_overlay_base || k >= g_nlayers || !g_layers[k]) return -1;
    absolute(host, abs, sizeof abs);
    for (int i = 0; i < g_nlayers; i++) if (g_layers[i] && under(abs, g_layers[i])) return -1;
    if (!under(abs, g_overlay_base)) return -1;
    snprintf(out, n, "%s", g_layers[k]);
    const char *p = abs + strlen(g_overlay_base);
    int exists = 1;
    while (*p) {
        while (*p == '/') p++;
        const char *e = p;
        while (*e && *e != '/') e++;
        if (e > p) exists = join_component(out, n, p, (size_t)(e - p)) && exists;
        p = e;
    }
    return exists;
}

/* The writable layer's path for host (the mod's). */
int hle_overlay_of(const char *host, char *out, size_t n) { return layer_path(0, host, out, n); }

/* Every layer's path for host, the mod's first: how many (0 when host is
 * not under base). */
int hle_overlay_layers(const char *host, char out[][1024], int max) {
    int n = 0;
    for (int k = 0; k < g_nlayers && n < max; k++)
        if (layer_path(k, host, out[n], 1024) >= 0) n++;
    return n;
}

/* The path a write to guest should go to: the overlay's copy when the file is
 * the game's (its directories made; with keep, the game's file copied there
 * first, so a write into the middle of it keeps the rest). */
static int host_path_plain(const char *guest, char *out, size_t n);

int hle_host_path_for_write(const char *guest, char *out, size_t n, int keep) {
    int exists = hle_host_path(guest, out, n);        /* as it is read: a layer's copy or the game's */
    char plain[1024], abs[1024], ov[1024];
    struct stat st;
    host_path_plain(guest, plain, sizeof plain);
    if (!plain[0]) return exists;
    absolute(plain, abs, sizeof abs);
    if (!g_overlay_base || !under(abs, g_overlay_base)) return exists;   /* not the game's: as read */
    if (hle_overlay_of(abs, ov, sizeof ov) < 0) {       /* no mod: the game's own file, never a read-only layer's */
        snprintf(out, n, "%s", abs);
        return stat(out, &st) == 0;
    }
    for (char *s = ov + strlen(g_layers[0]); *s && (s = strchr(s + 1, '/')); ) {   /* mkdir -p its directory */
        *s = 0;
        mkdir(ov, 0755);
        *s = '/';
    }
    if (keep && exists && strcmp(out, ov)) {           /* the copy read (a layer's or the game's) into the mod first */
        FILE *in = fopen(out, "rb"), *to = in ? fopen(ov, "wb") : NULL;
        char buf[65536];
        size_t r;
        while (to && (r = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, r, to);
        if (in) fclose(in);
        if (to) fclose(to);
    }
    snprintf(out, n, "%s", ov);
    return keep && exists;
}

int hle_host_path(const char *guest, char *out, size_t n) {
    int exists = host_path_plain(guest, out, n);
    char ov[1024];
    struct stat st;
    /* A file only: a directory stays the game's (the cwd is set through here,
     * and FindFirstFileA lists both) */
    for (int k = 0; k < g_nlayers; k++)
        if (layer_path(k, out, ov, sizeof ov) == 1 && stat(ov, &st) == 0 && !S_ISDIR(st.st_mode)) {
            snprintf(out, n, "%s", ov);
            return 1;
        }
    return exists;
}

/* The game's own path for guest, ignoring any overlay. */
int hle_host_path_base(const char *guest, char *out, size_t n) { return host_path_plain(guest, out, n); }

static int host_path_plain(const char *guest, char *out, size_t n) {
    out[0] = 0;
    if (!guest) return 0;
    const char *p = guest;
    if (((p[0] | 0x20) >= 'a' && (p[0] | 0x20) <= 'z') && p[1] == ':') {
        const char *root = drive_root((p[0] | 0x20) - 'a');
        if (!root) return 0;
        snprintf(out, n, "%s", *root ? root : "/");
        p += 2;
        while (*p == '\\' || *p == '/') p++;
    } else if (*p == '\\' || *p == '/') {
        /* rooted without a drive: the C: drive's root, if there is one */
        const char *root = drive_root(2);
        snprintf(out, n, "%s", root && *root ? root : "/");
        while (*p == '\\' || *p == '/') p++;
    }
    int exists = 1;
    while (*p) {
        const char *e = p;
        while (*e && *e != '\\' && *e != '/') e++;
        if (e > p) exists = join_component(out, n, p, (size_t)(e - p)) && exists;
        p = e;
        while (*p == '\\' || *p == '/') p++;
    }
    if (!out[0]) snprintf(out, n, ".");
    return exists;
}

/* A host path (absolute) as the guest sees it: under a mapped drive, that
 * drive; otherwise Z: and the whole path. Backslashes. */
void hle_guest_path(const char *host, char *out, size_t n) {
    char abs[1024];
    if (host[0] != '/') {
        char cwd[768];
        if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
        snprintf(abs, sizeof abs, "%s/%s", cwd, host);
    } else {
        snprintf(abs, sizeof abs, "%s", host);
    }
    char *real = realpath(abs, NULL);
    const char *h = real ? real : abs;
    int best = -1;
    size_t best_len = 0;
    for (int i = 0; i < 26; i++) {
        const char *r = g_drive[i];
        size_t l = r ? strlen(r) : 0;
        if (r && !strncmp(h, r, l) && (h[l] == '/' || !h[l]) && l >= best_len) best = i, best_len = l;
    }
    const char *rest = best >= 0 ? h + best_len : h;
    snprintf(out, n, "%c:%s", best >= 0 ? 'A' + best : 'Z', *rest ? "" : "\\");
    size_t k = strlen(out);
    for (; *rest && k + 1 < n; rest++) out[k++] = *rest == '/' ? '\\' : *rest;
    out[k] = 0;
    free(real);
}
