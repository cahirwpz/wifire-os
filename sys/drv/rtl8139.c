#define KL_LOG KL_DEV
#include <sys/klog.h>
#include <sys/mimiker.h>
#include <sys/pci.h>
#include <sys/interrupt.h>
#include <sys/errno.h>
#include <sys/libkern.h>
#include <sys/devfs.h>
#include <sys/vnode.h>
#include <sys/ringbuf.h>
#include <sys/pmap.h>
#include <sys/devclass.h>
#include <dev/rtl8139_reg.h>
#include <sys/kmem.h>

#define RTL8139_VENDOR_ID 0x10ec
#define RTL8139_DEVICE_ID 0x8139

typedef struct rtl8139_state {
  resource_t *regs;
  resource_t *irq_res;
  ringbuf_t rx_buf;
  ringbuf_t dev_rx_buf;
  paddr_t rx_buf_physaddr;
} rtl8139_state_t;

#define RL_PKTHDR_SIZE 4

typedef struct __packed {
  uint16_t __reserved;
  uint16_t len;
  uint8_t payload[];
} rtl8139_packet_t;

static int rtl8139_probe(device_t *dev) {
  pci_device_t *pcid = pci_device_of(dev);
  return pci_device_match(pcid, RTL8139_VENDOR_ID, RTL8139_DEVICE_ID);
}

static int rtl_reset(rtl8139_state_t *state) {
  bus_write_1(state->regs, RL_8139_CFG1, RL_CFG1_PWRON);
  bus_write_1(state->regs, RL_COMMAND, RL_CMD_RESET);
  for (int i = 0; i < RL_TIMEOUT; i++) {
    if (!(bus_read_1(state->regs, RL_COMMAND) & RL_CMD_RESET))
      return 0;
  }

  return EAGAIN;
}

static void do_receive(rtl8139_state_t *state) {
  /* the device insert their own header which contains size and status of packet
   */
  ringbuf_produce(&state->rx_buf, RL_PKTHDR_SIZE);
  rtl8139_packet_t pkt;
  ringbuf_getnb(&state->rx_buf, (uint8_t *)&pkt, RL_PKTHDR_SIZE);
  ringbuf_restorenb(&state->rx_buf, RL_PKTHDR_SIZE);
  size_t real_len = pkt.len + RL_PKTHDR_SIZE;
  ringbuf_produce(&state->rx_buf, real_len);
  /* TODO: SPINLOCK? */
  if (!ringbuf_movenb(&state->rx_buf, &state->dev_rx_buf, real_len)) {
    /* discard packet silently */
    ringbuf_consume(&state->rx_buf, real_len);
  }

  bus_write_2(state->regs, RL_ISR, RL_ISR_RX_OK);
}

static intr_filter_t rtl8139_intr(void *data) {
  rtl8139_state_t *state = data;
  uint16_t status = bus_read_2(state->regs, RL_ISR);

  if (status & RL_ISR_RX_OK) {
    do_receive(state);
    return IF_FILTERED;
  }

  return IF_STRAY;
}

static int rtl8139_read(vnode_t *v, uio_t *uio, int ioflag) {
  rtl8139_state_t *state = devfs_node_data(v);
  uio->uio_offset = 0;

  /* TODO: WITH_SPIN/MUTEX_LOCK ? */
  if (!ringbuf_empty(&state->dev_rx_buf)) {
    size_t uio_resid = uio->uio_resid;
    rtl8139_packet_t pkt;
    ringbuf_getnb(&state->dev_rx_buf, (uint8_t *)&pkt, RL_PKTHDR_SIZE);
    if (uio->uio_resid >= pkt.len) {
      uio->uio_resid = pkt.len;
    } else {
      /* cannot read part of packet - just consume it because the size of buffer
       should be at least of mtu size.
       in such cases (packet is larger than mtu) linux silently
       drops that packets */
      ringbuf_consume(&state->dev_rx_buf, pkt.len);
      return 0;
    }
    int res = ringbuf_read(&state->dev_rx_buf, uio);
    /* fix uio values */
    if (!res)
      uio->uio_resid = uio_resid - pkt.len;
    else
      uio->uio_resid = uio_resid;
    return res;
  } else
    return 0;
}

static vnodeops_t rtl8139_vnodeops = {.v_read = rtl8139_read};

static int rtl8139_attach(device_t *dev) {
  rtl8139_state_t *state = dev->state;
  int err = 0;

  pci_enable_busmaster(dev);

  ringbuf_init(&state->rx_buf,
               (void *)kmem_alloc_contig(&state->rx_buf_physaddr, RL_RXBUFLEN,
                                         PMAP_NOCACHE),
               RL_RXBUFLEN);
  if (!state->rx_buf.data) {
    klog("Failed to alloc memory for the receive buffer!");
    err = ENOMEM;
    goto fail;
  }
  state->regs = device_take_memory(dev, 1, RF_ACTIVE);
  if (!state->regs) {
    klog("Failed to init regs resource!");
    err = ENXIO;
    goto fail;
  }

  if ((err = rtl_reset(state))) {
    klog("Failed to reset device!");
    goto fail;
  }

  state->irq_res = device_take_irq(dev, 0, RF_ACTIVE);
  if (!state->irq_res) {
    klog("Failed to init irq resources!");
    err = ENXIO;
    goto fail;
  }
  bus_intr_setup(dev, state->irq_res, rtl8139_intr, NULL, state, "RTL8139");

  ringbuf_init(&state->dev_rx_buf, kmalloc(M_DEV, RL_RXBUFLEN, M_ZERO),
               RL_RXBUFLEN);
  devfs_makedev(NULL, "rtl8139", &rtl8139_vnodeops, state, NULL);

  /* set-up address for DMA */
  bus_write_4(state->regs, RL_RXADDR, state->rx_buf_physaddr);
  bus_write_4(state->regs, RL_RXCFG, RL_RXCFG_CONFIG);
  bus_write_1(state->regs, RL_COMMAND, RL_CMD_RX_ENB | RL_CMD_TX_ENB);
  /* IMR - mask rx enable */
  bus_write_2(state->regs, RL_IMR, RL_ISR_RX_OK);

  return 0;
fail:
  if (state->rx_buf.data)
    kmem_free((void *)state->rx_buf.data, RL_RXBUFLEN);

  return err;
}

static driver_t rtl8139_driver = {
  .desc = "RTL8139 driver",
  .size = sizeof(rtl8139_state_t),
  .pass = SECOND_PASS,
  .probe = rtl8139_probe,
  .attach = rtl8139_attach,
};

DEVCLASS_ENTRY(pci, rtl8139_driver);
