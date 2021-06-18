#define KL_LOG KL_DEV
#include <bitstring.h>
#include <sys/klog.h>
#include <sys/errno.h>
#include <sys/time.h>
#include <sys/bus.h>
#include <sys/kmem.h>
#include <sys/pmap.h>
#include <sys/sched.h>
#include <sys/interrupt.h>
#include <sys/devclass.h>
#include <sys/device.h>
#include <dev/usb.h>
#include <dev/usbhc.h>
#include <dev/dwc_otg.h>
#include <dev/dwc_otgreg.h>

/*
 * The following definitions assume an implicit `dotg` argument.
 */

/* Read the specified DWC OTG register. */
#define in(addr) bus_read_4(dotg->mem, (addr))

/* Write the specified DWC OTG register. */
#define out(addr, val) bus_write_4(dotg->mem, (addr), (val))

/* Set the specified bit in the given DWC OTG register. */
#define set(r, b) out((r), in(r) | (b))

/* Clear the specified bit in the given DWC OTG register. */
#define clr(r, b) out((r), in(r) & ~(b))

/* Clear the specified bit in the given DWC OTG write clear register. */
#define wclr(r, b) set((r), b)

/* Check if the specified bit is set in the given DWC OTG register. */
#define chk(r, b) ((in(r) & (b)) != 0)

static void dotg_callout(void *arg);
static intr_filter_t dotg_isr(void *arg);
static void dotg_thread(void *arg);

/*
 * Auxiliary functions.
 */

/* Wait for the specified bit in the given DWC OTG register to establish
 * the designated state. */
static int dotg_wait(dotg_state_t *dotg, int reg, uint32_t bit, uint8_t state,
                     systime_t ms) {
  mdelay(ms);
  if (chk(reg, bit) ^ state)
    return 1;
  return 0;
}

/* Convert host controller error flags into USB bus error flags. */
static usb_error_t dotge2usbe(uint32_t error) {
  usb_error_t uerr = 0;

  /* XXX: FTTB, we only distinguish a STALL condition. */
  if (error & HCINT_STALL)
    uerr |= USB_ERR_STALLED;

  /* Signal any other errors. */
  error &= ~HCINT_STALL;
  if (error)
    uerr |= USB_ERR_OTHER;

  return uerr;
}

/*
 * Host controller interface port functions.
 */

static uint8_t dotg_number_of_ports(device_t *dev) {
  /* DWC OTG host controller has only a single root hub port. */
  return 1;
}

static bool dotg_device_present(device_t *dev, uint8_t port) {
  assert(port == 0);
  dotg_state_t *dotg = dev->state;
  return chk(DOTG_HPRT, HPRT_PRTCONNSTS);
}

static usb_speed_t dotg_device_speed(device_t *dev, uint8_t port) {
  dotg_state_t *dotg = dev->state;
  int speed = HPRT_PRTSPD_GET(in(DOTG_HPRT));

  if (speed == HPRT_PRTSPD_LOW)
    return USB_SPD_LOW;
  else if (speed == HPRT_PRTSPD_FULL)
    return USB_SPD_FULL;

  assert(speed == HPRT_PRTSPD_HIGH);
  return USB_SPD_HIGH;
}

static void dotg_reset_port(device_t *dev, uint8_t port) {
  assert(port == 0);
  dotg_state_t *dotg = dev->state;

  set(DOTG_HPRT, HPRT_PRTRES);
  mdelay(USB_PORT_ROOT_RESET_DELAY_SPEC);
  clr(DOTG_HPRT, HPRT_PRTRES);
  mdelay(USB_PORT_RESET_RECOVERY_SPEC);

  /* Clear any pending host port status changes. */
  wclr(DOTG_HPRT, HPRT_PRTOVRCURRCHNG | HPRT_PRTENCHNG | HPRT_PRTCONNDET);
}

/*
 * Initialization functions.
 */

/* Initialize provided host channles. */
static void dotg_init_chans(dotg_state_t *dotg) {
  /* Obtain the number of provided host channels. */
  uint8_t nchans = GHWCFG2_NUMHSTCHNL_GET(in(DOTG_GHWCFG2));

  dotg->chan_map = kmalloc(M_DEV, bitstr_size(nchans), M_ZERO);
  assert(dotg->chan_map);
  dotg->chans = kmalloc(M_DEV, nchans * sizeof(dotg_chan_t), M_ZERO);
  assert(dotg->chans);

  for (uint8_t i = 0; i < nchans; i++) {
    dotg_chan_t *chan = &dotg->chans[i];

    /* Assing channel numbers. */
    dotg->chans[i].num = i;

    /* Initiate a callout for periodic requests. */
    chan->args = (dotg_callout_args_t){
      .dotg = dotg,
    };
    callout_setup(&chan->callout, dotg_callout, &chan->args);

    /* Allocate contiguous memory. */
    chan->buf = (void *)kmem_alloc_contig(
      &chan->buf_pa, HCTSIZ_XFERSIZE_MAX + 1, PMAP_NOCACHE);

    /* Enable basic channel interrupts. */
    set(DOTG_HCINTMSK(i), HCINTMSK_XFERCOMPLMSK | HCINTMSK_CHHLTDMSK |
                            HCINTMSK_AHBERRMSK | HCINTMSK_STALLMSK |
                            HCINTMSK_XACTERRMSK | HCINTMSK_BBLERRMSK |
                            HCINTMSK_FRMOVRUNMSK | HCINTMSK_DATATGLERRMSK);
  }

  dotg->nchans = nchans;
}

/*
 * Driver interface functions.
 */

static int dotg_probe(device_t *dev) {
  return dev->unit == 2;
}

/* Identify controller's release. */
static int dotg_check_release(dotg_state_t *dotg) {
  const char *str = NULL;

  switch (in(DOTG_GSNPSID)) {
    case DOTG_GSNPSID_REV_2_71a:
      str = "revision 2.71a";
      break;
    case DOTG_GSNPSID_REV_2_80a:
      str = "revision 2.80a";
      break;
    case DOTG_GSNPSID_REV_2_90a:
      str = "revision 2.90a";
      break;
    case DOTG_GSNPSID_REV_2_92a:
      str = "revision 2.92a";
      break;
    case DOTG_GSNPSID_REV_2_94a:
      str = "revision 2.94a";
      break;
    case DOTG_GSNPSID_REV_3_00a:
      str = "revision 3.00a";
      break;
    case DOTG_GSNPSID_REV_3_10a:
      str = "revision 3.10a";
      break;
    default:
      return 1;
  }

  klog("DWC OTG revision %s", str);
  return 0;
}

static int dotg_attach(device_t *dev) {
  dotg_state_t *dotg = dev->state;

  /*
   * Initialize the software context.
   */
  cv_init(&dotg->chan_cv, "channel available");
  spin_init(&dotg->chan_lock, 0);
  cv_init(&dotg->txn_cv, "pending transaction");
  spin_init(&dotg->txn_lock, 0);

  void *buf = kmalloc(M_DEV, DOTG_PNDGBUFSIZ, M_WAITOK);
  assert(buf);
  ringbuf_init(&dotg->txn_rb, buf, DOTG_PNDGBUFSIZ);

  dotg->mem = device_take_memory(dev, 0, RF_ACTIVE);
  assert(dotg->mem);

  if (dotg_check_release(dotg))
    return ENXIO;

  /* TODO: turn on the controller via video core (mail boxes). */

  /* Disable interrupts globally. */
  clr(DOTG_GAHBCFG, GAHBCFG_GLBLINTRMSK);

  /* Wait for the AHB master state machine to enter the IDLE condition. */
  if (dotg_wait(dotg, DOTG_GRSTCTL, GRSTCTL_AHBIDLE, 1, DOTG_AHBIDLE_DELAY))
    return ENXIO;

  /* Perform a soft reset of the core. */
  set(DOTG_GRSTCTL, GRSTCTL_CSFTRST);
  if (dotg_wait(dotg, DOTG_GRSTCTL, GRSTCTL_CSFTRST, 0, USB_BUS_RESET_DELAY))
    return ENXIO;

  /* Select the 8-bit UTMI+ interface. */
  clr(DOTG_GUSBCFG, GUSBCFG_ULPI_UTMI_SEL);
  clr(DOTG_GUSBCFG, GUSBCFG_PHYIF);

  /* Dsiable the Session Request Protocol (SRP). */
  clr(DOTG_GUSBCFG, GUSBCFG_SRPCAP);

  /* Disable the Host Negation Protocol (HNP). */
  clr(DOTG_GUSBCFG, GUSBCFG_HNPCAP);

  /* Use internal PHY charge pump to drive VBUS in ULPI PHY. */
  clr(DOTG_GUSBCFG, GUSBCFG_ULPIEXTVBUSDRV);

  dotg_init_chans(dotg);

  /* We rely on the internal DMA. */
  if (GHWCFG2_OTGARCH_GET(in(DOTG_GHWCFG2)) != GHWCFG2_OTGARCH_INTERNAL_DMA)
    return ENXIO;

  /* Set DMA burst length to 32 bits. */
  clr(DOTG_GAHBCFG, GAHBCFG_HBSTLEN_MASK);

  /* Select DMA mode as operation mode. */
  set(DOTG_GAHBCFG, GAHBCFG_DMAEN);

  /* experiment */
  set(DOTG_GAHBCFG, (1 << 4));
  clr(DOTG_GAHBCFG, (3 << 1));

  /* Restart the PHY clock. */
  out(DOTG_PCGCCTL, 0x00000000);

  /* Set PHY clock's frequency to 30/60 MHz. */
  clr(DOTG_HCFG, HCFG_FSLSPCLKSEL_MASK);

  /* Set receive FIFO depth. */
  out(DOTG_GRXFSIZ, DOTG_RXFSIZ);

  /* Set non-periodic transmit FIFO depth and start address. */
  out(DOTG_GNPTXFSIZ,
      (DOTG_NPTXFSIZ << GNPTXFSIZ_NPTXFDEP_SHIFT) | DOTG_RXFSIZ);

  /* Set periodic transmit FIFO depth and start address. */
  out(DOTG_HPTXFSIZ, (DOTG_PTXFSIZ << HPTXFSIZ_PTXFSIZE_SHIFT) |
                       (DOTG_RXFSIZ + DOTG_NPTXFSIZ));

  /* Flush receive FIFO. */
  set(DOTG_GRSTCTL, GRSTCTL_RXFFLSH);
  if (dotg_wait(dotg, DOTG_GRSTCTL, GRSTCTL_RXFFLSH, 0, DOTG_RXFFLSH_DELAY))
    return ENXIO;

  /* Flush all transmit FIFOs. */
  clr(DOTG_GRSTCTL, GRSTCTL_TXFNUM_MASK);
  set(DOTG_GRSTCTL, GRSTCTL_TXFIFO(DOTG_TXFMAXNUM) | GRSTCTL_TXFFLSH);
  if (dotg_wait(dotg, DOTG_GRSTCTL, GRSTCTL_TXFFLSH, 0, DOTG_TXFFLSH_DELAY))
    return ENXIO;

  /* Create and schedule the scheduling thread. */
  dotg->thread = thread_create("DWC OTG", dotg_thread, dotg,
                               prio_ithread(PRIO_ITHRD_QTY - 1));
  sched_add(dotg->thread);

  /* Acquire the HC interrupt and register an interrupt handler. */
  dotg->irq = device_take_irq(dev, 0, RF_ACTIVE);
  assert(dotg->irq);
  bus_intr_setup(dev, dotg->irq, dotg_isr, NULL, dotg, "DWC OTG");

  /* Clear pending interrupts. */
  wclr(DOTG_GINTSTS, GINTSTS_ALL);

  /* Enable host channel interrupt. */
  out(DOTG_GINTMSK, GINTMSK_HCHINTMSK);

  /* Enable interrupts globally. */
  set(DOTG_GAHBCFG, GAHBCFG_GLBLINTRMSK);

  uint32_t hprt = in(DOTG_HPRT);
  klog("%x", hprt);

  /* Initialize the underlying USB bus. */
  usb_init(dev);

  /* Detect and configure attached devices. */
  int error = usb_enumerate(dev);
  if (error)
    bus_intr_teardown(dev, dotg->irq);
  return error;
}

/*
 * Transaction stage handling functions.
 */

/* Allocate a new stage. */
static dotg_stage_t *dotg_stage_alloc(dotg_stg_type_t type, void *data,
                                      size_t size, usb_endpt_t *endpt,
                                      usb_direction_t dir) {
  dotg_stage_t *stage = kmalloc(M_DEV, sizeof(dotg_stage_t), M_WAITOK);
  assert(stage);

  stage->data = data;
  stage->size = size;
  stage->type = type;
  stage->dir = dir;

  /* Obtain number of packets needed to transform the stage towards
   * the destination endpoint. */
  stage->pktcnt = roundup(stage->size, endpt->maxpkt) / endpt->maxpkt;
  assert(stage->pktcnt <= HCTSIZ_PKTCNT_MAX);

  /* Obtain the corresponding PID. */
  if (type == DOTG_STG_SETUP) {
    stage->pid = DOTG_PID_SETUP;
  } else if (type == DOTG_STG_DATA) {
    if (!endpt->addr)
      stage->pid = DOTG_PID_DATA1;
    else
      stage->pid = DOTG_PID_DATA0;
  } else {
    assert(type == DOTG_STG_STATUS);
    stage->pid = DOTG_PID_DATA1;
  }

  return stage;
}

/* Release a stage. */
static void dotg_stage_free(dotg_stage_t *stage) {
  kfree(M_DEV, stage);
}

/*
 * Transaction handling functions.
 */

/* Alloc a new transaction. */
static dotg_txn_t *dotg_txn_alloc(usb_device_t *udev, usb_buf_t *buf) {
  dotg_txn_t *txn = kmalloc(M_DEV, sizeof(dotg_txn_t), M_ZERO);
  assert(txn);

  txn->udev = udev;
  txn->buf = buf;
  txn->status = DOTG_TXN_UNKNOWN;

  return txn;
}

static void dotg_txn_free(dotg_txn_t *txn) {
  assert(txn->status == DOTG_TXN_FINISHED);
  assert(txn->curr < txn->nstages);

  /* Release the accommodated stages. */
  for (uint8_t i = 0; i < txn->nstages; i++)
    dotg_stage_free(txn->stages[i]);

  kfree(M_DEV, txn);
}

/* Add stage `stage` to transaction `txn`. */
static void dotg_txn_add_stage(dotg_txn_t *txn, dotg_stage_t *stage) {
  txn->stages[txn->nstages++] = stage;
}

/*
 * Host controller interface transfer functions.
 */

/* Add transaction `txn` to the scheduling queue. */
static void dotg_schedule(dotg_state_t *dotg, dotg_txn_t *txn) {
  SCOPED_SPIN_LOCK(&dotg->txn_lock);

  if (!ringbuf_putnb(&dotg->txn_rb, (void *)&txn, sizeof(dotg_txn_t *)))
    panic("DWC OTG pending transaction queue too small!");
  txn->status = DOTG_TXN_PENDING;

  /* Signal a pending transaction. */
  cv_signal(&dotg->txn_cv);
}

static void dotg_control_transfer(device_t *hcdev, device_t *dev,
                                  usb_buf_t *buf, usb_dev_req_t *req,
                                  usb_direction_t status_dir) {
  assert(buf->transfer_size + sizeof(usb_dev_req_t) <= HCTSIZ_XFERSIZE_MAX);

  dotg_state_t *dotg = hcdev->state;
  usb_device_t *udev = usb_device_of(dev);
  usb_endpt_t *endpt = buf->endpt;
  dotg_txn_t *txn = dotg_txn_alloc(udev, buf);

  /* Create the stetup stage. */
  dotg_stage_t *stage = dotg_stage_alloc(
    DOTG_STG_SETUP, req, sizeof(usb_dev_req_t), endpt, USB_DIR_OUTPUT);
  dotg_txn_add_stage(txn, stage);

  /* Create the data stage. */
  if (buf->transfer_size) {
    stage = dotg_stage_alloc(DOTG_STG_DATA, buf->data, buf->transfer_size,
                             endpt, endpt->dir);
    dotg_txn_add_stage(txn, stage);
  }

  /* Create the status stage. */
  stage = dotg_stage_alloc(DOTG_STG_STATUS, NULL, 0, endpt, status_dir);
  dotg_txn_add_stage(txn, stage);

  /* Schedule the transaction. */
  dotg_schedule(dotg, txn);
}

static void dotg_data_transfer(device_t *hcdev, device_t *dev, usb_buf_t *buf) {
  assert(buf->transfer_size <= HCTSIZ_XFERSIZE_MAX);
  assert(buf->transfer_size);

  dotg_state_t *dotg = hcdev->state;
  usb_device_t *udev = usb_device_of(dev);
  usb_endpt_t *endpt = buf->endpt;
  dotg_txn_t *txn = dotg_txn_alloc(udev, buf);

  /* Create the data stage. */
  dotg_stage_t *stage = dotg_stage_alloc(DOTG_STG_DATA, buf->data,
                                         buf->transfer_size, endpt, endpt->dir);
  dotg_txn_add_stage(txn, stage);

  /* Schedule the transaction. */
  dotg_schedule(dotg, txn);
}

/*
 * Host channel handling functions.
 */

/* Allocate a free host channel. */
static dotg_chan_t *dotg_alloc_chan(dotg_state_t *dotg) {
  assert(spin_owned(&dotg->chan_lock));

  /* Find first free host channel. */
  int idx;
  bit_ffc(dotg->chan_map, dotg->nchans, &idx);
  if (idx < 0)
    return NULL;

  bit_set(dotg->chan_map, idx);
  return &dotg->chans[idx];
}

/* Release a host channel. */
static void dotg_free_chan(dotg_state_t *dotg, dotg_chan_t *chan) {
  SCOPED_SPIN_LOCK(&dotg->chan_lock);

  int idx = chan->num;
  assert(bit_test(dotg->chan_map, idx));
  bit_clear(dotg->chan_map, idx);

  /* Signal an available channel. */
  cv_signal(&dotg->chan_cv);
}

/* Enable the interrupt corresponding to channel `chan`. */
static void dotg_enable_chan_intr(dotg_state_t *dotg, dotg_chan_t *chan) {
  SCOPED_SPIN_LOCK(&dotg->chan_lock);

  set(DOTG_HAINTMSK, 1 << chan->num);
}

/* Disable the interrupt corresponding to channel `chan`. */
static void dotg_disable_chan_intr(dotg_state_t *dotg, dotg_chan_t *chan) {
  SCOPED_SPIN_LOCK(&dotg->chan_lock);

  clr(DOTG_HAINTMSK, 1 << chan->num);
}

/* Enable channel `chan`. */
static void dotg_enable_chan(dotg_state_t *dotg, dotg_chan_t *chan) {
  int chan_chars = DOTG_HCCHAR(chan->num);
  uint32_t value = in(chan_chars);
  out(chan_chars, (value | HCCHAR_CHENA) & ~HCCHAR_CHDIS);
}

/* Disable channel `chan` in oreder to stop any current transactions
 * along the channel and prepare for next transaction. */
static void dotg_disable_chan(dotg_state_t *dotg, dotg_chan_t *chan) {
  dotg_txn_t *txn = chan->txn;
  assert(txn);
  assert(txn->status == DOTG_TXN_PENDING);

  /* We'll need to wait for an interrupt indicating that the channel
   * has been disabled, so mark the corresponding channed as waiting
   * for that event. */
  txn->status = DOTG_TXN_DISABLING_CHAN;

  int chan_chars = DOTG_HCCHAR(chan->num);
  uint32_t value = in(chan_chars);
  out(chan_chars, (value & ~HCCHAR_CHENA) | HCCHAR_CHDIS);
}

/*
 * Host channel transfer functions.
 */

static void dotg_transfer_stage(dotg_state_t *dotg, dotg_chan_t *chan);

/* Initiate transaction along channel `chan`. */
static void dotg_init_transfer(dotg_state_t *dotg, dotg_chan_t *chan) {
  dotg_txn_t *txn = chan->txn;
  assert(txn);
  assert(txn->nstages);
  assert(!txn->curr);

  /* Mark the transaction as waiting for data transfer. */
  txn->status = DOTG_TXN_TRANSFERING;

  /* Transfer the first stage of the transaction. */
  dotg_transfer_stage(dotg, chan);
}

/* Terminate the current transaction of channel `chan`. */
static void dotg_end_transfer(dotg_state_t *dotg, dotg_chan_t *chan) {
  dotg_txn_t *txn = chan->txn;
  assert(txn);

  /* Mark the transaction as finished. */
  txn->status = DOTG_TXN_FINISHED;

  /* Release the transaction. */
  dotg_txn_free(txn);

  /* Disable channel's interrupt. */
  dotg_disable_chan_intr(dotg, chan);

  /* Release the host channel. */
  dotg_free_chan(dotg, chan);
}

/* Callout for rescheduling periodic transactions. */
static void dotg_callout(void *arg) {
  dotg_callout_args_t *args = arg;
  dotg_state_t *dotg = args->dotg;
  dotg_chan_t *chan = container_of(args, dotg_chan_t, args);

  dotg_init_transfer(dotg, chan);
}

/* Prepare the channel's periodic transfer to be reissued after enpoint's
 * time interval. */
static void dotg_reactivate_transfer(dotg_state_t *dotg, dotg_chan_t *chan) {
  dotg_txn_t *txn = chan->txn;
  assert(txn);
  assert(txn->status == DOTG_TXN_FINISHED);

  /* Reset transaction's state. */
  txn->curr = 0;

  usb_endpt_t *endpt = txn->buf->endpt;
  assert(endpt);

  /* Schedule a callout to reissue the transaction. */
  callout_schedule(&chan->callout, endpt->interval);
}

/* Transfer a single stage of the transaction
 * corresponding to channel `chan`. */
static void dotg_transfer_stage(dotg_state_t *dotg, dotg_chan_t *chan) {
  dotg_txn_t *txn = chan->txn;
  assert(txn);
  assert(txn->status == DOTG_TXN_TRANSFERING);

  dotg_stage_t *stage = txn->stages[txn->curr];
  assert(stage);

  /* Discard any pending channel interrupts. */
  wclr(DOTG_HCINT(chan->num), HCINT_ALL);

  /*
   * Host channel transfer size register contains:
   * - PID used for the initial transaction
   * - number of packets to be transmitted
   * - number of bytes to send during the transfer
   */
  int chan_xfersiz = DOTG_HCTSIZ(chan->num);
  out(chan_xfersiz, (stage->pid << HCTSIZ_PID_SHIFT) |
                      (stage->pktcnt << HCTSIZ_PKTCNT_SHIFT) | stage->size);

  /*
   * Host channel characteristics register contains:
   * - channel enable/disable bits
   * - device address
   * - multi count
   * - endpoint type
   * - device speed indicator
   * - endpoint direction
   * - endpoint number (within the device)
   * - maximum packet size
   */
  int chan_chars = DOTG_HCCHAR(chan->num);
  usb_device_t *udev = txn->udev;
  usb_endpt_t *endpt = txn->buf->endpt;
  uint32_t speed = (udev->speed == USB_SPD_LOW ? HCCHAR_LSPDDEV : 0);
  out(chan_chars, (udev->addr << HCCHAR_DEVADDR_SHIFT) |
                    (DOTG_MC << HCCHAR_MC_SHIFT) |
                    ((endpt->transfer - 1) << HCCHAR_EPTYPE_SHIFT) | speed |
                    (stage->dir << HCCHAR_EPDIR_SHIFT) |
                    (endpt->addr << HCCHAR_EPNUM_SHIFT) | endpt->maxpkt);

  /* We don't perform any spliting. */
  out(DOTG_HCSPLT(chan->num), 0x00000000);

  /* Copy the data into channel's DMA buffer. */
  if (stage->dir == USB_DIR_OUTPUT)
    memcpy(chan->buf, stage->data, stage->size);

  /* Set host channel DMA address register. */
  out(DOTG_HCDMA(chan->num), chan->buf_pa);

  /* The transaction will be performed after the channel is enabled. */
  dotg_enable_chan(dotg, chan);
}

/*
 * Scheduling thread.
 */

static void dotg_thread(void *arg) {
  dotg_state_t *dotg = arg;
  dotg_txn_t *txn = NULL;
  dotg_chan_t *chan = NULL;

  for (;;) {
    /* Get a pending transaction. */
    WITH_SPIN_LOCK (&dotg->txn_lock) {
      while (!ringbuf_getnb(&dotg->txn_rb, (void *)&txn, sizeof(dotg_txn_t *)))
        cv_wait(&dotg->txn_cv, &dotg->txn_lock);
    }
    assert(txn->status == DOTG_TXN_PENDING);

    /* Acquire a free host channel. */
    WITH_SPIN_LOCK (&dotg->chan_lock) {
      while (!(chan = dotg_alloc_chan(dotg)))
        cv_wait(&dotg->chan_cv, &dotg->chan_lock);
    }

    /* Couple the transaction with the channel. */
    chan->txn = txn;

    /* Enable channel interrupt. */
    dotg_enable_chan_intr(dotg, chan);

    /* Flush the channel if needed. */
    if (chk(DOTG_HCCHAR(chan->num), HCCHAR_CHENA)) {
      dotg_disable_chan(dotg, chan);
    } else {
      dotg_init_transfer(dotg, chan);
    }
  }
}

/*
 * Intrrupt handling functions.
 */

/* Process host channel `chan`.
 * XXX: FTTB, we don't sense the shortcut condition. */
static void dotg_process_chan(dotg_state_t *dotg, dotg_chan_t *chan) {
  dotg_txn_t *txn = chan->txn;
  assert(txn);

  dotg_txn_status_t status = txn->status;
  assert(status == DOTG_TXN_DISABLING_CHAN || status == DOTG_TXN_TRANSFERING);

  if (status == DOTG_TXN_DISABLING_CHAN) {
    /* We're ready for the transfer now. */
    dotg_init_transfer(dotg, chan);
    return;
  }

  usb_buf_t *buf = txn->buf;
  assert(buf);

  /* Host channel interrupt register. */
  uint32_t chan_intr = in(DOTG_HCINT(chan->num));

  /* If an error has occured, signal the error and discard the transfer. */
  if (chan_intr & HCINT_ERROR) {
    usb_error_t uerr = dotge2usbe(chan_intr);
    usb_buf_process(buf, NULL, uerr);
    dotg_end_transfer(dotg, chan);
    return;
  }

  /* Restart a halted stage. */
  if (chan_intr == HCINT_CHHLTD) {
    dotg_transfer_stage(dotg, chan);
    return;
  }

  /* If there's any stage left, proceed. */
  if (txn->curr != txn->nstages - 1) {
    txn->curr++;
    dotg_transfer_stage(dotg, chan);
    return;
  }

  /* The data in channel's buffer must be from the data stage,
   * so hand it to the user. */
  usb_buf_process(buf, chan->buf, 0);

  /* We need to reactivate periodic transfers. */
  if (usb_buf_periodic(buf)) {
    txn->status = DOTG_TXN_FINISHED;
    dotg_reactivate_transfer(dotg, chan);
  } else {
    dotg_end_transfer(dotg, chan);
  }
}

static intr_filter_t dotg_isr(void *arg) {
  dotg_state_t *dotg = arg;

  /* Should we bother about it? */
  if (!chk(DOTG_GINTSTS, GINTSTS_HCHINT))
    return IF_STRAY;

  /*
   * Host all channels interrupt register contains:
   * - bit n: pending interrupt on channel `n`
   */
  uint32_t chans_intr = in(DOTG_HAINT);

  /* Process each host channel. */
  for (uint8_t i = 0; i < dotg->nchans; i++) {
    /* If there's no interrupt pending, let's move on to the next channel. */
    if (!(chans_intr & (1 << i)))
      continue;

    /* Process the channel. */
    dotg_process_chan(dotg, &dotg->chans[i]);

    /* Clear channel's interrupts. */
    wclr(DOTG_HCINT(i), HCINT_ALL);
  }

  /* Mark the interrupt as handled. */
  wclr(DOTG_GINTSTS, GINTSTS_ALL);

  return IF_FILTERED;
}

static usbhc_methods_t dotg_usbhc_if = {
  .number_of_ports = dotg_number_of_ports,
  .device_present = dotg_device_present,
  .device_speed = dotg_device_speed,
  .reset_port = dotg_reset_port,
  .control_transfer = dotg_control_transfer,
  .data_transfer = dotg_data_transfer,
};

static driver_t dotg_driver = {
  .desc = "DWC OTG driver",
  .size = sizeof(dotg_state_t),
  .pass = SECOND_PASS,
  .probe = dotg_probe,
  .attach = dotg_attach,
  .interfaces =
    {
      [DIF_USBHC] = &dotg_usbhc_if,
    },
};

DEVCLASS_ENTRY(root, dotg_driver);
