#define KL_LOG KL_FILESYS
#include <sys/mimiker.h>
#include <sys/vfs.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/malloc.h>
#include <sys/types.h>
#include <sys/pool.h>
#include <sys/dirent.h>
#include <sys/libkern.h>

static KMALLOC_DEFINE(M_EXT2, "ext2fs");

/*
 * The first super block and block group descriptors offsets are given in
 * absolute disk addresses.
 */
#define EXT2_SBOFF ((off_t)1024)
#define EXT2_GDOFF ((off_t)2048)

#define EXT2_BLKSIZE 1024

#define EXT2_NDADDR 12 /* Direct addresses in inode. */
#define EXT2_NIADDR 3  /* Indirect addresses in inode. */
#define EXT2_NADDR (EXT2_NDADDR + EXT2_NIADDR)

#define EXT2_MAXSYMLINKLEN ((EXT2_NDADDR + EXT2_NIADDR) * sizeof(uint32_t))

/* How many block pointers fit into one block? */
#define EXT2_BLK_POINTERS (EXT2_BLKSIZE / sizeof(uint32_t))

/* How many i-nodes fit into one block? */
#define EXT2_BLK_INODES (EXT2_BLKSIZE / sizeof(ext2_inode_t))

/* How many block pointers fit into one block? */
#define EXT2_BLK_POINTERS (EXT2_BLKSIZE / sizeof(uint32_t))

/* EXT2_BLK_ZERO is a special value that reflect the fact that block 0 may be
 * used to represent a block filled with zeros. */
#define EXT2_BLK_ZERO ((void *)-1L)

/*
 * Filesystem identification
 */
#define EXT2_MAGIC 0xEF53 /* the ext2fs magic number */
#define EXT2_REV0 0       /* GOOD_OLD revision */
#define EXT2_REV1 1       /* Support compat/incompat features */

/* File permissions. */
#define EXT2_IEXEC 0000100  /* Executable. */
#define EXT2_IWRITE 0000200 /* Writable. */
#define EXT2_IREAD 0000400  /* Readable. */
#define EXT2_ISVTX 0001000  /* Sticky bit. */
#define EXT2_ISGID 0002000  /* Set-gid. */
#define EXT2_ISUID 0004000  /* Set-uid. */

/* File types. (dirent) */
#define EXT2_FT_UNKNOWN 0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR 2
#define EXT2_FT_CHRDEV 3
#define EXT2_BLKDEV 4
#define EXT2_FIFO 5
#define EXT2_SOCK 6
#define EXT2_SYMLINK 7

/* File types (inode's i_mode field) */
#define EXT2_IFMT 0170000   /* Mask of file type. */
#define EXT2_IFIFO 0010000  /* Named pipe (fifo). */
#define EXT2_IFCHR 0020000  /* Character device. */
#define EXT2_IFDIR 0040000  /* Directory file. */
#define EXT2_IFBLK 0060000  /* Block device. */
#define EXT2_IFREG 0100000  /* Regular file. */
#define EXT2_IFLNK 0120000  /* Symbolic link. */
#define EXT2_IFSOCK 0140000 /* UNIX domain socket. */

#define EXT2_I_MODE_FT_MASK 0xf000

/*
 * File system super block.
 */
typedef struct ext2_superblock {
  uint32_t sb_icount;        /* Inode count */
  uint32_t sb_bcount;        /* blocks count */
  uint32_t sb_rbcount;       /* reserved blocks count */
  uint32_t sb_fbcount;       /* free blocks count */
  uint32_t sb_ficount;       /* free inodes count */
  uint32_t sb_first_dblock;  /* first data block */
  uint32_t sb_log_bsize;     /* bsize = 1024*(2^sb_log_bsize) */
  uint32_t sb_fsize;         /* fragment size */
  uint32_t sb_bpg;           /* blocks per group */
  uint32_t sb_fpg;           /* frags per group */
  uint32_t sb_ipg;           /* inodes per group */
  uint32_t sb_mtime;         /* mount time */
  uint32_t sb_wtime;         /* write time */
  uint16_t sb_mnt_count;     /* mount count */
  uint16_t sb_max_mnt_count; /* max mount count */
  uint16_t sb_magic;         /* magic number */
  uint16_t sb_state;         /* file system state */
  uint16_t sb_beh;           /* behavior on errors */
  uint16_t sb_minrev;        /* minor revision level */
  uint32_t sb_lastfsck;      /* time of last fsck */
  uint32_t sb_fsckintv;      /* max time between fscks */
  uint32_t sb_creator;       /* creator OS */
  uint32_t sb_rev;           /* revision level */
  uint16_t sb_ruid;          /* default uid for reserved blocks */
  uint16_t sb_rgid;          /* default gid for reserved blocks */
  /* EXT2_DYNAMIC_REV superblocks */
  uint32_t sb_first_ino;         /* first non-reserved inode */
  uint16_t sb_inode_size;        /* size of inode structure */
  uint16_t sb_block_group_nr;    /* block grp number of this sblk */
  uint32_t sb_features_compat;   /*  compatible feature set */
  uint32_t sb_features_incompat; /* incompatible feature set */
  uint32_t sb_features_rocompat; /* RO-compatible feature set */
  uint8_t sb_uuid[16];           /* 128-bit uuid for volume */
  char sb_vname[16];             /* volume name */
  char sb_fsmnt[64];             /* name mounted on */
  uint32_t sb_algo;              /* For compression */
  uint8_t sb_prealloc;           /* # of blocks to preallocate */
  uint8_t sb_dir_prealloc;       /* # of blocks to preallocate for dir */
  uint16_t sb_reserved_ngdb;     /* # of reserved gd blocks for resize */
} ext2_superblock_t;

/*
 * Structure of an inode on the disk.
 */
typedef struct ext2_inode {
  uint16_t i_mode;               /*   0: IFMT, permissions; see below. */
  uint16_t i_uid;                /*   2: Owner UID */
  uint32_t i_size;               /*   4: Size (in bytes) */
  uint32_t i_atime;              /*   8: Access time */
  uint32_t i_ctime;              /*  12: Change time */
  uint32_t i_mtime;              /*  16: Modification time */
  uint32_t i_dtime;              /*  20: Deletion time */
  uint16_t i_gid;                /*  24: Owner GID */
  uint16_t i_nlink;              /*  26: File link count */
  uint32_t i_nblock;             /*  28: Blocks count */
  uint32_t i_flags;              /*  32: Status flags (chflags) */
  uint32_t i_version;            /*  36: Low 32 bits inode version */
  uint32_t i_blocks[EXT2_NADDR]; /*  40: disk blocks */
  uint32_t i_gen;                /* 100: generation number */
  uint32_t i_facl;               /* 104: Low EA block */
  uint32_t i_size_high;          /* 108: Upper bits of file size */
  uint32_t i_faddr;              /* 112: Fragment address (obsolete) */
  uint16_t i_nblock_high;        /* 116: Blocks count bits 47:32 */
  uint16_t i_facl_high;          /* 118: File EA bits 47:32 */
  uint16_t i_uid_high;           /* 120: Owner UID top 16 bits */
  uint16_t i_gid_high;           /* 122: Owner GID top 16 bits */
  uint16_t i_chksum_lo;          /* 124: Lower inode checksum */
  uint16_t i_lx_reserved;        /* 126: Unused */
} ext2_inode_t;

/*
 * File system block group descriptor.
 */
typedef struct ext2_groupdesc {
  uint32_t gd_b_bitmap; /* blocks bitmap block */
  uint32_t gd_i_bitmap; /* inodes bitmap block */
  uint32_t gd_i_tables; /* first inodes table block */
  uint16_t gd_nbfree;   /* number of free blocks */
  uint16_t gd_nifree;   /* number of free inodes */
  uint16_t gd_ndirs;    /* number of directories */

  /*
   * Following only valid when either GDT_CSUM or METADATA_CKSUM feature is on.
   */
  uint16_t gd_flags;                /* ext4 bg flags (INODE_UNINIT, ...)*/
  uint32_t gd_exclude_bitmap_lo;    /* snapshot exclude bitmap */
  uint16_t gd_block_bitmap_csum_lo; /* Low block bitmap checksum */
  uint16_t gd_inode_bitmap_csum_lo; /* Low inode bitmap checksum */
  uint16_t gd_itable_unused_lo;     /* Low unused inode offset */
  uint16_t gd_checksum;             /* Group desc checksum */
} ext2_groupdesc_t;

/*
 * The EXT2_DIRSIZE macro gives the minimum record length which will hold
 * the directory entry for a name of length `len` (without the terminating null
 * byte). This requires the amount of space in struct direct without the
 * de_name field, plus enough space for the name without a terminating null
 * byte, rounded up to a 4 byte boundary.
 */
#define EXT2_DIRSIZE(len) roundup2(8 + len, 4)

#define EXT2_MAXNAMLEN 255

typedef struct ext2_dirent {
  uint32_t de_ino;                  /* inode number of entry */
  uint16_t de_reclen;               /* length of this record */
  uint8_t de_namelen;               /* length of string in de_name */
  uint8_t de_type;                  /* file type */
  char de_name[EXT2_MAXNAMLEN + 1]; /* name with length <= EXT2_MAXNAMLEN */
} ext2_dirent_t;


typedef struct ext2state {
  vnode_t *src;
  /* Properties extracted from a superblock and block group descriptors. */
  size_t inodes_per_group;      /* number of i-nodes in block group */
  size_t blocks_per_group;      /* number of blocks in block group */
  size_t group_desc_count;      /* numbre of block group descriptors */
  size_t block_count;           /* number of blocks in the filesystem */
  size_t inode_count;           /* number of i-nodes in the filesystem */
  size_t first_data_block;      /* first block managed by block bitmap */
  ext2_groupdesc_t *group_desc; /* block group descriptors in memory */
} ext2_state_t;

typedef TAILQ_HEAD(, vnode) vnode_list_t;

typedef struct ext2_vndata {
  ino_t inode;
  mount_t *mount;
  vnode_t *parent;
  vnode_list_t children;  /* Holds pointers to currently active vnodes */
} ext2_vndata_t;

/* Reads block bitmap entry for `blkaddr`. Returns 0 if the block is free,
 * 1 if it's in use, and EINVAL if `blkaddr` is out of range. */
static int ext2_block_used(ext2_state_t *state, uint32_t blkaddr) {
  if (blkaddr >= state->block_count)
    return EINVAL;
  int used = 0;
  size_t gdidx = (blkaddr - state->first_data_block) / state->blocks_per_group;
  size_t biidx = (blkaddr - state->first_data_block) % state->blocks_per_group;
  void *blk = blk_get(state, 0, state->group_desc[gdidx].gd_b_bitmap);
  uint8_t *bbm = blk;
  used = (bbm[biidx / 8] >> (biidx % 8)) & 1;
  blk_put(blk);
  return used;
}

/* Reads i-node bitmap entry for `ino`. Returns 0 if the i-node is free,
 * 1 if it's in use, and EINVAL if `ino` value is out of range. */
static int ext2_inode_used(ext2_state_t *state, uint32_t ino) {
  if (!ino || ino >= state->inode_count)
    return EINVAL;
  int used = 0;
#ifndef STUDENT
  size_t gdidx = (ino - 1) / state->inodes_per_group;
  size_t diidx = (ino - 1) % state->inodes_per_group;
  void *blk = blk_get(0, state->group_desc[gdidx].gd_i_bitmap);
  /*
   * The first block of this block group is represented by bit 0 of byte 0,
   * the second by bit 1 of byte 0. The 8th block is represented by bit 7
   * (most significant bit) of byte 0 while the 9th block is represented
   * by bit 0 (least significant bit) of byte 1.
   */
  uint8_t *ibm = blk;
  used = (ibm[diidx / 8] >> (diidx % 8)) & 1;
  blk_put(blk);
#endif /* STUDENT */
  return used;
}

/* Reads i-node identified by number `ino`.
 * Returns 0 on success. If i-node is not allocated returns ENOENT. */
static int ext2_inode_read(ext2_state_t *state, off_t ino,
                           ext2_inode_t *inode) {
#ifndef STUDENT
  if (!ext2_inode_used(state, ino))
    return ENOENT;

  size_t gdidx = (ino - 1) / state->inodes_per_group;
  size_t diidx = (ino - 1) % state->inodes_per_group;
  void *blk =
    blk_get(0, state->group_desc[gdidx].gd_i_tables + diidx / EXT2_BLK_INODES);
  memcpy(inode, blk + (diidx % EXT2_BLK_INODES) * sizeof(ext2_inode_t),
         sizeof(ext2_inode_t));
  blk_put(blk);
  return 0;
#endif /* STUDENT */
  return ENOENT;
}

/* Returns block pointer `blkidx` from block of `blkaddr` address. */
static uint32_t ext2_blkptr_read(ext2_state_t *state, uint32_t blkaddr,
                                 uint32_t blkidx) {
  assert(blkidx < EXT2_BLK_POINTERS);
  void *blk = blk_get(state, 0, blkaddr);
  assert(blk != EXT2_BLK_ZERO && blk != NULL);
  uint32_t *ptrs = blk;
  uint32_t blkptr = ptrs[blkidx];
  blk_put(blk);
  return blkptr;
}

long ext2_blkaddr_read(ext2_state_t *state, uint32_t ino, uint32_t blkidx) {
  /* No translation for filesystem metadata blocks. */
  if (ino == 0)
    return blkidx;

  ext2_inode_t inode;
  if (ext2_inode_read(state, ino, &inode))
    return -1;

  /* Read direct pointers or pointers from indirect blocks. */
  /* Check if `blkidx` is not past the end of file. */
  if (blkidx * EXT2_BLKSIZE >= inode.i_size)
    return -1;

  if (blkidx < EXT2_NDADDR)
    return inode.i_blocks[blkidx];

  blkidx -= EXT2_NDADDR;
  if (blkidx < EXT2_BLK_POINTERS)
    return ext2_blkptr_read(state, inode.i_blocks[EXT2_NDADDR], blkidx);

  uint32_t l2blkaddr, l3blkaddr, l1blkidx;

  blkidx -= EXT2_BLK_POINTERS;
  if (blkidx < EXT2_BLK_POINTERS * EXT2_BLK_POINTERS) {
    l2blkaddr =
      ext2_blkptr_read(state, inode.i_blocks[EXT2_NDADDR + 1],
                       blkidx / EXT2_BLK_POINTERS);
    return ext2_blkptr_read(state, l2blkaddr, blkidx % EXT2_BLK_POINTERS);
  }

  blkidx -= EXT2_BLK_POINTERS * EXT2_BLK_POINTERS;
  l1blkidx = blkidx / (EXT2_BLK_POINTERS * EXT2_BLK_POINTERS);
  blkidx %= EXT2_BLK_POINTERS * EXT2_BLK_POINTERS;
  l2blkaddr =
    ext2_blkptr_read(state, inode.i_blocks[EXT2_NDADDR + 2],l1blkidx);
  l3blkaddr = ext2_blkptr_read(state, l2blkaddr, blkidx / EXT2_BLK_POINTERS);
  return ext2_blkptr_read(state, l3blkaddr, blkidx % EXT2_BLK_POINTERS);
  return -1;
}

static int ext2_read(ext2_state_t *state, ino_t ino, void *data, size_t pos,
                     size_t len) {
  
  long off = pos % EXT2_BLKSIZE;
  long idx = pos / EXT2_BLKSIZE;

  while (len > 0) {
    size_t cnt = min(EXT2_BLKSIZE - off, len);
    void *blk = blk_get(state, ino, idx++);
    if (blk == NULL)
      return EINVAL;
    if (blk == EXT2_BLK_ZERO) {
      memset(data, 0, cnt);
    } else {
      memcpy(data, blk + off, cnt);
      blk_put(state, blk);
    }
    data += cnt;
    len -= cnt;
    off = 0;
  }
}

static int ext2_mount(mount_t *mp, vnode_t *src) {
  int error;

  ext2_state_t *state = kmalloc(M_EXT2, sizeof(ext2_state_t), 0);
  mp->mnt_data = state;

  vnode_hold(src);
  state->src = src;

  ext2_superblock_t *sb = kmalloc(M_EXT2, sizeof(ext2_superblock_t), 0);
  ext2_read(state, 0, &sb, EXT2_SBOFF, sizeof(ext2_superblock_t));
  
  klog("EXT2 super block:\n"
       "  # of inodes      : %d\n"
       "  # of blocks      : %d\n"
       "  block size       : %ld\n"
       "  blocks per group : %d\n"
       "  inodes per group : %d\n"
       "  inode size       : %d\n",
       sb->sb_icount, sb->sb_bcount, 1024UL << sb->sb_log_bsize, sb->sb_bpg,
       sb->sb_ipg, sb->sb_inode_size);
  
  if (sb->sb_magic != EXT2_MAGIC)
    return EINVAL;
  /* Only ext2 revision 1 is currently supported! */
  if (sb->sb_rev != EXT2_REV1)
    return EINVAL;
  
  size_t blksize = 1024UL << sb->sb_log_bsize;
  if (blksize != EXT2_BLKSIZE)
    panic("ext2 filesystem with block size %ld not supported!", blksize);

  if (sb->sb_inode_size != sizeof(ext2_inode_t))
    panic("The only i-node size supported is %d!", sizeof(ext2_inode_t));

  /* Load interesting data from superblock into global variables.
   * Read group descriptor table into memory. */
  state->block_count = sb->sb_bcount;
  state->inode_count = sb->sb_icount;
  state->group_desc_count = howmany(sb->sb_bcount, sb->sb_bpg);
  state->blocks_per_group = sb->sb_bpg;
  state->inodes_per_group = sb->sb_ipg;
  state->first_data_block = sb->sb_first_dblock;

  size_t gd_size = state->group_desc_count * sizeof(ext2_groupdesc_t);
  state->group_desc = malloc(gd_size);
  ext2_read(state, 0, state->group_desc, EXT2_GDOFF, gd_size);

  /* debug("\n>>> block group descriptor table (%ld entries)\n\n",
        state->group_desc_count);
  for (size_t i = 0; i < state->group_desc_count; i++) {
    ext2_groupdesc_t *gd __unused = &group_desc[i];
    debug("* block group descriptor %ld (has backup: %s):\n"
          "  i-nodes: %ld - %ld, blocks: %d - %ld, directories: %d\n"
          "  blocks bitmap: %d, inodes bitmap: %d, inodes table: %d\n",
          i, ext2_gd_has_backup(i) ? "yes" : "no",
          1 + i * state->inodes_per_group,
          (i + 1) * inodes_per_group, gd->gd_b_bitmap,
          min(gd->gd_b_bitmap + blocks_per_group, block_count) - 1,
          gd->gd_ndirs, gd->gd_b_bitmap, gd->gd_i_bitmap, gd->gd_i_tables);
  } */
  return 0;
}

#define de_name_offset offsetof(ext2_dirent_t, de_name)

/* Reads a directory entry at position stored in `off_p` from `ino` i-node that
 * is assumed to be a directory file. The entry is stored in `de` and
 * `de->de_name` must be NULL-terminated. Assumes that entry offset is 0 or was
 * set by previous call to `ext2_readdir`. Returns 1 on success, 0 if there are
 * no more entries to read. */
int ext2_readdir(ext2_state_t *state,uint32_t ino, uint32_t *off_p,
                 ext2_dirent_t *de) {
  de->de_ino = 0;
  while (true) {
    if (ext2_read(state, ino, de, *off_p, de_name_offset))
      return 0;
    if (de->de_ino)
      break;
    /* Bogus entry -> skip it! */
    *off_p += de->de_reclen;
  }
  if (ext2_read(state, ino, &de->de_name, *off_p + de_name_offset,
                de->de_namelen))
    panic("Attempt to read past the end of directory!");
  de->de_name[de->de_namelen] = '\0';
  *off_p += de->de_reclen;
  return 1;
}

/* Read the target of a symbolic link identified by `ino` i-node into buffer
 * `buf` of size `buflen`. Returns 0 on success, EINVAL if the file is not a
 * symlink or read failed. */
int ext2_readlink(ext2_state_t *state, uint32_t ino, char *buf, size_t buflen) {
  int error;

  ext2_inode_t inode;
  if ((error = ext2_inode_read(state, ino, &inode)))
    return error;

  /* Check if it's a symlink and read it. */
#ifndef STUDENT
  if (!(inode.i_mode & EXT2_IFLNK))
    return EINVAL;

  size_t len = min(inode.i_size, buflen);
  if (inode.i_size < EXT2_MAXSYMLINKLEN) {
    memcpy(buf, inode.i_blocks, len);
  } else {
    if ((error = ext2_read(state, ino, buf, 0, len)))
      return error;
  }

  return 0;
#endif /* STUDENT */
  return ENOTSUP;
}

/* Reads file identified by `ino` i-node as directory and performs a lookup of
 * `name` entry. If an entry is found, its i-inode number is stored in `ino_p`
 * and its type in stored in `type_p`. On success returns 0, or EINVAL if `name`
 * is NULL or zero length, or ENOTDIR is `ino` file is not a directory, or
 * ENOENT if no entry was found. */
static int ext2_lookup(ext2_state_t *state, uint32_t ino, const char *name,
                       uint32_t *ino_p, uint8_t *type_p) {
  int error;

  if (name == NULL || !strlen(name))
    return EINVAL;

  ext2_inode_t inode;
  if ((error = ext2_inode_read(state, ino, &inode)))
    return error;
  
  if (!(inode.i_mode & EXT2_IFDIR))
    return ENOTDIR;

  ext2_dirent_t de;
  uint32_t off = 0;
  while (ext2_readdir(state, ino, &off, &de)) {
    if (!strcmp(name, de.de_name)) {
      if (ino_p)
        *ino_p = de.de_ino;
      if (type_p)
        *type_p = de.de_type;
      return 0;
    }
  }

  return ENOENT;
}

static vnodetype_t ext2_ino_type_to_vnode_type_lookup[] = {
  [EXT2_FT_UNKNOWN] = V_NONE,
  [EXT2_FT_REG_FILE] = V_REG,
  [EXT2_FT_DIR] = V_DIR,
  [EXT2_FT_CHRDEV] = V_REG,
  [EXT2_BLKDEV] = V_REG,
  [EXT2_FIFO] = V_REG,
  [EXT2_SOCK] = V_REG,
  [EXT2_SYMLINK] = V_LNK,
};

static vnodetype_t ext2_ino_type_to_vdir_type_lookup[] = {
  [EXT2_FT_UNKNOWN] = DT_UNKNOWN,
  [EXT2_FT_REG_FILE] = DT_REG,
  [EXT2_FT_DIR] = DT_DIR,
  [EXT2_FT_CHRDEV] = DT_CHR,
  [EXT2_BLKDEV] = DT_BLK,
  [EXT2_FIFO] = DT_FIFO,
  [EXT2_SOCK] = DT_SOCK,
  [EXT2_SYMLINK] = DT_LNK,
};

static vnodeops_t ext2_vops;

static vnodetype_t ext2_inode_type_to_vnode_type(uint8_t ino_type) {
  if (ino_type >= sizeof(ext2_ino_type_to_vnode_type_lookup) /
      sizeof(vnodeops_t))
    panic("Unrecognizable ext2 inode type");
  return ext2_ino_type_to_vnode_type_lookup[ino_type];
}

/* Return a vnode for given inode, that is a child of `parent`. Use an already
 * existing vnode, or create a new one if necessary. */
static vnode_t *ext2_vnode_open(vnode_t *parent, uint32_t ino,
                                vnodetype_t type) {
  vnode_get(parent);
  ext2_vndata_t *parent_data = parent->v_data;
  vnode_t *child;
  TAILQ_FOREACH(child, &parent_data->children, v_list) {
    vnode_get(child);
    ext2_vndata_t *child_data = child->v_data;
    if (child_data->inode == ino) {
      vnode_hold(child);
      vnode_put(child);
      vnode_put(parent);
      return child;
    }
    vnode_put(child);
  }
  ext2_vndata_t *child_data = kmalloc(M_EXT2, sizeof(ext2_vndata_t), 0);
  child_data->inode = ino;
  child_data->mount = parent_data->mount;
  child_data->parent = parent;
  TAILQ_INIT(&child_data->children);
  child = vnode_new(type, &ext2_vops, child_data);
  TAILQ_INSERT_TAIL(&parent_data->children, child, v_list);
  vnode_hold(parent);
  vnode_put(parent);
  return child;
}

static vnode_t *ext2_dirent_to_vnode(const ext2_dirent_t *de, vnode_t *parent) {
  vnodetype_t vtype = ext2_ino_type_to_vnode_type(de->de_type);
  return ext2_vnode_open(parent, de->de_ino, vtype);
}

static int ext2_lookup_vop(vnode_t *vdir, componentname_t *cn, vnode_t **res) {
  int error;
  ext2_vndata_t *vndata = vdir->v_data;
  ext2_state_t *state = vndata->mount->mnt_data;

  if (vdir->v_type != V_DIR)
    return ENOTDIR;
  
  ext2_inode_t inode;
  if ((error = ext2_inode_read(state, vndata->inode, &inode)))
    return error;
  
  assert(inode.i_mode & EXT2_IFDIR);

  ext2_dirent_t *de = kmalloc(M_EXT2, sizeof(ext2_dirent_t), 0);
  uint32_t off = 0;
  uint32_t ino;
  uint8_t type;
  while (ext2_readdir(state, vndata->inode, &off, de)) {
    if (componentname_equal(cn, de->de_name)) {
      *res = ext2_dirent_to_vnode(vdir, de);
      kfree(M_EXT2, de);
      return 0;
    }
  }

  kfree(M_EXT2, de);
  return ENOENT;
}

static uint8_t ext2_i_mode_to_d_type(uint16_t i_mode) {
  return ext2_ino_type_to_vnode_type_lookup[i_mode];
}

/* Convert ext2 dirent to vfs dirent */
static void ext2_convert_to_vfs_dirent(ext2_state_t *state, uint32_t parent_ino,
                                       uint32_t my_ino,
                                       const ext2_dirent_t *from,
                                       dirent_t *to) {
  const char *name;
  ino_t fileno;
  if (!strncmp(from->de_name, ".", max(from->de_namelen, 0))) {
    fileno = my_ino;
    name = ".";
  } else if (!strncmp(from->de_name, "..", max(from->de_namelen, 0))) {
    fileno = parent_ino;
    name = "..";
  } else {
    fileno = from->de_ino;
    name = from->de_name;
  }

  memcpy(to->d_name, name, from->de_namelen);
  to->d_name[from->de_namelen] = '\0';

  ext2_inode_t inode;
  if (ext2_inode_read(state, fileno, &inode))
    panic("Failed to read inode: 0x%x", fileno);
  to->d_type = ext2_i_mode_to_d_type(inode.i_mode);
}

/* Hacky way to fake `.` and `..` entries */
static int ext2_readdir_wdots(ext2_state_t *state, uint32_t ino,
                              uint32_t *off_p, ext2_dirent_t *de) {
  if (off_p == 0) {
    de->de_ino = (unsigned)(-1);
    strcpy(de->de_name, ".");
    de->de_namelen = 1;
    de->de_type = EXT2_FT_DIR;
    de->de_reclen = 1;
    return 1;
  }
  if (off_p == 1) {
    de->de_ino = (unsigned)(-1);
    strcpy(de->de_name, "..");
    de->de_namelen = 1;
    de->de_type = EXT2_FT_DIR;
    de->de_reclen = 1;
    return 1;
  }
  ext2_readdir(state, ino, off_p - 2, de);
}

/* Based on vfs_readdir. Unfortunately the generic routine was insufficient
 * because it cannot allocate iterators. It just assumes all posible states
 * exist somewhere in the memory. Also it assumes that filesystem's structure
 * is persistent and won't change in the middle of execution, so it does not
 * vnodes. */
static int ext2_readdir_vop(vnode_t *v, uio_t *uio) {
  vnode_lock(v);
  
  ext2_vndata_t *vndata = v->v_data;
  ext2_state_t *state = vndata->mount->mnt_data;
  dirent_t *dir;
  int error;
  off_t offset = 0;

  /* Locate proper directory based on offset */
  ext2_dirent_t *de = kmalloc(M_EXT2, sizeof(ext2_dirent_t), 0);
  uint32_t off = 0;
  while (ext2_readdir(state, vndata->inode, off, de)) {
    if (offset + de->de_namelen > (unsigned)uio->uio_offset)
      break;
    off += de->de_reclen;
  }

  while (ext2_readdir_wdots(state, vndata->inode, off, &de)) {
    unsigned reclen = _DIRENT_RECLEN(dir, de->de_namelen);
    
    if (uio->uio_resid < reclen)
      break;
    
    dir = kmalloc(M_TEMP, reclen, M_ZERO);
    dir->d_namlen = de->de_namelen;
    dir->d_reclen = reclen;


    uint32_t my_ino = vndata->inode;
    uint32_t parent_ino = vndata->parent
      ? ((ext2_vndata_t*)(vndata->parent->v_data))->inode
      : vndata->inode;
    ext2_convert_to_vfs_dirent(state, parent_ino, my_ino, de, dir);

    error = uiomove(dir, reclen, uio);
    kfree(M_TEMP, dir);
    if (error) {
      vnode_unlock(v);
      return error;
    }
  }

  vnode_unlock(v);

  return 0;
}

static ext2_read_vop(vnode_t *v, uio_t *uio) {
  ext2_vndata_t *vndata = v->v_data;
  ext2_state_t *state = vndata->mount->mnt_data;

  /* TODO: eeeeeetm... read stuff? */
  size_t offset = vndata->inode * state->;

  vnode_t *src = state->src;
  vnode_lock(src);
  VOP_READ(src, uio);
  vnode_unlock(src);
}

static vnodeops_t ext2_vops = {
  .v_lookup = ext2_lookup_vop,
  .v_readdir = ext2_readdir_vop,
  .v_open = vnode_open_generic,

};

static int ext2_root(mount_t *mp, vnode_t **vp) {
  panic("`ext2_root` is unimplemented!");
}

static int ext2_init(vfsconf_t *vfc) {
  panic("`ext2_init` is unimplemented!");
}

static vfsops_t ext2_vfsops = {
  .vfs_mount = ext2_mount,
  .vfs_root = ext2_root,
  .vfs_init = ext2_init,
};

static vfsconf_t ext2_conf = {
  .vfc_name = "ext2",
  .vfc_vfsops = &ext2_vfsops,
};
