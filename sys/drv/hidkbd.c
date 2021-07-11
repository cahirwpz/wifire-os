/*
 * USB HID keyboard driver.
 *
 * Relevant documents:
 *
 * - Device Class Definition for Human Interface Devices (HID) Version 1.11,
 *   5/27/01:
 *     https://www.usb.org/sites/default/files/hid1_11.pdf
 */
#define KL_LOG KL_DEV
#include <sys/klog.h>
#include <sys/libkern.h>
#include <sys/errno.h>
#include <sys/devclass.h>
#include <sys/device.h>
#include <sys/sched.h>
#include <dev/evdev.h>
#include <dev/usb.h>

#define HIDKBD_NMODKEYS 8
#define HIDKBD_NKEYCODES 6

/* HID keyboard input report. */
typedef struct hidkbd_in_report {
  uint8_t modifier_keys;
  uint8_t reserved;
  uint8_t keycodes[HIDKBD_NKEYCODES];
} __packed hidkbd_in_report_t;

/* HID keyboard software context. */
typedef struct hidkbd_state {
  hidkbd_in_report_t prev_report; /* the most recent report */
  thread_t *thread;               /* thread which gathers scancodes */
  evdev_dev_t *evdev;             /* evdev device structure */
} hidkbd_state_t;

/*
 * Evdev handling functions.
 */

/* Create and setup a corresponding evdev device. */
static void hidkbd_init_evdev(device_t *dev) {
  usb_device_t *udev = usb_device_of(dev);
  hidkbd_state_t *hidkbd = dev->state;
  evdev_dev_t *evdev = evdev_alloc();

  /* Set the basic evdev parameters. */
  evdev_set_name(evdev, "HID keyboard");
  evdev_set_id(evdev, BUS_USB, udev->vendor_id, udev->product_id, 0);

  /* EV_SYN is required for every evdev device. */
  evdev_support_event(evdev, EV_SYN);

  evdev_support_event(evdev, EV_KEY);
  evdev_hidkbd_support_all_known_keys(evdev);
  /* NOTE: we assume that the keyboard supports hardware key repetiton,
   * which isn't said to always be true. */
  evdev_support_event(evdev, EV_REP);

  /* Bind `evdev` to `hidkbd` for future references. */
  hidkbd->evdev = evdev;

  /* Finally, register the device in the file system. */
  evdev_register(evdev);
}

/*
 * Report processing functions.
 */

/* Process modifier keys. */
static void hidkbd_process_modkeys(hidkbd_state_t *hidkbd, uint8_t modkeys) {
  uint8_t prev_modkeys = hidkbd->prev_report.modifier_keys;

  for (size_t i = 0; i <= HIDKBD_NMODKEYS; i++) {
    uint8_t prev = prev_modkeys & (1 << i);
    uint8_t cur = modkeys & (1 << i);

    if (prev == cur)
      continue;

    uint16_t keycode = evdev_mod2key(i);
    int32_t value = cur ? 1 : 0;
    evdev_push_event(hidkbd->evdev, EV_KEY, keycode, value);
    evdev_sync(hidkbd->evdev);
  }
}

/* Process keycodes. */
static void hidkbd_process_keycodes(hidkbd_state_t *hidkbd, uint8_t *keycodes) {
  uint8_t *prev_keycodes = hidkbd->prev_report.keycodes;

  for (int i = 0; i < HIDKBD_NKEYCODES; i++) {
    uint16_t keycode;
    int32_t value = 1;

    if (*prev_keycodes == *keycodes) {
      if (*keycodes == 0)
        break;
      keycode = evdev_hid2key(*keycodes);
      goto both_up;
    }

    /* Has the button been pressed? */
    if (*prev_keycodes == 0) {
      keycode = evdev_hid2key(*keycodes);
      goto both_up;
    }

    /* The button has been released. */
    keycode = evdev_hid2key(*prev_keycodes);
    value = 0;
    goto prev_up;

  both_up:
    keycodes++;
  prev_up:
    prev_keycodes++;

    if (keycode != KEY_RESERVED) {
      evdev_push_event(hidkbd->evdev, EV_KEY, keycode, value);
      evdev_sync(hidkbd->evdev);
    }
  }
}

/* Process an input report. */
static void hidkbd_process_in_report(hidkbd_state_t *hidkbd,
                                     hidkbd_in_report_t *report) {
  hidkbd_process_modkeys(hidkbd, report->modifier_keys);
  hidkbd_process_keycodes(hidkbd, report->keycodes);
}

/* A thread which gathers USB HID scancodes, converts them to evdev-compatible
 * keycodes and then hands them over to the evdev layer. */
static void hidkbd_thread(void *arg) {
  device_t *dev = arg;
  hidkbd_state_t *hidkbd = dev->state;
  hidkbd_in_report_t report;

  /* Obtain a USB buf. */
  usb_buf_t *buf = usb_buf_alloc();

  /* Register an interrupt input transfer. */
  usb_data_transfer(dev, buf, &report, sizeof(hidkbd_in_report_t),
                    USB_TFR_INTERRUPT, USB_DIR_INPUT);

  while (true) {
    /* Wait for the data to arrvie. */
    int error = usb_buf_wait(buf);
    if (!error) {
      hidkbd_process_in_report(hidkbd, &report);
      memcpy(&hidkbd->prev_report, &report, sizeof(hidkbd_in_report_t));
      continue;
    }

    /* Recover and reenable the transfer. */
    if ((error = usb_unhalt_endpt(dev, USB_TFR_INTERRUPT, USB_DIR_INPUT)))
      panic("unable to unhalt an endpoint");

    usb_data_transfer(dev, buf, &report, sizeof(hidkbd_in_report_t),
                      USB_TFR_INTERRUPT, USB_DIR_INPUT);
  }
}

/*
 * Driver interface implementation.
 */

static int hidkbd_probe(device_t *dev) {
  usb_device_t *udev = usb_device_of(dev);
  if (!udev)
    return 0;

  /* We're looking for a HID keyboard supporting the boot protocol. */
  if (udev->class_code != UICLASS_HID ||
      udev->subclass_code != UISUBCLASS_BOOT ||
      udev->protocol_code != UIPROTO_BOOT_KEYBOARD)
    return 0;

  return 1;
}

static int hidkbd_attach(device_t *dev) {
  hidkbd_state_t *hidkbd = dev->state;

  if (usb_hid_set_idle(dev))
    return ENXIO;

  /* We rely on the boot protocol report layout. */
  if (usb_hid_set_boot_protocol(dev))
    return ENXIO;

  hidkbd->thread = thread_create("hidkbd", hidkbd_thread, dev,
                                 prio_ithread(PRIO_ITHRD_QTY - 1));

  hidkbd_init_evdev(dev);

  sched_add(hidkbd->thread);

  return 0;
}

static driver_t hidkbd_driver = {
  .desc = "HID keyboard driver",
  .size = sizeof(hidkbd_state_t),
  .pass = SECOND_PASS,
  .probe = hidkbd_probe,
  .attach = hidkbd_attach,
};

DEVCLASS_ENTRY(usb, hidkbd_driver);
