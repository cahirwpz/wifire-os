#include <file.h>
#include <filedesc.h>
#include <malloc.h>
#include <stdc.h>
#include <errno.h>
#include <mutex.h>

static MALLOC_DEFINE(fd_pool, "file descriptors pool");

void fd_init() {
  kmalloc_init(fd_pool, 2, 2);
}

/* Test whether a file descriptor is in use. */
static int fd_is_used(fdtab_t *fdt, int fd) {
  return bit_test(fdt->fdt_map, fd);
}

static void fd_mark_used(fdtab_t *fdt, int fd) {
  assert(!fd_is_used(fdt, fd));
  bit_set(fdt->fdt_map, fd);
}

static void fd_mark_unused(fdtab_t *fdt, int fd) {
  assert(fd_is_used(fdt, fd));
  bit_clear(fdt->fdt_map, fd);
}

/* Grows given file descriptor table to contain new_size file descriptors
 * (up to MAXFILES) */
static void fd_growtable(fdtab_t *fdt, size_t new_size) {
  assert(fdt->fdt_nfiles < new_size && new_size <= MAXFILES);
  assert(mtx_owned(&fdt->fdt_mtx));

  file_t **old_fdt_files = fdt->fdt_files;
  bitstr_t *old_fdt_map = fdt->fdt_map;

  file_t **new_fdt_files =
    kmalloc(fd_pool, sizeof(file_t *) * new_size, M_ZERO);
  bitstr_t *new_fdt_map = kmalloc(fd_pool, bitstr_size(new_size), M_ZERO);

  memcpy(new_fdt_files, old_fdt_files, sizeof(file_t *) * fdt->fdt_nfiles);
  memcpy(new_fdt_map, old_fdt_map, bitstr_size(fdt->fdt_nfiles));
  kfree(fd_pool, old_fdt_files);
  kfree(fd_pool, old_fdt_map);

  fdt->fdt_files = new_fdt_files;
  fdt->fdt_map = new_fdt_map;
  fdt->fdt_nfiles = new_size;
}

/* Allocates a new file descriptor in a file descriptor table. Returns 0 on
 * success and sets *result to new descriptor number. Must be called with
 * fd->fd_mtx already locked. */
static int fd_alloc(fdtab_t *fdt, int *fdp) {
  assert(mtx_owned(&fdt->fdt_mtx));

  int first_free;
  bit_ffc(fdt->fdt_map, fdt->fdt_nfiles, &first_free);

  if (first_free < 0) {
    /* No more space to allocate a descriptor... grow describtor table! */
    if (fdt->fdt_nfiles == MAXFILES) {
      /* Reached limit of opened files. */
      return -EMFILE;
    }
    size_t new_size = min(fdt->fdt_nfiles * 2, MAXFILES);
    first_free = fdt->fdt_nfiles;
    fd_growtable(fdt, new_size);
  }
  fd_mark_used(fdt, first_free);
  *fdp = first_free;
  return 0;
}

static void fd_free(fdtab_t *fdt, int fd) {
  file_t *f = fdt->fdt_files[fd];
  assert(f != NULL);
  file_release(f);
  fdt->fdt_files[fd] = NULL;
  fd_mark_unused(fdt, fd);
}

void fdtab_ref(fdtab_t *fdt) {
  mtx_lock(&fdt->fdt_mtx);
  assert(fdt->fdt_count >= 0);
  ++fdt->fdt_count;
  mtx_unlock(&fdt->fdt_mtx);
}

void fdtab_unref(fdtab_t *fdt) {
  mtx_lock(&fdt->fdt_mtx);
  assert(fdt->fdt_count > 0);
  if (--fdt->fdt_count == 0)
    fdt->fdt_count = -1;
  mtx_unlock(&fdt->fdt_mtx);
}

/* In FreeBSD this function takes a filedesc* argument, so that
   current dir may be copied. Since we don't use these fields, this
   argument does not make sense yet. */
fdtab_t *fdtab_alloc() {
  fdtab_t *fdt = kmalloc(fd_pool, sizeof(fdtab_t), M_ZERO);
  fdt->fdt_nfiles = NDFILE;
  fdt->fdt_files = kmalloc(fd_pool, sizeof(file_t *) * NDFILE, M_ZERO);
  fdt->fdt_map = kmalloc(fd_pool, bitstr_size(NDFILE), M_ZERO);
  mtx_init(&fdt->fdt_mtx, MTX_DEF);
  return fdt;
}

fdtab_t *fdtab_copy(fdtab_t *fdt) {
  fdtab_t *newfdt = fdtab_alloc();

  if (fdt == NULL)
    return newfdt;

  mtx_scoped_lock(&fdt->fdt_mtx);

  if (fdt->fdt_nfiles > newfdt->fdt_nfiles) {
    mtx_scoped_lock(&newfdt->fdt_mtx);
    fd_growtable(newfdt, fdt->fdt_nfiles);
  }

  for (int i = 0; i < fdt->fdt_nfiles; i++) {
    if (fd_is_used(fdt, i)) {
      file_t *f = fdt->fdt_files[i];
      newfdt->fdt_files[i] = f;
      file_ref(f);
    }
  }

  memcpy(newfdt->fdt_map, fdt->fdt_map,
         sizeof(bitstr_t) * bitstr_size(fdt->fdt_nfiles));

  fdtab_ref(newfdt);
  return newfdt;
}

void fdtab_destroy(fdtab_t *fdt) {
  assert(fdt->fdt_count <= 0);
  /* No need to lock mutex, we have the only reference left. */

  /* Clean up used descriptors. This possibly closes underlying files. */
  for (int i = 0; i < fdt->fdt_nfiles; i++)
    if (fd_is_used(fdt, i))
      fd_free(fdt, i);

  kfree(fd_pool, fdt->fdt_files);
  kfree(fd_pool, fdt->fdt_map);
  kfree(fd_pool, fdt);
}

void fdtab_release(fdtab_t *fdt) {
  if (fdt == NULL)
    return;
  fdtab_unref(fdt);
  if (fdt->fdt_count < 0)
    fdtab_destroy(fdt);
}

int fdtab_install_file(fdtab_t *fdt, file_t *f, int *fd) {
  assert(f != NULL);
  assert(fd != NULL);

  mtx_scoped_lock(&fdt->fdt_mtx);
  int res = fd_alloc(fdt, fd);
  if (res < 0)
    return res;

  fdt->fdt_files[*fd] = f;
  file_ref(f);
  return 0;
}

/* Extracts file pointer from descriptor number in given table.
 * If flags are non-zero, returns EBADF if the file does not match flags. */
int fdtab_get_file(fdtab_t *fdt, int fd, int flags, file_t **fp) {
  if (!fdt)
    return -EBADF;

  mtx_scoped_lock(&fdt->fdt_mtx);

  if (fd < 0 || fd >= fdt->fdt_nfiles || !fd_is_used(fdt, fd))
    return -EBADF;

  file_t *f = fdt->fdt_files[fd];
  file_ref(f);

  if ((flags & FF_READ) && !(f->f_flags & FF_READ))
    goto fail;
  if ((flags & FF_WRITE) && !(f->f_flags & FF_WRITE))
    goto fail;

  *fp = f;
  return 0;

fail:
  file_unref(f);
  return -EBADF;
}

/* Closes a file descriptor. If it was the last reference to a file, the file is
 * also closed. */
int fdtab_close_fd(fdtab_t *fdt, int fd) {
  mtx_scoped_lock(&fdt->fdt_mtx);

  if (fd < 0 || fd > fdt->fdt_nfiles || !fd_is_used(fdt, fd))
    return -EBADF;

  fd_free(fdt, fd);
  return 0;
}
