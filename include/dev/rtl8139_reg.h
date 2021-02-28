#ifndef _RTL8139_REG_H_
#define _RTL8139_REG_H_
/*
 * Copied from FreeBSD: sys/dev/rl/if_rlreg.h
 */

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

#define RL_RXBUFLEN 0x2000
/* we want to receive all packets - promiscious mode
 * 0xf - accept broadcast, multicast, physical match, all other packets
 * (promisc) */
/* 0x80 - wrap bit */
#define RL_RXCFG_CONFIG 0x8f

#define RL_TIMEOUT (10 * 1000)
#endif /* !_RTL8139_REG_H_ */
