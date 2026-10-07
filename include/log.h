#ifndef LOG_H
#define LOG_H

void log_init(const char *path);
void log_close(void);
/* 终端案情板会清屏；client 应关闭控制台回显，只写文件 */
void log_set_console(int enable);

void log_info(const char *fmt, ...);
void log_error(const char *fmt, ...);

#endif