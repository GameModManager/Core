#define _GNU_SOURCE
#include "engine/core/util/debug_env.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static __thread int gmm_recursing = 0;
static int gmm_debug = 0;

static char *gmm_game_dir = NULL;
static size_t gmm_game_dir_len = 0;
static char *gmm_overwrite_dir = NULL;
static size_t gmm_overwrite_dir_len = 0;

static void gmm_log(const char *fmt, ...) {
    if (!gmm_debug) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[GMM] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

typedef int (*real_open_t)(const char *, int, mode_t);
typedef int (*real_openat_t)(int, const char *, int, mode_t);
typedef int (*real_creat_t)(const char *, mode_t);
typedef int (*real_rename_t)(const char *, const char *);
typedef int (*real_renameat_t)(int, const char *, int, const char *);
typedef int (*real_unlink_t)(const char *);
typedef int (*real_unlinkat_t)(int, const char *, int);
typedef int (*real_mkdir_t)(const char *, mode_t);
typedef int (*real_mkdirat_t)(int, const char *, mode_t);
typedef int (*real_rmdir_t)(const char *);
typedef int (*real_truncate_t)(const char *, off_t);
typedef int (*real_symlink_t)(const char *, const char *);
typedef int (*real_symlinkat_t)(const char *, int, const char *);
typedef int (*real_link_t)(const char *, const char *);
typedef int (*real_linkat_t)(int, const char *, int, const char *, int);
typedef int (*real_mknod_t)(const char *, mode_t, dev_t);
typedef int (*real_mknodat_t)(int, const char *, mode_t, dev_t);

static real_open_t real_open = NULL;
static real_openat_t real_openat = NULL;
static real_creat_t real_creat = NULL;
static real_rename_t real_rename = NULL;
static real_renameat_t real_renameat = NULL;
static real_unlink_t real_unlink = NULL;
static real_unlinkat_t real_unlinkat = NULL;
static real_mkdir_t real_mkdir = NULL;
static real_mkdirat_t real_mkdirat = NULL;
static real_rmdir_t real_rmdir = NULL;
static real_truncate_t real_truncate = NULL;
static real_symlink_t real_symlink = NULL;
static real_symlinkat_t real_symlinkat = NULL;
static real_link_t real_link = NULL;
static real_linkat_t real_linkat = NULL;
static real_mknod_t real_mknod = NULL;
static real_mknodat_t real_mknodat = NULL;

__attribute__((constructor))
void gmm_overlay_init(void) {
    const char *game = getenv("GMM_GAME_DIR");
    const char *overwrite = getenv("GMM_OVERWRITE_DIR");
    if (!game || !overwrite) return;

    gmm_debug = gmm_debug_enabled();

    gmm_game_dir = strdup(game);
    if (!gmm_game_dir) return;
    gmm_game_dir_len = strlen(gmm_game_dir);
    while (gmm_game_dir_len > 0 && gmm_game_dir[gmm_game_dir_len - 1] == '/')
        gmm_game_dir[--gmm_game_dir_len] = '\0';

    gmm_overwrite_dir = strdup(overwrite);
    if (!gmm_overwrite_dir) {
        free(gmm_game_dir);
        gmm_game_dir = NULL;
        gmm_game_dir_len = 0;
        return;
    }
    gmm_overwrite_dir_len = strlen(gmm_overwrite_dir);
    while (gmm_overwrite_dir_len > 0 && gmm_overwrite_dir[gmm_overwrite_dir_len - 1] == '/')
        gmm_overwrite_dir[--gmm_overwrite_dir_len] = '\0';

    if (gmm_debug) {
        fprintf(stderr, "[GMM] Overlay debug enabled\n");
        fprintf(stderr, "[GMM]   game_dir=%s\n", gmm_game_dir);
        fprintf(stderr, "[GMM]   overwrite_dir=%s\n", gmm_overwrite_dir);
    }
}

static int is_under_game_dir(const char *path) {
    if (!path || !gmm_game_dir) return 0;
    if (strncmp(path, gmm_game_dir, gmm_game_dir_len) != 0) return 0;
    char c = path[gmm_game_dir_len];
    return c == '/' || c == '\0';
}

static char *rewrite_path(const char *path) {
    if (!path || !gmm_game_dir || !is_under_game_dir(path)) return NULL;
    const char *rel = path + gmm_game_dir_len;
    if (*rel == '/') rel++;
    size_t new_len = gmm_overwrite_dir_len + 1 + strlen(rel) + 1;
    char *newpath = (char *)malloc(new_len);
    if (!newpath) return NULL;
    snprintf(newpath, new_len, "%s/%s", gmm_overwrite_dir, rel);
    return newpath;
}

static char *resolve_under_game_dir(const char *path) {
    size_t len = gmm_game_dir_len + 1 + strlen(path) + 1;
    char *full = (char *)malloc(len);
    if (!full) return NULL;
    snprintf(full, len, "%s/%s", gmm_game_dir, path);
    char *result = rewrite_path(full);
    free(full);
    return result;
}

static char *intercept_path(const char *path) {
    if (gmm_recursing || !gmm_game_dir) return NULL;
    return path[0] == '/' ? rewrite_path(path) : resolve_under_game_dir(path);
}

static int ensure_parent_dir(const char *path) {
    if (!real_mkdir) real_mkdir = (real_mkdir_t)dlsym(RTLD_NEXT, "mkdir");
    char *copy = strdup(path);
    if (!copy) return -1;

    char *p = copy + 1;
    while (*p) {
        if (*p == '/') {
            *p = '\0';
            if (real_mkdir(copy, 0755) != 0 && errno != EEXIST) {
                free(copy);
                return -1;
            }
            *p = '/';
        }
        p++;
    }
    if (real_mkdir(copy, 0755) != 0 && errno != EEXIST) {
        free(copy);
        return -1;
    }
    free(copy);
    return 0;
}

int open(const char *path, int flags, ...) {
    if (!real_open) real_open = (real_open_t)dlsym(RTLD_NEXT, "open");

    va_list ap;
    va_start(ap, flags);
    mode_t mode = (flags & O_CREAT) ? (mode_t)va_arg(ap, int) : 0;
    va_end(ap);

    char *rw = intercept_path(path);
    if (rw) {
        int is_write = (flags & (O_CREAT | O_WRONLY | O_RDWR));
        if (is_write) {
            ensure_parent_dir(rw);
            int ret = real_open(rw, flags, mode);
            gmm_log("W %s -> %s (fd=%d)", path, rw, ret);
            free(rw);
            return ret;
        }
        // Read-only: try overwrite first, fall back to original path
        int ret = real_open(rw, flags, mode);
        int saved = errno;
        free(rw);
        if (ret >= 0) {
            gmm_log("R %s -> overwrite (fd=%d)", path, ret);
            return ret;
        }
        // Not in overwrite - fall back to original
        errno = saved != ENOENT ? saved : 0;
        ret = real_open(path, flags, mode);
        gmm_log("R %s -> original (fd=%d)", path, ret);
        return ret;
    }
    return real_open(path, flags, mode);
}

int openat(int dirfd, const char *path, int flags, ...) {
    if (!real_openat) real_openat = (real_openat_t)dlsym(RTLD_NEXT, "openat");
    if (dirfd != AT_FDCWD) {
        va_list ap;
        va_start(ap, flags);
        mode_t mode = (flags & O_CREAT) ? (mode_t)va_arg(ap, int) : 0;
        va_end(ap);
        return real_openat(dirfd, path, flags, mode);
    }

    va_list ap;
    va_start(ap, flags);
    mode_t mode = (flags & O_CREAT) ? (mode_t)va_arg(ap, int) : 0;
    va_end(ap);

    char *rw = intercept_path(path);
    if (rw) {
        int is_write = (flags & (O_CREAT | O_WRONLY | O_RDWR));
        if (is_write) {
            ensure_parent_dir(rw);
            int ret = real_openat(AT_FDCWD, rw, flags, mode);
            gmm_log("W %s -> %s (fd=%d)", path, rw, ret);
            free(rw);
            return ret;
        }
        // Read-only: try overwrite first, fall back to original path
        int ret = real_openat(AT_FDCWD, rw, flags, mode);
        int saved = errno;
        free(rw);
        if (ret >= 0) {
            gmm_log("R %s -> overwrite (fd=%d)", path, ret);
            return ret;
        }
        errno = saved != ENOENT ? saved : 0;
        ret = real_openat(AT_FDCWD, path, flags, mode);
        gmm_log("R %s -> original (fd=%d)", path, ret);
        return ret;
    }
    return real_openat(AT_FDCWD, path, flags, mode);
}

int creat(const char *path, mode_t mode) {
    if (!real_creat) real_creat = (real_creat_t)dlsym(RTLD_NEXT, "creat");
    char *rw = intercept_path(path);
    if (rw) {
        ensure_parent_dir(rw);
        int ret = real_creat(rw, mode);
        gmm_log("creat %s -> %s (fd=%d)", path, rw, ret);
        free(rw);
        return ret;
    }
    return real_creat(path, mode);
}

int rename(const char *oldpath, const char *newpath) {
    if (!real_rename) real_rename = (real_rename_t)dlsym(RTLD_NEXT, "rename");
    char *old_rw = intercept_path(oldpath);
    char *new_rw = intercept_path(newpath);
    if (old_rw || new_rw) {
        if (new_rw) ensure_parent_dir(new_rw);
        int ret = real_rename(old_rw ? old_rw : oldpath, new_rw ? new_rw : newpath);
        gmm_log("rename %s -> %s  (%s -> %s)", oldpath, newpath,
                old_rw ? old_rw : oldpath, new_rw ? new_rw : newpath);
        free(old_rw);
        free(new_rw);
        return ret;
    }
    return real_rename(oldpath, newpath);
}

int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath) {
    if (!real_renameat) real_renameat = (real_renameat_t)dlsym(RTLD_NEXT, "renameat");
    if (olddirfd != AT_FDCWD || newdirfd != AT_FDCWD)
        return real_renameat(olddirfd, oldpath, newdirfd, newpath);
    char *old_rw = intercept_path(oldpath);
    char *new_rw = intercept_path(newpath);
    if (old_rw || new_rw) {
        if (new_rw) ensure_parent_dir(new_rw);
        int ret = real_renameat(AT_FDCWD, old_rw ? old_rw : oldpath,
                                AT_FDCWD, new_rw ? new_rw : newpath);
        gmm_log("renameat %s -> %s", oldpath, newpath);
        free(old_rw);
        free(new_rw);
        return ret;
    }
    return real_renameat(AT_FDCWD, oldpath, AT_FDCWD, newpath);
}

int unlink(const char *path) {
    if (!real_unlink) real_unlink = (real_unlink_t)dlsym(RTLD_NEXT, "unlink");
    char *rw = intercept_path(path);
    if (rw) {
        int ret = real_unlink(rw);
        gmm_log("unlink %s -> %s (ret=%d)", path, rw, ret);
        free(rw);
        return ret;
    }
    return real_unlink(path);
}

int unlinkat(int dirfd, const char *path, int flags) {
    if (!real_unlinkat) real_unlinkat = (real_unlinkat_t)dlsym(RTLD_NEXT, "unlinkat");
    if (dirfd != AT_FDCWD) return real_unlinkat(dirfd, path, flags);
    char *rw = intercept_path(path);
    if (rw) {
        int ret = real_unlinkat(AT_FDCWD, rw, flags);
        gmm_log("unlinkat %s -> %s (ret=%d)", path, rw, ret);
        free(rw);
        return ret;
    }
    return real_unlinkat(AT_FDCWD, path, flags);
}

int mkdir(const char *path, mode_t mode) {
    if (!real_mkdir) real_mkdir = (real_mkdir_t)dlsym(RTLD_NEXT, "mkdir");
    char *rw = intercept_path(path);
    if (rw) {
        ensure_parent_dir(rw);
        int ret = real_mkdir(rw, mode);
        gmm_log("mkdir %s -> %s (ret=%d)", path, rw, ret);
        free(rw);
        return ret;
    }
    return real_mkdir(path, mode);
}

int mkdirat(int dirfd, const char *path, mode_t mode) {
    if (!real_mkdirat) real_mkdirat = (real_mkdirat_t)dlsym(RTLD_NEXT, "mkdirat");
    if (dirfd != AT_FDCWD) return real_mkdirat(dirfd, path, mode);
    char *rw = intercept_path(path);
    if (rw) {
        ensure_parent_dir(rw);
        int ret = real_mkdirat(AT_FDCWD, rw, mode);
        gmm_log("mkdirat %s -> %s (ret=%d)", path, rw, ret);
        free(rw);
        return ret;
    }
    return real_mkdirat(AT_FDCWD, path, mode);
}

int rmdir(const char *path) {
    if (!real_rmdir) real_rmdir = (real_rmdir_t)dlsym(RTLD_NEXT, "rmdir");
    char *rw = intercept_path(path);
    if (rw) {
        int ret = real_rmdir(rw);
        gmm_log("rmdir %s -> %s (ret=%d)", path, rw, ret);
        free(rw);
        return ret;
    }
    return real_rmdir(path);
}

int truncate(const char *path, off_t length) {
    if (!real_truncate) real_truncate = (real_truncate_t)dlsym(RTLD_NEXT, "truncate");
    char *rw = intercept_path(path);
    if (rw) {
        int ret = real_truncate(rw, length);
        gmm_log("truncate %s -> %s (ret=%d)", path, rw, ret);
        free(rw);
        return ret;
    }
    return real_truncate(path, length);
}

int symlink(const char *target, const char *linkpath) {
    if (!real_symlink) real_symlink = (real_symlink_t)dlsym(RTLD_NEXT, "symlink");
    char *rw = intercept_path(linkpath);
    if (rw) {
        ensure_parent_dir(rw);
        int ret = real_symlink(target, rw);
        gmm_log("symlink %s -> %s  (target=%s)", linkpath, rw, target);
        free(rw);
        return ret;
    }
    return real_symlink(target, linkpath);
}

int symlinkat(const char *target, int newdirfd, const char *linkpath) {
    if (!real_symlinkat) real_symlinkat = (real_symlinkat_t)dlsym(RTLD_NEXT, "symlinkat");
    if (newdirfd != AT_FDCWD) return real_symlinkat(target, newdirfd, linkpath);
    char *rw = intercept_path(linkpath);
    if (rw) {
        ensure_parent_dir(rw);
        int ret = real_symlinkat(target, AT_FDCWD, rw);
        gmm_log("symlinkat %s -> %s  (target=%s)", linkpath, rw, target);
        free(rw);
        return ret;
    }
    return real_symlinkat(target, AT_FDCWD, linkpath);
}

int link(const char *oldpath, const char *newpath) {
    if (!real_link) real_link = (real_link_t)dlsym(RTLD_NEXT, "link");
    char *new_rw = intercept_path(newpath);
    if (new_rw) {
        ensure_parent_dir(new_rw);
        int ret = real_link(oldpath, new_rw);
        gmm_log("link %s -> %s  (target=%s)", newpath, new_rw, oldpath);
        free(new_rw);
        return ret;
    }
    return real_link(oldpath, newpath);
}

int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags) {
    if (!real_linkat) real_linkat = (real_linkat_t)dlsym(RTLD_NEXT, "linkat");
    if (olddirfd != AT_FDCWD || newdirfd != AT_FDCWD)
        return real_linkat(olddirfd, oldpath, newdirfd, newpath, flags);
    char *new_rw = intercept_path(newpath);
    if (new_rw) {
        ensure_parent_dir(new_rw);
        int ret = real_linkat(AT_FDCWD, oldpath, AT_FDCWD, new_rw, flags);
        gmm_log("linkat %s -> %s  (target=%s)", newpath, new_rw, oldpath);
        free(new_rw);
        return ret;
    }
    return real_linkat(AT_FDCWD, oldpath, AT_FDCWD, newpath, flags);
}

int mknod(const char *path, mode_t mode, dev_t dev) {
    if (!real_mknod) real_mknod = (real_mknod_t)dlsym(RTLD_NEXT, "mknod");
    char *rw = intercept_path(path);
    if (rw) {
        ensure_parent_dir(rw);
        int ret = real_mknod(rw, mode, dev);
        gmm_log("mknod %s -> %s (ret=%d)", path, rw, ret);
        free(rw);
        return ret;
    }
    return real_mknod(path, mode, dev);
}

int mknodat(int dirfd, const char *path, mode_t mode, dev_t dev) {
    if (!real_mknodat) real_mknodat = (real_mknodat_t)dlsym(RTLD_NEXT, "mknodat");
    if (dirfd != AT_FDCWD) return real_mknodat(dirfd, path, mode, dev);
    char *rw = intercept_path(path);
    if (rw) {
        ensure_parent_dir(rw);
        int ret = real_mknodat(AT_FDCWD, rw, mode, dev);
        gmm_log("mknodat %s -> %s (ret=%d)", path, rw, ret);
        free(rw);
        return ret;
    }
    return real_mknodat(AT_FDCWD, path, mode, dev);
}
