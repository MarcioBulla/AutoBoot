#include "input_system/input_test.h"

#include <stdio.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/unistd.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"

static const char *TAG = "input_test";

#define CONSOLE_UART      UART_NUM_0
#define CONSOLE_BAUD_RATE 115200
#define RX_BUFFER_SIZE    2048

static int s_console_fd = -1;
static bool s_console_ready = false;

static void console_write_bytes(const char *data, size_t len)
{
    if (len == 0 || s_console_fd < 0) {
        return;
    }

    ssize_t written = write(s_console_fd, data, len);
    if (written < 0) {
        ESP_LOGW(TAG, "Write error: errno %d (%s)", errno, strerror(errno));
    }
}

static void console_write_char(char c)
{
    console_write_bytes(&c, 1);
}

static esp_err_t console_uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = CONSOLE_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(CONSOLE_UART, RX_BUFFER_SIZE, 0, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_param_config(CONSOLE_UART, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART param config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    uart_vfs_dev_use_driver(CONSOLE_UART);

    s_console_fd = open("/dev/uart/0", O_RDWR);
    if (s_console_fd < 0) {
        ESP_LOGE(TAG, "Cannot open /dev/uart/0: errno %d (%s)", errno, strerror(errno));
        return ESP_FAIL;
    }

    s_console_ready = true;
    return ESP_OK;
}

static int read_line(char *buffer, size_t buffer_size, bool accept_immediate_newline)
{
    size_t index = 0;

    while (index < buffer_size - 1) {
        char c;
        int len = read(s_console_fd, &c, 1);

        if (len < 0) {
            ESP_LOGE(TAG, "Read error: errno %d (%s)", errno, strerror(errno));
            return -1;
        }

        if (len == 0) {
            continue;
        }

        if (c == '\r' || c == '\n') {
            if (index == 0 && !accept_immediate_newline) {
                continue;
            }

            console_write_bytes("\r\n", 2);
            break;
        }

        if (c == '\b' || c == 0x7f) {
            if (index > 0) {
                index--;
                console_write_bytes("\b \b", 3);
            }
            continue;
        }

        if (c < 32 || c > 126) {
            continue;
        }

        buffer[index++] = c;
        console_write_char(c);
    }

    buffer[index] = '\0';
    return (int)index;
}

esp_err_t input_test_init(void)
{
    if (s_console_ready) {
        return ESP_OK;
    }

    return console_uart_init();
}

int input_test_prompt_line_mode(const char *prompt,
                                char *buffer,
                                size_t buffer_size,
                                bool allow_empty,
                                bool accept_immediate_newline)
{
    if (prompt == NULL || buffer == NULL || buffer_size < 2) {
        return -1;
    }

    if (!s_console_ready) {
        return -1;
    }

    while (1) {
        printf("%s", prompt);
        fflush(stdout);

        int length = read_line(buffer, buffer_size, accept_immediate_newline);
        if (length < 0) {
            return -1;
        }

        if (allow_empty || length > 0) {
            return length;
        }

        ESP_LOGW(TAG, "Empty input is not allowed");
    }
}

int input_test_prompt_line(const char *prompt, char *buffer, size_t buffer_size, bool allow_empty)
{
    return input_test_prompt_line_mode(prompt, buffer, buffer_size, allow_empty, false);
}
