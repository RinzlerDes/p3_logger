/**
 ******************************************************************************
 * @file    rgb_led.c
 * @brief   TI TLC59731 single-wire RGB LED driver for the STM32WB5MM-DK
 *          (provided driver).
 *
 *          SWEN 563 / CMPE 663 — P2 "DG-30 DecuGuard" INSTRUCTOR SOLUTION.
 *          This is the provided driver of P1, ported as spec R16 asks.
 *          Only the delay plumbing changed. The protocol bit-banging is
 *          the P1 code, byte for byte.
 *
 *  Hardware (board BSP / UM2825):
 *      PA7  LED serial data (SDI)  — shared with OLED SPI1 MOSI through JP5
 *      PH1  LED select             — gates the data input of the LED chip.
 *                                    While PH1 stays low, the chip ignores
 *                                    SPI traffic on PA7
 *
 *  Protocol (TLC59731 "EasySet", transcribed from the ST BSP, which times
 *  it in 5 us units): the chip learns the bit period T_CYCLE from the gap
 *  between the first two rising edges. Then it samples one bit in each
 *  cycle. A rising edge opens each bit. A second pulse in the first half
 *  of the cycle encodes a '1'. A 32-bit frame is  0x3A | RED | GREEN |
 *  BLUE  (8-bit grayscale for each channel). The chip latches the frame
 *  when the line stays idle.
 *
 *  PA7 handoff: the code switches the pin to a plain GPIO output for the
 *  frame. Then it returns the pin to SPI1 (AF5), thus an OLED call can
 *  follow immediately. Call the provided drivers from the main loop only.
 *  Do not call them from an ISR, and do not call them from in a capture
 *  loop.
 *
 *  Pin discipline: read-modify-write on the owned bits only. That is the
 *  same rule that student code must obey everywhere else.
 ******************************************************************************
 */
#include "main.h"
#include "rgb_led.h"

/**
  * @brief  Busy-wait microsecond delay on the DWT cycle counter. P1 used
  *         its clock.c for this delay. rgb_init() enables the counter.
  *         Cycle math: us × (SystemCoreClock / 1e6) = us × 64 at 64 MHz.
  *         The delay blocks by design, because the TLC59731 protocol needs
  *         5 µs bit timing that HAL_Delay cannot give.
  * @param  us  delay in microseconds (the protocol uses 5–20 µs).
  * @retval None
  */
static void dwt_delay_us(uint32_t us)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t cycles = us * (SystemCoreClock / 1000000u);
    while ((uint32_t)(DWT->CYCCNT - start) < cycles) { }
}

#define TLC_WRITE_CMD 0x3Au

/* BSP timing: one protocol "cycle" is 5 us. All delays below are in cycles. */
#define TLC_CYCLE_US  5u
#define TLC_DELAY     1u   /* high/low phase in a bit          */
#define TLC_TCYCLE_0  4u   /* tail wait, '0' bit               */
#define TLC_TCYCLE_1  1u   /* tail wait, '1' bit               */

static uint8_t rgb_up;     /* init guard */

static inline void tlc_wait(uint32_t cycles)
{
    dwt_delay_us(cycles * TLC_CYCLE_US);
}

static inline void sdi_high(void)   { GPIOA->BSRR = GPIO_BSRR_BS7; }
static inline void sdi_low(void)    { GPIOA->BSRR = GPIO_BSRR_BR7; }
static inline void sel_high(void)   { GPIOH->BSRR = GPIO_BSRR_BS1; }
static inline void sel_low(void)    { GPIOH->BSRR = GPIO_BSRR_BR1; }

/**
  * @brief  Borrow PA7 from SPI1. The function configures the pin as a
  *         plain GPIO output and sets it low. The board shares PA7 between
  *         the SPI1 MOSI line of the OLED and the serial data line of the
  *         LED (JP5). The function does a read-modify-write on MODER bits 7
  *         only, because port A carries the pins of other owners.
  * @retval None
  */
static void pa7_to_gpio(void)
{
    sdi_low();
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODE7) | GPIO_MODER_MODE7_0;
}

/**
  * @brief  Return PA7 to SPI1. The function programs AFR[0] back to AF5,
  *         thus OLED traffic can start immediately after the LED frame.
  * @retval None
  */
static void pa7_to_spi(void)
{
    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~GPIO_AFRL_AFSEL7)
                    | (5u << GPIO_AFRL_AFSEL7_Pos);
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODE7) | GPIO_MODER_MODE7_1;
}

/**
  * @brief  Clock one bit of the TLC59731 "EasySet" single-wire protocol.
  *         A rising edge opens each bit cycle. A second pulse in the first
  *         half-cycle encodes a '1' (timing in 5 µs units, from the ST BSP).
  * @param  bit  zero sends '0', nonzero sends '1'.
  * @retval None
  */
static void tlc_send_bit(uint32_t bit)
{
    /* Rising edge opens the bit ... */
    sdi_high();
    tlc_wait(TLC_DELAY);
    sdi_low();
    tlc_wait(TLC_DELAY);

    if (bit != 0u) {
        /* ... and a second pulse in the first half-cycle encodes '1'. */
        sdi_high();
        tlc_wait(TLC_DELAY);
        sdi_low();
        tlc_wait(TLC_TCYCLE_1);
    } else {
        tlc_wait(TLC_TCYCLE_0);
    }
}

/**
  * @brief  Clock one byte MSB-first through tlc_send_bit().
  * @param  byte  the byte to transmit.
  * @retval None
  */
static void tlc_send_byte(uint8_t byte)
{
    for (uint32_t i = 0u; i < 8u; i++) {
        tlc_send_bit((uint32_t)byte & (0x80u >> i));
    }
}

/**
  * @brief  Prepare the LED path. The function enables the GPIOA and GPIOH
  *         clocks with a read-modify-write. It starts the DWT cycle counter
  *         that gives the protocol timing. It configures PH1 (LED select)
  *         as an output and sets PH1 low, thus the LED chip ignores OLED
  *         traffic on PA7. Call it one time at init, before the first
  *         rgb_set().
  * @retval None
  */
void rgb_init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOHEN;
    (void)RCC->AHB2ENR;

    /* DWT cycle counter for the 5 µs protocol timing. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    /* PH1 -> push-pull output, low: the LED chip ignores PA7. */
    GPIOH->MODER = (GPIOH->MODER & ~GPIO_MODER_MODE1) | GPIO_MODER_MODE1_0;
    sel_low();

    rgb_up = 1u;
}

/**
  * @brief  Transmit one 32-bit color frame (0x3A | R | G | B) to the
  *         TLC59731. The call BLOCKS for about 11 ms. The 10 ms wait for
  *         the chip select gives most of that time. Call it from the main
  *         loop only, and only on a state CHANGE (led_task obeys that rule,
  *         R16/R22). Sequence: borrow PA7 as GPIO → select the chip →
  *         timing preamble → 32-bit frame → idle low and PH1 low, which
  *         latches the frame → PA7 back to SPI1.
  * @param  r,g,b  8-bit grayscale for each channel (0 = off, about 32 is a
  *                comfortable indoor level).
  * @retval None. Before rgb_init() the function does nothing, and it gives
  *         the caller no indication of this.
  */
void rgb_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (rgb_up == 0u) {
        return;
    }

    pa7_to_gpio();

    /* Select the LED chip. The BSP gives the enable 10 ms to become stable. */
    sel_low();
    HAL_Delay(10u);
    sel_high();

    /* T_CYCLE measurement preamble: two rising edges 5 cycles apart teach
     * the chip the bit period (transcribed 1:1 from the ST BSP). */
    sdi_low();
    tlc_wait(TLC_DELAY);
    sdi_high();
    tlc_wait(TLC_TCYCLE_0);
    sdi_low();
    tlc_wait(TLC_DELAY);
    sdi_high();
    tlc_wait(TLC_TCYCLE_0);

    /* 32-bit frame: write command, then 8-bit grayscale for each channel. */
    tlc_send_byte(TLC_WRITE_CMD);
    tlc_send_byte(r);
    tlc_send_byte(g);
    tlc_send_byte(b);

    /* Idle low. PH1 low gates the data line, and the chip latches. */
    sdi_low();
    sel_low();

    pa7_to_spi();
}
