/*
 * evdev: event device infrastructure
 *
 * Heavily based on FreeBSD's code in `sys/dev/evdev/`.
 */

#include <dev/evdev.h>
#include <sys/devfs.h>
#include <sys/queue.h>
#include <sys/mutex.h>
#include <sys/devfs.h>
#include <sys/vnode.h>
#include <sys/malloc.h>
#include <sys/device.h>
#include <bitstring.h>
#include <sys/libkern.h>

/* Maximum length of an evdev device */
#define EVDEV_NAMELEN 80

/* Default parameters for a software emulation of key repetition. */
#define DEFAULT_REP_DELAY 250
#define DEFAULT_REP_PERIOD 33

/* The size of client's event queue (counted in the number of events) */
#define CLIENT_QUEUE_SIZE 32

/* evdev clock IDs in Linux semantic */
typedef enum {
  EV_CLOCK_REALTIME = 0, /* UTC clock */
  EV_CLOCK_MONOTONIC,    /* monotonic, stops on suspend */
  EV_CLOCK_BOOTTIME      /* monotonic, suspend-aware */
} evdev_clock_id_t;

/* Type definitions to abbreviate types used in the code below. */
typedef LIST_HEAD(, evdev_client) evdev_client_list_t;
typedef struct input_id input_id_t;

/* The vnode of the /dev/input directory. */
static devfs_node_t *evdev_input_dir;

/*
 * `evdev_dev_t` is the main structure of every input device. It contains both
 * general informations about the device and its state. A name, device ID and
 * supported features flags are only used by user-space.
 *
 * `evdev_client_t` is created whenever the evdev file is opened. Its jobs is to
 * handle an interaction with the user-space. Every client has its private event
 * buffer. Every time an input event is issued, it firstly goes through
 * `evdev_dev_t` and then it is propagated to clients. This allows us to have
 * multiple, non-interfering readers of a single evdev device. Hence, every
 * input device has exactly one `evdev_dev_t`, but possibly can have many
 * `evdev_client_t` structures linked to it.
 *
 * Field markings and the corresponding locks:
 * (!) - read-only access
 * (s) - evdev_dev::ev_lock
 * (c) - evdev_client::ec_lock
 */
typedef struct evdev_dev {
  char ev_name[EVDEV_NAMELEN]; /* (!) name of the device */
  input_id_t ev_id;            /* (!) device ID */

  /* Supported features: */
  bitstr_t bit_decl(ev_type_flags, EV_CNT);    /* (!) supported event types */
  bitstr_t bit_decl(ev_key_flags, KEY_CNT);    /* (!) supported key codes */
  bitstr_t bit_decl(ev_flags, EVDEV_FLAG_CNT); /* (!) supported features */

  /* Protects the data that is changed by incoming evdev events
   * and some ioctls. */
  mtx_t ev_lock;

  /* Repeat parameters & callout: */
  int ev_rep[REP_CNT]; /* (s) key repetition parameters (period and delay) */
  callout_t ev_rep_callout; /* (s) callout used for scheduling repetition */
  uint16_t ev_rep_key;      /* (s) currently repeated key */

  /* State: */
  bitstr_t bit_decl(ev_key_states, KEY_CNT); /* (s) state of all keys */
  evdev_client_list_t ev_clients; /* (s) clients associated with this device */
} evdev_dev_t;

/*
 * Events in an evdev client are stored in a ring buffer.
 *
 * Evdev client (e.g. user-space) is allowed to read events up to last EV_SYN,
 * which may not be the last element in the ring buffer. Thus we need to keep
 * track of last EV_SYN position in `ec_buffer_ready`.
 */
typedef struct evdev_client {
  devnode_t ev_dev;                 /* (!) device node of this client */
  evdev_dev_t *ec_evdev;            /* (!) associated evdev device */
  LIST_ENTRY(evdev_client) ec_link; /* (s) link on client list */

  /* Event ring buffer implementation: */
  mtx_t ec_lock;                /* serializes access to ring buffer data */
  evdev_clock_id_t ec_clock_id; /* (c) clock used to timestamp events */
  condvar_t ec_buffer_cv;       /* (c) wait here for state change to happen */
  size_t ec_buffer_size;        /* (c) ring buffer capacity */
  size_t ec_buffer_ready;       /* (c) read limit (see note above) */
  size_t ec_buffer_head;        /* (c) read end */
  size_t ec_buffer_tail;        /* (c) write end */
  input_event_t ec_buffer[];    /* (c) stored events */
} evdev_client_t;

/*
 * Event client functions.
 */

static bool evdev_client_empty(evdev_client_t *client) {
  return client->ec_buffer_head == client->ec_buffer_ready;
}

/* Get time depending on the client clock id */
static timeval_t evdev_client_gettime(evdev_client_t *client) {
  bintime_t btnow;

  switch (client->ec_clock_id) {
    case EV_CLOCK_BOOTTIME:
      __fallthrough;
    case EV_CLOCK_MONOTONIC:
      btnow = binuptime();
      break;
    case EV_CLOCK_REALTIME:
      __fallthrough;
    default:
      btnow = bintime();
      break;
  }

  timeval_t now;
  bt2tv(&btnow, &now);
  return now;
}

/* Push a single event to client's queue */
static void evdev_client_push(evdev_client_t *client, uint16_t type,
                              uint16_t code, int32_t value) {
  assert(mtx_owned(&client->ec_lock));

  size_t head = client->ec_buffer_head;
  size_t tail = client->ec_buffer_tail;
  size_t ready = client->ec_buffer_ready;
  size_t count = client->ec_buffer_size;

  /* If queue is full drop its content and place SYN_DROPPED event */
  if (tail + 1 == head || (tail + 1 - count) == head) {
    head = ready = 0;
    client->ec_buffer[head] =
      (input_event_t){.type = EV_SYN, .code = SYN_DROPPED, .value = 0};
    client->ec_buffer_head = head;
    client->ec_buffer_ready = head;
    tail = 1;
  }

  client->ec_buffer[tail].type = type;
  client->ec_buffer[tail].code = code;
  client->ec_buffer[tail].value = value;
  client->ec_buffer_tail = (tail == count - 1) ? 0 : (tail + 1);
}

/* Call this function only if EV_SYN arrived and reporting was turned on. */
static void evdev_client_notify(evdev_client_t *client) {
  assert(mtx_owned(&client->ec_lock));

  size_t ready = client->ec_buffer_ready;
  size_t count = client->ec_buffer_size;
  timeval_t tv = evdev_client_gettime(client);

  /* give all reported events the same timestamp */
  while (ready != client->ec_buffer_tail) {
    client->ec_buffer[ready].time = tv;
    ready = (ready == count - 1) ? 0 : (ready + 1);
  }

  /* move `ready` pointer and notify readers */
  client->ec_buffer_ready = client->ec_buffer_tail;
  cv_broadcast(&client->ec_buffer_cv);
}

/* Pop one event from the client's queue. Assumes the queue is nonempty! */
static void evdev_client_pop(evdev_client_t *client, input_event_t *event) {
  assert(mtx_owned(&client->ec_lock));
  assert(!evdev_client_empty(client));

  input_event_t *head = client->ec_buffer + client->ec_buffer_head;
  bcopy(head, event, sizeof(input_event_t));
  client->ec_buffer_head = client->ec_buffer_head + 1;
  if (client->ec_buffer_head == client->ec_buffer_size)
    client->ec_buffer_head = 0;
}

/*
 * Event device functions.
 */

static bool evdev_event_supported(struct evdev_dev *evdev, uint16_t type) {
  assert(type < EV_CNT);
  return bit_test(evdev->ev_type_flags, type);
}

static void evdev_set_repeat_params(struct evdev_dev *evdev, uint16_t property,
                                    int value) {
  assert(property < REP_CNT);
  evdev->ev_rep[property] = value;
}

static bool evdev_handle_event_key(evdev_dev_t *evdev, uint16_t type,
                                   uint16_t code, int32_t value) {
  switch (value) {
    case KEY_EVENT_UP:
    case KEY_EVENT_DOWN:
      if (bit_test(evdev->ev_key_states, code) == (uint32_t)value)
        return false;

      if (value)
        bit_set(evdev->ev_key_states, code);
      else
        bit_clear(evdev->ev_key_states, code);
      break;

    case KEY_EVENT_REPEAT:
      if (!bit_test(evdev->ev_key_states, code) ||
          !evdev_event_supported(evdev, EV_REP))
        return false;
      break;
  }

  return true;
}

static bool evdev_handle_event_rep(evdev_dev_t *evdev, uint16_t type,
                                   uint16_t code, int32_t value) {
  if (evdev->ev_rep[code] == value)
    return false;
  evdev_set_repeat_params(evdev, code, value);
  return true;
}

/* Handle incoming event. Returns false if the event should be skipped. */
static bool evdev_handle_event(evdev_dev_t *evdev, uint16_t type, uint16_t code,
                               int32_t value) {
  assert(mtx_owned(&evdev->ev_lock));

  if (type == EV_KEY)
    return evdev_handle_event_key(evdev, type, code, value);
  if (type == EV_REP)
    return evdev_handle_event_rep(evdev, type, code, value);
  return true;
}

/* Send an event to evdev. */
static void evdev_send_event(evdev_dev_t *evdev, uint16_t type, uint16_t code,
                             int32_t value) {
  if (!evdev_handle_event(evdev, type, code, value))
    return;

  /* Propagate one event to all clients
   * and notify them if it is the end of the event batch. */
  evdev_client_t *client;
  LIST_FOREACH (client, &evdev->ev_clients, ec_link) {
    WITH_MTX_LOCK (&client->ec_lock) {
      evdev_client_push(client, type, code, value);
      /* Allow users to read events only after report has been completed */
      if (type == EV_SYN && code == SYN_REPORT)
        evdev_client_notify(client);
    }
  }
}

/* Callout function called every key repetition. */
static void evdev_repeat_callout(void *arg) {
  evdev_dev_t *evdev = (evdev_dev_t *)arg;

  SCOPED_MTX_LOCK(&evdev->ev_lock);

  evdev_send_event(evdev, EV_KEY, evdev->ev_rep_key, KEY_EVENT_REPEAT);
  evdev_send_event(evdev, EV_SYN, SYN_REPORT, 1);

  if (evdev->ev_rep[REP_PERIOD])
    callout_reschedule(&evdev->ev_rep_callout,
                       evdev->ev_rep[REP_PERIOD] * CLK_TCK / 1000);
  else
    evdev->ev_rep_key = KEY_RESERVED;
}

static void evdev_start_repeat(evdev_dev_t *evdev, uint16_t key) {
  assert(mtx_owned(&evdev->ev_lock));

  if (evdev->ev_rep[REP_DELAY]) {
    evdev->ev_rep_key = key;
    callout_schedule(&evdev->ev_rep_callout,
                     evdev->ev_rep[REP_DELAY] * CLK_TCK / 1000);
  }
}

static void evdev_stop_repeat(evdev_dev_t *evdev) {
  assert(mtx_owned(&evdev->ev_lock));

  if (evdev->ev_rep_key != KEY_RESERVED) {
    callout_stop(&evdev->ev_rep_callout);
    evdev->ev_rep_key = KEY_RESERVED;
  }
}

/* Start/stop the key repetition callout depending on the key and its state. */
static void evdev_soft_repeat(evdev_dev_t *evdev, uint16_t key, int32_t state) {
  assert(mtx_owned(&evdev->ev_lock));

  /* Don't start the callout when there are no clients */
  if (LIST_EMPTY(&evdev->ev_clients))
    return;

  if (bit_test(evdev->ev_key_states, key) == (unsigned)state)
    return;

  if (state == KEY_EVENT_DOWN)
    evdev_start_repeat(evdev, key);
  else
    evdev_stop_repeat(evdev);
}

/*
 * Device file interface implementation.
 */

static int evdev_open(devnode_t *dev, file_t *fp, int oflags);
static int evdev_close(devnode_t *dev, file_t *fp);
static int evdev_read(devnode_t *dev, uio_t *uio);
static int evdev_ioctl(devnode_t *dev, u_long cmd, void *data, int fflags);

static devops_t evdev_devops = {
  .d_type = DT_OTHER,
  .d_open = evdev_open,
  .d_close = evdev_close,
  .d_read = evdev_read,
  .d_ioctl = evdev_ioctl,
};

static int evdev_read(devnode_t *dev, uio_t *uio) {
  evdev_client_t *client = dev->data;
  int error = 0;

  uio->uio_offset = 0; /* This device does not support offsets. */

  /* Zero-sized reads are allowed for error checking */
  if (uio->uio_resid != 0 && uio->uio_resid < sizeof(input_event_t))
    return EINVAL;

  WITH_MTX_LOCK (&client->ec_lock) {
    int remaining = uio->uio_resid / sizeof(input_event_t);

    if (evdev_client_empty(client) && remaining) {
      error = cv_wait_intr(&client->ec_buffer_cv, &client->ec_lock);
      error = (error == EINTR) ? ERESTARTSYS : error;
    }

    while (!error && remaining && !evdev_client_empty(client)) {
      input_event_t event;
      evdev_client_pop(client, &event);

      mtx_unlock(&client->ec_lock);
      error = uiomove(&event, sizeof(input_event_t), uio);
      mtx_lock(&client->ec_lock);

      remaining--;
    }
  }

  return error;
}

/* Handle EVIOCGBIT ioctl, which returns the bitmaps of supported features. */
static int evdev_ioctl_eviocgbit(evdev_dev_t *evdev, int type, int len,
                                 caddr_t data) {
  bitstr_t *bitmap;
  int limit;

  switch (type) {
    case 0:
      bitmap = evdev->ev_type_flags;
      limit = bitstr_size(EV_CNT);
      break;
    case EV_KEY:
      bitmap = evdev->ev_key_flags;
      limit = bitstr_size(KEY_CNT);
      break;
    default:
      return EINVAL;
  }

  /* Clear ioctl data buffer in case it's bigger than bitmap size. */
  bzero(data, len);
  memcpy(data, bitmap, min(limit, len));
  return 0;
}

static int evdev_ioctl(devnode_t *dev, u_long cmd, void *data, int fflags) {
  evdev_client_t *client = dev->data;
  evdev_dev_t *evdev = client->ec_evdev;

  /* evdev fixed-length ioctls handling */
  switch (cmd) {
    case EVIOCGVERSION:
      *(int *)data = EV_VERSION;
      return 0;
    case EVIOCGID:
      memcpy(data, &evdev->ev_id, sizeof(struct input_id));
      return 0;
    case EVIOCGREP:
      if (!evdev_event_supported(evdev, EV_REP))
        return ENOTSUP;

      WITH_MTX_LOCK (&evdev->ev_lock)
        memcpy(data, evdev->ev_rep, sizeof(evdev->ev_rep));
      return 0;
    case EVIOCSCLOCKID:
      WITH_MTX_LOCK (&client->ec_lock) {
        switch (*(int *)data) {
          case CLOCK_REALTIME:
            client->ec_clock_id = EV_CLOCK_REALTIME;
            return 0;
          case CLOCK_MONOTONIC:
            client->ec_clock_id = EV_CLOCK_MONOTONIC;
            return 0;
          default:
            return EINVAL;
        }
      }
  }

  /* evdev variable-length ioctls handling */
  size_t len = IOCPARM_LEN(cmd);
  int limit, type_num;

  switch (IOCBASECMD(cmd)) {
    case EVIOCGNAME(0):
      /* Linux evdev does not terminate truncated strings with 0 */
      limit = min(strlen(evdev->ev_name) + 1, len);
      memcpy(data, evdev->ev_name, limit);
      return 0;

    case EVIOCGKEY(0):
      limit = min(len, bitstr_size(KEY_CNT));
      WITH_MTX_LOCK (&evdev->ev_lock)
        memcpy(data, evdev->ev_key_states, limit);
      return 0;

    case EVIOCGBIT(0, 0)... EVIOCGBIT(EV_MAX, 0):
      type_num = IOCBASECMD(cmd) - EVIOCGBIT(0, 0);
      return evdev_ioctl_eviocgbit(evdev, type_num, len, data);
  }

  return EINVAL;
}

static int evdev_open(devnode_t *master_dev, file_t *fp, int oflags) {
  evdev_dev_t *evdev = master_dev->data;

  evdev_client_t *client = kmalloc(
    M_DEV, sizeof(evdev_client_t) + sizeof(input_event_t) * CLIENT_QUEUE_SIZE,
    M_WAITOK | M_ZERO);

  devnode_t *dev = &client->ev_dev;
  dev->data = client;
  dev->ops = &evdev_devops;
  refcnt_acquire(&dev->refcnt);

  client->ec_buffer_size = CLIENT_QUEUE_SIZE;

  client->ec_evdev = evdev;
  mtx_init(&client->ec_lock, 0);
  cv_init(&client->ec_buffer_cv, "ec_buffer_cv");

  WITH_MTX_LOCK (&evdev->ev_lock)
    LIST_INSERT_HEAD(&evdev->ev_clients, client, ec_link);

  fp->f_data = dev;

  return 0;
}

static void evdev_dispose_client(evdev_dev_t *evdev, evdev_client_t *client) {
  assert(mtx_owned(&evdev->ev_lock));

  LIST_REMOVE(client, ec_link);
  if (!LIST_EMPTY(&evdev->ev_clients))
    return;

  /* If there are no active clients left, stop the callout. */
  if (evdev_event_supported(evdev, EV_REP) &&
      bit_test(evdev->ev_flags, EVDEV_FLAG_SOFTREPEAT)) {
    evdev_stop_repeat(evdev);
  }
}

static int evdev_close(devnode_t *dev, file_t *fp) {
  evdev_client_t *client = dev->data;

  /* Unlink and free a `evdev_client_t` structure. */
  WITH_MTX_LOCK (&client->ec_evdev->ev_lock)
    evdev_dispose_client(client->ec_evdev, client);

  mtx_destroy(&client->ec_mtx);
  cv_destroy(&client->ec_buffer_cv);
  kfree(M_DEV, client);
  return 0;
}

/* Create a devfs device for a given evdev. */
static int evdev_dev_create(evdev_dev_t *evdev) {
  char buf[16];
  int ret, unit = 0;

  /* Find the first free number for that device.
   * FIXME Not the best solution, but will do for now. */
  do {
    snprintf(buf, sizeof(buf), "event%d", unit);
    ret = devfs_makedev_new(evdev_input_dir, buf, &evdev_devops, evdev, NULL);
    unit++;
  } while (ret == EEXIST);

  return ret;
}

/*
 * Evdev public interface (described in the header file).
 */

evdev_dev_t *evdev_alloc(void) {
  return kmalloc(M_DEV, sizeof(evdev_dev_t), M_WAITOK | M_ZERO);
}

void evdev_free(evdev_dev_t *evdev) {
  kfree(M_DEV, evdev);
}

void evdev_set_name(evdev_dev_t *evdev, const char *name) {
  snprintf(evdev->ev_name, EVDEV_NAMELEN, "%s", name);
}

void evdev_set_id(evdev_dev_t *evdev, uint16_t bustype, uint16_t vendor,
                  uint16_t product, uint16_t version) {
  evdev->ev_id = (input_id_t){.bustype = bustype,
                              .vendor = vendor,
                              .product = product,
                              .version = version};
}

void evdev_support_event(evdev_dev_t *evdev, uint16_t type) {
  assert(type < EV_CNT);
  bit_set(evdev->ev_type_flags, type);
}

void evdev_support_key(evdev_dev_t *evdev, uint16_t code) {
  assert(code < KEY_CNT);
  bit_set(evdev->ev_key_flags, code);
}

void evdev_set_flag(evdev_dev_t *evdev, uint16_t flag) {
  assert(flag < EVDEV_FLAG_CNT);
  bit_set(evdev->ev_flags, flag);
}

int evdev_register(evdev_dev_t *evdev) {
  mtx_init(&evdev->ev_lock, 0);
  LIST_INIT(&evdev->ev_clients);

  /* Initialize callout */
  callout_setup(&evdev->ev_rep_callout, evdev_repeat_callout, evdev);
  if (evdev->ev_rep[REP_DELAY] == 0 && evdev->ev_rep[REP_PERIOD] == 0) {
    /* Supply default values */
    evdev_set_repeat_params(evdev, REP_DELAY, DEFAULT_REP_DELAY);
    evdev_set_repeat_params(evdev, REP_PERIOD, DEFAULT_REP_PERIOD);
  }

  return evdev_dev_create(evdev);
}

void evdev_push_event(evdev_dev_t *evdev, uint16_t type, uint16_t code,
                      int32_t value) {
  SCOPED_MTX_LOCK(&evdev->ev_lock);

  if (type == EV_KEY && evdev_event_supported(evdev, EV_REP)) {
    if (bit_test(evdev->ev_flags, EVDEV_FLAG_SOFTREPEAT)) {
      /* Start/stop callout for evdev repeats */
      evdev_soft_repeat(evdev, code, value);
    } else {
      /* Detect driver key repeats. */
      if (bit_test(evdev->ev_key_states, code) && value == KEY_EVENT_DOWN)
        value = KEY_EVENT_REPEAT;
    }
  }

  evdev_send_event(evdev, type, code, value);
}

static void init_evdev(void) {
  if (devfs_makedir(NULL, "input", &evdev_input_dir) != 0)
    panic("failed to create /dev/input directory");
}

SET_ENTRY(devfs_init, init_evdev);
