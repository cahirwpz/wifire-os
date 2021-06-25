#define KL_LOG KL_DEV

/*
 * For detailed information on the controller, please refer to:
 * https://cs140e.sergio.bz/docs/BCM2837-ARM-Peripherals.pdf
 */

#include <sys/mimiker.h>
#include <sys/rman.h>
#include <sys/devclass.h>
#include <sys/klog.h>
#include <sys/types.h>
#include <dev/bcm2835_gpio.h>
#include <sys/device.h>
#include <sys/bus.h>
#include <sys/time.h>
#include <sys/libkern.h>
#include <sys/callout.h>
#include <sys/condvar.h>
#include <sys/bus.h>
#include <dev/emmc.h>
#include <sys/errno.h>
#include <sys/bitops.h>
#include <dev/bcm2835_emmcreg.h>

typedef struct bcmemmc_state {
  resource_t *gpio;      /* GPIO resource (needed until we have a decent
                          * way of setting up GPIO) */
  resource_t *emmc;      /* e.MMC controller registers */
  resource_t *irq;       /* e.MMC controller interrupt */
  condvar_t intr_recv;   /* Used to wake up a thread waiting for an interrupt */
  spin_t lock;           /* Covers `pending`, `intr_recv` and `emmc`. */
  uint64_t rca;          /* Relative Card Address */
  uint64_t host_version; /* Host specification version */
  volatile uint32_t pending; /* All interrupts received */
} bcmemmc_state_t;

#define b_in bus_read_4
#define b_out bus_write_4
#define b_clr(mem, reg, b) b_out((mem), (reg), b_in((mem), (reg)) & ~(b))
#define b_set(mem, reg, b) b_out((mem), (reg), b_in((mem), (reg)) | (b))

/**
 * Delay function.
 * This just loops `count` times in a way that the compiler
 * won't optimize away.
 * \param count number of cycles to delay
 */
static void delay(int64_t count) {
  __asm__ volatile("1: subs %[count], %[count], #1; bne 1b"
                   : [ count ] "+r"(count));
}

static inline uint32_t emmc_wait_flags_to_hwflags(emmc_wait_flags_t mask) {
  return ((mask & EMMC_I_DATA_DONE) ? INT_DATA_DONE : 0) |
         ((mask & EMMC_I_READ_READY) ? INT_READ_RDY : 0) |
         ((mask & EMMC_I_WRITE_READY) ? INT_WRITE_RDY : 0);
}

/* Returns new pending interrupts.
 * All pending interrupts are stored in `bmcemmc_state_t::pending`. */
static uint32_t bcmemmc_read_intr(bcmemmc_state_t *state) {
  resource_t *emmc = state->emmc;
  uint32_t newpend = b_in(emmc, BCMEMMC_INTERRUPT);
  /* Pending interrupts need to be cleared manually. */
  if (newpend) {
    state->pending |= newpend;
    b_out(emmc, BCMEMMC_INTERRUPT, newpend);
  }
  return newpend;
}

#define BCMEMMC_TIMEOUT 10000 /* Timeout when awaiting an interrupt */

/**
 * \brief Wait for all specified interrupts.
 * \param dev eMMC device
 * \param mask expected interrupts
 * \param clear additional interrupt bits to be cleared
 * \return 0 on success, EIO on internal error, ETIMEDOUT on device timeout.
 */
static int bcmemmc_intr_wait(device_t *dev, uint32_t mask) {
  bcmemmc_state_t *state = (bcmemmc_state_t *)dev->state;
  resource_t *emmc = state->emmc;

  assert(mask != 0);

  SCOPED_SPIN_LOCK(&state->lock);

  for (;;) {
    (void)bcmemmc_read_intr(state);

    if ((state->pending & mask) == mask) {
      state->pending &= ~mask;
      return 0;
    }

    if (state->pending & INT_ERROR_MASK) {
      state->pending = 0;
      return EIO;
    }

    /* Sleep for a while if no interrupts have been received so far. */
    if (cv_wait_timed(&state->intr_recv, &state->lock, BCMEMMC_TIMEOUT)) {
      b_out(emmc, BCMEMMC_INTERRUPT, INT_ALL_MASK);
      return ETIMEDOUT;
    }
  }
}

static int bcmemmc_wait(device_t *cdev, emmc_wait_flags_t wflags) {
  return bcmemmc_intr_wait(cdev->parent, emmc_wait_flags_to_hwflags(wflags));
}

static intr_filter_t bcmemmc_intr_filter(void *data) {
  bcmemmc_state_t *state = (bcmemmc_state_t *)data;
  WITH_SPIN_LOCK (&state->lock) {
    if (!bcmemmc_read_intr(state))
      return IF_STRAY;
    /* Wake up the thread if all expected interrupts have been received */
    cv_signal(&state->intr_recv);
  }
  return IF_FILTERED;
}

/* Find a divisor that provides the best approximation.
 * That is an integer `c` such as for every integer `c'` != `c`
 * |`frq` - (`clk` / `c`)| <= |`frq` - (`clk` / `c'`)| */
static uint32_t bcmemmc_clk_approx_divisor(uint32_t clk, uint32_t frq) {
  int32_t c1 = clk / frq;
  if (c1 == 0)
    c1++;
  int32_t c2 = c1 + 1;
  int32_t est1 = abs((int32_t)frq - (int32_t)clk / c1);
  int32_t est2 = abs((int32_t)frq - (int32_t)clk / c2);
  return (est1 < est2) ? c1 : c2;
}

/* Set e.MMC clock's divisor to match frequency `frq` */
static void bcmemmc_clk_set_divisor(bcmemmc_state_t *state, uint32_t frq) {
  resource_t *emmc = state->emmc;

  uint32_t clk = GPIO_CLK_EMMC_DEFAULT_FREQ;
  uint32_t divisor = bcmemmc_clk_approx_divisor(clk, frq);
  uint32_t ctl1 = b_in(emmc, BCMEMMC_CONTROL1) & BCMEMMC_CLKDIV_INVMASK;

  ctl1 |= (divisor << C1_CLK_FREQ8_SHIFT) & C1_CLK_FREQ8;
  ctl1 |= ((divisor >> 8) << C1_CLK_FREQ_MS2_SHIFT) & C1_CLK_FREQ_MS2;

  b_out(emmc, BCMEMMC_CONTROL1, ctl1);
  klog("e.MMC: clock set to %luHz / %lu (requested %luHz)", clk, divisor, frq);
}

#define NSPINS 10000

/* Set SD clock to frequency in Hz (approximately), divided mode */
static int bcmemmc_set_clk_freq(device_t *dev, uint32_t frq) {
  bcmemmc_state_t *state = (bcmemmc_state_t *)dev->state;
  resource_t *emmc = state->emmc;
  int cnt;

  /* TODO(mohrcore): Not sure if this is necessary. If the will run fine on
   * a real hardware without it, it should be removed. */
  cnt = NSPINS;
  while (b_in(emmc, BCMEMMC_STATUS) & SR_INHIBIT) {
    if (--cnt < 0)
      return ETIMEDOUT;
    delay(3);
  }

  b_clr(emmc, BCMEMMC_CONTROL1, C1_CLK_EN);
  /* host_version <= HOST_SPEC_V2 needs a power-of-two divisor.
   * It would require a different calculation method. */
  assert(state->host_version > HOST_SPEC_V2);
  bcmemmc_clk_set_divisor(state, frq);
  b_set(emmc, BCMEMMC_CONTROL1, C1_CLK_EN);

  /* Wait until the clock becomes stable */
  cnt = NSPINS;
  while (!(b_in(emmc, BCMEMMC_CONTROL1) & C1_CLK_STABLE)) {
    if (--cnt < 0)
      return ETIMEDOUT;
    delay(30);
  }

  return 0;
}

/* Encode `cmd` to command an appropriate command register value */
static uint32_t bcmemmc_encode_cmd(emmc_cmd_t cmd) {
  uint32_t code = (uint32_t)cmd.cmd_idx << 24;

  switch (cmd.exp_resp) {
    case EMMCRESP_NONE:
      break;
    case EMMCRESP_R2:
      code |= CMD_RESP136;
      break;
    case EMMCRESP_R1B:
      code |= CMD_RESP48B;
      break;
    default:
      code |= CMD_RESP48;
  }
  switch (emmc_cmdtype(cmd.flags)) {
    case EMMC_F_SUSPEND:
      code |= CMD_TYPE_SUSPEND;
      break;
    case EMMC_F_RESUME:
      code |= CMD_TYPE_RESUME;
      break;
    case EMMC_F_ABORT:
      code |= CMD_TYPE_ABORT;
      break;
    default:
      break;
  }
  if (cmd.flags & EMMC_F_DATA_READ)
    code |= CMD_DATA_TRANSFER | CMD_DATA_READ;
  if (cmd.flags & EMMC_F_DATA_WRITE)
    code |= CMD_DATA_TRANSFER;
  if (cmd.flags & EMMC_F_DATA_MULTI)
    code |= CMD_DATA_MULTI;
  if (cmd.flags & EMMC_F_CHKIDX)
    code |= CMD_CHECKIDX;
  if (cmd.flags & EMMC_F_CHKCRC)
    code |= CMD_CHECKCRC;

  return code;
}

static int bcmemmc_set_bus_width(bcmemmc_state_t *state, uint32_t bw) {
  switch (bw) {
    case EMMC_BUSWIDTH_1:
      b_clr(state->emmc, BCMEMMC_CONTROL0, C0_HCTL_8BIT);
      b_clr(state->emmc, BCMEMMC_CONTROL0, C0_HCTL_DWITDH);
      return 0;
    case EMMC_BUSWIDTH_4:
      b_clr(state->emmc, BCMEMMC_CONTROL0, C0_HCTL_8BIT);
      b_set(state->emmc, BCMEMMC_CONTROL0, C0_HCTL_DWITDH);
      return 0;
    case EMMC_BUSWIDTH_8:
      b_set(state->emmc, BCMEMMC_CONTROL0, C0_HCTL_8BIT);
      b_clr(state->emmc, BCMEMMC_CONTROL0, C0_HCTL_DWITDH);
      return 0;
    default:
      return EINVAL;
  }
}

static int bcmemmc_get_bus_width(bcmemmc_state_t *state) {
  uint32_t ctl = b_in(state->emmc, BCMEMMC_CONTROL0);
  if (ctl & C0_HCTL_8BIT)
    return EMMC_BUSWIDTH_8;
  if (ctl & C0_HCTL_DWITDH)
    return EMMC_BUSWIDTH_4;
  return EMMC_BUSWIDTH_1;
}

#define MAXBLKSIZE 512 /* Currently only blocks of 512 bytes are supported. */
#define MAXBLKCNT 255  /* Transfers cannot exceed 255 consecutive blocks. */

static int bcmemmc_get_prop(device_t *cdev, uint32_t id, uint64_t *var) {
  bcmemmc_state_t *state = (bcmemmc_state_t *)cdev->parent->state;
  resource_t *emmc = state->emmc;

  switch (id) {
    case EMMC_PROP_RW_BLKCNT:
      *var = (b_in(emmc, BCMEMMC_BLKSIZECNT) & BSC_BLKCNT) >> BSC_BLKCNT_SHIFT;
      break;
    case EMMC_PROP_RW_BLKSIZE:
      *var = b_in(emmc, BCMEMMC_BLKSIZECNT) & BSC_BLKSIZE;
      break;
    case EMMC_PROP_R_MAXBLKSIZE:
      *var = MAXBLKSIZE;
      break;
    case EMMC_PROP_R_MAXBLKCNT:
      *var = MAXBLKCNT;
      break;
    case EMMC_PROP_R_VOLTAGE_SUPPLY:
      *var = EMMC_VOLTAGE_WINDOW_LOW;
      break;
    case EMMC_PROP_RW_RESP_LOW:
      *var = (uint64_t)b_in(emmc, BCMEMMC_RESP0) |
             (uint64_t)b_in(emmc, BCMEMMC_RESP1) << 32;
      break;
    case EMMC_PROP_RW_RESP_HI:
      *var = (uint64_t)b_in(emmc, BCMEMMC_RESP2) |
             (uint64_t)b_in(emmc, BCMEMMC_RESP3) << 32;
      break;
    case EMMC_PROP_RW_BUSWIDTH:
      *var = bcmemmc_get_bus_width(state);
      break;
    case EMMC_PROP_RW_RCA:
      *var = state->rca;
      break;
    default:
      return EINVAL;
  }

  return 0;
}

static int bcmemmc_set_prop(device_t *cdev, uint32_t id, uint64_t var) {
  bcmemmc_state_t *state = (bcmemmc_state_t *)cdev->parent->state;
  resource_t *emmc = state->emmc;

  uint32_t reg = 0;
  switch (id) {
    case EMMC_PROP_RW_BLKCNT:
      if (var >= 65536)
        return EINVAL;
      reg = b_in(emmc, BCMEMMC_BLKSIZECNT) & ~BSC_BLKCNT;
      b_out(emmc, BCMEMMC_BLKSIZECNT, reg | (var << BSC_BLKCNT_SHIFT));
      break;
    case EMMC_PROP_RW_BLKSIZE:
      if (var >= 1024)
        return EINVAL;
      reg = b_in(emmc, BCMEMMC_BLKSIZECNT) & ~BSC_BLKSIZE;
      b_out(emmc, BCMEMMC_BLKSIZECNT, reg | var);
      break;
    case EMMC_PROP_RW_RESP_LOW:
      b_out(emmc, BCMEMMC_RESP0, (uint32_t)var);
      b_out(emmc, BCMEMMC_RESP1, (uint32_t)(var >> 32));
      break;
    case EMMC_PROP_RW_RESP_HI:
      b_out(emmc, BCMEMMC_RESP2, (uint32_t)var);
      b_out(emmc, BCMEMMC_RESP3, (uint32_t)(var >> 32));
      break;
    case EMMC_PROP_RW_CLOCK_FREQ:
      return bcmemmc_set_clk_freq(cdev->parent, var);
    case EMMC_PROP_RW_BUSWIDTH:
      return bcmemmc_set_bus_width(state, var);
    case EMMC_PROP_RW_RCA:
      state->rca = var;
      break;
    default:
      return EINVAL;
  }

  return 0;
}

static int bcmemmc_send_cmd(device_t *cdev, emmc_cmd_t cmd, uint32_t arg,
                            emmc_resp_t *resp) {
  device_t *dev = cdev->parent;
  bcmemmc_state_t *state = (bcmemmc_state_t *)dev->state;
  resource_t *emmc = state->emmc;
  uint32_t code = bcmemmc_encode_cmd(cmd);

  if (cmd.flags & EMMC_F_APP)
    bcmemmc_send_cmd(cdev, EMMC_CMD(APP_CMD), state->rca << 16, NULL);

  b_out(emmc, BCMEMMC_ARG1, arg);
  b_out(emmc, BCMEMMC_CMDTM, code);

  int error = bcmemmc_intr_wait(dev, INT_CMD_DONE);
  if (error) {
    klog("ERROR: failed to send EMMC command %p", code);
    return error;
  }

  if (resp) {
    resp->r[0] = b_in(emmc, BCMEMMC_RESP0);
    resp->r[1] = b_in(emmc, BCMEMMC_RESP1);
    resp->r[2] = b_in(emmc, BCMEMMC_RESP2);
    resp->r[3] = b_in(emmc, BCMEMMC_RESP3);
  }

  return 0;
}

static int bcmemmc_read(device_t *cdev, void *buf, size_t len, size_t *donep) {
  device_t *emmcdev = cdev->parent;
  bcmemmc_state_t *state = (bcmemmc_state_t *)emmcdev->state;
  resource_t *emmc = state->emmc;
  uint32_t *data = buf;

  assert(is_aligned(len, sizeof(uint32_t)));

  /* TODO(morhcore): should use DMA transfer if possible. */
  for (size_t i = 0; i < len / sizeof(uint32_t); i++)
    data[i] = b_in(emmc, BCMEMMC_DATA);

  /* TODO(mohrcore): check if the transfer fully succeeded! */
  if (donep)
    *donep = len;
  return 0;
}

static int bcmemmc_write(device_t *cdev, const void *buf, size_t len,
                         size_t *donep) {
  device_t *emmcdev = cdev->parent;
  bcmemmc_state_t *state = (bcmemmc_state_t *)emmcdev->state;
  resource_t *emmc = state->emmc;
  const uint32_t *data = buf;

  assert(is_aligned(len, sizeof(uint32_t)));

  /* TODO(morhcore): should use DMA transfer if possible. */
  for (size_t i = 0; i < len / sizeof(uint32_t); i++)
    b_out(emmc, BCMEMMC_DATA, data[i]);

  /* TODO(mohrcore): check wether the transfer fully succeeded! */
  if (donep)
    *donep = len;
  return 0;
}

/* e.MMC requires some GPIO setup to work properly. This however is different
 * than what is described in BCM2835 Peripherals datasheet. I don't know the
 * reason why, but based on other drivers, looks like these particular setting
 * are needed. */
static void emmc_gpio_init(device_t *dev) {
  bcmemmc_state_t *state = (bcmemmc_state_t *)dev->state;
  resource_t *gpio = state->gpio;

  /* GPIO_CD: interrupt pin */
  bcm2835_gpio_function_select(gpio, 47, BCM2835_GPIO_ALT3);
  bcm2835_gpio_set_pull(gpio, 47, BCM2838_GPIO_GPPUD_PULLDOWN);
  bcm2835_gpio_set_high_detect(gpio, 47, true);

  /* GPIO_CLK, GPIO_CMD */
  bcm2835_gpio_function_select(gpio, 48, BCM2835_GPIO_ALT3);
  bcm2835_gpio_function_select(gpio, 49, BCM2835_GPIO_ALT3);
  bcm2835_gpio_set_pull(gpio, 48, BCM2838_GPIO_GPPUD_PULLDOWN);
  bcm2835_gpio_set_pull(gpio, 49, BCM2838_GPIO_GPPUD_PULLDOWN);

  /* GPIO_DAT0, GPIO_DAT1, GPIO_DAT2, GPIO_DAT3 */
  bcm2835_gpio_function_select(gpio, 50, BCM2835_GPIO_ALT3);
  bcm2835_gpio_function_select(gpio, 51, BCM2835_GPIO_ALT3);
  bcm2835_gpio_function_select(gpio, 52, BCM2835_GPIO_ALT3);
  bcm2835_gpio_function_select(gpio, 53, BCM2835_GPIO_ALT3);
  bcm2835_gpio_set_pull(gpio, 50, BCM2838_GPIO_GPPUD_PULLDOWN);
  bcm2835_gpio_set_pull(gpio, 51, BCM2838_GPIO_GPPUD_PULLDOWN);
  bcm2835_gpio_set_pull(gpio, 52, BCM2838_GPIO_GPPUD_PULLDOWN);
  bcm2835_gpio_set_pull(gpio, 53, BCM2838_GPIO_GPPUD_PULLDOWN);
}

static int bcmemmc_init(device_t *dev) {
  bcmemmc_state_t *state = (bcmemmc_state_t *)dev->state;
  resource_t *emmc = state->emmc;
  int cnt;

  state->host_version =
    (b_in(emmc, BCMEMMC_SLOTISR_VER) & HOST_SPEC_NUM) >> HOST_SPEC_NUM_SHIFT;
  /* Reset the controller. */
  b_out(emmc, BCMEMMC_CONTROL0, 0);
  b_set(emmc, BCMEMMC_CONTROL1, C1_SRST_HC);
  cnt = NSPINS;
  while (b_in(emmc, BCMEMMC_CONTROL1) & C1_SRST_HC) {
    if (--cnt < 0) {
      klog("ERROR: failed to reset EMMC");
      return ETIMEDOUT;
    }
    delay(30); /* TODO(mohrcore): Test it on hardware */
  }
  b_set(emmc, BCMEMMC_CONTROL1, C1_CLK_INTLEN | C1_TOUNIT_MAX);
  /* Set clock to setup frequency. */
  int error = bcmemmc_set_clk_freq(dev, BCMEMMC_INIT_FREQ);
  if (error)
    return error;
  /* Enable interrupts. */
  b_out(emmc, BCMEMMC_INT_EN,
        INT_CMD_DONE | INT_DATA_DONE | INT_READ_RDY | INT_WRITE_RDY |
          INT_CTO_ERR | INT_DTO_ERR);
  b_out(emmc, BCMEMMC_INT_MASK,
        INT_CMD_DONE | INT_DATA_DONE | INT_READ_RDY | INT_WRITE_RDY |
          INT_CTO_ERR | INT_DTO_ERR);

  return 0;
}

static int bcmemmc_probe(device_t *dev) {
  return dev->unit == 3;
}

DEVCLASS_DECLARE(emmc);

static int bcmemmc_attach(device_t *dev) {
  bcmemmc_state_t *state = (bcmemmc_state_t *)dev->state;

  state->gpio = device_take_memory(dev, 0, RF_ACTIVE);
  state->emmc = device_take_memory(dev, 1, RF_ACTIVE);

  spin_init(&state->lock, 0);
  cv_init(&state->intr_recv, "e.MMC command response wakeup");

  b_out(state->emmc, BCMEMMC_INTERRUPT, INT_ALL_MASK);
  emmc_gpio_init(dev);

  state->irq = device_take_irq(dev, 0, RF_ACTIVE);
  bus_intr_setup(dev, state->irq, bcmemmc_intr_filter, NULL, state,
                 "e.MMC interrupt");

  int error = bcmemmc_init(dev);
  if (error) {
    klog("e.MMC initialzation failed with code %d.", error);
    return ENXIO;
  }

  /* This is not a legitimate bus in a sense that it implements `DIF_BUS`, but
   * it should work nevertheless */
  device_t *child = device_add_child(dev, 0);
  child->bus = DEV_BUS_EMMC;
  child->devclass = &DEVCLASS(emmc);
  child->unit = 0;

  return bus_generic_probe(dev);
}

static emmc_methods_t bcmemmc_emmc_if = {
  .send_cmd = bcmemmc_send_cmd,
  .wait = bcmemmc_wait,
  .read = bcmemmc_read,
  .write = bcmemmc_write,
  .get_prop = bcmemmc_get_prop,
  .set_prop = bcmemmc_set_prop,
};

static driver_t bcmemmc_driver = {
  .desc = "e.MMC controller driver",
  .size = sizeof(bcmemmc_state_t),
  .probe = bcmemmc_probe,
  .attach = bcmemmc_attach,
  .pass = SECOND_PASS,
  .interfaces =
    {
      [DIF_EMMC] = &bcmemmc_emmc_if,
    },
};

DEVCLASS_ENTRY(root, bcmemmc_driver);
