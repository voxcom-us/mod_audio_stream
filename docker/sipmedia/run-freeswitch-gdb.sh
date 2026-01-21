#!/bin/bash
# Run FreeSWITCH under gdb to catch crashes

# Enable core dumps
ulimit -c unlimited

# Run FreeSWITCH under gdb
gdb -batch -ex "run -nonat" -ex "thread apply all bt" -ex "quit" /usr/bin/freeswitch 2>&1 | tee /tmp/freeswitch-crash.log

# If we get here, FreeSWITCH exited/crashed
echo "FreeSWITCH exited. Check /tmp/freeswitch-crash.log for backtrace"
cat /tmp/freeswitch-crash.log
