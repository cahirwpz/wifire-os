import gdb

from .cpu import Cpu
from .kdump import Kdump
from .kgmon import Kgmon
from .klog import Klog
from .ktrace import Ktrace
from .proc import Kprocess, Process, ShowProcess
from .struct import BinTime
from .sync import CondVar, Mutex
from .thread import Kthread, Thread, CurrentThread
from .events import stop_handler


def addPrettyPrinters():
    pp = gdb.printing.RegexpCollectionPrettyPrinter('mimiker')
    pp.add_printer('mtx', 'mtx', Mutex)
    pp.add_printer('condvar', 'condvar', CondVar)
    pp.add_printer('thread', 'thread', Thread)
    pp.add_printer('process', 'process', Process)
    pp.add_printer('bintime', 'bintime', BinTime)
    gdb.printing.register_pretty_printer(gdb.current_objfile(), pp)


addPrettyPrinters()

# Commands
Cpu()
Kdump()
Klog()
Kprocess()
Kthread()
Ktrace()
Kgmon()

# Functions
CurrentThread()
ShowProcess()

# Events
gdb.events.stop.connect(stop_handler)
