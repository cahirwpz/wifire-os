from .virtmem import VmAddress, VmPhysSeg, VmFreePages, VmMapSeg, PhysMap
from .virtmem import VmAmap
from .memory import Vmem, Malloc, MallocStats, PoolStats
from .cmd import CommandDispatcher


class Kdump(CommandDispatcher):
    """Examine kernel data structures."""

    def __init__(self):
        super().__init__('kdump', [VmAddress(), VmAmap(), VmPhysSeg(),
                                   VmFreePages(), VmMapSeg(), PhysMap(),
                                   Vmem(), Malloc(), MallocStats(),
                                   PoolStats()])
