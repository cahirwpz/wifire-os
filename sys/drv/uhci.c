/*
 * UHCI host controller PCI deriver.
 *
 * For explanation of terms used throughout the code
 * please see the following documents:
 *
 * - Universal Host Controller Interface (UHCI) Design Guide, March 1996:
 *     ftp://ftp.netbsd.org/pub/NetBSD/misc/blymn/uhci11d.pdfunit
 *
 * - Universal Serial Bus Specification Revision 2.0, April 27, 2000:
 *     http://sdpha2.ucsd.edu/Lab_Equip_Manuals/usb_20.pdf
 *
 * Each inner function if given a description. For description
 * of the rest of contained functions please see `include/dev/usbhc.h`.
 */
#define KL_LOG KL_DEV
#include <sys/errno.h>
#include <sys/bus.h>
#include <sys/devclass.h>
#include <sys/libkern.h>
#include <sys/klog.h>
#include <sys/kmem.h>
#include <sys/pmap.h>
#include <sys/pool.h>
#include <sys/time.h>
#include <dev/pci.h>
#include <dev/usb.h>
#include <dev/usbhc.h>
#include <dev/uhci.h>
#include <dev/uhcireg.h>

/* Number of main queues.
 * Available shedule intervals are: 1, 2, ..., 2^(`UHCI_NMAINQS` - 1). */
#define UHCI_NMAINQS 5
#define UHCI_MAX_INTERVAL (1 << (UHCI_NMAINQS - 1))

typedef struct uhci_state {
  uhci_qh_t *mainqs[UHCI_NMAINQS]; /* main queues (i.e. schedule queues) */
  uint32_t *frames;                /* UHCI frame list */
  resource_t *regs;                /* host controller registers */
  resource_t *irq;                 /* host controller interrupt */
  uint8_t nports;                  /* number of root hub ports */
} uhci_state_t;

/*
 * For each transfer we need a contiguous memory range to compose appropriate
 * UCHI specific structures which state a USB transaction. Besides that,
 * a transfer also needs another contiguous span for I/O data to transfer.
 * To meet these requirements we supply two pools:
 * - `P_TFR`  - memory buffers for UHCI transfer constructs,
 * - `P_DATA` - memory buffers for I/O data to transfer.
 */

#define UHCI_DATA_BUF_SIZE 512

#define UHCI_UNIT_SIZE max(sizeof(uhci_qh_t), sizeof(uhci_td_t))
#define UHCI_TFR_BUF_SIZE                                                      \
  (UHCI_DATA_BUF_SIZE / USB_MAX_IPACKET * UHCI_UNIT_SIZE)

#define UHCI_MAX_NREQS 16 /* maximum number of scheduled request */

#define UHCI_DATA_POOL_SIZE (UHCI_MAX_NREQS * UHCI_DATA_BUF_SIZE)

#define UHCI_TFR_POOL_SIZE (UHCI_MAX_NREQS * UHCI_TFR_BUF_SIZE)

#define UHCI_ALIGNMENT max(UHCI_TD_ALIGN, UHCI_QH_ALIGN)

static POOL_DEFINE(P_TFR, "UHCI transfer buffers", UHCI_TFR_BUF_SIZE,
                   UHCI_ALIGNMENT);
static POOL_DEFINE(P_DATA, "UHCI data  buffers", UHCI_DATA_BUF_SIZE);

/*
 * How do we manage the UHCI frame list?
 *
 * We have `UHCI_NMAINQS` main queues. We'd like main queue number n to execute
 * every 2^n ms. In order to achive this goal we apply the following schema:
 * 1. We chain all the main queues in the following way:
 *
 *             ------------------             --------     --------
 *             |      MQ        | --> ... --> |  MQ  | --> |  MQ  |
 *             | UHCI_NMAINQS-1 |             |  #1  |     |  #0  |
 *             ------------------             --------     --------
 *
 * 2. We point frame list pointer number k at the main queue which number is
 * a log of the smallest power of 2 that divides k + 1, or main queue number
 * `UHCI_NMAINQS` - 1 if the resulting number is too large. This yields the
 * following layout:
 *
 *        frame pointers                             main queues
 *
 *  start ---------------
 *        |   frame 0   | -->                                            MQ#0
 *        ---------------
 *        |   frame 1   | -->                                   MQ#1 --> MQ#0
 *        ---------------
 *        |   frame 2   | -->                                            MQ#0
 *        ---------------
 *        |   frame 3   | -->                          MQ#2 --> MQ#1 --> MQ#0
 *        ---------------
 *        |   frame 4   | -->                                            MQ#0
 *        ---------------
 *        |   frame 5   | -->                                   MQ#1 --> MQ#0
 *        ---------------
 *        |   frame 6   | -->                                            MQ#0
 *        ---------------
 *        |             |
 *              ...
 *        |             |
 *        ---------------
 *        | frame 1023  | -->    MQ        --> ... --> MQ#2 --> MQ#1 --> MQ#0
 *        ---------------   #UHCI_NMAINQS-1
 *
 *
 * How do we schedule a UHCI transfer?
 *
 * Each main queue contains a list of scheduled transfers.
 * With every transfer request we are given an interval which tells when to
 * execute the transfer. We round down the given interval to the nearest
 * value corresponding to the period of some of the supplied main queues,
 * and then we append the transfer to the main queue.
 *
 * Conceptual drawing:
 *
 *             --------     --------
 *  main   --> |  MQ  | --> |  MQ  | --> ...
 *  queues     |  #i  |     | #i-1 |
 *             --------     --------
 *                |
 *                |
 *                v
 *             ------------     ------------              ------------
 *  scheduled  | transfer | --> | transfer | ---> ... --> | transfer |
 *  transfers  |    #0    |     |    #1    |              |    #n    |
 *             ------------     ------------              ------------
 *
 *
 * How do we build a UHCI transfer?
 *
 * For each transfer we need a buffer for UHCI constructs and a buffer
 * for I/O data. The former contains a single UHCI queue followed by
 * a number of UHCI transfer descriptors. These describe a UHCI transfer.
 * Each transfer descriptor contains a pointer which points to some location
 * in the I/O data buffer.
 *
 * Conceptual drawing:
 *
 *         transfer buffer                           data I/O buffer
 *        (UHCI constructs)                 (buffer for data/bytes to transfer)
 *                              `qh_data`
 *   start ---------------           -------> start ---------------
 *         |             |          /               |             |
 *         |    queue    |---------/                |             |
 *         |             |                          |             |
 *         ---------------                          |             |
 *         |   transfer  |      `td_buffer`         |             |
 *         |  descriptor |------------------------> |             |
 *         |     #0      |                          |  row data   |
 *         ---------------                          |    stram    |
 *         |             |                          |             |
 *               ...                                      ...
 *         |             |                          |             |
 *         ---------------      `td_buffer`         |             |
 *         |   transfer  |         ---------------> |             |
 *         |  descriptor |--------/                 |             |
 *         |     #n      |                          |             |
 *     end ---------------                          ---------------
 */

/*
 * The following definitions assume an implicit `uhci` argument.
 */

/*
 * Read byte/word/double word from specified UHCI port.
 */
#define in8(addr) bus_read_1(uhci->regs, (addr))
#define in16(addr) bus_read_2(uhci->regs, (addr))
#define in32(addr) bus_read_4(uhci->regs, (addr))

/*
 * Write byte/word/double word to specified UHCI port.
 */
#define out8(addr, val) bus_write_1(uhci->regs, (addr), (val))
#define out16(addr, val) bus_write_2(uhci->regs, (addr), (val))
#define out32(addr, val) bus_write_4(uhci->regs, (addr), (val))

/*
 * Set specified bit in a given UHCI port.
 */
#define set8(p, b) out8((p), in8(p) | (b))
#define set16(p, b) out16((p), in16(p) | (b))
#define set32(p, b) out32((p), in32(p) | (b))

/*
 * Clear specified bit in a given UHCI port.
 */
#define clr8(p, b) out8((p), in8(p) & ~(b))
#define clr16(p, b) out16((p), in16(p) & ~(b))
#define clr32(p, b) out32((p), in32(p) & ~(b))

/*
 * Clear specified bit in a given UHCI write clear port.
 */
#define wclr8(p, b) set8((p), b)
#define wclr16(p, b) set16((p), b)
#define wclr32(p, b) set32((p), b)

/*
 * Check if a specified bit is set in a given UHCI port.
 */
#define chk8(p, b) ((in8(p) & (b)) != 0)
#define chk16(p, b) ((in16(p) & (b)) != 0)
#define chk32(p, b) ((in32(p) & (b)) != 0)

/*
 * Helper functions.
 */

/* Obtain the physical address corresponding to `vaddr`. */
static uhci_physaddr_t uhci_physaddr(void *vaddr) {
  paddr_t paddr = 0;
  pmap_kextract((vaddr_t)vaddr, &paddr);
  return (uhci_physaddr_t)paddr;
}

/*
 * Transfer descriptor handling functions.
 */

/*
 * Initialize a transfer descriptor.
 *
 * - `ioc` - UHCI_TD_IOC (Interrupt On Completion), or 0
 * - `token` - one of `UHCI_TD_{SETUP,OUT,IN}`
 * - `data`  - virtual address of an I/O buffer
 */
static void td_init(uhci_td_t *td, usb_device_t *udev, uint32_t ioc,
                    uint32_t token, void *data) {
  uint32_t ls = (udev->speed == USB_SPD_LOW ? UHCI_TD_LS : 0);

  bzero(td, sizeof(uhci_td_t));
  /* Consecutive transfer descriptors are always contiguous.
   * Note: we assume that only the last transfer descriptor in a transfer
   * will trigger IOC. */
  td->td_next =
    (ioc ? UHCI_PTR_T : uhci_physaddr(td + 1) | UHCI_PTR_VF | UHCI_PTR_TD);
  td->td_status = UHCI_TD_SET_ERRCNT(3) | ioc | ls | UHCI_TD_ACTIVE;
  td->td_token = token;
  td->td_buffer = (data ? uhci_physaddr(data) : 0);
}

/* Create a SETUP transfer descriptor. */
static void td_setup(uhci_td_t *td, usb_device_t *udev, usb_endpt_t *endpt,
                     usb_dev_req_t *req) {
  uint32_t token =
    UHCI_TD_SETUP(sizeof(usb_dev_req_t), endpt->addr, udev->addr);
  /* A SETUP packet is always the first packet of a transfer, thus
   * no Interrupt On Competion is used. */
  td_init(td, udev, 0, token, req);
}

/* Compose a DATA IN/DATA OUT token. */
static uint32_t td_data_token(uint8_t dev_addr, uint8_t endpt_addr,
                              usb_direction_t dir, uint16_t pktsize,
                              uint8_t data_toggle) {
  if (dir == USB_DIR_INPUT)
    return UHCI_TD_IN(pktsize, endpt_addr, dev_addr, data_toggle);
  assert(dir == USB_DIR_OUTPUT);
  return UHCI_TD_OUT(pktsize, endpt_addr, dev_addr, data_toggle);
}

/* Create a DATA IN/DATA OUT transfer descriptor. */
static void td_data(uhci_td_t *td, usb_device_t *udev, usb_endpt_t *endpt,
                    uint32_t ioc, uint16_t pktsize, uint8_t data_toggle,
                    void *data) {
  uint32_t token =
    td_data_token(udev->addr, endpt->addr, endpt->dir, pktsize, data_toggle);
  td_init(td, udev, ioc, token, data);
}

/* Create a STATUS transfer descriptor. */
static void td_status(uhci_td_t *td, usb_device_t *udev, usb_endpt_t *endpt,
                      uint16_t transfer_size, usb_direction_t dir) {
  uint32_t token = td_data_token(udev->addr, endpt->addr, dir, 0, 1);
  /* A SETUP packet is always the last packet of a control transfer, thus
   * an Interrupt On Competion shuld be triggered at the end. */
  td_init(td, udev, UHCI_TD_IOC, token, NULL);
}

/* Discard all errors and mark a transfer descriptor as to be executed. */
static void td_reactivate(uhci_td_t *td) {
  uint32_t ioc = td->td_status & UHCI_TD_IOC;
  uint32_t ls = td->td_status & UHCI_TD_LS;
  td->td_status = UHCI_TD_SET_ERRCNT(3) | ioc | ls | UHCI_TD_ACTIVE;
}

/* Chect whether a transfer descriptor is the last one in a transfer. */
static inline bool td_last(uhci_td_t *td) {
  return td->td_next & UHCI_PTR_T;
}

/*
 * Queue handling functions.
 */

/* Point the queue's element link pointer
 * at the specified transfer descriptor. */
static inline void qh_add_td(uhci_qh_t *qh, uhci_td_t *td) {
  qh->qh_e_next = uhci_physaddr(td) | UHCI_PTR_TD;
}

/* Return a pointer to the first transfer descriptor composing `qh`. */
static inline uhci_td_t *qh_first_td(uhci_qh_t *qh) {
  return (uhci_td_t *)(qh + 1);
}

/* Initialize a regular (i.e. not main) queue. */
static void qh_init(uhci_qh_t *qh, usb_buf_t *buf) {
  qh->qh_h_next = UHCI_PTR_T;
  qh->qh_e_next = UHCI_PTR_T;
  qh->qh_buf = buf;
  qh_add_td(qh, qh_first_td(qh));
}

/* Allocate a new queue (including transfer I/O data buffer). */
static uhci_qh_t *qh_alloc(usb_buf_t *buf) {
  uhci_qh_t *qh = pool_alloc(P_TFR, M_ZERO);
  qh_init(qh, buf);
  qh->qh_data = pool_alloc(P_DATA, M_ZERO);
  return qh;
}

/* Release a queue (including transfer I/O data buffer). */
static void qh_free(uhci_qh_t *qh) {
  void *data = qh->qh_data;
  assert(data);
  pool_free(P_DATA, data);
  pool_free(P_TFR, qh);
}

/* Return `true` if the trnasfer corresponding to `qh` has been executrd. */
static bool qh_executed(uhci_qh_t *qh) {
  uhci_td_t *td = qh_first_td(qh);
  while (!td_last(td))
    td++;
  return !(td->td_status & UHCI_TD_ACTIVE);
}

/* Initialize a main queue. */
static void qh_init_main(uhci_qh_t *mq) {
  bzero(mq, sizeof(uhci_qh_t));
  mq->qh_h_next = UHCI_PTR_T;
  mq->qh_e_next = UHCI_PTR_T;
  spin_init(&mq->qh_lock, 0);
  TAILQ_INIT(&mq->qh_list);
}

/* Point the queue's element link pointer at the specified queue. */
static inline void qh_add_qh(uhci_qh_t *qh1, uhci_qh_t *qh2) {
  qh1->qh_e_next = uhci_physaddr(qh2) | UHCI_PTR_QH;
}

/* Point the queue's head link pointer at the specified queue. */
static inline void qh_chain(uhci_qh_t *qh1, uhci_qh_t *qh2) {
  qh1->qh_h_next = uhci_physaddr(qh2) | UHCI_PTR_QH;
}

/* Tell the controller to omit the specified queue from now on. */
static inline void qh_halt(uhci_qh_t *qh) {
  qh->qh_e_next |= UHCI_PTR_T;
}

/* Tell the controller to continue executing the specified queue when
 * the queue is encountered. */
static inline void qh_unhalt(uhci_qh_t *qh) {
  qh->qh_e_next &= ~UHCI_PTR_T;
}

/* Insert the specified queue into the specified main queue for execution. */
static void qh_insert(uhci_qh_t *mq, uhci_qh_t *qh) {
  SCOPED_SPIN_LOCK(&mq->qh_lock);

  uhci_qh_t *last = TAILQ_LAST(&mq->qh_list, qh_list);

  TAILQ_INSERT_TAIL(&mq->qh_list, qh, qh_link);

  /* If we're inserting the first queue,
   * then we have to enable the main queue. */
  if (!last)
    qh_add_qh(mq, qh);
  else
    qh_chain(last, qh);
}

/* Remove the specified queue from the specified main queue. */
static void qh_remove(uhci_qh_t *mq, uhci_qh_t *qh) {
  assert(spin_owned(&mq->qh_lock));

  uhci_qh_t *prev = TAILQ_PREV(qh, qh_list, qh_link);

  TAILQ_REMOVE(&mq->qh_list, qh, qh_link);

  /* If we're removing the last queue,
   * then we need to disable the main queue. */
  if (prev)
    prev->qh_h_next = qh->qh_h_next;
  else
    mq->qh_e_next = UHCI_PTR_T;
}

/* Obtain queue's error status. */
static uint32_t qh_error_status(uhci_qh_t *qh) {
  uint32_t error = 0;

  for (uhci_td_t *td = qh_first_td(qh); !td_last(td); td++)
    if ((error = td->td_status & UHCI_TD_ERROR))
      break;

  return error;
}

/* Remove the specified queue from the pointed main queue and reclaim
 * UHCI buffers associated with the queue. */
static void qh_discard(uhci_qh_t *mq, uhci_qh_t *qh) {
  assert(spin_owned(&mq->qh_lock));
  qh_remove(mq, qh);
  qh_free(qh);
}

/* Set a queue to be executed. */
static void qh_reactivate(uhci_qh_t *qh) {
  uhci_td_t *td = qh_first_td(qh);
  qh_add_td(qh, td);
  do {
    td_reactivate(td);
  } while (!td_last(td++));
}

/*
 * Software context initialization functions.
 */

/* Initialize main queues. */
static void uhci_init_mainqs(uhci_state_t *uhci) {
  uhci_qh_t *mqs = (void *)kmem_alloc_contig(NULL, PAGESIZE, PMAP_NOCACHE);

  /* Organize the main queues in a list. */
  for (int i = 0; i < UHCI_NMAINQS; i++) {
    qh_init_main(&mqs[i]);
    qh_chain(&mqs[i], &mqs[i - 1]);
    uhci->mainqs[i] = &mqs[i];
  }
  /* Revise the boundary case. */
  mqs[0].qh_h_next = UHCI_PTR_T;
}

/* Initialize the UHCI frame list. */
static void uhci_init_frames(uhci_state_t *uhci) {
  uhci->frames = (void *)kmem_alloc_contig(
    NULL, UHCI_FRAMELIST_COUNT * sizeof(uint32_t), PMAP_NOCACHE);

  /* Set each frame pointer to the appropriate main queue. */
  for (int i = 0; i < UHCI_FRAMELIST_COUNT; i++) {
    int maxpow2 = ffs(i + 1);
    int qn = min(maxpow2, UHCI_NMAINQS) - 1;
    uhci->frames[i] = uhci_physaddr(uhci->mainqs[qn]) | UHCI_PTR_QH;
  }

  /* Set frame list base address. */
  out32(UHCI_FLBASEADDR, uhci_physaddr(uhci->frames));
}

/* Supply a contiguous physical memory for further buffer allocation. */
static void uhci_init_pool(void) {
  void *tfr_pool =
    (void *)kmem_alloc_contig(NULL, UHCI_TFR_POOL_SIZE, PMAP_NOCACHE);
  pool_add_page(P_TFR, tfr_pool, UHCI_TFR_POOL_SIZE);

  void *data_pool =
    (void *)kmem_alloc_contig(NULL, UHCI_DATA_POOL_SIZE, PMAP_NOCACHE);
  pool_add_page(P_DATA, data_pool, UHCI_DATA_POOL_SIZE);
}

/*
 * UHCI transfer handling functions.
 */

/* Convert host controller error flags into USB bus error flags. */
static usb_error_t uhcie2usbe(uint32_t error) {
  usb_error_t uerr = 0;

  /* FTTB, we only distinguish a STALL condition. */
  if (error & UHCI_TD_STALLED)
    uerr |= USB_ERR_STALLED;

  /* Signal any other errors. */
  error &= ~UHCI_TD_STALLED;
  if (error)
    uerr |= USB_ERR_OTHER;

  return uerr;
}

/* Process the transfer identified by queue `qh`. */
static void uhci_process(uhci_state_t *uhci, uhci_qh_t *mq, uhci_qh_t *qh) {
  assert(spin_owned(&mq->qh_lock));

  qh_halt(qh);

  usb_buf_t *buf = qh->qh_buf;

  /* Obtain the queue's status. */
  uint32_t error = qh_error_status(qh);

  /* If an error has occured, signal the error and discard the transfer. */
  if (error) {
    usb_error_t uerr = uhcie2usbe(error);
    usb_buf_process(buf, NULL, uerr);
    qh_discard(mq, qh);
    return;
  }

  /* If the transfer isn't done yet, we'll come back to it later. */
  if (!qh_executed(qh)) {
    qh_unhalt(qh);
    return;
  }

  /* Let the USB bus layer handle the received data. */
  usb_buf_process(buf, qh->qh_data, 0);

  /* If this is a periodic transfer we have to reactivate it. */
  if (usb_buf_periodic(buf)) {
    /* Adding a transfer descriptor will unhalt the queue automatically. */
    qh_reactivate(qh);
  } else {
    qh_discard(mq, qh);
  }
}

/* UHCI Interrupt Service Routine. */
static intr_filter_t uhci_isr(void *data) {
  uhci_state_t *uhci = data;
  /* FTTB, we're interested in IOC and error events. */
  uint16_t intrmask = UHCI_STS_USBINT | UHCI_STS_USBEI;

  /* Sould we bother about this? */
  if (!chk16(UHCI_STS, intrmask))
    return IF_STRAY;

  /* Clear the interrupt flags so that new requests can be queued. */
  wclr16(UHCI_STS, intrmask);

  for (int i = 0; i < UHCI_NMAINQS; i++) {
    uhci_qh_t *mq = uhci->mainqs[i];
    qh_halt(mq);

    /* Travers each main queue to find the delinquent. */
    WITH_SPIN_LOCK (&mq->qh_lock) {
      uhci_qh_t *qh, *next;
      TAILQ_FOREACH_SAFE (qh, &mq->qh_list, qh_link, next)
        uhci_process(uhci, mq, qh);
      /* Unhalt only non-empty main queues. */
      if (!TAILQ_EMPTY(&mq->qh_list))
        qh_unhalt(mq);
    }
  }

  return IF_FILTERED;
}

/* Schedule a queue for execution in `flr(log(interval))` ms. */
static void uhci_schedule(uhci_state_t *uhci, uhci_qh_t *qh, uint8_t interval) {
  if (interval)
    interval = log2(interval);

  /* Select a main queue to accommodate the request. */
  uint8_t qn = min(interval, UHCI_NMAINQS - 1);
  uhci_qh_t *mq = uhci->mainqs[qn];
  qh_insert(mq, qh);
}

/*
 * Setup the data stage of a transaction.
 *
 * - `td`   - address of the first transfer descriptor to fill,
 * - `toggle` - firs data toggle to grant,
 * - `data`   - pointer to the I/O data.
 *
 * Returns pointer to the next available transfer descriptor.
 */
static uhci_td_t *uhci_data_stage(usb_device_t *udev, usb_buf_t *buf,
                                  uhci_td_t *td, uint16_t toggle, void *data) {
  usb_endpt_t *endpt = buf->endpt;

  /* Copyin data to transfer. */
  if (endpt->dir == USB_DIR_OUTPUT)
    memcpy(data, buf->data, buf->transfer_size);

  /* Prepare DATA packets. */
  for (uint16_t nbytes = 0; nbytes != buf->transfer_size; toggle ^= 1) {
    uint16_t pktsize = min(buf->transfer_size - nbytes, endpt->maxpkt);
    td_data(td++, udev, endpt, 0, pktsize, toggle, data);
    nbytes += pktsize;
    data += pktsize;
  }

  return td;
}

/* Issue a control transfer. */
static void uhci_control_transfer(device_t *hcdev, device_t *dev,
                                  usb_buf_t *buf, usb_dev_req_t *req,
                                  usb_direction_t status_dir) {
  assert(buf->transfer_size + sizeof(usb_dev_req_t) <= UHCI_DATA_BUF_SIZE);

  uhci_state_t *uhci = hcdev->state;
  usb_device_t *udev = usb_device_of(dev);
  usb_endpt_t *endpt = buf->endpt;
  uhci_qh_t *qh = qh_alloc(buf);

  /* Copyin the USB device request at the end of the data buffer. */
  void *req_cpy = qh->qh_data + buf->transfer_size;
  memcpy(req_cpy, req, sizeof(usb_dev_req_t));

  /* Prepare a SETUP packet. */
  uhci_td_t *td = qh_first_td(qh);
  td_setup(td, udev, endpt, req_cpy);

  /* Preapre the data stage. */
  td = uhci_data_stage(udev, buf, td + 1, 1, qh->qh_data);

  /* Prepare a STATUS packet. */
  td_status(td, udev, endpt, buf->transfer_size, status_dir);

  uhci_schedule(uhci, qh, 0);
}

/* Issue a data stage only transfer. */
static void uhci_data_transfer(device_t *hcdev, device_t *dev, usb_buf_t *buf) {
  assert(buf->transfer_size <= UHCI_DATA_BUF_SIZE);

  uhci_state_t *uhci = hcdev->state;
  usb_device_t *udev = usb_device_of(dev);
  usb_endpt_t *endpt = buf->endpt;
  uhci_qh_t *qh = qh_alloc(buf);

  /* Preapre the data stage. */
  uhci_td_t *td = uhci_data_stage(udev, buf, qh_first_td(qh), 0, qh->qh_data);

  /* The last transfer descriptor has to finish the transfer. */
  td--;
  td->td_next = UHCI_PTR_T;
  td->td_status |= UHCI_TD_IOC;

  uhci_schedule(uhci, qh, endpt->interval);
}

/*
 * UHCI host controller manage functions.
 */

/* The UHCI_PORTSC_ONE bit of a UHCI port register is always read as 1.
 * This provides a simple method to verify whether a memory word is
 * a root hub port status/control register. */
static bool uhci_is_port(uhci_state_t *uhci, uint8_t port) {
  if (!chk16(UHCI_PORTSC(port), UHCI_PORTSC_ONE))
    return false;

  /* Try to clear the bit. */
  clr16(UHCI_PORTSC(port), UHCI_PORTSC_ONE);
  if (!chk16(UHCI_PORTSC(port), UHCI_PORTSC_ONE))
    return false;

  return true;
}

/* Verify the initial values of a specified root hub port. */
static bool uhci_check_port(uhci_state_t *uhci, uint8_t port) {
  return !chk16(UHCI_PORTSC(port), UHCI_PORTSC_SUSP | UHCI_PORTSC_PR |
                                     UHCI_PORTSC_RD | UHCI_PORTSC_LS |
                                     UHCI_PORTSC_POEDC | UHCI_PORTSC_PE);
}

/* Check whether a device is attached to the specified root hub port. */
static bool uhci_device_present(device_t *dev, uint8_t port) {
  uhci_state_t *uhci = dev->state;
  return chk16(UHCI_PORTSC(port), UHCI_PORTSC_CCS);
}

/* Reset the specified root hub port. */
static void uhci_reset_port(device_t *dev, uint8_t port) {
  uhci_state_t *uhci = dev->state;

  set16(UHCI_PORTSC(port), UHCI_PORTSC_PR);
  mdelay(USB_PORT_ROOT_RESET_DELAY_SPEC);
  clr16(UHCI_PORTSC(port), UHCI_PORTSC_PR);
  mdelay(USB_PORT_RESET_RECOVERY_SPEC);

  /* Only enable the port if some device is attached. */
  if (!uhci_device_present(dev, port))
    return;

  /* Clear status chenge indicators. */
  wclr16(UHCI_PORTSC(port), UHCI_PORTSC_POEDC | UHCI_PORTSC_CSC);

  /* Enable the port. */
  while (!chk16(UHCI_PORTSC(port), UHCI_PORTSC_PE))
    set16(UHCI_PORTSC(port), UHCI_PORTSC_PE);
}

/* Obtain the number of available root hub ports. */
static uint8_t uhci_detect_ports(uhci_state_t *uhci) {
  uint8_t port = 0;

  for (; uhci_is_port(uhci, port); port++) {
    if (!uhci_check_port(uhci, port))
      return 0;
  }

  uhci->nports = port;
  klog("detected %hhu ports", uhci->nports);

  return port;
}

/* Return the number of available root hub ports. */
static uint8_t uhci_number_of_ports(device_t *dev) {
  uhci_state_t *uhci = dev->state;
  return uhci->nports;
}

/* Check whether the device attached to the specified port
 * is a low speed device. */
static usb_speed_t uhci_device_speed(device_t *dev, uint8_t port) {
  uhci_state_t *uhci = dev->state;
  if (chk16(UHCI_PORTSC(port), UHCI_PORTSC_LSDA))
    return USB_SPD_LOW;
  return USB_SPD_FULL;
}

/*
 * Driver interface functions.
 */

static int check_usb_release(device_t *dev) {
  uint8_t rev = pci_read_config_1(dev, PCI_USBREV);
  const char *str = NULL;

  if (rev == PCI_USB_REV_PRE_1_0)
    str = "pre-revision 1.0";
  else if (rev == PCI_USB_REV_1_0)
    str = "revision 1.0";
  else if (rev == PCI_USB_REV_1_1)
    str = "revision 1.1";
  else
    return 1;

  klog("host controller compliant with USB %s", str);
  return 0;
}

static int uhci_probe(device_t *dev) {
  pci_device_t *pcid = pci_device_of(dev);

  if (!pcid)
    return 0;

  /* Is it a USB compatible controller? */
  if (pcid->class_code != PCI_USB_CLASSCODE ||
      pcid->subclass_code != PCI_USB_SUBCLASSCODE)
    return 0;

  /* Is it a UHCI controller? */
  if (pcid->progif != PCI_INTERFACE_UHCI)
    return 0;

  if (check_usb_release(dev))
    return 0;

  return 1;
}

static int uhci_attach(device_t *dev) {
  uhci_state_t *uhci = dev->state;

  /* Gather I/O ports resources. */
  uhci->regs = device_take_ioports(dev, 4, RF_ACTIVE);
  assert(uhci->regs);

  /* Perform the global reset of the UHCI controller. */
  set16(UHCI_CMD, UHCI_CMD_GRESET);
  mdelay(USB_BUS_RESET_DELAY);
  set16(UHCI_CMD, 0x0000);

  /* After global reset, all registers are set to their
   * default values. */
  if (in16(UHCI_CMD) != 0x0000)
    return ENXIO;
  if (in16(UHCI_STS) != UHCI_STS_HCH)
    return ENXIO;
  if (in8(UHCI_SOF) != 0x40) /* each frame is 1 ms */
    return ENXIO;

  /* Unhalt the controller. */
  wclr16(UHCI_STS, UHCI_STS_HCH);

  /* Perform the host controller reset. */
  set16(UHCI_CMD, UHCI_CMD_HCRESET);
  while (in16(UHCI_CMD) & UHCI_CMD_HCRESET)
    ;

  if (!uhci_detect_ports(uhci))
    return ENXIO;

  /* Initialize the software context. */
  uhci_init_mainqs(uhci);
  uhci_init_frames(uhci);
  uhci_init_pool();

  /* Set current frame number to the first frame pointer. */
  out16(UHCI_FRNUM, 0x0000);

  /* Enable bus master mode. */
  pci_enable_busmaster(dev);

  /* Setup host controller's interrupt. */
  uhci->irq = device_take_irq(dev, 0, RF_ACTIVE);
  assert(uhci->irq);
  bus_intr_setup(dev, uhci->irq, uhci_isr, NULL, uhci, "UHCI");

  /* Turn on the IOC and error interrupts. */
  set16(UHCI_INTR, UHCI_INTR_TOCRCIE | UHCI_INTR_IOCE);

  /* Start the controller. */
  out16(UHCI_CMD, UHCI_CMD_RS);

  /* Initialize the underlying USB bus. */
  usb_init(dev);

  /* Detect and configure attached devices. */
  int error = usb_enumerate(dev);
  if (error)
    bus_intr_teardown(dev, uhci->irq);
  return error;
}

static usbhc_methods_t uhci_usbhc_if = {
  .number_of_ports = uhci_number_of_ports,
  .device_present = uhci_device_present,
  .device_speed = uhci_device_speed,
  .reset_port = uhci_reset_port,
  .control_transfer = uhci_control_transfer,
  .data_transfer = uhci_data_transfer,
};

static driver_t uhci = {
  .desc = "UHCI driver",
  .size = sizeof(uhci_state_t),
  .pass = SECOND_PASS,
  .probe = uhci_probe,
  .attach = uhci_attach,
  .interfaces =
    {
      [DIF_USBHC] = &uhci_usbhc_if,
    },
};

DEVCLASS_ENTRY(pci, uhci);
