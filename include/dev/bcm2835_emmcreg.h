#ifndef _BCM2835_EMMCREG_H_
#define _BCM2835_EMMCREG_H_

/* Registers */
#define BCMEMMC_ARG2 0x0000
#define BCMEMMC_BLKSIZECNT 0x0004
#define BCMEMMC_ARG1 0x0008
#define BCMEMMC_CMDTM 0x000C
#define BCMEMMC_RESP0 0x0010
#define BCMEMMC_RESP1 0x0014
#define BCMEMMC_RESP2 0x0018
#define BCMEMMC_RESP3 0x001C
#define BCMEMMC_DATA 0x0020
#define BCMEMMC_STATUS 0x0024
#define BCMEMMC_CONTROL0 0x0028
#define BCMEMMC_CONTROL1 0x002C
#define BCMEMMC_INTERRUPT 0x0030
#define BCMEMMC_INT_MASK 0x0034
#define BCMEMMC_INT_EN 0x0038
#define BCMEMMC_CONTROL2 0x003C
#define BCMEMMC_SLOTISR_VER 0x00FC

/* STATUS register fields */
#define SR_DAT_INHIBIT 0x00000002
#define SR_CMD_INHIBIT 0x00000001

/* INTERRUPT register fields */
#define INT_DATA_TIMEOUT 0x00100000
#define INT_CMD_TIMEOUT 0x00010000
#define INT_READ_RDY 0x00000020
#define INT_WRITE_RDY 0x00000010
#define INT_DATA_DONE 0x00000002
#define INT_CMD_DONE 0x00000001

#define INT_ERROR_MASK 0x017E8000

/* CONTROL register fields */
#define C0_HCTL_DWITDH 0x00000002
#define C0_HCTL_8BIT 0x00000020

#define C1_SRST_HC 0x01000000
#define C1_TOUNIT_MAX 0x000e0000
#define C1_CLK_EN 0x00000004
#define C1_CLK_STABLE 0x00000002
#define C1_CLK_INTLEN 0x00000001

/* SLOTISR_VER values */
#define HOST_SPEC_NUM 0x00ff0000
#define HOST_SPEC_NUM_SHIFT 16
#define HOST_SPEC_V3 2
#define HOST_SPEC_V2 1
#define HOST_SPEC_V1 0

/* This seems to be the default frequency of the clk_emmc.
 * Preferably, it should be somewhere between 50MHz and 100MHz, but changing
 * it requires messing around with Clock Manager, which at the moment is beyond
 * the scope of this driver.
 */
#define GPIO_CLK_EMMC_DEFAULT_FREQ 41666666

/* (inverted) Clock divisor mask for BCMEMMC_CONTROL1 */
#define BCMEMMC_CLKDIV_INVMASK 0xffff003f

/* Initial clocking frequency for the e.MMC controller */
#define BCMEMMC_INIT_FREQ 400000

/* Flags for command register */
#define CMD_TYPE_SUSPEND 0x00400000
#define CMD_TYPE_RESUME 0x00800000
#define CMD_TYPE_ABORT 0x00c00000
#define CMD_DATA_TRANSFER 0x00200000
#define CMD_DATA_READ 0x00000010
#define CMD_DATA_MULTI 0x00000020
#define CMD_CHECKCRC 0x00080000
#define CMD_CHECKIDX 0x00100000
#define CMD_RESP136 0x00010000
#define CMD_RESP48 0x00020000
#define CMD_RESP48B 0x00030000

/* Spooky GPIO stuff */
#define GPHEN1 0x0068

#endif