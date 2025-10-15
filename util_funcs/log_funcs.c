#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#define LOGFILE "log_file.log"

int log_append_logfile(const char *add_string, ...) {
    va_list args;
    va_start(args, add_string);
    FILE *fp = NULL;
    fp = fopen(LOGFILE, "a");
    if (!fp)
        fp = fopen(LOGFILE, "w");
    if (!fp)
        return -1;
    int rc = vfprintf(fp, add_string, args);
    va_end(args);
    fclose(fp);
    return rc;
}

char *log_getline_logfile(int line_num) {
    FILE *fp = NULL;
    fp = fopen(LOGFILE, "r");
    if (!fp)
        return NULL;
    char *return_line = NULL;
    unsigned int curr_line = 0;
    unsigned int line_size = 0;
    unsigned int found = 0;
    char c;
    while (!feof(fp)) {
        c = fgetc(fp);
        if (curr_line == line_num) {
            line_size += 1;
            if (!return_line) {
                return_line = (char *)malloc(sizeof(char));
                if (!return_line)
                    return NULL;
            } else {
                return_line =
                    (char *)realloc(return_line, sizeof(char) * line_size);
                if (!return_line)
                    return NULL;
            }
            if (c == '\n') {
                return_line[line_size - 1] = '\0';
                found = 1;
                break;
            }
            if (found == 1)
                break;
            return_line[line_size - 1] = c;
        } else {
            if (c == '\n') {
                curr_line += 1;
            }
        }
        if (found == 1)
            break;
    }
    fclose(fp);
    return return_line;
}

unsigned int log_calclines_logfile() {
    FILE *fp = NULL;
    fp = fopen(LOGFILE, "r");
    if (!fp)
        return 0;
    char *return_line = NULL;
    unsigned int curr_line = 0;
    char c;
    while (!feof(fp)) {
        c = fgetc(fp);
        if (c == '\n') {
            curr_line += 1;
        }
    }
    fclose(fp);
    return curr_line;
}

int log_clear_logfile() {
    FILE *fp = NULL;
    fp = fopen(LOGFILE, "w");
    if (!fp)
        return -1;
    fclose(fp);
    return 0;
}
