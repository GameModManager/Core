#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *expected_syms[] = {
    "open", "openat", "creat",
    "rename", "renameat",
    "unlink", "unlinkat",
    "mkdir", "mkdirat", "rmdir",
    "truncate",
    "symlink", "symlinkat",
    "link", "linkat",
    "mknod", "mknodat",
    NULL
};

int main(void) {
    setenv("GMM_GAME_DIR", "/tmp/gmm_test_game", 1);
    setenv("GMM_OVERWRITE_DIR", "/tmp/gmm_test_overwrite", 1);

    void *so = dlopen(GMM_INTERCEPT_SO, RTLD_NOW);
    if (!so) {
        fprintf(stderr, "FAIL: dlopen failed: %s\n", dlerror());
        return 1;
    }

    int failures = 0;
    for (int i = 0; expected_syms[i]; i++) {
        void *sym = dlsym(so, expected_syms[i]);
        if (!sym) {
            fprintf(stderr, "FAIL: missing symbol '%s': %s\n",
                    expected_syms[i], dlerror());
            failures++;
        }
    }

    dlclose(so);

    if (failures > 0) {
        fprintf(stderr, "FAIL: %d symbol(s) missing\n", failures);
        return 1;
    }

    printf("PASS: all %d symbols resolved\n", 17);
    return 0;
}
