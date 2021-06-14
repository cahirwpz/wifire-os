#ifndef _SYS_KGPROF_H_
#define _SYS_KGPROF_H_

typedef struct timer timer_t;

#if KGPROF
timer_t *get_stat_timer(void);
void init_kgprof(void);
void kgprof_tick(void);
void set_kgprof_profrate(int profrate);
#else
#define get_stat_timer() NULL
#define init_kgprof() __nothing
#define kgprof_tick() __nothing
#define set_kgprof_profrate(x) __nothing
#endif

#endif /* !_SYS_KGPROF_H_ */
