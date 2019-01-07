#include <ringbuf.h>
#include <uio.h>
#include <malloc.h>

ringbuf_t ringbuf_alloc(kmem_pool_t *pool, size_t size) {
  ringbuf_t buf;

  assert(size > 0);

  buf.data = kmalloc(pool, size, M_ZERO);
  buf.size = size;
  ringbuf_reset(&buf);
  return buf;
}

void ringbuf_destroy(kmem_pool_t *pool, ringbuf_t buf) {
  kfree(pool, buf.data);
}

void ringbuf_reset(ringbuf_t *buf) {
  buf->head = 0;
  buf->tail = 0;
  buf->count = 0;
}

static void produce(ringbuf_t *buf, unsigned bytes) {
  assert(buf->count + bytes <= buf->size);
  assert(buf->head + bytes <= buf->size);
  buf->count += bytes;
  buf->head += bytes;
  if (buf->head == buf->size)
    buf->head = 0;
}

static void consume(ringbuf_t *buf, unsigned bytes) {
  assert(buf->count >= bytes);
  assert(buf->tail + bytes <= buf->size);
  buf->count -= bytes;
  buf->tail += bytes;
  if (buf->tail == buf->size)
    buf->tail = 0;
}

bool ringbuf_putb(ringbuf_t *buf, uint8_t byte) {
  if (buf->count == buf->size)
    return false;
  buf->data[buf->head] = byte;
  produce(buf, 1);
  return true;
}

bool ringbuf_getb(ringbuf_t *buf, uint8_t *byte_p) {
  if (buf->count == 0)
    return false;
  *byte_p = buf->data[buf->tail];
  consume(buf, 1);
  return true;
}

int ringbuf_read(ringbuf_t *buf, uio_t *uio) {
  assert(uio->uio_op == UIO_READ);
  /* repeat when used space is split into two parts */
  while (uio->uio_resid > 0 && !ringbuf_empty(buf)) {
    /* used space is either [tail, head) or [tail, size) */
    size_t size =
      (buf->tail < buf->head) ? buf->head - buf->tail : buf->size - buf->tail;
    if (size > uio->uio_resid)
      size = uio->uio_resid;
    int res = uiomove((char *)buf->data + buf->tail, size, uio);
    if (res)
      return res;
    consume(buf, size);
  }
  return 0;
}

int ringbuf_write(ringbuf_t *buf, uio_t *uio) {
  assert(uio->uio_op == UIO_WRITE);
  /* repeat when free space is split into two parts */
  while (uio->uio_resid > 0 && !ringbuf_full(buf)) {
    /* free space is either [head, tail) or [head, size) */
    size_t size =
      (buf->head < buf->tail) ? buf->tail - buf->head : buf->size - buf->head;
    if (size > uio->uio_resid)
      size = uio->uio_resid;
    int res = uiomove((char *)buf->data + buf->head, size, uio);
    if (res)
      return res;
    produce(buf, size);
  }
  return 0;
}
