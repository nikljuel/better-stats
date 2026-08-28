#ifndef FILE_HANDLER_CONFIG_H
#define FILE_HANDLER_CONFIG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int ok;
    int changed;
    int handler_present;
    int handler_first;
    int koreader_present;
    char stock_handler[128];
    char *output;
    size_t output_size;
    char error[128];
} handler_config_result;

int patch_handler_config(const char *input, size_t input_size,
                         const char *format, const char *handler_name,
                         int enable, handler_config_result *out);
void free_handler_config(handler_config_result *result);

#ifdef __cplusplus
}
#endif

#endif
