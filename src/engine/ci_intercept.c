#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Case-insensitive syscall interposer (LD_PRELOAD, v0.3.1).
//
// The Linux overlay the game runs on is case-SENSITIVE (btrfs), but Windows
// games resolve paths case-insensitively. Deploy writes exactly one casing per
// path, so a game that queries a different casing (SkyParkourNG: looks up
// `0_master.hxk`, Pandora output deployed `0_Master.hxk`) hits ENOENT.
//
// This library re-resolves READ-side syscalls under GMM_CI_ROOT: if the real
// syscall fails ENOENT, it walks each path component case-insensitively
// against the on-disk tree and retries the spelling that actually exists.
//
// Intercepted: open/openat (read-only, no O_CREAT), stat/lstat/fstatat,
// access/faccessat. WRITES and open-with-create pass straight through: the
// game must create files with exactly the casing it requested.
//
// On 64-bit glibc, open64/stat64/... are aliases of the base functions, so
// only the base symbols are interposed.

static int gmm_ci_enabled = 0;
static int gmm_ci_debug = 0;
static char gmm_ci_root[PATH_MAX];

static void gmm_ci_log(const char *fmt, ...) {
    if (!gmm_ci_debug) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[GMM-CI] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static int is_true(const char *v) {
    if (!v) return 0;
    return !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes") || !strcmp(v, "on");
}

__attribute__((constructor))
static void gmm_ci_ctor(void) {
    gmm_ci_enabled = is_true(getenv("GMM_CI_ENABLED"));
    gmm_ci_debug = is_true(getenv("GMM_CI_DEBUG"));
    if (!gmm_ci_enabled) return;

    const char *root = getenv("GMM_CI_ROOT");
    if (!root || !*root) { gmm_ci_enabled = 0; return; }
    if (realpath(root, gmm_ci_root) == NULL) { gmm_ci_enabled = 0; return; }
    size_t l = strlen(gmm_ci_root);
    while (l > 1 && gmm_ci_root[l - 1] == '/') gmm_ci_root[--l] = '\0';
    if (gmm_ci_debug) gmm_ci_log("enabled root=%s", gmm_ci_root);
}

/* case-insensitive compare of two byte strings of equal length n */
static int name_eq_ci(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return 1;
}

/* ---- dlsym(RTLD_NEXT) resolved lazily ---- */
typedef int (*openfn_t)(const char *, int, ...);
typedef int (*openatfn_t)(int, const char *, int, ...);
typedef int (*statfn_t)(const char *, struct stat *);
typedef int (*fstatatfn_t)(int, const char *, struct stat *, int);
typedef int (*accessfn_t)(const char *, int);
typedef int (*faccessatfn_t)(int, const char *, int, int);

/* absolute `path` at-or-under root? *rel_out = suffix (no leading '/'). */
static int under_root(const char *path, const char **rel_out) {
    if (!path || path[0] != '/') return 0;
    size_t rl = strlen(gmm_ci_root);
    if (strncmp(path, gmm_ci_root, rl) != 0) return 0;
    const char *rest = path + rl;
    if (*rest == '/') rest++;
    else if (*rest != '\0') return 0;
    if (rel_out) *rel_out = rest;
    return 1;
}

/* CI-resolve an absolute path under root into out (cap). Returns 1 and writes
 * the (existing, possibly different-casing) spelling; returns 0 when the
 * requested spelling already exists or nothing CI-equal resolves. */
static statfn_t real_lstat_fn(void);

static int ci_resolve(const char *abs_path, char *out, size_t cap) {
    const char *rel = NULL;
    if (!under_root(abs_path, &rel)) return 0;

    size_t root_len = strlen(gmm_ci_root);
    size_t olen = root_len;
    memcpy(out, gmm_ci_root, olen);
    out[olen] = '\0';
    int changed = 0;

    const char *p = rel;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t clen = (size_t)(p - start);

        if (olen + 1 + clen + 1 >= cap) return 0;
        out[olen++] = '/';
        memcpy(out + olen, start, clen);
        out[olen + clen] = '\0';

        struct stat sb;
        int exact = (real_lstat_fn()(out, &sb) == 0);
        if (!exact) {
            /* parent prefix = out up to (and excluding) the component.
             * olen currently points at start of this component; the path up
             * to olen-1 (the preceding '/') is the parent dir path. */
            char parent[PATH_MAX];
            size_t pl = olen - 1;
            memcpy(parent, out, pl);
            parent[pl] = '\0';

            DIR *d = opendir(parent);
            if (!d) return 0;
            int matched = 0;
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (de->d_name[0] == '.') continue;
                size_t nl = strlen(de->d_name);
                if (nl == clen && name_eq_ci(de->d_name, start, clen)) {
                    if (strncmp(de->d_name, start, clen) != 0) {
                        memcpy(out + olen, de->d_name, clen);
                        changed = 1;
                    }
                    matched = 1;
                    break;
                }
            }
            closedir(d);
            if (!matched) return 0;
        }
        olen += clen;
        out[olen] = '\0';
    }
    return changed;
}

/* __O_TMPFILE = 010000000 */
static int is_read_open(int flags) {
    return !(flags & (O_WRONLY | O_RDWR | O_CREAT | 010000000));
}

/* non-interposed lstat: cached, used for existence checks inside ci_resolve so
 * we never recurse back into our own interposer. */
/* next-function pointers (interposed symbols call these via RTLD_NEXT) */
static openfn_t F_open;
static openatfn_t F_openat;
static statfn_t F_stat;
static statfn_t F_lstat_real;
static fstatatfn_t F_fstatat;
static accessfn_t F_access;
static faccessatfn_t F_faccessat;

/* non-interposed lstat: cached, used for existence checks inside ci_resolve so
 * we never recurse back into our own interposer. */
static statfn_t real_lstat_fn(void) {
    if (!F_lstat_real) F_lstat_real = (statfn_t)dlsym(RTLD_NEXT, "lstat");
    return F_lstat_real;
}

static int open_with_ci(const char *path, int flags,
                        int (*real_fn)(const char *, int, ...), const char *what) {
    if (!gmm_ci_enabled || !is_read_open(flags))
        return real_fn(path, flags, 0);

    int ret = real_fn(path, flags, 0);
    if (ret >= 0) return ret;
    int saved = errno;
    if (saved != ENOENT) return ret;

    const char *rel;
    if (!under_root(path, &rel)) return ret;

    char buf[PATH_MAX];
    if (ci_resolve(path, buf, sizeof(buf))) {
        int ret2 = real_fn(buf, flags, 0);
        if (ret2 >= 0) {
            gmm_ci_log("%s '%s' -> '%s'", what, path, buf);
            return ret2;
        }
        if (gmm_ci_debug) gmm_ci_log("%s ci-open %s failed (%s)", what, buf, strerror(errno));
    }
    errno = saved;
    return -1;
}

int open(const char *path, int flags, ...) {
    if (!F_open) F_open = (openfn_t)dlsym(RTLD_NEXT, "open");
    return open_with_ci(path, flags, F_open, "open");
}

int openat(int dirfd, const char *path, int flags, ...) {
    if (!F_openat) F_openat = (openatfn_t)dlsym(RTLD_NEXT, "openat");
    va_list ap;
    va_start(ap, flags);
    mode_t mode = (flags & O_CREAT) ? (mode_t)va_arg(ap, int) : 0;
    va_end(ap);

    if (!gmm_ci_enabled || dirfd != AT_FDCWD || !is_read_open(flags))
        return F_openat(dirfd, path, flags, mode);

    int ret = F_openat(dirfd, path, flags, mode);
    if (ret >= 0) return ret;
    int saved = errno;
    if (saved != ENOENT) return ret;

    const char *rel;
    if (!under_root(path, &rel)) return ret;
    char buf[PATH_MAX];
    if (ci_resolve(path, buf, sizeof(buf))) {
        int ret2 = F_openat(AT_FDCWD, buf, flags, mode);
        if (ret2 >= 0) return ret2;
    }
    errno = saved;
    return -1;
}

static int stat_with_ci(const char *path, struct stat *sb,
                        int (*real_fn)(const char *, struct stat *), const char *what) {
    if (!gmm_ci_enabled) return real_fn(path, sb);

    int ret = real_fn(path, sb);
    if (ret == 0) return 0;
    int saved = errno;
    if (saved != ENOENT) return ret;

    const char *rel;
    if (!under_root(path, &rel)) return ret;
    char buf[PATH_MAX];
    if (ci_resolve(path, buf, sizeof(buf))) {
        int ret2 = real_fn(buf, sb);
        if (ret2 == 0) {
            gmm_ci_log("%s '%s' -> '%s'", what, path, buf);
            return 0;
        }
    }
    errno = saved;
    return -1;
}

int stat(const char *path, struct stat *sb) {
    if (!F_stat) F_stat = (statfn_t)dlsym(RTLD_NEXT, "stat");
    return stat_with_ci(path, sb, F_stat, "stat");
}

int lstat(const char *path, struct stat *sb) {
    if (!F_stat) F_stat = (statfn_t)dlsym(RTLD_NEXT, "stat");
    return stat_with_ci(path, sb, F_stat, "lstat");
}

int fstatat(int dirfd, const char *path, struct stat *sb, int flags) {
    if (!F_fstatat) F_fstatat = (fstatatfn_t)dlsym(RTLD_NEXT, "fstatat");
    if (!gmm_ci_enabled || dirfd != AT_FDCWD)
        return F_fstatat(dirfd, path, sb, flags);

    int ret = F_fstatat(dirfd, path, sb, flags);
    if (ret == 0) return 0;
    int saved = errno;
    if (saved != ENOENT) return ret;

    const char *rel;
    if (!under_root(path, &rel)) return ret;
    char buf[PATH_MAX];
    if (ci_resolve(path, buf, sizeof(buf))) {
        int ret2 = F_fstatat(AT_FDCWD, buf, sb, flags);
        if (ret2 == 0) return 0;
    }
    errno = saved;
    return -1;
}

static int access_with_ci(const char *path, int mode,
                          int (*real_fn)(const char *, int), const char *what) {
    if (!gmm_ci_enabled) return real_fn(path, mode);

    int ret = real_fn(path, mode);
    if (ret == 0) return 0;
    int saved = errno;
    if (saved != ENOENT) return ret;

    const char *rel;
    if (!under_root(path, &rel)) return ret;
    char buf[PATH_MAX];
    if (ci_resolve(path, buf, sizeof(buf))) {
        int ret2 = real_fn(buf, mode);
        if (ret2 == 0) {
            gmm_ci_log("%s '%s' -> '%s'", what, path, buf);
            return 0;
        }
    }
    errno = saved;
    return -1;
}

int access(const char *path, int mode) {
    if (!F_access) F_access = (accessfn_t)dlsym(RTLD_NEXT, "access");
    return access_with_ci(path, mode, F_access, "access");
}

int faccessat(int dirfd, const char *path, int mode, int flags) {
    if (!F_faccessat) F_faccessat = (faccessatfn_t)dlsym(RTLD_NEXT, "faccessat");
    if (!gmm_ci_enabled || dirfd != AT_FDCWD)
        return F_faccessat(dirfd, path, mode, flags);

    int ret = F_faccessat(dirfd, path, mode, flags);
    if (ret == 0) return 0;
    int saved = errno;
    if (saved != ENOENT) return ret;

    const char *rel;
    if (!under_root(path, &rel)) return ret;
    char buf[PATH_MAX];
    if (ci_resolve(path, buf, sizeof(buf))) {
        int ret2 = F_faccessat(AT_FDCWD, buf, mode, flags);
        if (ret2 == 0) return 0;
    }
    errno = saved;
    return -1;
}