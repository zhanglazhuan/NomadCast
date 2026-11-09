"""Compile the real SD guard against mock newlib/VFS calls; no ESP hardware needed.
Run: python tests/usb_storage/test_sd_access_guard.py (requires gcc).
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
GUARD = ROOT / "system/usb_storage/sd_access_guard.c"

with tempfile.TemporaryDirectory(prefix="nomadcast-sd-guard-") as temp:
    work = Path(temp)
    headers = {
        "esp_err.h": "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_NO_MEM 1\n",
        "sys/reent.h": "#pragma once\nstruct _reent { int _errno; };\n",
        "freertos/FreeRTOS.h": "#pragma once\n#define portMAX_DELAY 0xffffffffu\n#ifndef FD_SETSIZE\n#define FD_SETSIZE 64\n#endif\n",
        "freertos/semphr.h": "#pragma once\ntypedef void *SemaphoreHandle_t;\nstatic inline void *xSemaphoreCreateRecursiveMutex(void) { return (void *)1; }\nstatic inline int xSemaphoreTakeRecursive(void *m, unsigned t) { (void)m; (void)t; return 1; }\nstatic inline int xSemaphoreGiveRecursive(void *m) { (void)m; return 1; }\n",
    }
    for name, content in headers.items():
        file = work / name
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(content)
    # Include the production source so tests can exercise every exported wrapper.
    source = '#include "' + GUARD.as_posix() + '"\n'
    source += r"""
#include <assert.h>
#include <stdint.h>
static int next_fd = 5, close_error, path_calls, closed_files;
static uintptr_t next_dir = 1;
int __real__open_r(struct _reent *r, const char *p, int f, int m) {
    (void)r; (void)p; (void)f; (void)m; return next_fd;
}
int __real__close_r(struct _reent *r, int fd) {
    (void)r; (void)fd; ++closed_files; return close_error ? -1 : 0;
}
DIR *__real_opendir(const char *p) { (void)p; return (DIR *)next_dir++; }
int __real_closedir(DIR *d) { (void)d; return 0; }
"""
    # Stub path calls from the production wrapper signatures.
    import re
    for name, args in re.findall(r"int __real_(_stat_r|_unlink_r|_rename_r|_link_r|mkdir|rmdir|access|truncate|utime)\(([^;]+)\);", GUARD.read_text()):
        source += f"int __real_{name}({args}) {{ ++path_calls; return 0; }}\n"
    source += r"""
int main(void) {
    struct _reent r = {0};
    assert(sd_access_guard_init() == ESP_OK);
    assert(sd_access_guard_idle());
    assert(__wrap__open_r(&r, "/sdcard/test", 0, 0) == 5);
    DIR *dir = __wrap_opendir("/sdcard");
    assert(dir && !sd_access_guard_idle());
    sd_access_guard_block(true);
    assert(__wrap__open_r(&r, "/sdcard/new", 0, 0) == -1 && r._errno == EBUSY);
    assert(__wrap_opendir("/sdcard") == NULL && errno == EBUSY);
    assert(__wrap__unlink_r(&r, "/sdcard/test") == -1 && path_calls == 0);
    assert(__wrap__rename_r(&r, "/other/test", "/sdcard/test") == -1);
    assert(__wrap_mkdir("/sdcard/a", 0755) == -1 && errno == EBUSY);
    assert(__wrap__stat_r(&r, "/sdcard", NULL) == -1);
    assert(__wrap_rmdir("/sdcard/a") == -1);
    assert(__wrap_access("/sdcard/a", 0) == -1);
    assert(__wrap_truncate("/sdcard/a", 0) == -1);
    assert(__wrap_utime("/sdcard/a", NULL) == -1);
    assert(__wrap__link_r(&r, "/sdcard/a", "/other/a") == -1);
    // A failed close cannot make an outstanding SD handle disappear.
    close_error = 1;
    assert(__wrap__close_r(&r, 5) == -1 && !sd_access_guard_idle());
    close_error = 0;
    assert(__wrap__close_r(&r, 5) == 0 && !sd_access_guard_idle());
    assert(__wrap_closedir(dir) == 0 && sd_access_guard_idle());
    // Prefix boundaries and other filesystems are unaffected.
    assert(__wrap__open_r(&r, "/sdcard-other/test", 0, 0) == 5);
    assert(sd_access_guard_idle());
    assert(__wrap__close_r(&r, 5) == 0 && sd_access_guard_idle());
    assert(__wrap_mkdir("/other/a", 0755) == 0 && path_calls == 1);
    sd_access_guard_block(false);
    assert(__wrap__open_r(&r, "/sdcard", 0, 0) == 5);
    assert(__wrap__close_r(&r, 5) == 0 && sd_access_guard_idle());
    // Directory-table exhaustion closes the rejected handle; no leak.
    DIR *dirs[32];
    for (unsigned i = 0; i < 32; ++i) dirs[i] = __wrap_opendir("/sdcard");
    assert(__wrap_opendir("/sdcard") == NULL && errno == EMFILE);
    for (unsigned i = 0; i < 32; ++i) assert(__wrap_closedir(dirs[i]) == 0);
    assert(sd_access_guard_idle());
    next_fd = FD_SETSIZE + 1;
    int before = closed_files;
    assert(__wrap__open_r(&r, "/sdcard/file", 0, 0) == -1 && r._errno == EMFILE);
    assert(closed_files == before + 1 && sd_access_guard_idle());
    puts("SD guard tests passed: handles, drain, failed close, path blocking, isolation, exhaustion");
    return 0;
}
"""
    test = work / "test.c"
    test.write_text(source)
    binary = work / ("test.exe" if os.name == "nt" else "test")
    compiler = shutil.which("gcc")
    if not compiler:
        raise SystemExit("gcc is required")
    subprocess.run([compiler, "-std=c11", "-Werror", "-I", str(work), str(test), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
