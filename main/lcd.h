//
// Created by pickaxehit on 2023/7/3.
//

#ifndef MAIN_LCD_H
#define MAIN_LCD_H

#include "driver/gpio.h"
#include "esp_err.h"

#define D0 6
#define D1 7
#define D2 8
#define D3 9
#define D4 10
#define D5 11
#define D6 12
#define D7 13

#define PCLK 5
#define CS 3
#define DC 4
#define RST 2
#define BK 1

#define H_RES 240
#define V_RES 240

#define CMD_BITS 8
#define PARAM_BITS 8

esp_err_t lcd_init(gpio_num_t cs_pin);

#endif//MAIN_LCD_H
