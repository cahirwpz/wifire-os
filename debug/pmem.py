import gdb
from ptable import ptable, as_hex


PAGESIZE = 0x1000


def is_global(lo):
    return bool(lo & 1)


def is_valid(lo):
    return bool(lo & 2)


def is_dirty(lo):
    return bool(lo & 4)


def vpn_of(hi):
    return hi & 0xffffe000


def ppn_of(lo):
    return (lo & 0x03ffffc0) << 6


def asid_of(hi):
    return hi & 0x000000ff


class TLB():

    def invoke(self):
        self.dump_tlb()

    def get_tlb_size(self):
        return gdb.parse_and_eval('tlb_size()')

    def get_tlb_entry(self, idx):
        gdb.parse_and_eval('_gdb_tlb_read_index(%d)' % idx)
        return gdb.parse_and_eval('_gdb_tlb_entry')

    def dump_entrylo(self, vpn, lo):
        if not is_valid(lo):
            return "-"
        vpn = as_hex(vpn)
        ppn = as_hex(ppn_of(lo))
        dirty = '-D'[is_dirty(lo)]
        globl = '-G'[is_global(lo)]
        return "%s => %s %c%c" % (vpn, ppn, dirty, globl)

    def dump_tlb_index(self, idx, hi, lo0, lo1):
        if not is_valid(lo0) and not is_valid(lo1):
            return []
        return ["%02d" % idx, "$%02x" % asid_of(hi),
                self.dump_entrylo(vpn_of(hi), lo0),
                self.dump_entrylo(vpn_of(hi) + PAGESIZE, lo1)]

    def dump_tlb(self):
        tlb_size = self.get_tlb_size()
        rows = [["Index", "ASID", "PFN0", "PFN1"]]
        for idx in range(tlb_size):
            entry = self.get_tlb_entry(idx)
            hi, lo0, lo1 = entry['hi'], entry['lo0'], entry['lo1']
            row = self.dump_tlb_index(idx, hi, lo0, lo1)
            if not row:
                continue
            rows.append(row)
        ptable(rows, fmt="rrll", header=True)
