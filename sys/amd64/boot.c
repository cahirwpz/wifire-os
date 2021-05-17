#include <sys/mimiker.h>

/* TODO(MichalBlk): remove those after architecture split of gdb debug scripts.
 */
typedef struct {
} tlbentry_t;

static __unused __boot_data volatile tlbentry_t _gdb_tlb_entry;
