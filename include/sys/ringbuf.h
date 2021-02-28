#ifndef _SYS_RINGBUF_H_
#define _SYS_RINGBUF_H_

#include <sys/cdefs.h>
#include <stdbool.h>

typedef struct uio uio_t;

typedef struct ringbuf {
  size_t head;   /*!< producing data moves head forward */
  size_t tail;   /*!< consuming data moves tail forward */
  size_t count;  /*!< number of bytes currently stored in the buffer */
  size_t size;   /*!< total size of the buffer */
  uint8_t *data; /*!< buffer that stores data */
} ringbuf_t;

static inline bool ringbuf_empty(ringbuf_t *buf) {
  return buf->count == 0;
}

static inline bool ringbuf_full(ringbuf_t *buf) {
  return buf->count == buf->size;
}

void ringbuf_init(ringbuf_t *rb, void *buf, size_t size);
void ringbuf_produce(ringbuf_t *buf, unsigned bytes);
void ringbuf_consume(ringbuf_t *buf, unsigned bytes);
bool ringbuf_putb(ringbuf_t *buf, uint8_t byte);
/*! \brief Put exactly n bytes into buf if there's enough space. */
bool ringbuf_putnb(ringbuf_t *buf, uint8_t *data, size_t n);
bool ringbuf_getb(ringbuf_t *buf, uint8_t *byte_p);
/*! \brief Get exactly n bytes from buf if there's enough data. */
bool ringbuf_getnb(ringbuf_t *buf, uint8_t *data, size_t n);
bool ringbuf_moveb(ringbuf_t *src, ringbuf_t *dst);
bool ringbuf_movenb(ringbuf_t *src, ringbuf_t *dst, size_t n);
bool ringbuf_restoreb(ringbuf_t *buf);
bool ringbuf_restorenb(ringbuf_t *buf, size_t n);
int ringbuf_read(ringbuf_t *buf, uio_t *uio);
int ringbuf_write(ringbuf_t *buf, uio_t *uio);
void ringbuf_reset(ringbuf_t *buf);

#endif /* !_SYS_RINGBUF_H_ */
