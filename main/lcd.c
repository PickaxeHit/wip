//
// Created by pickaxehit on 2023/7/3.
//

#include "lcd.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

char *TAG = "lcd";

static esp_lcd_i80_bus_handle_t i80_handle = NULL;
static esp_lcd_i80_bus_config_t bus_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num = DC,
        .wr_gpio_num = PCLK,
        .data_gpio_nums =
                {
                        D0,
                        D1,
                        D2,
                        D3,
                        D4,
                        D5,
                        D6,
                        D7,
                },
        .bus_width = 8,
        .max_transfer_bytes = H_RES * V_RES * sizeof(uint16_t),
        .psram_trans_align = 64,
        .sram_trans_align = 4,
};

typedef struct lcd_llist_s {
    gpio_num_t cs_pin;
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t panel_handle;
    struct lcd_llist_s *next;
} lcd_llist_t;

lcd_llist_t *create_lcd_node(void) {
    lcd_llist_t *node = NULL;
    node = (lcd_llist_t *) calloc(sizeof(lcd_llist_t));
    if (node == NULL) {
        ESP_LOGE(TAG, "No free memory for new llist node.");
    }
    node->next = NULL;
    return node;
}

esp_err_t lcd_init(gpio_num_t cs_pin) {
    if (i80_handle == NULL) {
        ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &i80_handle));
    }

    ESP_LOGI(TAG, "Turn on LCD");
    gpio_config_t bk_gpio_config = {.mode = GPIO_MODE_OUTPUT,
                                    .pin_bit_mask = 1ULL << BK};
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
    gpio_set_level(BK, 1);

    ESP_LOGI(TAG, "Initialize Intel 8080 bus");
    esp_lcd_i80_bus_handle_t i80_bus = NULL;
    esp_lcd_i80_bus_config_t bus_config = {
            .clk_src = LCD_CLK_SRC_DEFAULT,
            .dc_gpio_num = DC,
            .wr_gpio_num = PCLK,
            .data_gpio_nums =
                    {
                            D0,
                            D1,
                            D2,
                            D3,
                            D4,
                            D5,
                            D6,
                            D7,
                    },
            .bus_width = 8,
            .max_transfer_bytes = 240 * 480 * sizeof(uint16_t),
            .psram_trans_align = 64,
            .sram_trans_align = 4,
    };
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &i80_bus));
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i80_config_t io_config = {
            .cs_gpio_num = CS,
            .pclk_hz = 2 * 1000 * 1000,
            .trans_queue_depth = 10,
            .dc_levels =
                    {
                            .dc_idle_level = 0,
                            .dc_cmd_level = 0,
                            .dc_dummy_level = 1,
                            .dc_data_level = 1,
                    },
            .lcd_cmd_bits = CMD_BITS,
            .lcd_param_bits = PARAM_BITS,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_LOGI(TAG, "Install LCD driver of st7789");
    esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = RST,
            .rgb_endian = LCD_RGB_ENDIAN_RGB,
            .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(
            esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
    esp_lcd_panel_reset(panel_handle);
    //esp_lcd_panel_init(panel_handle);
    esp_lcd_panel_invert_color(panel_handle, true);
    esp_lcd_panel_set_gap(panel_handle, 0, 80);
    esp_lcd_panel_io_tx_param(io_handle, 0x11, NULL, 0);
    esp_lcd_panel_io_tx_param(io_handle, 0x36, (uint8_t[]){0x00}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0x3A, (uint8_t[]){0x05}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xB2,
                              (uint8_t[]){0x1F, 0x1F, 0x00, 0x33, 0x33}, 5);
    esp_lcd_panel_io_tx_param(io_handle, 0xB7, (uint8_t[]){0x35}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xBB, (uint8_t[]){0x20}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xC0, (uint8_t[]){0x2C}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xC2, (uint8_t[]){0x01}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xC3, (uint8_t[]){0x01}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xC4, (uint8_t[]){0x18}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xC6, (uint8_t[]){0x13}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xD0, (uint8_t[]){0xA4, 0xA1}, 2);
    esp_lcd_panel_io_tx_param(io_handle, 0xD6, (uint8_t[]){0xA1}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xE0,
                              (uint8_t[]){0xF0, 0x04, 0x07, 0x04, 0x04, 0x04,
                                          0x25, 0x33, 0x3C, 0x36, 0x14, 0x12,
                                          0x29, 0x30},
                              14);
    esp_lcd_panel_io_tx_param(io_handle, 0xE1,
                              (uint8_t[]){0xF0, 0x02, 0x04, 0x05, 0x05, 0x21,
                                          0x25, 0x32, 0x3B, 0x38, 0x12, 0x14,
                                          0x27, 0x31},
                              14);
    esp_lcd_panel_io_tx_param(io_handle, 0xE4, (uint8_t[]){0x1D, 0x00, 0x00},
                              3);

    esp_lcd_panel_io_tx_param(io_handle, 0x21, NULL, 0);
    esp_lcd_panel_io_tx_param(io_handle, 0x11, NULL, 0);
    vTaskDelay(120 / portTICK_PERIOD_MS);
    esp_lcd_panel_io_tx_param(io_handle, 0x2A, (uint8_t[]){0, 239, 0, 239}, 2);
    esp_lcd_panel_io_tx_param(io_handle, 0x2B, (uint8_t[]){0, 239, 0, 239}, 2);
    esp_lcd_panel_io_tx_param(io_handle, 0x13, NULL, 0);

    esp_lcd_panel_io_tx_param(io_handle, 0x29, NULL, 0);
    return 0;
}
