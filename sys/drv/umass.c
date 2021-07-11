/*
 * USB mass storage device driver.
 *
 * For explanation of terms used throughout the code
 * please see the following documents:
 *
 * - Universal Serial Bus Mass Storage Class Specification Overview Revision 1.2
 *   June 23, 2003:
 *     https://www.rockbox.org/wiki/pub/Main/DataSheets/usb_msc_overview_1.2.pdf
 *
 * - Universal Serial BusMass Storage ClassBulk-Only Transport Revision 1.0,
 *   September 31, 1999:
 *     https://www.usb.org/sites/default/files/usbmassbulk_10.pdf
 *
 * - SCSI Primary Commands - 4 Revision 18
 *     https://www.t10.org/
 */
#define KL_LOG KL_DEV
#include <sys/klog.h>
#include <sys/endian.h>
#include <sys/devclass.h>
#include <sys/device.h>
#include <sys/libkern.h>
#include <sys/vnode.h>
#include <sys/devfs.h>
#include <dev/scsi.h>
#include <dev/usb.h>
#include <dev/umass.h>

/* We assume that block size >= 512. */
#define UMASS_MIN_BLOCK_SIZE 512

typedef struct umass_state {
  uint32_t next_tag;                    /* next tab to grant */
  uint32_t nblocks;                     /* number of available blocks */
  uint32_t block_size;                  /* size of a single block */
  char vendor[SID_VENDOR_SIZE + 1];     /* vandor string */
  char product[SID_PRODUCT_SIZE + 1];   /* product string */
  char revision[SID_REVISION_SIZE + 1]; /* revision string */
} umass_state_t;

/*
 * Auxiliary functions.
 */

/* Perform a reset recoery process. */
static int umass_reset(device_t *dev) {
  int error = 0;

  /* Bulk-Only reset request. */
  if ((error = usb_bbb_reset(dev)))
    return error;

  /* Unhalt the bulk input endpoint. */
  if ((error = usb_unhalt_endpt(dev, USB_TFR_BULK, USB_DIR_INPUT)))
    return error;

  /* Unhalt the bulk input endpoint. */
  if ((error = usb_unhalt_endpt(dev, USB_TFR_BULK, USB_DIR_OUTPUT)))
    return error;

  return 0;
}

/* Print a USB mass storage device. */
static void umass_print(device_t *dev) {
  umass_state_t *umass = dev->state;

  klog("USB mass storage device:");
  klog("command set: SCSI Block Commands version 3");

  if (*umass->vendor)
    klog("vendor: %s", umass->vendor);

  if (*umass->product)
    klog("product: %s", umass->product);

  if (*umass->revision)
    klog("revision: %s", umass->revision);

  klog("number of logical blocks: %u", umass->nblocks);
  klog("block size: %u", umass->block_size);
}

/*
 * Command Block Wrapper handling functions.
 */

/* Translate USB direction flags to CBW flags. */
static uint8_t cbw_flags(usb_direction_t dir) {
  if (dir == USB_DIR_INPUT)
    return CBWFLAGS_IN;
  assert(dir == USB_DIR_OUTPUT);
  return CBWFLAGS_OUT;
}

/* Setup a command block wrapper. */
static void cbw_setup(umass_bbb_cbw_t *cbw, uint32_t tag, uint32_t size,
                      uint8_t flags, void *cmd, uint8_t cmdsize) {
  assert(cmdsize <= CBWCDBLENGTH);

  cbw->dCBWSignature = CBWSIGNATURE;
  cbw->dCBWTag = tag;
  cbw->dCBWDataTransferLength = size;
  cbw->bCBWFlags = flags;
  cbw->bCBWLUN = 0;
  cbw->bCDBLength = cmdsize;

  memcpy(cbw->CBWCDB, cmd, cmdsize);
}

/* Send a command block wrapper. */
static int umass_send_cbw(device_t *dev, usb_buf_t *buf, umass_bbb_cbw_t *cbw) {
  int error = 0;

  usb_data_transfer(dev, buf, cbw, sizeof(umass_bbb_cbw_t), USB_TFR_BULK,
                    USB_DIR_OUTPUT);
  if (!(error = usb_buf_wait(buf)))
    return 0;

  /* If a STALL ocurrs upon the first attempt, the device should be reset
   * and another try should be perform. */
  if (buf->error != USB_ERR_STALLED)
    return error;

  if ((error = umass_reset(dev)))
    return error;

  usb_data_transfer(dev, buf, cbw, sizeof(umass_bbb_cbw_t), USB_TFR_BULK,
                    USB_DIR_OUTPUT);
  return usb_buf_wait(buf);
}

/*
 * Command Status Wrapper handling functions.
 */

/* Receive a command status wrapper. */
static int umass_receive_csw(device_t *dev, usb_buf_t *buf,
                             umass_bbb_csw_t *csw) {
  int error = 0;

  usb_data_transfer(dev, buf, csw, sizeof(umass_bbb_csw_t), USB_TFR_BULK,
                    USB_DIR_INPUT);
  if (!(error = usb_buf_wait(buf)))
    return 0;

  if (buf->error != USB_ERR_STALLED)
    return error;

  if ((error = umass_reset(dev)))
    return error;

  return EIO;
}

/* Examine a received CSW. */
static int umass_check_csw(device_t *dev, umass_bbb_csw_t *csw, uint32_t tag) {
  int error = 0;

  /* If the CSW isn't meaningful, we perform the reset recovery. */
  if (csw->dCSWSignature != CSWSIGNATURE || csw->dCSWTag != tag) {
    if ((error = umass_reset(dev)))
      return error;
    return EIO;
  }

  /* Has there been any error? */
  if (csw->bCSWStatus & CSWSTATUS_FAILED)
    return EIO;

  return 0;
}

/*
 * Bulk-Only transfer functions.
 */

/* Perform a Bulk-Only transfer. */
static int umass_transfer(device_t *dev, void *data, uint32_t size,
                          usb_direction_t dir, void *cmd, size_t cmdsize) {
  umass_state_t *umass = dev->state;
  usb_buf_t *buf = usb_buf_alloc();
  int error = 0;

  /*
   * Command Block Wrapper phase.
   */

  umass_bbb_cbw_t cbw;
  uint32_t tag = umass->next_tag++;
  cbw_setup(&cbw, tag, size, cbw_flags(dir), cmd, cmdsize);

  if ((error = umass_send_cbw(dev, buf, &cbw)))
    goto end;

  /*
   * Data transfer phase.
   */

  /* Transfer data using block size units. */
  for (uint32_t nbytes = 0; nbytes != size;) {
    uint32_t tfrsize = min(size, umass->block_size);

    usb_data_transfer(dev, buf, data, tfrsize, USB_TFR_BULK, dir);
    if ((error = usb_buf_wait(buf))) {
      /* If we encounter an error in the data phase,
       * we still need to receive a CSW. */
      break;
    }

    nbytes += tfrsize;
    data += tfrsize;
  }

  /*
   * Command Status Block phase.
   */

  umass_bbb_csw_t csw;
  int csw_error = umass_receive_csw(dev, buf, &csw);
  if (csw_error) {
    error = csw_error;
    goto end;
  }

  if (!error)
    error = umass_check_csw(dev, &csw, tag);

end:
  usb_buf_free(buf);
  return error;
}

/*
 * SCSI command functions.
 */

/* Perform the inquiry command. */
static int umass_inquiry(device_t *dev) {
  scsi_inquiry_data_t inq_data;
  scsi_inquiry_t inq = (scsi_inquiry_t){
    .opcode = INQUIRY,
    .length = htobe16(SHORT_INQUIRY_LENGTH),
  };

  int error = umass_transfer(dev, &inq_data, SHORT_INQUIRY_LENGTH,
                             USB_DIR_INPUT, &inq, sizeof(scsi_inquiry_t));
  if (error)
    return error;

  /* We only handle direct access block devices (i.e. command set SBC-3). */
  if (SID_TYPE(&inq_data) != 0x00)
    return ENXIO;

  /* The device must be connected to the logical unit. */
  if (SID_QUAL(&inq_data) != SID_QUAL_LU_CONNECTED)
    return ENXIO;

  /* Response format should be 0x02 (0x01 is possible but obsolete). */
  uint8_t format = SID_FORMAT(&inq_data);
  if (format != 0x02 && format != 0x01)
    return ENXIO;

  /* Copy supplied strings. */
  umass_state_t *umass = dev->state;
  memcpy(umass->vendor, inq_data.vendor, SID_VENDOR_SIZE);
  memcpy(umass->product, inq_data.product, SID_PRODUCT_SIZE);
  memcpy(umass->revision, inq_data.revision, SID_REVISION_SIZE);

  return 0;
}

/* Perform the request sense command. */
static int umass_request_sense(device_t *dev) {
  scsi_sense_data_t sen_data;
  scsi_request_sense_t sen = (scsi_request_sense_t){
    .opcode = REQUEST_SENSE,
    .length = sizeof(scsi_sense_data_t),
  };
  int error = umass_transfer(dev, &sen_data, sizeof(scsi_sense_data_t),
                             USB_DIR_INPUT, &sen, sizeof(scsi_request_sense_t));

  /* XXX: in the future, some recovery logic may be needed.
   * FTTB, just performing the command is sufficient. */
  return error;
}

/*
 * For the read cpapcity request to succeed, most devices will require to
 * perform a sequence of the following pairs of requests:
 * (read capacity, request sense). We have to establish some maximum number
 * of possible iterations.
 */
#define READ_CAPACITY_MAX_ITR 4

/* Perform the read capacity command. */
static int umass_read_capacity(device_t *dev) {
  scsi_read_capacity_data_t cap_data;
  scsi_read_capacity_t cap = (scsi_read_capacity_t){
    .opcode = READ_CAPACITY,
  };
  int i, error = 0;

  for (i = 0; i < READ_CAPACITY_MAX_ITR; i++) {
    /* Try to issue the read capacity command. */
    error = umass_transfer(dev, &cap_data, sizeof(scsi_read_capacity_data_t),
                           USB_DIR_INPUT, &cap, sizeof(scsi_read_capacity_t));
    if (!error)
      break;

    /* We've failed. Let's perform the request sense command. */
    error = umass_request_sense(dev);
    if (error) {
      /* This kind of error is unacceptable. Return with error. */
      return error;
    }
  }
  if (i == READ_CAPACITY_MAX_ITR) {
    /* We did our best. */
    return ENODEV;
  }

  /* We assume that number of logical blocks is <= UINT32_MAX. */
  if (cap_data.addr == UINT32_MAX)
    return ENXIO;

  /* Save number of logical blocks and block size. */
  umass_state_t *umass = dev->state;
  umass->nblocks = be32toh(cap_data.addr) + 1;
  umass->block_size = be32toh(cap_data.length);

  return 0;
}

/*
 * Read/Write functions.
 */

static int umass_read(vnode_t *v, uio_t *uio) {
  device_t *dev = devfs_node_data(v);
  umass_state_t *umass = dev->state;
  uint32_t block_size = umass->block_size;
  off_t offset = uio->uio_offset;
  int error = 0;

  /* Obtain the LBA of the first block to read. */
  uint32_t start = offset / block_size;

  /* Obtain the LBA of the last block to read. */
  uint32_t end = roundup(offset + uio->uio_resid, block_size) / block_size;

  /* How many blocks is it?
   * Note: we assume that number of blocks to read is <= UINT16_MAX. */
  uint16_t nblocks = end - start;
  uint32_t size = nblocks * block_size;

  void *buf = kmalloc(M_DEV, size, M_WAITOK);
  if (!buf)
    return ENOMEM;

  scsi_rw_10_t rw10 = (scsi_rw_10_t){
    .opcode = READ_10,
    .addr = htobe32(start),
    .length = htobe16(nblocks),
  };
  error =
    umass_transfer(dev, buf, size, USB_DIR_INPUT, &rw10, sizeof(scsi_rw_10_t));
  if (error)
    goto end;

  /* Copy the data to the user. */
  uio->uio_offset -= start;
  error = uiomove_frombuf(buf, size, uio);

  /* XXX: this is a cheat which keeps file offset at 0.
   * This should be removed when a better interface is presended. */
  uio->uio_offset = 0;

end:
  kfree(M_DEV, buf);
  return error;
}

static int umass_write(vnode_t *v, uio_t *uio) {
  device_t *dev = devfs_node_data(v);
  umass_state_t *umass = dev->state;
  uint32_t block_size = umass->block_size;
  off_t offset = uio->uio_offset;
  int error = 0;

  /* Obtain the LBA of the first block invloved. */
  uint32_t start = offset / block_size;

  /* Obtain the LBA of the last block involved. */
  uint32_t end = roundup(offset + uio->uio_resid, block_size) / block_size;

  /* How many blocks is it?
   * Note: we assume that number of blocks to read is <= UINT16_MAX. */
  uint16_t nblocks = end - start;
  uint32_t size = nblocks * block_size;

  void *buf = kmalloc(M_DEV, size, M_ZERO);
  if (!buf)
    return ENOMEM;

  /*
   * If we won't overwrite the edge blocks, we have to preserve their content.
   * To achive this, we have to read their current data.
   */

  scsi_rw_10_t rw10 = (scsi_rw_10_t){
    .opcode = READ_10,
    .length = htobe16(1),
  };

  /* Should we bother about the first block? */
  if (offset % block_size) {
    rw10.addr = htobe32(start);
    error = umass_transfer(dev, buf, block_size, USB_DIR_INPUT, &rw10,
                           sizeof(scsi_rw_10_t));
    if (error)
      goto end;
  }

  /* Should we bother about the last block? */
  if ((offset + uio->uio_resid) % block_size) {
    rw10.addr = htobe32(end - 1);
    error = umass_transfer(dev, buf + size - block_size, block_size,
                           USB_DIR_INPUT, &rw10, sizeof(scsi_rw_10_t));
    if (error)
      goto end;
  }

  /* Copy data from the user. */
  error = uiomove(buf + (offset % block_size), uio->uio_resid, uio);
  if (error)
    goto end;

  /* Write the data to the drive. */
  rw10.opcode = WRITE_10;
  rw10.addr = htobe32(start);
  rw10.length = htobe16(nblocks);
  error =
    umass_transfer(dev, buf, size, USB_DIR_OUTPUT, &rw10, sizeof(scsi_rw_10_t));

  /* XXX: this is a cheat which keeps file offset at 0.
   * This should be removed when a better interface is presended. */
  uio->uio_offset = 0;

end:
  kfree(M_DEV, buf);
  return error;
}

static vnodeops_t umass_ops = {
  .v_read = umass_read,
  .v_write = umass_write,
};

/*
 * Driver interface implementation.
 */

static int umass_probe(device_t *dev) {
  usb_device_t *udev = usb_device_of(dev);
  if (!udev)
    return 0;

  /* We're looking for a USB mass storage device using SCSI transparent
   * command set, and Bulk-Only transfer protocol. */
  if (udev->class_code != UICLASS_MASS ||
      udev->subclass_code != UISUBCLASS_SCSI ||
      udev->protocol_code != UIPROTO_MASS_BBB)
    return 0;

  return 1;
}

static int umass_attach(device_t *dev) {
  int error = 0;

  /* Obtain the max Logical Unit Number of the drive. */
  uint8_t maxlun;
  if (usb_bbb_get_max_lun(dev, &maxlun))
    return ENXIO;

  /* We only support drives with a single logical unit. */
  if (maxlun)
    return ENXIO;

  /* Set the initial block size. */
  umass_state_t *umass = dev->state;
  umass->block_size = UMASS_MIN_BLOCK_SIZE;

  /* Identify the connected device. */
  if ((error = umass_inquiry(dev)))
    return ENXIO;

  /*
   * After receiving a successful inquiry transfer,
   * the drive is ready for use.
   */

  /* Obtain the number and length of the logical blocks. */
  if ((error = umass_read_capacity(dev)))
    return ENXIO;

  umass_print(dev);

  /* Prepare /dev/umass interface. */
  devfs_makedev(NULL, "umass", &umass_ops, dev, NULL);

  return 0;
}

static driver_t umass_driver = {
  .desc = "USB mass storage device driver",
  .size = sizeof(umass_state_t),
  .pass = SECOND_PASS,
  .probe = umass_probe,
  .attach = umass_attach,
};

DEVCLASS_ENTRY(usb, umass_driver);
