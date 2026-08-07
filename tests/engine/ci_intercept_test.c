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

int main(void) {
    /* ---- helper mode: exec'd by the parent WITH the shim LD_PRELOADed ---- */
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

    /* parent (no shim) must still fail ENOENT: proves fs is case-sensitive. */
    struct stat sb;
    int parent_failed = (stat(lower_nested, &sb) != 0 && errno == ENOENT);

    unlink(deployed_upper);
    rmdir(nested_upper);
    rmdir(meshes);
    rmdir(base);

    int child_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (child_ok && parent_failed) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    fprintf(stderr, "child_ok=%d parent_failed=%d\n", child_ok, parent_failed);
    if (!parent_failed)
        fprintf(stderr, "FAIL: parent stat'ed lowercase WITHOUT shim (fs is CI?)\n");
    return 1;
}