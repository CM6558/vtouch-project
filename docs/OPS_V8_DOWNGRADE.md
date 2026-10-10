# OPS_V8 降级说明 —— 从 V8（脚本态）回退到旧版

> 适用：设备已迁移到 V8「操作语言化」态（`#vtouch-ops v8` + `progs/`），需要**回退到旧版**或**把数据还原成 v5 原样**的场景。
> 一句话：**V8 的数据文件，旧端（不认识 v8 的版本）会整份丢弃 → 降级 = 用备份把操作表还原成 v5，再换回旧软件。**
> 备份与核验：`~/vtouch-shots/v8t5/`（`t53-verify.txt` 全量核验 / `t53-dryrun.txt` 副本演练 / `t53-verify.py` 可复跑）。

## 一屏说明

- **能回退吗**：能。迁移前的操作表原样（v5）以 `ops.conf.bak-v8` 形式在设备上逐字节保留着（6 处 = live + 全部方案；已全量核验）。
- **降级动作**：① 停核 → ② 还原操作表（1 条命令块）→ ③ 校验 md5 → ④ 起旧软件。
- **只动操作表**：区域 / 方案名 / 模板点集 / 触发绑定 / 当前方案 全部不涉及（迁移就没碰它们；regions 的 `.bak-v8` 与现态本就一致）。
- **不会丢东西**：v8 侧迁移产物（脚本 / 字节码）本地有完整快照（`~/vtouch-shots/v8t5/out-mig/`），想再升回去随时可用（见文末）。
- **顺序**：**先还原数据、后换软件** —— 反过来旧端启动会把 v8 的 ops.conf 丢弃（即使发生了，`.bak-v8` 不受影响，本说明照样能还原）。

## 还原步骤（照抄；设备 root）

### ① 停核

```sh
su -c 'sh /sdcard/vtouch.sh stop'
```

（核心本来没跑就跳过。）

### ② 还原操作表（live + 全部方案）：ops.conf 回 v5 + 清 progs/

```sh
su -c '
cd /data/local/vtouch-runtime
cp -f ops.conf.bak-v8 ops.conf
rm -rf progs
for d in schemes/*/; do
  [ -f "$d/ops.conf.bak-v8" ] && cp -f "$d/ops.conf.bak-v8" "$d/ops.conf"
  rm -rf "$d/progs"
done
'
```

说明：`[ -f … ]` = 护栏，跳过没有备份的方案（迁移后新建、没有 v5 原件的方案；它的 v8 文件旧端会丢弃，需单独决策）。当前 5 个方案全部有备份、都会还原。

### ③ 校验（期望值见下表）

```sh
su -c 'md5sum /data/local/vtouch-runtime/ops.conf /data/local/vtouch-runtime/schemes/*/ops.conf'
su -c 'ls /data/local/vtouch-runtime/progs /data/local/vtouch-runtime/schemes/*/progs 2>&1'
```

第二条应全部报 `No such file or directory`（progs 已清）。

| 位置 | 还原后 ops.conf（期望） |
|---|---|
| live、吕布 | `666a45f9…` |
| 干将莫邪、默认、默认2 | `dd2a0fee…` |
| 橘右京 | `4dda0d3f…` |

（regions 无需还原：迁移从未改动，`.bak-v8` 与现态逐字节一致。）

### ④ 起核并看回执

```sh
su -c 'sh /sdcard/vtouch.sh start'
su -c 'tail -n 10 /data/local/tmp/vt_ui_core.log'
```

期望日志：`op 编辑 put op1 步数=13`、`put 保命装 步数=68` 等上表行，无「被拒」。（若暂不换软件，直接用现版核起也可以——现版仍读 v5，等 T6 后才评估退役。）

## 边界与注意

- **软件回退（可选，紧跟数据还原之后）**：把核 / 面板 / 编辑器换回目标旧版构建，走各自既有部署路径。
  ⚠ `/sdcard/vtouch.sh start` 有 md5 兜底（发现设备核与内嵌载荷不符会用载荷反向覆盖）——**软件回退若含换核，必须连 `vtouch.sh` 一起回退**（用旧版重建 pack_su 产物），或按 AGENTS.md「设备侧等价一行」手动起核：
  `su -c 'cd /data/local/tmp && nohup ./vtouchd_ui >/data/local/tmp/vt_ui_core.log 2>&1 </dev/null &'`
  已知的设备核备份（供参考）：`/data/local/tmp/vtouchd_ui.bak-20261008` · `bak-2211`（`0d8506dc`，2026-10-08）、`bak-ebf81f93`（T3.6 期）。
- **降级后继续用现版编辑器**：可以（v5 读路径保留）。存盘按文件现有版本回写（`Ops.java:414-420`：v5 保持 `#vtouch-ops v5`、无 prog 元数据不写）——不会自动升回 v8。
- **再升回 V8**：核 / 编辑器换回 v8 版（核副本 `~/vtouch-shots/v8t5/vtouchd_ui-3a6e723c`，载荷更新走 pack_su 标准链）；数据侧用快照 `~/vtouch-shots/v8t5/out-mig/<方案>/` 经 `t52.sh push` 重新推送，或重跑迁移工具（`VtMigrateRun`）。快照 = 迁移当时状态；若其后有编辑，重跑迁移基于最新 v5 数据即可。

## 取证（本轮）

- **全量核验（只读）**：`~/vtouch-shots/v8t5/t53-verify.txt`（`t53-verify.py` 可复跑）——12 件 `.bak-v8` 全对、语料同源链全对、核三件套同一 md5 `3a6e723c`。
- **副本演练**：`~/vtouch-shots/v8t5/t53-dryrun.txt` —— 上述 ② 命令块在 `/data/local/tmp` 副本上逐字跑过：还原后 md5 全对、progs 清扫、备份保留。
- **迁移报告**：`docs/OPS_V8_T53_MIGRATION_REPORT.md`。
