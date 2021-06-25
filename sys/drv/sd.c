#define KL_LOG KL_DEV

/* This driver is based on SD Specifications Part 1: Physical Layer Simplified
 * Specification, Version 6.00. This document is a free resource, available
 * here (link split into two lines due to code formatting requirements):
 * https://www.taterli.com/wp-content/uploads/2017/05/
 *   Physical-Layer-Simplified-SpecificationV6.0.pdf */

#include <sys/mimiker.h>
#include <dev/emmc.h>
#include <sys/device.h>
#include <sys/devclass.h>
#include <sys/rman.h>
#include <sys/klog.h>
#include <sys/vnode.h>
#include <sys/devfs.h>
#include <dev/sd.h>

#define TRY(expr, error_var)                                                   \
  if (((error_var) = (expr)))                                                  \
  return error_var
#define TRYN(expr, error_var)                                                  \
  if (((error_var) = (expr)))                                                  \
  return -error_var
#define ASSIGN_OPTIONAL(ptr, val)                                              \
  if ((ptr))                                                                   \
  *(ptr) = (val)

typedef struct sd_state {
  sd_props_t props; /* SD Card's flags */
  void *block_buf;  /* A buffer for reading data into */
} sd_state_t;

static int sd_probe(device_t *dev) {
  return dev->unit == 0;
}

static const char standard_cap_str[] = "Ver 2.0 or later, Standard Capacity "
                                       "SD Memory Card";
static const char high_cap_str[] = "Ver 2.0 or later, High Capacity or Extended"
                                   " Capacity SD Memory Card";

static int sd_init(device_t *dev) {
  int error;
  sd_state_t *state = (sd_state_t *)dev->state;

  emmc_resp_t response;
  uint64_t propv;  /* Property's Value */
  size_t of = 0;   /* Offset (Used only for checking the number of bytes read */
  uint32_t scr[2]; /* SD Configuration Register */
  uint16_t rca;    /* Relative Card Address */

  state->props = 0;

  /* The routine below is based on SD Specifications: Part 1 Physical Layer
   * Simplified Specification, Version 6.0. See: page 30 */

  klog("Attaching SD/SDHC block device interface...");

  TRY(emmc_send_cmd(dev, EMMC_CMD(GO_IDLE), 0, NULL), error);
  if (emmc_get_prop(dev, EMMC_PROP_R_VOLTAGE_SUPPLY, &propv)) {
    klog("Unable to determine e.MMC controller's voltage supply.");
    return ENXIO;
  }
  uint8_t chkpat = ~propv + 1;
  if (emmc_set_prop(dev, EMMC_PROP_RW_RESP_LOW, chkpat - 1)) {
    klog("Unable to determine whether CMD8 responds.");
    return ENXIO;
  }
  TRY(emmc_send_cmd(dev, SD_CMD_SET_IF_COND, propv << 8 | chkpat, &response),
      error);
  if (SD_R7_CHKPAT(&response) != chkpat) {
    klog("SD 2.0 voltage supply is mismatched, or the card is at Version 1.x");
    return ENXIO;
  }

  /* Counter-intuitively, the busy bit is set ot 0 if the card is not ready */
  SD_ACMD41_RESP_SET_BUSY(&response, 0);
  int trial_cnt = 120; /* XXX: test it on a real hardware */
  while (trial_cnt & ~SD_ACMD41_RESP_READ_BUSY(&response)) {
    if (trial_cnt-- < 0) {
      klog("Card timedout on ACMD41 polling.");
      return ETIMEDOUT;
    }
    TRY(emmc_send_cmd(dev, SD_CMD_SEND_OP_COND, SD_ACMD41_SD2_0_POLLRDY_ARG1,
                      &response), error);
  }
  if (SD_ACMD41_RESP_READ_CCS(&response))
    state->props |= SD_SUPP_CCS;
  klog("e.MMC device detected as %s",
       (state->props & SD_SUPP_CCS) ? high_cap_str : standard_cap_str);

  /* Let's assume there's no voltage switching needed */

  TRY(emmc_send_cmd(dev, EMMC_CMD(ALL_SEND_CID), 0, NULL), error);
  TRY(emmc_send_cmd(dev, SD_CMD_SEND_REL_ADDR, 0, &response), error);
  rca = SD_R6_RCA(&response);
  TRY(emmc_set_prop(dev, EMMC_PROP_RW_RCA, rca), error);

  /* At this point we should have just enetered data transfer mode */

  if (emmc_set_prop(dev, EMMC_PROP_RW_CLOCK_FREQ, 25000000))
    klog("Failed to set e.MMC clock for SD card. Transfers might be slow.");

  TRY(emmc_send_cmd(dev, EMMC_CMD(SELECT_CARD), rca << 16, NULL), error);
  TRY(emmc_set_prop(dev, EMMC_PROP_RW_BLKSIZE, 8), error);
  TRY(emmc_set_prop(dev, EMMC_PROP_RW_BLKCNT, 1), error);

  TRY(emmc_send_cmd(dev, SD_CMD_SEND_SCR, 0, NULL), error);
  TRY(emmc_wait(dev, EMMC_I_READ_READY), error);
  TRY(emmc_read(dev, scr, 64, &of), error);
  TRY(emmc_wait(dev, EMMC_I_DATA_DONE), error);
  if (of != 64) {
    klog("Failed to read SD Card's SCR");
    return EIO;
  }

  if (scr[0] & SCR_SUPP_SET_BLKCNT)
    state->props |= SD_SUPP_BLKCNT;
  if (scr[0] & SCR_SD_BUS_WIDTH_4) {
    state->props |= SD_SUPP_BUSWIDTH_4;
    TRY(emmc_send_cmd(dev, SD_CMD_SET_BUS_WIDTH, SD_BUSWIDTH_4, NULL), error);
    TRY(emmc_set_prop(dev, EMMC_PROP_RW_BUSWIDTH, EMMC_BUSWIDTH_4), error);
  }

  klog("Card's feature support:\n* CCS: %s\n* SET_BLOCK_COUNT: %s",
       (state->props & SD_SUPP_CCS) ? "YES" : "NO",
       (state->props & SD_SUPP_BLKCNT) ? "YES" : "NO");

  return 0;
}

/* Read blocks from sd card. Returns 0 on success. */
static int sd_read_blk(device_t *dev, uint32_t lba, void *buffer, uint32_t num,
                       size_t *read) {
  sd_state_t *state = (sd_state_t *)dev->state;

  if (num < 1)
    return EINVAL;

  uint32_t *buf = (uint32_t *)buffer;
  int error;

  ASSIGN_OPTIONAL(read, 0);

  TRY(emmc_set_prop(dev, EMMC_PROP_RW_BLKCNT, num), error);
  TRY(emmc_set_prop(dev, EMMC_PROP_RW_BLKSIZE, DEFAULT_BLKSIZE), error);

  if (state->props & SD_SUPP_CCS) {
    /* Multiple block transfers either be terminated after a set amount of
     * block is transferred, or by sending CMD_STOP_TRANS command */
    if (num > 1 && (state->props & SD_SUPP_BLKCNT))
      TRY(emmc_send_cmd(dev, EMMC_CMD(SET_BLOCK_COUNT), num, NULL), error);
    
    if (num > 1) {
      TRY(emmc_send_cmd(dev, EMMC_CMD(READ_MULTIPLE_BLOCKS), lba, NULL), error);
    } else {
      TRY(emmc_send_cmd(dev, EMMC_CMD(READ_BLOCK), lba, NULL), error);
    }
    TRY(emmc_wait(dev, EMMC_I_READ_READY), error);
    TRY(emmc_read(dev, buf, num * DEFAULT_BLKSIZE, NULL), error);
    if ((num == 1) || (state->props & SD_SUPP_BLKCNT))
      TRY(emmc_wait(dev, EMMC_I_DATA_DONE), error);
    ASSIGN_OPTIONAL(read, num * DEFAULT_BLKSIZE);
  } else {
    for (uint32_t c = 0; c < num; c++) {
      /* See note no. 10 at page 222 of
       * SD Physical Layer Simplified Specification V6.0 */
      TRY(emmc_send_cmd(dev, EMMC_CMD(READ_BLOCK), (lba + c) * DEFAULT_BLKSIZE,
                        NULL), error);
      TRY(emmc_read(dev, buf, DEFAULT_BLKSIZE, NULL), error);
      TRY(emmc_wait(dev, EMMC_I_DATA_DONE), error);
      buf += DEFAULT_BLKSIZE / sizeof(uint32_t);
      ASSIGN_OPTIONAL(read, *read + DEFAULT_BLKSIZE);
    }
  }

  if (num > 1 && (~state->props & SD_SUPP_BLKCNT) &&
      (state->props & SD_SUPP_CCS))
    TRY(emmc_send_cmd(dev, EMMC_CMD(STOP_TRANSMISSION), 0, NULL), error);

  return 0;
}

/* Write blocks to the sd card. Returns 0 on success. */
static int sd_write_blk(device_t *dev, uint32_t lba, void *buffer, uint32_t num,
                        size_t *wrote) {
  sd_state_t *state = (sd_state_t *)dev->state;

  uint32_t *buf = (uint32_t *)buffer;
  int error;

  if (num < 1)
    return EINVAL;

  ASSIGN_OPTIONAL(wrote, 0);

  TRY(emmc_set_prop(dev, EMMC_PROP_RW_BLKCNT, num), error);
  TRY(emmc_set_prop(dev, EMMC_PROP_RW_BLKSIZE, DEFAULT_BLKSIZE), error);

  for (uint32_t c = 0; c < num; c++) {
    if (!(state->props & SD_SUPP_BLKCNT)) {
      uint32_t addr = lba + c;
      /* See note no. 10 at page 222 of
       * SD Physical Layer Simplified Specification V6.0 */
      if (!(state->props & SD_SUPP_CCS))
        addr *= DEFAULT_BLKSIZE;
      TRY(emmc_send_cmd(dev, EMMC_CMD(WRITE_BLOCK), addr, NULL), error);
      TRY(emmc_wait(dev, EMMC_I_WRITE_READY), error);
    }
    TRY(emmc_write(dev, buf, DEFAULT_BLKSIZE, NULL), error);
    TRY(emmc_wait(dev, EMMC_I_DATA_DONE), error);
    buf += 128;
    ASSIGN_OPTIONAL(wrote, *wrote + DEFAULT_BLKSIZE);
  }

  return 0;
}

static inline int sd_check_uio(uio_t *uio) {
  if (uio->uio_resid & (DEFAULT_BLKSIZE - 1))
    return EINVAL;
  if (uio->uio_offset & (DEFAULT_BLKSIZE - 1))
    return EINVAL;
  return 0;
}

typedef int (*sd_transfer_t)(device_t *dev, uint32_t lba, void *buffer,
                             uint32_t num, size_t *transferred);

static int sd_dop_uio(devnode_t *d, uio_t *uio) {
  device_t *dev = d->data;
  sd_state_t *state = (sd_state_t *)dev->state;
  int error;

  TRY(sd_check_uio(uio), error);

  uint32_t lba = uio->uio_offset / DEFAULT_BLKSIZE;
  uint32_t blk_cnt = uio->uio_resid / DEFAULT_BLKSIZE;

  sd_transfer_t transfer = uio->uio_op == UIO_READ ? sd_read_blk : sd_write_blk;

  for (uint32_t blocks_left = blk_cnt; blocks_left;
       blocks_left -= SD_KERNEL_BLOCKS) {
    uint32_t num = min(blocks_left, (uint32_t)SD_KERNEL_BLOCKS);
    TRY(transfer(dev, lba, state->block_buf, num, NULL), error);
    TRYN(uiomove_frombuf(state->block_buf, num * DEFAULT_BLKSIZE, uio), error);
    if (num < SD_KERNEL_BLOCKS)
      break;
  }

  return 0;
}

static devops_t sd_devops = {
  .d_read = sd_dop_uio,
  .d_write = sd_dop_uio,
};

static int sd_attach(device_t *dev) {
  int error;
  sd_state_t *state = (sd_state_t *)dev->state;

  state->block_buf = kmalloc(M_DEV, SD_KERNEL_BLOCKS * DEFAULT_BLKSIZE, 0);

  TRY(sd_init(dev), error);

  TRY(devfs_makedev_new(NULL, "sd_card", &sd_devops, dev, NULL), error);

  return 0;
}

static driver_t sd_block_device_driver = {
  .desc = "SD(SC/HC) block device driver",
  .size = sizeof(sd_state_t),
  .pass = SECOND_PASS,
  .probe = sd_probe,
  .attach = sd_attach,
};

DEVCLASS_ENTRY(emmc, sd_block_device_driver);