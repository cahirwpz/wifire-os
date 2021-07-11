#ifndef _DEV_EVDEV_H_
#define _DEV_EVDEV_H_

#include <sys/input.h>

/*
 * evdev (short for 'event device') is a generic input event interface. For the
 * detailed description of event types,  refer to the original documentation
 * https://www.kernel.org/doc/html/latest/input/input.html
 */

/* Flags describing an evdev device */
#define EVDEV_FLAG_SOFTREPEAT 0x00 /* use evdev to repeat keys */
#define EVDEV_FLAG_MAX 0x1F
#define EVDEV_FLAG_CNT (EVDEV_FLAG_MAX + 1)

typedef enum {
  KEY_EVENT_UP,
  KEY_EVENT_DOWN,
  KEY_EVENT_REPEAT
} evdev_key_events_t;

/* Stores event device and its clients states. */
typedef struct evdev_dev evdev_dev_t;

/* Stores information about an event. */
typedef struct input_event input_event_t;

/*
 * Input device interface.
 */

/* Allocate a new `evdev_dev_t` structure. */
evdev_dev_t *evdev_alloc(void);

/* Free a `evdev_dev_t` structure. */
void evdev_free(evdev_dev_t *evdev);

/* Set the `name` of a `evdev` device.
 * This data is only used by the user-space. */
void evdev_set_name(evdev_dev_t *evdev, const char *name);

/* Set the input device ID, i.e. bus, vendor, product, version.
 * This data is only used by the user-space. */
void evdev_set_id(evdev_dev_t *evdev, uint16_t bustype, uint16_t vendor,
                  uint16_t product, uint16_t version);

/* Mark the given event type (EV_SYN, EV_KEY, etc.) as supported.
 * This data is only used by the user-space. */
void evdev_support_event(evdev_dev_t *evdev, uint16_t type);

/* Mark the given key code as supported.
 * This data is only used by the user-space. */
void evdev_support_key(evdev_dev_t *evdev, uint16_t code);

/* Mark the relative event as supported.
 * This data is only used by the user-space. */
void evdev_support_rel(evdev_dev_t *evdev, uint16_t code);

/* Set a flag describing supported features by the evdev device. */
void evdev_set_flag(evdev_dev_t *evdev, uint16_t flag);

/* Finalize initialization and register an evdev device.
 * Should be called after all properties of the evdev device are set.
 *
 * Returns an error code when device could not be registered by devfs. */
int evdev_register(evdev_dev_t *evdev);

/* Push a new event to an evdev. The triple (`type`, `code`, `value`) describes
 * the event. It should be in the same format as in `struct input_event`. */
void evdev_push_event(evdev_dev_t *evdev, uint16_t type, uint16_t code,
                      int32_t value);

/*
 * Utility functions.
 */

/* Convert a USB keyboard scancode to a evdev-compatible keycode. */
uint16_t evdev_hid2key(int scancode);

/* Convert a AT keyboard scancode to a evdev-compatible keycode. The function is
 * stateful because of AT extended scan codes - some keys are encoded in two
 * numbers. Hence, `statep` should point to the same state in the consecutive
 * function calls. */
uint16_t evdev_scancode2key(int *statep, int scancode);

/* A wrapper for the evdev_support_key function - marks all the AT-compatible
 * keys as supported. */
void evdev_support_all_known_keys(evdev_dev_t *evdev);

/* Convert a USB mouse button number to an evdev-compatible keycode. */
uint16_t evdev_hid2btn(uint8_t btnum);

/* A wrapper for the evdev_support_key function -
 * marks all the supported mouse keys. */
void evdev_support_all_known_buttons(evdev_dev_t *evdev);

/*
 * Event reporting shortcuts.
 */

static inline void evdev_sync(evdev_dev_t *evdev) {
  evdev_push_event(evdev, EV_SYN, SYN_REPORT, 1);
}

static inline void evdev_push_key(evdev_dev_t *evdev, uint16_t code,
                                  int32_t value) {
  evdev_push_event(evdev, EV_KEY, code, value != 0);
}

#endif /* !_DEV_EVDEV_H_ */
