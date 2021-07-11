/*
 * USB HID mouse driver.
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

#define HIDM_NBUTTONS 3

/* HID mouse input report. */
typedef struct hidm_report {
  uint8_t buttons; /* button status */
  int8_t x;        /* relative X movement */
  int8_t y;        /* relative Y movement */
} __packed hidm_report_t;

/* HID mouse software context. */
typedef struct hidm_state {
  thread_t *thread;     /* thread which collects input packets */
  evdev_dev_t *evdev;   /* evdev device structure */
  uint8_t prev_buttons; /* the most recent buttons state */
} hidm_state_t;

/*
 * Evdev handling functions.
 */

/* Create and setup a corresponding evdev device. */
static void hidm_init_evdev(device_t *dev) {
  usb_device_t *udev = usb_device_of(dev);
  hidm_state_t *hidm = dev->state;
  evdev_dev_t *evdev = evdev_alloc();

  /* Set the basic evdev parameters. */
  evdev_set_name(evdev, "HID mouse");
  evdev_set_id(evdev, BUS_USB, udev->vendor_id, udev->product_id, 0);

  /* EV_SYN is required for every evdev device. */
  evdev_support_event(evdev, EV_SYN);

  /* Mouse buttons. */
  evdev_support_event(evdev, EV_KEY);
  evdev_support_all_known_buttons(evdev);
  evdev_support_event(evdev, EV_REP);

  /* Mouse movement. */
  evdev_support_event(evdev, EV_REL);
  evdev_support_rel(evdev, REL_X);
  evdev_support_rel(evdev, REL_Y);

  /* Bind `evdev` to `hidm` for future references. */
  hidm->evdev = evdev;

  /* Finally, register the device in the file system. */
  evdev_register(evdev);
}

/*
 * Report processing functions.
 */

/* Process mouse buttons. */
static void hidm_process_buttons(hidm_state_t *hidm, uint8_t buttons) {
  uint8_t prev_buttons = hidm->prev_buttons;

  for (size_t i = 0; i <= HIDM_NBUTTONS; i++) {
    uint8_t prev = prev_buttons & (1 << i);
    uint8_t cur = buttons & (1 << i);

    if (prev == cur && !cur)
      continue;

    uint16_t keycode = evdev_hid2btn(i);
    int32_t value = cur ? 1 : 0;
    evdev_push_event(hidm->evdev, EV_KEY, keycode, value);
    evdev_sync(hidm->evdev);
  }
}

/* Process an input report. */
static void hidm_process_report(hidm_state_t *hidm, hidm_report_t *report) {
  /* Hnadle buttons. */
  hidm_process_buttons(hidm, report->buttons);

  /* Handle the X-axis. */
  int8_t x = report->x;
  if (x) {
    evdev_push_event(hidm->evdev, EV_REL, REL_X, x);
    evdev_sync(hidm->evdev);
  }

  /* Handle the Y-axis. */
  int8_t y = report->y;
  if (y) {
    evdev_push_event(hidm->evdev, EV_REL, REL_Y, y);
    evdev_sync(hidm->evdev);
  }
}

/* A thread which gathers input reports, processes and converts them
 * to evdev-compatible events and then hands them over to the evdev layer. */
static void hidm_thread(void *arg) {
  device_t *dev = arg;
  hidm_state_t *hidm = dev->state;
  hidm_report_t report;

  /* Obtain a USB buf. */
  usb_buf_t *buf = usb_buf_alloc();

  /* Register an interrupt input transfer. */
  usb_data_transfer(dev, buf, &report, sizeof(hidm_report_t), USB_TFR_INTERRUPT,
                    USB_DIR_INPUT);

  while (true) {
    /* Wait for the data to arrvie. */
    int error = usb_buf_wait(buf);
    if (!error) {
      hidm_process_report(hidm, &report);
      hidm->prev_buttons = report.buttons;
      continue;
    }

    /* Recover and reenable the transfer. */
    if ((error = usb_unhalt_endpt(dev, USB_TFR_INTERRUPT, USB_DIR_INPUT)))
      panic("unable to unhalt an endpoint");

    usb_data_transfer(dev, buf, &report, sizeof(hidm_report_t),
                      USB_TFR_INTERRUPT, USB_DIR_INPUT);
  }
}

/*
 * Driver interface implementation.
 */

static int hidm_probe(device_t *dev) {
  usb_device_t *udev = usb_device_of(dev);
  if (!udev)
    return 0;

  /* We're looking for a HID mouse wwhich supports the boot interface. */
  if (udev->class_code != UICLASS_HID ||
      udev->subclass_code != UISUBCLASS_BOOT ||
      udev->protocol_code != UIPROTO_MOUSE)
    return 0;

  return 1;
}

static int hidm_attach(device_t *dev) {
  hidm_state_t *hidm = dev->state;

  if (usb_hid_set_idle(dev))
    return ENXIO;

  /* We rely on the boot protocol report layout. */
  if (usb_hid_set_boot_protocol(dev))
    return ENXIO;

  hidm->thread =
    thread_create("hidm", hidm_thread, dev, prio_ithread(PRIO_ITHRD_QTY - 1));

  hidm_init_evdev(dev);

  sched_add(hidm->thread);

  return 0;
}

static driver_t hidm_driver = {
  .desc = "HID mouse driver",
  .size = sizeof(hidm_state_t),
  .pass = SECOND_PASS,
  .probe = hidm_probe,
  .attach = hidm_attach,
};

DEVCLASS_ENTRY(usb, hidm_driver);
