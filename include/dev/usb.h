/*
 * Based on FreeBSD `sys/dev/usb/usb.h`.
 */
#ifndef _DEV_USB_H_
#define _DEV_USB_H_

#include <sys/condvar.h>
#include <sys/spinlock.h>
#include <sys/device.h>

/*
 * Constructs defined by the USB specification.
 */

/* Definition of some hardcoded USB constants. */

#define USB_MAX_IPACKET 8 /* initial USB packet size */

/* These are the values from the USB specification. */
#define USB_PORT_ROOT_RESET_DELAY_SPEC 50 /* ms */
#define USB_PORT_RESET_RECOVERY_SPEC 10   /* ms */

#define USB_BUS_RESET_DELAY 100 /* ms */

/*
 * USB device request.
 */
typedef struct usb_dev_req {
  uint8_t bmRequestType;
  uint8_t bRequest;
  uint16_t wValue;
  uint16_t wIndex;
  uint16_t wLength;
} __packed usb_dev_req_t;

#define UT_WRITE 0x00
#define UT_READ 0x80
#define UT_STANDARD 0x00
#define UT_CLASS 0x20
#define UT_DEVICE 0x00
#define UT_INTERFACE 0x01
#define UT_ENDPOINT 0x02

#define UT_READ_DEVICE (UT_READ | UT_STANDARD | UT_DEVICE)
#define UT_READ_INTERFACE (UT_READ | UT_STANDARD | UT_INTERFACE)
#define UT_READ_ENDPOINT (UT_READ | UT_STANDARD | UT_ENDPOINT)
#define UT_WRITE_DEVICE (UT_WRITE | UT_STANDARD | UT_DEVICE)
#define UT_WRITE_ENDPOINT (UT_WRITE | UT_STANDARD | UT_ENDPOINT)
#define UT_READ_CLASS_INTERFACE (UT_READ | UT_CLASS | UT_INTERFACE)
#define UT_READ_CLASS_ENDPOINT (UT_READ | UT_CLASS | UT_ENDPOINT)
#define UT_WRITE_CLASS_INTERFACE (UT_WRITE | UT_CLASS | UT_INTERFACE)

/* Requests. */
#define UR_GET_STATUS 0x00
#define UR_CLEAR_FEATURE 0x01
#define UR_SET_ADDRESS 0x05
#define UR_GET_DESCRIPTOR 0x06
#define USB_LANGUAGE_TABLE 0x00
#define UR_GET_CONFIG 0x08
#define UR_SET_CONFIG 0x09
#define UDESC_DEVICE 0x01
#define UDESC_CONFIG 0x02
#define UDESC_STRING 0x03

/* Feature numbers. */
#define UF_ENDPOINT_HALT 0

#define UV_MAKE(d, i) ((d) << 8 | (i))

/*
 * USB device descriptor.
 */
typedef struct usb_dev_dsc {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint16_t bcdUSB;
  uint8_t bDeviceClass;
  uint8_t bDeviceSubClass;
  uint8_t bDeviceProtocol;
  uint8_t bMaxPacketSize;
  /* The fields below are not part of the initial descriptor. */
  uint16_t idVendor;
  uint16_t idProduct;
  uint16_t bcdDevice;
  uint8_t iManufacturer;
  uint8_t iProduct;
  uint8_t iSerialNumber;
  uint8_t bNumConfigurations;
} __packed usb_dev_dsc_t;

#define US_DATASIZE 63

/*
 * USB string descriptor.
 */
typedef struct usb_str_dsc {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint16_t bString[US_DATASIZE];
  uint8_t bUnused;
} __packed usb_str_dsc_t;

/*
 * USB string language descriptor.
 */
typedef struct usb_str_lang {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint16_t bData[US_DATASIZE];
} __packed usb_str_lang_t;

#define US_ENG_LID 0x0409
#define US_ENG_STR "English (United States)"

/*
 * USB configuration descriptor.
 */
typedef struct usb_cfg_dsc {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint16_t wTotalLength;
  uint8_t bNumInterface;
  uint8_t bConfigurationValue;
  uint8_t iConfiguration;
  uint8_t bmAttributes;
  uint8_t bMaxPower; /* max current in 2 mA units */
} __packed usb_cfg_dsc_t;

/*
 * USB interface descriptor.
 */
typedef struct usb_if_dsc {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint8_t bInterfaceNumber;
  uint8_t bAlternateSetting;
  uint8_t bNumEndpoints;
  uint8_t bInterfaceClass;
  uint8_t bInterfaceSubClass;
  uint8_t bInterfaceProtocol;
  uint8_t iInterface;
} __packed usb_if_dsc_t;

/* Interface class codes */
#define UICLASS_HID 0x03
#define UISUBCLASS_BOOT 1
#define UIPROTO_BOOT_KEYBOARD 1
#define UIPROTO_MOUSE 2

#define UICLASS_MASS 0x08
#define UISUBCLASS_SCSI 6
#define UIPROTO_MASS_BBB 80

/*
 * USB endpoint descriptor.
 */
typedef struct usb_endpt_dsc {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint8_t bEndpointAddress;
  uint8_t bmAttributes;
  uint16_t wMaxPacketSize;
  uint8_t bInterval;
} __packed usb_endpt_dsc_t;

#define UE_ADDR 0x0f
#define UE_DIR_IN 0x80  /* IN-token endpoint */
#define UE_DIR_OUT 0x00 /* OUT-token endpoint */
#define UE_GET_DIR(a) ((a)&0x80)
#define UE_GET_ADDR(a) ((a)&UE_ADDR)

#define UE_TRANSFER_TYPE(at) ((at)&0x03)

#define UE_CONTROL 0x00
#define UE_ISOCHRONOUS 0x01
#define UE_BULK 0x02
#define UE_INTERRUPT 0x03

/*
 * Implementation specific constructs.
 */

typedef enum usb_error {
  USB_ERR_STALLED = 1, /* STALL condition encountered */
  USB_ERR_OTHER = 2,   /* errors other than STALL */
} __packed usb_error_t;

/* Don't alter the following values! */
typedef enum usb_transfer {
  USB_TFR_NONE = 0,
  USB_TFR_CONTROL = 1,
  USB_TFR_ISOCHRONOUS = 2,
  USB_TFR_BULK = 3,
  USB_TFR_INTERRUPT = 4,
} __packed usb_transfer_t;

/* Don't alter the following values! */
typedef enum usb_direction {
  USB_DIR_OUTPUT = 0,
  USB_DIR_INPUT = 1,
} __packed usb_direction_t;

typedef enum usb_speed {
  USB_SPD_LOW,
  USB_SPD_FULL,
  USB_SPD_HIGH,
} __packed usb_speed_t;

/* USB string kinds. */
typedef enum usb_str {
  USB_STR_MANUFACTURER,
  USB_STR_PRODUCT,
  USB_STR_SERIAL_NUMBER,
  USB_STR_CONFIGURATION,
  USB_STR_INTERFACE,
  USB_STR_COUNT
} usb_str_t;

typedef struct usb_endpt {
  TAILQ_ENTRY(usb_endpt) link; /* entry on device's endpoint list */
  uint16_t maxpkt;             /* max packet size */
  uint8_t addr;                /* address within a device */
  usb_transfer_t transfer;     /* transfer type */
  usb_direction_t dir;         /* transfer direction */
  uint8_t interval;            /* interval for polling data transfers */
} usb_endpt_t;

/* USB device software representation. */
typedef struct usb_device {
  TAILQ_HEAD(, usb_endpt) endpts; /* endpoints provided by the device */
  char *strs[USB_STR_COUNT];      /* strings provided by the device */
  usb_speed_t speed;              /* speed characteristic */
  uint16_t vendor_id;             /* vendor ID */
  uint16_t product_id;            /* product ID */
  uint8_t eng_support : 1;        /* 1 - device supports US ENG, 0 otherwise */
  uint8_t addr;                   /* address of the device */
  uint8_t ifnum;                  /* current interface number */
  uint8_t class_code;             /* device class code */
  uint8_t subclass_code;          /* device subclass code */
  uint8_t protocol_code;          /* protocol code */
} usb_device_t;

/* USB buffer used for USB transfers. */
typedef struct usb_buf {
  condvar_t cv;           /* wait for the transfer to complete */
  spin_t lock;            /* buffer guard */
  usb_endpt_t *endpt;     /* data stage destination endpoint */
  void *data;             /* data buffer */
  void *priv;             /* buffer's private data (do not alter!) */
  uint16_t transfer_size; /* size of data to transfer in the data stage */
  uint8_t executed : 1;   /* 1 - transfer has been executed, 0 otherwise */
  usb_error_t error;      /* errors encountered during transfer */
} usb_buf_t;

static inline usb_device_t *usb_device_of(device_t *dev) {
  return dev->bus == DEV_BUS_USB ? dev->instance : NULL;
}

static inline device_t *usb_bus_of(device_t *dev) {
  return dev->bus == DEV_BUS_USB ? dev->parent : TAILQ_FIRST(&dev->children);
}

/* Allocates a USB buffer. */
usb_buf_t *usb_buf_alloc(void);

/* Releases a previously allocated buffer. */
void usb_buf_free(usb_buf_t *buf);

/* Returns true if the transfer described by `buf` is periodic. */
bool usb_buf_periodic(usb_buf_t *buf);

/* Waits until the transfer described by `buf` completes, or until an error is
 * encountered. If an error is returned, further information may be obtained
 * through `buf::error`. */
int usb_buf_wait(usb_buf_t *buf);

/* When the transfer request finishes the `data` or `error` are available.
 * We need to update `buf` to reflect that change.
 * Only for host controller driver internal use! */
void usb_buf_process(usb_buf_t *buf, void *data, usb_error_t error);

/* Initializes the underlying USB bus of the host controller `hcdev`. */
void usb_init(device_t *hcdev);

/* Enumerates and configures all devices attached to root hub `hcdev`. */
int usb_enumerate(device_t *hcdev);

/*
 * USB standard interface.
 *
 * The following interface provides basic USB transfers: control and data stage
 * only transfers. If you need to create a new function that performs a new
 * type of request, as specified by the standard, **avoid** adding it to
 * `usb_methods`. Just write a new function that uses the interface.
 */

typedef void (*usb_control_transfer_t)(device_t *dev, usb_buf_t *buf,
                                       void *data, usb_direction_t dir,
                                       usb_dev_req_t *req);
typedef void (*usb_data_transfer_t)(device_t *dev, usb_buf_t *buf, void *data,
                                    uint16_t size, usb_transfer_t transfer,
                                    usb_direction_t dir);

typedef struct usb_methods {
  usb_control_transfer_t control_transfer;
  usb_data_transfer_t data_transfer;
} usb_methods_t;

static inline usb_methods_t *usb_methods(device_t *dev) {
  return (usb_methods_t *)dev->driver->interfaces[DIF_USB];
}

/*
 * Issues a control transfer asynchronously.
 *
 * Pass `buf` to `usb_buf_wait` to wait for the transfer to complete.
 *
 * Arguments:
 *  - `dev`: device requesting the transfer
 *  - `buf`: USB buffer used for transaction
 *  - `data`: data to transfer, or destination address
 *  - `dir`: transfer direction
 *  - `req`: USB device request
 */
static inline void usb_control_transfer(device_t *dev, usb_buf_t *buf,
                                        void *data, usb_direction_t dir,
                                        usb_dev_req_t *req) {
  usb_methods(dev->parent)->control_transfer(dev, buf, data, dir, req);
}

/*
 * Issues a data stage only transfer asynchronously.
 *
 * Pass `buf` to `usb_buf_wait` to wait for the transfer to complete.
 *
 * Arguments:
 *  - `dev`: device requesting the transfer
 *  - `buf`: USB buffer used for transaction
 *  - `data`: data to transfer, or destination address
 *  - `size`: transfer size
 *  - `transfer`: `USB_TFR_INTERRUPT` or `USB_TFR_BULK`
 *  - `dir`: transfer direction
 */
static inline void usb_data_transfer(device_t *dev, usb_buf_t *buf, void *data,
                                     uint16_t size, usb_transfer_t transfer,
                                     usb_direction_t dir) {
  usb_methods(dev->parent)->data_transfer(dev, buf, data, size, transfer, dir);
}

/*
 * USB standard requests.
 *
 * The following functions implement standard USB requests. Requests used for
 * device identification and configuration aren't exposed since USB bus
 * driver identifies and configures each device automatically during enumeration
 * process. Consider adding a new function to the following set instead of using
 * `usb_control_transfer` request directly.
 */

/*
 * Unhalts device's endpoint.
 *
 * Used in recovery process.
 *
 * Arguments:
 *  - `dev`: USB device
 *  - `transfer` + `dir`: identifies device's endpoint
 *
 * Error codes:
 *  - EINVAL: `transfer` + `dir` doesn't identify a `dev` endpoint
 *  - EIO: an error has been encountered during the transfer
 */
int usb_unhalt_endpt(device_t *dev, usb_transfer_t transfer,
                     usb_direction_t dir);

/*
 * USB HID specific standard requests.
 *
 * The following functions implement standard USB requests specific to
 * HID device class.
 */

/*
 * Tells a device to inhibit all reports until a report changes.
 *
 * Used in driver's configuration phase.
 *
 * Arguments:
 *  - `dev`: USB device
 *
 * Error codes:
 *  - EIO: an error has been encountered during the transfer
 */
int usb_hid_set_idle(device_t *dev);

/*
 * Sets device's report format to the boot interface report format.
 *
 * Used by drivers which don't implement HID descriptor parsing.
 *
 * Arguments:
 *  - `dev`: USB device
 *
 * Error codes:
 *  - EIO: an error has been encountered during the transfer
 */
int usb_hid_set_boot_protocol(device_t *dev);

/*
 * USB Bulk-Only specific standard requests.
 *
 * The following functions implements standard USB requests specific to
 * devices which rely on the Bulk-Only protocol.
 * (BBB refers Bulk/Bulk/Bulk for Command/Data/Status phases.)
 */

/*
 * Retrives the maximum Logical Unit Number of a device.
 *
 * Arguments:
 *  - `dev`: USB device
 *  - `maxlun_p`: destination address
 *
 * Error codes:
 *  - EIO: an error has been encountered during the transfer
 */
int usb_bbb_get_max_lun(device_t *dev, uint8_t *maxlun_p);

/*
 * Resets USB mass storage device.
 *
 * Used in recovery process.
 *
 * Arguments:
 *  - `dev`: USB device
 *
 * Error codes:
 *  - EIO: an error has been encountered during the transfer
 */
int usb_bbb_reset(device_t *dev);

#endif /* _DEV_USB_H_ */
