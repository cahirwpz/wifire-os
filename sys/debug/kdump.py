from .virtmem import VmAddress, VmPhysSeg, VmFreePages, VmMapSeg, PhysMap
from .memory import Vmem, Malloc, MallocStats, PoolStats
from .cmd import CommandDispatcher


class Kdump(CommandDispatcher):
    """Examine kernel data structures."""

    def __init__(self):
        super().__init__('kdump', [VmAddress(), VmPhysSeg(), VmFreePages(),
                                   VmMapSeg(), PhysMap(), Vmem(), Malloc(),
                                   MallocStats(), PoolStats()])
