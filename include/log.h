#ifndef LOG_H
#define LOG_H

void log_init(const char *path);
void log_close(void);

void log_info(const char *fmt, ...);
void log_error(const char *fmt, ...);

#endif