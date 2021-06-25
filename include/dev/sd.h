#ifndef _DEV_SD_H_
#define _DEV_SD_H_

/* Must be a power of two */
#define DEFAULT_BLKSIZE 512
#define SD_KERNEL_BLOCKS 4 /* Number of blocks buffered in kernel memory */

/* The custom R7 response is handled just like R1 response, but has different
 * bitfields, same goes for R6 */
#define SDRESP_R7 EMMCRESP_R1
#define SD_R7_CHKPAT(resp) EMMC_FMASK48(resp, 0, 8)

#define SDRESP_R6 EMMCRESP_R1
#define SD_R6_RCA(resp) EMMC_FMASK48(resp, 16, 16)

/* Custom commands */
#define SD_CMD_SET_IF_COND                                                     \
  (emmc_cmd_t) {                                                               \
    .cmd_idx = 8, .flags = 0, .exp_resp = SDRESP_R7,                           \
  }

#define SD_CMD_SEND_OP_COND                                                    \
  (emmc_cmd_t) {                                                               \
    .cmd_idx = 41, .flags = EMMC_F_APP, .exp_resp = EMMCRESP_R1,               \
  }

#define SD_CMD_SEND_SCR                                                        \
  (emmc_cmd_t) {                                                               \
    .cmd_idx = 51, .flags = EMMC_F_APP | EMMC_F_DATA_READ,                     \
    .exp_resp = EMMCRESP_R1,                                                   \
  }

#define SD_CMD_SEND_REL_ADDR                                                   \
  (emmc_cmd_t) {                                                               \
    .cmd_idx = 3, .flags = 0, .exp_resp = SDRESP_R6,                           \
  }

#define SD_CMD_SET_BUS_WIDTH                                                   \
  (emmc_cmd_t) {                                                               \
    .cmd_idx = 6, .flags = EMMC_F_APP, .exp_resp = EMMCRESP_R1,                \
  }

/* Custom response fields extractors */
#define SD_ACMD41_SD2_0_POLLRDY_ARG1 0x51ff8000
#define SD_ACMD41_RESP_BUSY_OFFSET 31
#define SD_ACMD41_RESP_BUSY_WIDTH 1
#define SD_ACMD41_RESP_CCS_OFFSET 30
#define SD_ACMD41_RESP_CCS_WIDTH 1
#define SD_ACMD41_RESP_UHSII_OFFSET 29
#define SD_ACMD41_RESP_UHSII_WIDTH 1
#define SD_ACMD41_RESP_SW18A_OFFSET 24
#define SD_ACMD41_RESP_SW18A_WIDTH 1

#define SD_ACMD41_RESP_SET_BUSY(r, b)                                          \
  EMMC_FMASK48_WR((r), SD_ACMD41_RESP_BUSY_OFFSET, SD_ACMD41_RESP_BUSY_WIDTH,  \
                  (b))
#define SD_ACMD41_RESP_READ_BUSY(r)                                            \
  EMMC_FMASK48((r), SD_ACMD41_RESP_BUSY_OFFSET, SD_ACMD41_RESP_BUSY_WIDTH)
#define SD_ACMD41_RESP_READ_CCS(r)                                             \
  EMMC_FMASK48((r), SD_ACMD41_RESP_CCS_OFFSET, SD_ACMD41_RESP_CCS_WIDTH)

/* SCR flags */
#define SCR_SD_BUS_WIDTH_4 0x00000400
#define SCR_SUPP_SET_BLKCNT 0x02000000

#define SD_BUSWIDTH_1 0x00
#define SD_BUSWIDTH_4 0x02

typedef enum sd_props {
  SD_SUPP_CCS = 1,
  SD_SUPP_BLKCNT = 2,
  SD_SUPP_BUSWIDTH_4 = 4,
} sd_props_t;

#endif