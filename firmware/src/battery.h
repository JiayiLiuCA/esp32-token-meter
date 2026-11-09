#pragma once
// 18650 battery level from the on-board divider (GPIO4 = ADC1_CH3, 1/3),
// scaled like Waveshare's 03_ADC_Test: 3.00 V = 0 %, 4.12 V = 100 %.
// The board exposes no charge-status pin, so USB power without a battery
// reads as the charger voltage (close to full).

#include <Arduino.h>

void battery_begin();
uint16_t battery_millivolts();  // averaged pack voltage
uint8_t battery_percent();      // 0..100
