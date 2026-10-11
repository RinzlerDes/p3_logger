/**
 ******************************************************************************
 * @file    rgb_led.h
 * @brief   TI TLC59731 single-wire RGB LED driver. This is the
 *          register-level driver of P1, moved into the HAL project
 *          (the path that R16 recommends).
 *
 *          SWEN 563 / CMPE 663 — P2 "DG-30 DecuGuard" INSTRUCTOR SOLUTION.
 *
 *  Register-level code runs without a change under HAL, because the
 *  silicon is the same. Two edits make this driver different from the P1
 *  version. The µs delays come from a local DWT cycle counter, and not
 *  from the clock.c of P1. The 10 ms wait for the chip enable uses
 *  HAL_Delay().
 *
 *  The board SHARES PA7 with SPI1 MOSI (OLED). rgb_set() borrows PA7 as a
 *  GPIO, clocks the frame, and returns the pin to AF5, thus an OLED call can
 *  follow immediately. The call blocks for about 11 ms. Call it from the
 *  main loop only, and only on a state CHANGE (R16/R22). The report asks
 *  you to tell how this handoff works.
 ******************************************************************************
 */
#ifndef RGB_LED_H
#define RGB_LED_H

#include <stdint.h>

/** Prepare the PH1/PA7 bookkeeping and the DWT µs timebase. Call it one
 *  time at init. */
void rgb_init(void);

/** Set the LED color (0..255 for each channel). Blocks about 11 ms. */
void rgb_set(uint8_t r, uint8_t g, uint8_t b);

#endif /* RGB_LED_H */
