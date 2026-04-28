#ifndef INPUT_SYSTEM_INPUT_TEST_H
#define INPUT_SYSTEM_INPUT_TEST_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

esp_err_t input_test_init(void);
int input_test_prompt_line(const char *prompt, char *buffer, size_t buffer_size, bool allow_empty);
int input_test_prompt_line_mode(const char *prompt,
                                char *buffer,
                                size_t buffer_size,
                                bool allow_empty,
                                bool accept_immediate_newline);

#endif
