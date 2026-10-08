#!/bin/bash
# Keep the smoke's TAP logs for t/033 and t/028 (the smoke itself keeps none),
# and print each file's own verdict lines.
mkdir -p /tmp/out/smoke-taplog
cp $HOME/pg_weave/tmp_check/log/regress_log_033* $HOME/pg_weave/tmp_check/log/regress_log_028* /tmp/out/smoke-taplog/ 2>/dev/null
ls /tmp/out/smoke-taplog
grep -hE "(not )?ok [0-9]+ - (at every cycle|and the crashes|quiet plain VACUUM)|excess of c_w" /tmp/out/smoke-taplog/* | sed 's/^\[[^]]*\] *//'
[ -n "$(ls /tmp/out/smoke-taplog)" ]
