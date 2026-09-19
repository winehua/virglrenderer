/*
 * winehua_gl_caps_log.h — P0-GL-2/3: VirGL Host / capset 能力探测日志 (2026-09-18)
 *
 * 目的: 把 "Host App EGL / VirGL Host EGL / VirGL capset" 三层的真实能力落到同一个
 * 文件里 (temp/gl-capability.log), 用来回答:
 *   为什么 app 自己按 GLES3 建 context, 而经 VirGL 暴露给 guest 的却只有 GLES2?
 *
 * 只诊断, 不改行为: 原样返回, 不参与任何分支判断。
 * 优先写文件 —— vtest server 会被 fork/重定向, stderr 不保证留得下来。
 */
#ifndef WINEHUA_GL_CAPS_LOG_H
#define WINEHUA_GL_CAPS_LOG_H

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static inline void winehua_gl_caps_log(const char *fmt, ...)
{
    char buf[1024];
    const char *path = getenv("WINEHUA_GL_CAP_LOG");
    va_list ap;
    int n;
    int fd;

    if (!path || !path[0])
        path = "/data/storage/el2/base/temp/gl-capability.log";

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(buf) - 2)
        n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    buf[n] = 0;

    (void)!write(2, buf, (size_t)n);
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        (void)!write(fd, buf, (size_t)n);
        close(fd);
    }
}

#endif /* WINEHUA_GL_CAPS_LOG_H */
