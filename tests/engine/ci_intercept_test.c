#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// Functional test for libgmm_ci_intercept.so: the interposer must
// case-insensitively resolve a READ-side lookup that fails ENOENT on a
// case-sensitive fs. We build a tree with an uppercase deployment
// ("0_Master.hxk") and exec OUR OWN BINARY AS A HELPER with LD_PRELOAD + CI
// env; the helper (run with the shim loaded) stats/opens a lowercase query
// ("0_master.hxk") that does not exist on disk. The shim must make both
// succeed. The parent process (no shim), running the same stat()/open() on
// the same lowercase path, must still see ENOENT - proving the fs is
// case-sensitive and the shim (not the fs) is what resolved it.

static void write_file(const char *p, const char *content) {
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("create"); exit(2); }
    if (write(fd, content, strlen(content)) < 0) { perror("write"); exit(2); }
    close(fd);
}

static int run_checks_read(const char *lower_path) {
    struct stat sb;
    if (stat(lower_path, &sb) != 0) {
        fprintf(stderr, "FAIL: stat('%s') not resolved: %s\n",
                lower_path, strerror(errno));
        return 1;
    }
    int fd = open(lower_path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "FAIL: open('%s') not resolved: %s\n",
                lower_path, strerror(errno));
        return 1;
    }
    char buf[16] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n != 1 || buf[0] != 'X') {
        fprintf(stderr, "FAIL: content mismatch (%zd, '%s')\n", n, buf);
        return 1;
    }
    printf("PASS: CI-resolved stat + open '%s'\n", lower_path);
    return 0;
}

/* Mode-regression helper: create a file with open(O_CREAT, 0644) THROUGH the
 * interposer and exit. The parent stats the result. Regression for the v0.3.2
 * bug where the interposed open() dropped the variadic mode argument, so every
 * game-created file landed mode 0000 (the game then could not re-read its own
 * config and crashed on the next launch). */
static int run_checks_create_mode(void) {
    const char *path = getenv("GMM_CI_CREATE");
    if (!path) return 1;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "FAIL: create('%s') failed: %s\n", path, strerror(errno));
        return 1;
    }
    if (write(fd, "Y", 1) != 1) { perror("write"); close(fd); return 1; }
    close(fd);
    return 0;
}

/* Wine-prefix helper: the path passed in GMM_CI_DOSDEVICES carries a wine
 * `<prefix>/dosdevices/z:` prefix (a symlink to unix root `/`) that the kernel
 * resolves transparently but our string comparison must strip before checking
 * against GMM_CI_ROOT. Regression for the v0.3.x bug where under_root()
 * rejected every wine path, so the shim was silently inert in-game. */
static int run_checks_dosdevices(void) {
    const char *path = getenv("GMM_CI_DOSDEVICES");
    if (!path) return 1;
    struct stat sb;
    if (stat(path, &sb) != 0) {
        fprintf(stderr, "FAIL: stat('%s') not resolved: %s\n",
                path, strerror(errno));
        return 1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "FAIL: open('%s') not resolved: %s\n",
                path, strerror(errno));
        return 1;
    }
    close(fd);
    printf("PASS: CI-resolved dosdevices/z:-prefixed '%s'\n", path);
    return 0;
}

int main(void) {
    /* ---- helper mode: exec'd by the parent WITH the shim LD_PRELOADed ---- */
    if (getenv("GMM_CI_CREATE")) {
        return run_checks_create_mode();
    }
    const char *dosdevices_for_helper = getenv("GMM_CI_DOSDEVICES");
    if (dosdevices_for_helper) {
        return run_checks_dosdevices();
    }
    const char *lower_for_helper = getenv("GMM_CI_LOWER");
    if (lower_for_helper) {
        return run_checks_read(lower_for_helper);
    }

/* ---- parent mode ---- */
    const char *tmp = getenv("TMPDIR");
    if (!tmp) tmp = "/tmp";
    char base[4096];
    snprintf(base, sizeof(base), "%s/gmm_ci_%d", tmp, (int)getpid());
    if (mkdir(base, 0755) != 0) { perror("mkdir base"); return 2; }

    /* Nested-tree shape (the SkyParkourNG case): an UPPERCASE dir + UPPERCASE
     * file deployed; the game queries a lowercase dir + lowercase file. */
    char meshes[4096];
    snprintf(meshes, sizeof meshes, "%s/Meshes", base);
    if (mkdir(meshes, 0755) != 0) { perror("mkdir meshes"); return 2; }
    char nested_upper[4096];
    snprintf(nested_upper, sizeof nested_upper, "%s/Meshes/Behaviors", base);
    if (mkdir(nested_upper, 0755) != 0) { perror("mkdir nested"); return 2; }
    char deployed_upper[4096];
    snprintf(deployed_upper, sizeof deployed_upper, "%s/Meshes/Behaviors/0_Master.hxk", base);
    write_file(deployed_upper, "X");

    char lower_nested[4096];
    snprintf(lower_nested, sizeof lower_nested, "%s/meshes/behaviors/0_master.hxk", base);

    /* Wine dosdevices tree: a fake prefix with `z:` symlinked to unix root `/`
     * (exactly what wine sets up). The game's path reaches libc as
     * `<prefix>/dosdevices/z:/home/.../Data/...`; the kernel resolves the
     * symlink, but our string check must too. We build the symlink pointing
     * at `/` and query `<base>/dosdevices/z:<base>/meshes/behaviors/0_master.hxk`.
     * The kernel follows `z:` -> `/`, so the raw path is `<base>/...` and the
     * rest of the query is the lowercase nested path -> ENOENT without the
     * shim; with the shim it must resolve through the prefix strip. */
    char dd[4096];
    snprintf(dd, sizeof dd, "%s/dosdevices", base);
    if (mkdir(dd, 0755) != 0) { perror("mkdir dosdevices"); return 2; }
    char dd_z[4096];
    snprintf(dd_z, sizeof dd_z, "%s/z:", dd);
    if (symlink("/", dd_z) != 0) { perror("symlink z:"); return 2; }
    char dd_query[4096];
    snprintf(dd_query, sizeof dd_query, "%s/dosdevices/z:%s/meshes/behaviors/0_master.hxk",
             base, base);

    /* Mode-regression target: created through the shim, must NOT be 0000. */
    char create_target[4096];
    snprintf(create_target, sizeof create_target, "%s/SettingsUser.json", base);

    /* Re-exec THIS binary as the helper, with the shim loaded + CI env.
     * LD_PRELOAD only takes effect on exec, so a fork() alone would not load
     * the .so - hence the self re-exec. */
    char self[4096];
    ssize_t selflen = readlink("/proc/self/exe", self, sizeof self - 1);
    if (selflen < 0 || selflen >= (ssize_t)sizeof self) {
        perror("readlink /proc/self/exe");
        return 2;
    }
    self[selflen] = '\0';

    setenv("LD_PRELOAD", GMM_CI_SO, 1);
    setenv("GMM_CI_ENABLED", "1", 1);
    setenv("GMM_CI_DEBUG", "1", 1);
    setenv("GMM_CI_ROOT", base, 1);
    setenv("GMM_CI_LOWER", lower_nested, 1);   /* nested dir + filename */

    pid_t pid = fork();
    if (pid == 0) {
        execv(self, NULL);
        perror("execv");
        _exit(2);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) { perror("waitpid"); return 2; }

    /* Dosdevices-prefix child: same tree, queried through a wine z: symlink. */
    setenv("GMM_CI_DOSDEVICES", dd_query, 1);
    unsetenv("GMM_CI_LOWER");
    pid_t pid3 = fork();
    if (pid3 == 0) {
        execv(self, NULL);
        perror("execv");
        _exit(2);
    }
    int status3;
    if (waitpid(pid3, &status3, 0) < 0) { perror("waitpid"); return 2; }

    /* Mode-regression child: create via interposer, then verify mode != 0. */
    setenv("GMM_CI_CREATE", create_target, 1);
    unsetenv("GMM_CI_LOWER");
    unsetenv("GMM_CI_DOSDEVICES");
    pid_t pid2 = fork();
    if (pid2 == 0) {
        execv(self, NULL);
        perror("execv");
        _exit(2);
    }
    int status2;
    if (waitpid(pid2, &status2, 0) < 0) { perror("waitpid"); return 2; }

    struct stat csb;
    int mode_ok = 0;
    if (stat(create_target, &csb) == 0) {
        mode_t m = csb.st_mode & 0777;
        mode_ok = (m != 0);
        printf("mode_ok=%d mode=%o\n", mode_ok, m);
    } else {
        perror("stat create_target");
    }

    /* parent (no shim) must still fail ENOENT: proves fs is case-sensitive. */
    struct stat sb;
    int parent_failed = (stat(lower_nested, &sb) != 0 && errno == ENOENT);
    /* same for the dosdevices-prefixed query: raw path is <base>/meshes/... */
    int dd_parent_failed = (stat(dd_query, &sb) != 0 && errno == ENOENT);

    unlink(create_target);
    unlink(deployed_upper);
    unlink(dd_z);
    unlink(dd_query);
    rmdir(nested_upper);
    rmdir(meshes);
    rmdir(dd);
    rmdir(base);

    int child_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    int child2_ok = WIFEXITED(status2) && WEXITSTATUS(status2) == 0;
    int child3_ok = WIFEXITED(status3) && WEXITSTATUS(status3) == 0;
    if (child_ok && child2_ok && child3_ok && parent_failed && dd_parent_failed && mode_ok) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    fprintf(stderr, "child_ok=%d child2_ok=%d child3_ok=%d parent_failed=%d dd_parent_failed=%d mode_ok=%d\n",
            child_ok, child2_ok, child3_ok, parent_failed, dd_parent_failed, mode_ok);
    if (!parent_failed)
        fprintf(stderr, "FAIL: parent stat'ed lowercase WITHOUT shim (fs is CI?)\n");
    if (!dd_parent_failed)
        fprintf(stderr, "FAIL: parent stat'ed dosdevices query WITHOUT shim (fs is CI?)\n");
    return 1;
}