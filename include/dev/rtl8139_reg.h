#ifndef _RTL8139_REG_H_
#define _RTL8139_REG_H_
/*
 * Copied from FreeBSD: sys/dev/rl/if_rlreg.h
 */

#define RL_TXSTAT0 0x0010 /* status of TX descriptor 0 */
#define RL_TXSTAT1 0x0014 /* status of TX descriptor 1 */
#define RL_TXSTAT2 0x0018 /* status of TX descriptor 2 */
#define RL_TXSTAT3 0x001C /* status of TX descriptor 3 */

#define RL_TXADDR0 0x0020 /* address of TX descriptor 0 */
#define RL_TXADDR1 0x0024 /* address of TX descriptor 1 */
#define RL_TXADDR2 0x0028 /* address of TX descriptor 2 */
#define RL_TXADDR3 0x002C /* address of TX descriptor 3 */

#define RL_RXADDR 0x0030  /* RX ring start address */
#define RL_COMMAND 0x0037 /* command register */
#define RL_IMR 0x003C     /* interrupt mask register */
#define RL_ISR 0x003E     /* interrupt status register */
#define RL_RXCFG 0x0044   /* receive config */

#define RL_8139_CFG1 0x0052 /* config register #1 */

/*
 * Interrupt status register bits.
 */
#define RL_ISR_RX_OK 0x0001
#define RL_ISR_TX_OK 0x0004

/*
 * Command register.
 */
#define RL_CMD_TX_ENB 0x0004
#define RL_CMD_RX_ENB 0x0008
#define RL_CMD_RESET 0x0010

/*
 * Config 1 register
 */
#define RL_CFG1_PWRON 0x00 /* power on */

#define RL_TIMEOUT (10 * 1000)
#endif /* !_RTL8139_REG_H_ */
