#ifndef _DEV_DWC_OTG_

#include <sys/callout.h>
#include <sys/spinlock.h>
#include <sys/ringbuf.h>

/*
 * FIFO depths in terms of 32-bit words.
 */
#define DOTG_RXFSIZ 1024   /* receive FIFO depth */
#define DOTG_NPTXFSIZ 1024 /* non-periodic transmit FIFO depth */
#define DOTG_PTXFSIZ 1024  /* periodic transmit FIFO depth */

#define DOTG_TXFMAXNUM 16 /* max possible number of transmit FIFOs */
#define DOTG_MC 1         /* multi count */

#define DOTG_AHBIDLE_DELAY 100
#define DOTG_RXFFLSH_DELAY 1
#define DOTG_TXFFLSH_DELAY 1

/*
 * Parameters of the queue of pending transactions.
 */
#define DOTG_MAXPNDG 32 /* maximum number of pending transactions */
#define DOTG_PNDGBUFSIZ (DOTG_MAXPNDG * sizeof(dotg_txn_t *)) /* buf size */

typedef enum dotg_pid dotg_pid_t;
typedef enum dotg_stg_type dotg_stg_type_t;
typedef enum dotg_txn_status dotg_txn_status_t;
typedef struct dotg_stage dotg_stage_t;
typedef struct dotg_txn dotg_txn_t;
typedef struct dotg_callout_args dotg_callout_args_t;
typedef struct dotg_chan dotg_chan_t;
typedef struct dotg_state dotg_state_t;

/* Packet identifier (don't alter the following values!). */
enum dotg_pid {
  DOTG_PID_DATA0 = 0,
  DOTG_PID_DATA1 = 2,
  DOTG_PID_SETUP = 3,
} __packed;

/* Stage type.
 * NOTE: there is at most one stage of each type per transaction. */
enum dotg_stg_type {
  DOTG_STG_SETUP,
  DOTG_STG_DATA,
  DOTG_STG_STATUS,
  DOTG_STG_COUNT,
} __packed;

/* Stage structure. */
struct dotg_stage {
  dotg_stg_type_t type; /* stage type */
  void *data;           /* data to transfer */
  size_t size;          /* payload size */
  uint16_t pktcnt;      /* number of packets composing the stage */
  dotg_pid_t pid;       /* PID */
  usb_direction_t dir;  /* transfer direction */
};

/* clang-format off */

/* Transaction status. */
enum dotg_txn_status {
  DOTG_TXN_UNKNOWN,          /* unknown status */
  DOTG_TXN_PENDING,          /* waiting to be scheduled */
  DOTG_TXN_DISABLING_CHAN,   /* waiting for the channel to be disabled */
  DOTG_TXN_TRANSFERING,      /* waiting for the transfer to complete */
  DOTG_TXN_FINISHED,         /* transaction's finished */
} __packed;

/* clang-format on */

/* Transaction structure. */
struct dotg_txn {
  usb_device_t *udev; /* USB device requesting the transaction */
  usb_buf_t *buf;     /* USB buffer associated with the transaction */
  dotg_stage_t *stages[DOTG_STG_COUNT]; /* stages composing the transaction */
  uint8_t nstages;                      /* number of stages */
  uint8_t curr;                         /* current stage index */
  dotg_txn_status_t status;             /* transaction status */
};

/* DWC OTG callout arguments. */
struct dotg_callout_args {
  dotg_state_t *dotg; /* host controller state */
};

/* Host channel structure. */
struct dotg_chan {
  callout_t callout; /* callout used for rescheduling periodic requests */
  dotg_callout_args_t args; /* callout arguments */
  void *buf;                /* contiguous memory for DMA transfers */
  paddr_t buf_pa;           /* buffer's physicall address */
  dotg_txn_t *txn;          /* current transaction */
  uint8_t num;              /* channel's sequential number in the host system */
};

/* Controller's software context. */
struct dotg_state {
  condvar_t chan_cv;  /* wait for a free channel */
  spin_t chan_lock;   /* guards the host channel map and interrupt reg */
  bitstr_t *chan_map; /* bitmap of used host channels */

  condvar_t txn_cv; /* wait for any transaction to schedule */
  spin_t txn_lock;  /* transaction ring buffer lock */
  ringbuf_t txn_rb; /* ring buffer of pending transactions */

  dotg_chan_t *chans; /* provided host channels */
  thread_t *thread;   /* transaction scheduler */
  resource_t *mem;    /* MMIO handle */
  resource_t *irq;    /* IRQ handle */
  uint8_t nchans;     /* number of provided host channels */
};

#endif /* _DEV_DWC_OTG_ */
