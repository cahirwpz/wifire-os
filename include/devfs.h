#ifndef _SYS_DEVFS_H_
#define _SYS_DEVFS_H_

#include <vnode.h>

#define DEVFS_NAME_MAX 64

typedef struct devfs_node devfs_node_t;

/* If parent is NULL new device will be attached to root devfs directory. */
int devfs_makedev(devfs_node_t *parent, const char *name, vnodeops_t *vops,
                  void *data);
int devfs_makedir(devfs_node_t *parent, const char *name, devfs_node_t **dir_p);

#endif /* !_SYS_DEVFS_H_ */
