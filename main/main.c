#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>

static char *TAG = "main";

void app_main(void) { lcd_init(CS); }
