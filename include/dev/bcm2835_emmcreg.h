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

/* BLKSIZECNT register fields */
#define BSC_BLKCNT 0xffff0000
#define BSC_BLKCNT_SHIFT 16
#define BSC_BLKSIZE 0x000003ff

/* STATUS register fields */
#define SR_DAT_INHIBIT 0x00000002
#define SR_CMD_INHIBIT 0x00000001
#define SR_INHIBIT (SR_CMD_INHIBIT | SR_DAT_INHIBIT)

/* INTERRUPT register fields */
#define INT_ACMD_ERR 0x01000000  /* Auto command error */
#define INT_DEND_ERR 0x00400000  /* End bit on data line not 1 */
#define INT_DCRC_ERR 0x00200000  /* Data CRC error */
#define INT_DTO_ERR 0x00100000   /* Timeout on data line */
#define INT_CBAD_ERR 0x00080000  /* Incorrect command index in response */
#define INT_CEND_ERR 0x00040000  /* End bit on command line not 1 */
#define INT_CCRC_ERR 0x00020000  /* Command CRC error */
#define INT_CTO_ERR 0x00010000   /* Timeout on command line */
#define INT_ERR 0x00008000       /* An error has occured */
#define INT_ENDBOOT 0x00004000   /* Boot operation has terminated */
#define INT_BOOTACK 0x00002000   /* Boot acknowledge has been received */
#define INT_RETUNE 0x00001000    /* Clock retune request was made */
#define INT_CARD 0x00000100      /* Card made interrupt request */
#define INT_READ_RDY 0x00000020  /* DATA register contains data to be read */
#define INT_WRITE_RDY 0x00000010 /* Data can be written to DATA register */
#define INT_BLOCK_GAP 0x00000004 /* Data transfer has stopped at block gap */
#define INT_DATA_DONE 0x00000002 /* Data transfer has finished */
#define INT_CMD_DONE 0x00000001  /* Command has finished */

#define INT_ERROR_MASK 0x017F8000
#define INT_ALL_MASK 0x017FF137

/* CONTROL register fields */
#define C0_HCTL_DWITDH 0x00000002
#define C0_HCTL_8BIT 0x00000020

#define C1_SRST_HC 0x01000000
#define C1_TOUNIT_MAX 0x000e0000
#define C1_CLK_FREQ8 0x0000ff00
#define C1_CLK_FREQ8_SHIFT 8
#define C1_CLK_FREQ_MS2 0x000000c0
#define C1_CLK_FREQ_MS2_SHIFT 6
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

#endif
