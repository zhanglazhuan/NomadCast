/* Track /sdcard handles at the newlib/VFS boundary. Block new path operations
 * during ownership transfer; existing files can drain and close normally.
 * Other mounted filesystems and console/socket descriptors remain available. */
#include "sd_access_guard.h"
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/reent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <utime.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static struct {
    SemaphoreHandle_t lock;
    bool blocked;
    bool fds[FD_SETSIZE];
    DIR *dirs[32];
    unsigned handles;
} s_ctx;

static bool is_sd(const char *path)
{
    return path && strncmp(path, "/sdcard", 7) == 0 &&
           (path[7] == '/' || path[7] == '\0');
}
static void lock(void) { if (s_ctx.lock) xSemaphoreTakeRecursive(s_ctx.lock, portMAX_DELAY); }
static void unlock(void) { if (s_ctx.lock) xSemaphoreGiveRecursive(s_ctx.lock); }
esp_err_t sd_access_guard_init(void)
{
    if (!s_ctx.lock) s_ctx.lock = xSemaphoreCreateRecursiveMutex();
    return s_ctx.lock ? ESP_OK : ESP_ERR_NO_MEM;
}
void sd_access_guard_block(bool blocked) { lock(); s_ctx.blocked = blocked; unlock(); }
bool sd_access_guard_idle(void) { lock(); bool idle = s_ctx.handles == 0; unlock(); return idle; }

int __real__open_r(struct _reent *, const char *, int, int);
int __real__close_r(struct _reent *, int);
int __wrap__open_r(struct _reent *r, const char *path, int flags, int mode)
{
    lock();
    bool sd = is_sd(path);
    if (sd && s_ctx.blocked) { r->_errno = EBUSY; unlock(); return -1; }
    int fd = __real__open_r(r, path, flags, mode);
    if (fd >= 0 && sd) {
        if (fd >= FD_SETSIZE) {
            __real__close_r(r, fd); r->_errno = EMFILE; fd = -1;
        } else { s_ctx.fds[fd] = true; ++s_ctx.handles; }
    }
    unlock();
    return fd;
}
int __wrap__close_r(struct _reent *r, int fd)
{
    lock();
    int rc = __real__close_r(r, fd);
    if (rc == 0 && fd >= 0 && fd < FD_SETSIZE && s_ctx.fds[fd]) {
        s_ctx.fds[fd] = false; --s_ctx.handles;
    }
    unlock();
    return rc;
}
DIR *__real_opendir(const char *);
int __real_closedir(DIR *);
DIR *__wrap_opendir(const char *path)
{
    lock();
    bool sd = is_sd(path);
    if (sd && s_ctx.blocked) { errno = EBUSY; unlock(); return NULL; }
    DIR *dir = __real_opendir(path);
    if (dir && sd) {
        unsigned i;
        for (i = 0; i < 32 && s_ctx.dirs[i]; ++i) {}
        if (i == 32) { __real_closedir(dir); dir = NULL; errno = EMFILE; }
        else { s_ctx.dirs[i] = dir; ++s_ctx.handles; }
    }
    unlock();
    return dir;
}
int __wrap_closedir(DIR *dir)
{
    lock();
    int rc = __real_closedir(dir);
    if (rc == 0) for (unsigned i = 0; i < 32; ++i) {
        if (s_ctx.dirs[i] == dir) { s_ctx.dirs[i] = NULL; --s_ctx.handles; break; }
    }
    unlock();
    return rc;
}

int __real__stat_r(struct _reent *r, const char *path, struct stat *st);
int __wrap__stat_r(struct _reent *r, const char *path, struct stat *st)
{
    lock();
    if (s_ctx.blocked && (is_sd(path))) { r->_errno = EBUSY; unlock(); return -1; }
    int rc = __real__stat_r(r, path, st);
    unlock();
    return rc;
}

int __real__unlink_r(struct _reent *r, const char *path);
int __wrap__unlink_r(struct _reent *r, const char *path)
{
    lock();
    if (s_ctx.blocked && (is_sd(path))) { r->_errno = EBUSY; unlock(); return -1; }
    int rc = __real__unlink_r(r, path);
    unlock();
    return rc;
}

int __real__rename_r(struct _reent *r, const char *src, const char *dst);
int __wrap__rename_r(struct _reent *r, const char *src, const char *dst)
{
    lock();
    if (s_ctx.blocked && (is_sd(src) || is_sd(dst))) { r->_errno = EBUSY; unlock(); return -1; }
    int rc = __real__rename_r(r, src, dst);
    unlock();
    return rc;
}

int __real__link_r(struct _reent *r, const char *src, const char *dst);
int __wrap__link_r(struct _reent *r, const char *src, const char *dst)
{
    lock();
    if (s_ctx.blocked && (is_sd(src) || is_sd(dst))) { r->_errno = EBUSY; unlock(); return -1; }
    int rc = __real__link_r(r, src, dst);
    unlock();
    return rc;
}

int __real_mkdir(const char *path, mode_t mode);
int __wrap_mkdir(const char *path, mode_t mode)
{
    lock();
    if (s_ctx.blocked && (is_sd(path))) { errno = EBUSY; unlock(); return -1; }
    int rc = __real_mkdir(path, mode);
    unlock();
    return rc;
}

int __real_rmdir(const char *path);
int __wrap_rmdir(const char *path)
{
    lock();
    if (s_ctx.blocked && (is_sd(path))) { errno = EBUSY; unlock(); return -1; }
    int rc = __real_rmdir(path);
    unlock();
    return rc;
}

int __real_access(const char *path, int mode);
int __wrap_access(const char *path, int mode)
{
    lock();
    if (s_ctx.blocked && (is_sd(path))) { errno = EBUSY; unlock(); return -1; }
    int rc = __real_access(path, mode);
    unlock();
    return rc;
}

int __real_truncate(const char *path, off_t length);
int __wrap_truncate(const char *path, off_t length)
{
    lock();
    if (s_ctx.blocked && (is_sd(path))) { errno = EBUSY; unlock(); return -1; }
    int rc = __real_truncate(path, length);
    unlock();
    return rc;
}

int __real_utime(const char *path, const struct utimbuf *times);
int __wrap_utime(const char *path, const struct utimbuf *times)
{
    lock();
    if (s_ctx.blocked && (is_sd(path))) { errno = EBUSY; unlock(); return -1; }
    int rc = __real_utime(path, times);
    unlock();
    return rc;
}
