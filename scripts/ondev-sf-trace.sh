#!/system/bin/sh
# ondev-sf-trace.sh —— 转屏现场取证：录屏（合成后的真实画面）+ 同步 dump 图层状态。
#
# 为什么两条都取：
#   · screenrecord 录的是**合成后的输出**（用户看到的东西），逐帧看能直接定位"面板出现在别处"是哪一帧；
#   · dumpsys 取的是**合成器侧的图层状态**（尺寸/位置/alpha/transform），能解释那一帧为什么那样。
# 用法（设备侧 su）：sh ondev-sf-trace.sh [秒数]
SEC=${1:-20}
OUT=/data/local/tmp/rot
rm -f $OUT.mp4 $OUT.log
# 后台录屏（--time-limit 上限 180s；这里按参数）
screenrecord --bit-rate 8000000 --time-limit $SEC $OUT.mp4 &
REC=$!
# 同步轮询图层状态（dumpsys 本身重，实际约 10 次/秒）
i=0
while [ $i -lt $((SEC * 8)) ]; do
    T=$(date +%s.%N)
    echo "== $i @ $T" >> $OUT.log
    dumpsys SurfaceFlinger 2>/dev/null | grep -E 'vtouch-ui' >> $OUT.log
    dumpsys SurfaceFlinger --list 2>/dev/null | grep -c 'vtouch-ui' >> $OUT.log
    i=$((i + 1))
done
wait $REC
echo "录屏: $(ls -l $OUT.mp4 2>/dev/null)"
echo "状态: $(wc -l < $OUT.log) 行"
