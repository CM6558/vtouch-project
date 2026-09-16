#!/system/bin/sh
# fdcheck.sh —— 探针的 fd 卫生检查：子进程（UI 侧）不得继承父进程的文件描述符。
# 这是"UI 继承带 EVIOCGRAB 的 fd → 核心退出后触摸回不来"的防线，必须为 0 命中。
D=/data/local/tmp/vtouch-probe
cd $D || exit 1
rm -f f.log
nohup ./probe_native $D --pump 300 > f.log 2>&1 &
sleep 3

CP=$(sed -n 's/.*forked child pid=\([0-9]*\).*/\1/p' f.log | head -1)
PP=
for p in /proc/[0-9]*; do
    if [ "$(cat $p/comm 2>/dev/null)" = "probe_native" ]; then PP="${p#/proc/}"; break; fi
done

echo "== 日志头 =="
head -6 f.log
echo "P=$PP C=$CP"
echo "== 子进程 /proc/$CP/fd =="
ls -l /proc/$CP/fd 2>/dev/null
echo "== 父进程 /proc/$PP/fd =="
ls -l /proc/$PP/fd 2>/dev/null
CH=$(ls -l /proc/$CP/fd 2>/dev/null | grep -cE 'event[0-9]+|uinput|/dev/input')
PA=$(ls -l /proc/$PP/fd 2>/dev/null | grep -cE 'event[0-9]+|uinput|/dev/input')
echo "== 判定 =="
echo "子进程命中 event/uinput 数量 = $CH  (期望 0)"
echo "父进程命中 event/uinput 数量 = $PA  (期望 1 —— 父确实持有触摸屏 fd，只读打开，未 grab)"
kill -9 $CP $PP 2>/dev/null
echo done
