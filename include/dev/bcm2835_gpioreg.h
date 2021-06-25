/*	$NetBSD: bcm2835_gpioreg.h,v 1.5 2019/09/28 07:24:52 mlelstv Exp $
 */

/*
 * Copyright (c) 2013 Jonathan A. Kollasch
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _AARCH64_BCM2835_GPIOREG_H_
#define _AARCH64_BCM2835_GPIOREG_H_

/* The function select registers are used to define the operation of the
 * general-purpose I/O pins. */
#define BCM2835_GPIO_GPFSEL(x) (0x000 + (x) * sizeof(uint32_t))
#define BCM2835_GPIO_GPFSEL_PINS_PER_REGISTER 10
#define BCM2835_GPIO_GPFSEL_BITS_PER_PIN 3

/* The output set registers are used to set a GPIO pin. */
#define BCM2835_GPIO_GPSET(x) (0x01C + (x) * sizeof(uint32_t))
#define BCM2835_GPIO_GPSET_PINS_PER_REGISTER 32

/* The output clear registers are used to clear a GPIO pin. */
#define BCM2835_GPIO_GPCLR(x) (0x028 + (x) * sizeof(uint32_t))
#define BCM2835_GPIO_GPCLR_PINS_PER_REGISTER                                   \
  BCM2835_GPIO_GPSET_PINS_PER_REGISTER

/* The pin level registers return the actual value of the pin. */
#define BCM2835_GPIO_GPLEV(x) (0x034 + (x) * sizeof(uint32_t))
#define BCM2835_GPIO_GPLEV_PINS_PER_REGISTER 32

/* The event detect status registers are used to record level and edge events
 * on the GPIO pins. */
#define BCM2835_GPIO_GPEDS(x) (0x040 + (x) * sizeof(uint32_t))

/* The rising edge detect enable registers define the pins for which a rising
 * edge transition sets a bit in the event detect status registers GPEDS. */
#define BCM2835_GPIO_GPREN(x) (0x04C + (x) * sizeof(uint32_t))

/* The falling edge detect enable registers define the pins for which a
 * falling edge transition sets a bit in the event detect status registers
 * GPEDS. */
#define BCM2835_GPIO_GPFEN(x) (0x058 + (x) * sizeof(uint32_t))

/* The high level detect enable registers define the pins for which a high
 * level sets a bit in the event detect status register GPEDS. */
#define BCM2835_GPIO_GPHEN(x) (0x064 + (x) * sizeof(uint32_t))
#define BCM2835_GPIO_GPHEN_PINS_PER_REGISTER 32

/* The low level detect enable registers define the pins for which a low level
 * sets a bit in the event detect status register GPEDS. */
#define BCM2835_GPIO_GPLEN(x) (0x070 + (x) * sizeof(uint32_t))

/* The asynchronous rising edge detect enable registers define the pins for
 * which a asynchronous rising edge transition sets a bit in the event detect
 * status registers GPEDS. */
#define BCM2835_GPIO_GPAREN(x) (0x07C + (x) * sizeof(uint32_t))

/* The asynchronous falling edge detect enable registers define the pins for
 * which a asynchronous falling edge transition sets a bit in the event detect
 * status registers GPEDS. */
#define BCM2835_GPIO_GPAFEN(x) (0x088 + (x) * sizeof(uint32_t))

/* Controls actuation of pull up/down to ALL GPIO pins. */
#define BCM2835_GPIO_GPPUD (0x094)

/* brcm,pull property */
enum {
  BCM2835_GPIO_GPPUD_PULLOFF = 0x0,
  BCM2835_GPIO_GPPUD_PULLDOWN = 0x1,
  BCM2835_GPIO_GPPUD_PULLUP = 0x2
};

/* Controls actuation of pull up/down for specific GPIO pin. */
#define BCM2835_GPIO_GPPUDCLK(x) (0x098 + (x) * sizeof(uint32_t))
#define BCM2835_GPIO_GPPUD_PINS_PER_REGISTER 32

typedef enum {
  BCM2838_GPIO_GPPUD_PULLOFF = 0x0,
  BCM2838_GPIO_GPPUD_PULLUP = 0x1,
  BCM2838_GPIO_GPPUD_PULLDOWN = 0x2
} bcm2838_gpio_gppud_t;

#define BCM2838_GPIO_GPPUPPDN(x) (0x0e4 + (x) * sizeof(uint32_t))
#define BCM2838_GPIO_GPPUD_REGID(n) ((n) / 16)
#define BCM2838_GPIO_GPPUD_MASK(n) (0x3 << ((n) % 16) * 2)

/* brcm,function property */
typedef enum {
  BCM2835_GPIO_IN = 0,
  BCM2835_GPIO_OUT = 1,
  BCM2835_GPIO_ALT5 = 2,
  BCM2835_GPIO_ALT4 = 3,
  BCM2835_GPIO_ALT0 = 4,
  BCM2835_GPIO_ALT1 = 5,
  BCM2835_GPIO_ALT2 = 6,
  BCM2835_GPIO_ALT3 = 7
} bcm2835_gpio_func_t;

#endif /* !_AARCH64_BCM2835_GPIOREG_H_ */
