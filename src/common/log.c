#include "log.h"

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>

static FILE *g_log_fp = NULL;

/* 内部函数：把一行日志同时写到终端和文件 */
static void log_write(const char *level, const char *fmt, va_list ap)
{
    char msg[1024];
    char line[1200];
    time_t now;
    struct tm *tm_info;

    /* 1. 把用户格式化的内容写进 msg */
    vsnprintf(msg, sizeof(msg), fmt, ap);

    /* 2. 取当前时间 */
    now = time(NULL);
    tm_info = localtime(&now);

    /* 3. 拼成一行：[时间] [级别] 内容 */
    snprintf(line, sizeof(line),
             "[%04d-%02d-%02d %02d:%02d:%02d] [%s] %s\n",
             tm_info->tm_year + 1900,
             tm_info->tm_mon + 1,
             tm_info->tm_mday,
             tm_info->tm_hour,
             tm_info->tm_min,
             tm_info->tm_sec,
             level,
             msg);

    /* 4. 输出到终端 */
    fputs(line, stdout);
    fflush(stdout);

    /* 5. 输出到文件 */
    if (g_log_fp) {
        fputs(line, g_log_fp);
        fflush(g_log_fp);
    }
}

/* 打开日志文件 */
void log_init(const char *path)
{
    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
    g_log_fp = fopen(path, "a");
    if (!g_log_fp) {
        fprintf(stderr, "log_init: cannot open %s\n", path);
    }
}

/* 关闭日志文件 */
void log_close(void)
{
    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
}

/* 普通信息 */
void log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_write("INFO", fmt, ap);
    va_end(ap);
}

/* 错误信息 */
void log_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_write("ERROR", fmt, ap);
    va_end(ap);
}