#!/system/bin/sh
# fake_touch.sh —— 用 sendevent 伪造「物理手指」（不用真人手指就能测合并链路）。
#
# 为什么有效：EVIOCGRAB 只挡读者、不挡写者 —— 往 daemon 抓着的那个物理节点写一套 Type-B
# 事件，daemon 照样读得到，于是「物理触点 → 合并 → uinput」整条链可被脚本驱动。
#
# 用法（设备上，root）：
#   D=/dev/input/event8  sh /sdcard/fake_touch.sh down 0  720 1584
#   D=/dev/input/event8  sh /sdcard/fake_touch.sh move 0  740 1500
#   D=/dev/input/event8  sh /sdcard/fake_touch.sh up   0
#
# 两个参数怎么来：
#   D      = daemon 启动日志里的 dev=（例：vtouchd: dev=/dev/input/event8 …）
#   SCALE  = 内核 raw 轴 ÷ 逻辑尺寸；看 daemon 的 `res` 应答：raw xmax ÷ 宽（本机实测 16）
#
# 注意：伪造手指与真人共用内核槽位，同时按屏会互相改写坐标 —— 挑用户不碰屏的时候跑。
D="${D:-/dev/input/event8}"
SCALE="${SCALE:-16}"

case "$1" in
down)
  s=$2; x=$3; y=$4
  sendevent $D 3 47 $s                      # ABS_MT_SLOT
  sendevent $D 3 57 $((100+s))              # ABS_MT_TRACKING_ID（新 id = 按下）
  sendevent $D 3 53 $((x*SCALE))            # ABS_MT_POSITION_X
  sendevent $D 3 54 $((y*SCALE))            # ABS_MT_POSITION_Y
  sendevent $D 3 48 100                     # ABS_MT_TOUCH_MAJOR
  sendevent $D 0 0 0                        # SYN_REPORT
  ;;
move)
  s=$2; x=$3; y=$4
  sendevent $D 3 47 $s
  sendevent $D 3 53 $((x*SCALE))
  sendevent $D 3 54 $((y*SCALE))
  sendevent $D 0 0 0
  ;;
up)
  s=$2
  sendevent $D 3 47 $s
  sendevent $D 3 57 -1                      # TRACKING_ID=-1 = 抬起
  sendevent $D 0 0 0
  ;;
*)
  echo "用法: D=/dev/input/eventN SCALE=16 sh $0 {down|move} <slot> <x> <y> | $0 up <slot>"
  exit 1
  ;;
esac
