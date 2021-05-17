#include <sys/mimiker.h>
#include <sys/cpu.h>
#include <aarch64/armreg.h>

__no_instrument_kgprof void cpu_intr_disable(void) {
  __asm __volatile("msr daifset, %0" ::"i"(DAIF_I));
}

__no_instrument_kgprof void cpu_intr_enable(void) {
  __asm __volatile("msr daifclr, %0" ::"i"(DAIF_I));
}

__no_instrument_kgprof bool cpu_intr_disabled(void) {
  uint32_t daif = READ_SPECIALREG(daif);
  return (daif & PSR_I) != 0;
}
