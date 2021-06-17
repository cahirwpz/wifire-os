#include <sys/klog.h>
#include <sys/rman.h>
#include <sys/bus.h>
#include <dev/bcm2835reg.h>
#include <dev/bcm2835_gpioreg.h>

/*
 * \brief delay function
 * \param delay number of cycles to delay
 *
 * This just loops <delay> times in a way that the compiler
 * won't optimize away.
 *
 * clang-format is off because of different clang-format versions on build
 * server and CI
 */
/* clang-format off */
static void delay(int64_t count) {
  __asm__ volatile("1: subs %[count], %[count], #1; bne 1b"
                   : [ count ] "+r"(count));
}

#define NGPIO_PIN 54

/* clang-format on */

void bcm2835_gpio_function_select(resource_t *r, unsigned pin,
                                  bcm2835_gpio_func_t func) {
  assert(pin < NGPIO_PIN);

  unsigned mask = (1 << BCM2835_GPIO_GPFSEL_BITS_PER_PIN) - 1;
  unsigned reg = pin / BCM2835_GPIO_GPFSEL_PINS_PER_REGISTER;
  unsigned shift = (pin % BCM2835_GPIO_GPFSEL_PINS_PER_REGISTER) *
                   BCM2835_GPIO_GPFSEL_BITS_PER_PIN;

  unsigned val = bus_read_4(r, BCM2835_GPIO_GPFSEL(reg));
  val &= ~(mask << shift);
  val |= (func << shift);
  bus_write_4(r, BCM2835_GPIO_GPFSEL(reg), val);
}

void bcm2835_gpio_set_pull(resource_t *r, unsigned pin,
                           bcm2838_gpio_gppud_t pud) {
  assert(pin < NGPIO_PIN);

  unsigned mask = 1 << (pin % BCM2835_GPIO_GPPUD_PINS_PER_REGISTER);
  unsigned reg = pin / BCM2835_GPIO_GPPUD_PINS_PER_REGISTER;

  bus_write_4(r, BCM2835_GPIO_GPPUD, pud);
  /* Wait 150 cycles – this provides the required set-up time for the control
   * signal. */
  delay(150);
  bus_write_4(r, BCM2835_GPIO_GPPUDCLK(reg), mask);
  /* Wait 150 cycles – this provides the required hold time for the control
   * signal. */
  delay(150);
  bus_write_4(r, BCM2835_GPIO_GPPUD, BCM2838_GPIO_GPPUD_PULLOFF);
  bus_write_4(r, BCM2835_GPIO_GPPUDCLK(reg), 0);
}
