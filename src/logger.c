#include "logger.h"
void log_message(LogLevel level, const char* format, ...) {
    if (level < LOG_LEVEL) { 
        return; 
    }

    FILE* log_file = fopen(LOG_FILE, "a");
    if (!log_file) {
        perror("Error opening log file");
        return;
    }

    // Получение времени в удобной строковой форме
    time_t rawtime = time(NULL);
    struct tm* timeinfo = localtime(&rawtime);
    char time_str[50];
    strftime(time_str, sizeof(time_str), "[%Y-%m-%d %H:%M:%S]", timeinfo);

    // Соответствие уровней логирования текстовым представлениям
    static const char* log_levels[] = {
        [LOG_DEBUG] = "[DEBUG] ",
        [LOG_INFO] = "[INFO] ",
        [LOG_WARNING] = "[WARNING] ",
        [LOG_ERROR] = "[ERROR] "
    };

    // Логирование сообщения
    va_list args;
    va_start(args, format);
    fprintf(log_file, "%s %s", time_str, log_levels[level]);
    vfprintf(log_file, format, args);
    va_end(args);

    fclose(log_file);
}
