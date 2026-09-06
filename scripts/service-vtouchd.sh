#!/system/bin/sh
# KernelSU service.d: start vtouchd and loopback WebSocket bridge
BASE=/data/local/tmp
D=$BASE/vtouchd
C=$BASE/vtouchctl
W=$BASE/vtouchws
sleep 5
if [ -x "$D" ] && [ -x "$C" ]; then
  "$C" reset >/dev/null 2>&1 || true
  killall vtouchws >/dev/null 2>&1 || true
  killall vtouchd >/dev/null 2>&1 || true
  rm -f "$BASE/vtouch.sock"
  nohup "$D" -x 1440 -y 3168 >"$BASE/vtouchd.log" 2>&1 </dev/null &
  sleep 2
  if [ -x "$W" ]; then nohup "$W" >"$BASE/vtouchws.log" 2>&1 </dev/null & fi
fi
