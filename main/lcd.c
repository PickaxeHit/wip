//
// Created by pickaxehit on 2023/7/3.
//

#include "lcd.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>

static char *TAG = "lcd";

SemaphoreHandle_t lcd_list_mutex = NULL;

static esp_lcd_i80_bus_handle_t i80_bus = NULL;
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

typedef struct lcd_node_s {
    gpio_num_t cs_pin;
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t panel_handle;
    struct lcd_node_s *next;
} lcd_node_t;

static lcd_node_t *header = NULL;

static lcd_node_t *lcd_node_create(void) {
    lcd_node_t *node = NULL;
    node = (lcd_node_t *) calloc(1, sizeof(lcd_node_t));
    if (node == NULL) {
        ESP_LOGE(TAG, "No free memory for new list node.");
        return NULL;
    }
    node->next = NULL;
    return node;
}

static void lcd_list_insert(lcd_node_t *list, lcd_node_t *node) {
    xSemaphoreTake(lcd_list_mutex, portMAX_DELAY);
    lcd_node_t *p = list;
    node->next = p->next;
    p->next = node;
    xSemaphoreGive(lcd_list_mutex);
}

static lcd_node_t *lcd_search(lcd_node_t *list, gpio_num_t cs_pin) {
    lcd_node_t *p = list;
    if (p->next == NULL) {
        return NULL;
    };
    p = p->next;
    while (p->cs_pin != cs_pin) {
        if (p->next == NULL) {
            return NULL;
        } else {
            p = p->next;
        }
    }
    return p;
}

static int lcd_node_delete(lcd_node_t *list, gpio_num_t cs_pin) {
    xSemaphoreTake(lcd_list_mutex, portMAX_DELAY);
    lcd_node_t *p = list;
    lcd_node_t *prev = NULL;
    while (p->next != NULL) {
        prev = p;
        p = p->next;
        if (p->cs_pin == cs_pin) {
            if (p->next != NULL) {
                prev->next = p->next;
                free(p);
                p = NULL;
            } else {
                prev->next = NULL;
                free(p);
                p = NULL;
            }
            xSemaphoreGive(lcd_list_mutex);
            return 0;
        }
    }
    xSemaphoreGive(lcd_list_mutex);
    return -1;
}

esp_err_t lcd_panel_init(gpio_num_t cs_pin) {
    if (lcd_list_mutex == NULL) {
        lcd_list_mutex = xSemaphoreCreateMutex();
        if (lcd_list_mutex == NULL) {
            ESP_LOGE(TAG, "Mutex creat failed");
            return ESP_FAIL;
        }
    }
    if (i80_bus == NULL) {
        ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &i80_bus));
        if (header == NULL) {
            header = lcd_node_create();
        }
    }
    if (lcd_search(header, cs_pin) != NULL) {
        ESP_LOGE(TAG, "This LCD has already initialized.");
        return ESP_ERR_INVALID_ARG;
    }
    lcd_node_t *lcd = lcd_node_create();
    lcd_list_insert(header, lcd);
    lcd->cs_pin = cs_pin;

    ESP_LOGI(TAG, "Turn off LCD");
    gpio_config_t bk_gpio_config = {.mode = GPIO_MODE_OUTPUT, .pin_bit_mask = 1ULL << BK};
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
    gpio_set_level(BK, 0);

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i80_config_t io_config = {
            .cs_gpio_num = cs_pin,
            .pclk_hz = 20 * 1000 * 1000,
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
    lcd->io_handle = io_handle;

    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_LOGI(TAG, "Install LCD driver of st7789");
    esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = RST,
            .rgb_endian = LCD_RGB_ENDIAN_RGB,
            .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
    lcd->panel_handle = panel_handle;

    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_invert_color(panel_handle, true);
    esp_lcd_panel_set_gap(panel_handle, 0, 80);
    esp_lcd_panel_io_tx_param(io_handle, 0x11, NULL, 0);
    esp_lcd_panel_io_tx_param(io_handle, 0x36, (uint8_t[]){0x00}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0x3A, (uint8_t[]){0x05}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xB2, (uint8_t[]){0x1F, 0x1F, 0x00, 0x33, 0x33}, 5);
    esp_lcd_panel_io_tx_param(io_handle, 0xB7, (uint8_t[]){0x35}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xBB, (uint8_t[]){0x20}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xC0, (uint8_t[]){0x2C}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xC2, (uint8_t[]){0x01}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xC3, (uint8_t[]){0x01}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xC4, (uint8_t[]){0x18}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xC6, (uint8_t[]){0x13}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xD0, (uint8_t[]){0xA4, 0xA1}, 2);
    esp_lcd_panel_io_tx_param(io_handle, 0xD6, (uint8_t[]){0xA1}, 1);
    esp_lcd_panel_io_tx_param(
            io_handle, 0xE0,
            (uint8_t[]){0xF0, 0x04, 0x07, 0x04, 0x04, 0x04, 0x25, 0x33, 0x3C, 0x36, 0x14, 0x12, 0x29, 0x30}, 14);
    esp_lcd_panel_io_tx_param(
            io_handle, 0xE1,
            (uint8_t[]){0xF0, 0x02, 0x04, 0x05, 0x05, 0x21, 0x25, 0x32, 0x3B, 0x38, 0x12, 0x14, 0x27, 0x31}, 14);
    esp_lcd_panel_io_tx_param(io_handle, 0xE4, (uint8_t[]){0x1D, 0x00, 0x00}, 3);

    esp_lcd_panel_io_tx_param(io_handle, 0x21, NULL, 0);
    esp_lcd_panel_io_tx_param(io_handle, 0x11, NULL, 0);
    vTaskDelay(120 / portTICK_PERIOD_MS);


    esp_lcd_panel_io_tx_param(io_handle, 0x2A,
                              (uint8_t[]){
                                      (0 >> 8) & 0xFF,
                                      0 & 0xFF,
                                      ((240 - 1) >> 8) & 0xFF,
                                      (240 - 1) & 0xFF,
                              },
                              4);
    esp_lcd_panel_io_tx_param(io_handle, 0x2B,
                              (uint8_t[]){
                                      (0 >> 8) & 0xFF,
                                      0 & 0xFF,
                                      ((240 - 1) >> 8) & 0xFF,
                                      (240 - 1) & 0xFF,
                              },
                              4);
    esp_lcd_panel_io_tx_param(io_handle, 0x2C, NULL, 0);
    for (uint16_t i = 0; i < 3600; i++) {
        esp_lcd_panel_io_tx_color(io_handle, -1,
                                  (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
                                  32);
    }

    esp_lcd_panel_io_tx_param(io_handle, 0x29, NULL, 0);
    ESP_LOGI(TAG, "Turn on LCD");
    gpio_set_level(BK, 1);
    return 0;
}

esp_err_t lcd_delete(gpio_num_t cs_pin) {
    lcd_node_t *p = lcd_search(header, cs_pin);
    if (p == NULL) {
        ESP_LOGE(TAG, "This LCD has not initialized.");
        return ESP_ERR_INVALID_ARG;
    } else {
        esp_lcd_panel_reset(p->panel_handle);
        ESP_ERROR_CHECK(esp_lcd_panel_del(p->panel_handle));
        ESP_LOGI(TAG, "Panel deleted.");
        ESP_ERROR_CHECK(esp_lcd_panel_io_del(p->io_handle));
        ESP_LOGI(TAG, "Panel IO deleted.");
        lcd_node_delete(header, cs_pin);
        if (header->next == NULL) {
            ESP_ERROR_CHECK(esp_lcd_del_i80_bus(i80_bus));
            i80_bus = NULL;
            ESP_LOGI(TAG, "I80 bus deleted.");
        }
        return ESP_OK;
    }
}
