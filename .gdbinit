set tcp connect-timeout 30
target remote localhost:1234
break main
break assert_fail
break kernel_init
continue

source debug/ktrace.py
source debug/kdump.py
source debug/mutex.py
