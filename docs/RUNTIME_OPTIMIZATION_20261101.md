# 运行可靠性优化（2026-11-01）

本批只构建与离机测试，未部署、未重启设备核心、未注入真实触摸。改动前源码完整备份位于
`/data/data/com.termux/files/usr/tmp/opencode/vtouch-before-opt-20261101-151221.tar.gz`。
仓库此前已有大量未提交修改，本批基于当前文件继续，未回退历史内容。

## 已实现

- **VM 公平调度**：`vt_vm_tick` 最多执行 2000 条指令后返回 `VT_VM_YIELD`；主循环 1ms 后续跑。
  连续不经过 WAIT、跨拍动作或异步视觉的计算超过 2,000,000 条，按 `连续计算超限` 中止。
  两个阈值均可通过编译宏覆盖；200 次 CJMP 兼容守卫仍保留。不对合法长期带等待脚本设置总时长硬上限。
- **真实时钟**：核心绑定单调钟给 VM NOW；纯 VM 单测保留假时钟。
- **异步视觉**：脚本 VM 和面板试查的请求帧/等待帧/重试搜索跨 tick 推进，不在抓帧等待期间阻塞主循环。
  同一抓帧请求跨拍复用，视觉完成才入栈结果；中止清任务；试查与运行串行，避免抢帧和延长按住动作。
  保留旧步骤执行器同步兼容路径；匹配本体仍是一次同步运算，并非完整可抢占算法。
- **找色轻量帧**：单点/多点找色不再生成整帧灰度和两级金字塔；找图仍按原 NEON/标量语义。
- **数值防御**：表下标拒绝 NaN；等待拒绝非有限/超大时长；滑动插值和采样时刻计算使用 64 位中间值，防溢出。
- **并发一致性**：满事件队列原地合并与消费者复制用短锁互斥，保留合并/丢新策略；触发名字和参数一起快照，日志在锁外。
- **非阻塞 WS 握手**：HTTP 请求跨 poll 增量解析，总期限 1s；未经握手成功不踢旧客户端。
  `VTOUCH_WS_TOKEN` 非空时要求 `Authorization: Bearer <token>`；默认空保留现有客户端兼容。
- **诊断**：增加 `script_run`（run ID/源哈希）、`script_done`、`script_error`（源码行、PC、原因），保留原日志族。
- **构建**：`pack_su.py` 使用线性 Base64 分行，替代对数 MB 单词反复切片的 textwrap；输出布局仍为 76 列。
  `scripts/build_termux.py` 在设备上重建当前 Java/C/C++、面板三件套与内嵌核心，输出 `build/optimized/`，不部署。
  不混入旧 class/object，并移除 Termux 私有 RUNPATH，避免 root/app_process 负载带入应用私有库搜索路径。
- **热载**：失败限频重试（2s 起，最多 32s）；修复强制重载反写旧文件、current 未初始化、重放失败后刷新基线不重试。
- **发布协作**：App 和面板用 `.publish-lock` 原子 mkdir 互斥；`.publish-pending` 阻止旧表落盘，加载成功才清除。
  面板以 `reload-status.txt` 报告当前核心 PID、实际读入 ops 哈希、当前方案、loaded/partial/failed 与坏记录数。
  程序 PUT 后回读字节、长度和 FNV；已有同名定义不一致不冒充已加载。

## App 配套

编辑器新增发布计划、失败回滚、中断恢复入口；`.vts/.vtbc/ops.conf` 和当前方案副本同一计划提交。
索引、源码、字节码一致后才写；锁内再次核对 current，方案已切换拒绝提交。
区域和步骤配置保存也用同样的 live/方案成对提交。发布中发生强杀会保留日志与锁，**不会自动删除**。
须停止核心、确认无其他保存任务后，使用 App 状态页“恢复未完成发布”。普通失败自动回滚。

这是一套**协作锁保护的多文件发布 + 失败恢复**，不是文件系统级多文件原子事务；掉电持久化、SIGKILL 恢复仍需设备演练。
旧核心不识别发布锁和待确认标记，不能获得本批一致性保证；建议成对升级。旧核心状态明确显示未确认。
加载确认仅覆盖 ops 表及读取/投递校验，不代表区域几何、素材文件和真实触摸均已验证。

## 验证

`sh scripts/test_runtime.sh`：无 root/输入设备访问，包含：

1. VM 时间片/计算上限/行号、真实时钟钩子、找图和找色 pending 操作数恢复、非法时长。
2. NEON 默认与强制标量找色帧一致。
3. 百万事件生产消费无混读，分段 WS 握手、鉴权与超时。
4. 管道模拟物理输入，慢 WS 握手不会挡住 poll。
5. 真实 `vt_ops.c` 视觉状态机以假帧头验证请求续跑、命中、持续超时、抓帧超时、取消。

原 `build/test_vt_vm.c` + `build/vmfix/` 全通过（夹具不在仓库）；函数文档 `apply_funcdoc.py --check` 通过。
CI 已接 `test_runtime.sh`。App Java17/D8/签名构建与六项离机检查通过。

## 尚待设备验证 / 后续范围

- 亮屏真实触摸：停止响应 p95/p99、抓帧失败、旋转、暂停/恢复、忙触发、uinput 写失败。
- 双端发布/热载联动、面板死亡重启、App 强杀恢复、磁盘满和权限错误。
- 队列短锁尾延迟、连续视觉查找 CPU/能耗、帧缓冲生命周期、真实截图 NEON/标量全量对拍。
- 断点/单步/变量监视/完整运行时间线、源码跳转、全脚本模拟器、模块化语言扩展未在本批实现。
- 尚未做：匹配算法分块可取消、素材版本快照、核心整表原子切换、去除对面板抓帧的依赖。
- WS 认证为兼容可选，不设置 token 时仍是本地端口信任模型；令牌须由用户安全配置且不能硬编码进发布包。
- 默认无面板核心也改变了队列与握手逻辑，属于有意变更；没有修改共享内存结构布局，契约仍为 v17。

不要以“编译通过”替代上述运行验收；无设备回归前，不应直接覆盖正在使用的核心。

## 构建产物和复现

- App：`../vtouch-editor/out/vtouch-editor.apk`（沿用现有签名密钥）。
- 内嵌核心与启动包：`build/optimized/vtouchd_ui`、`build/optimized/vtouch.sh`。
- 默认核心与面板：`build/optimized/vtouchd`、`build/optimized/libtestimgui.so`。
- 哈希：`build/optimized/SHA256SUMS`；CI 一致性门验证 real 面板及 su 内嵌核心相同（7 通过 / 0 失败）。
- 原 `build/vtouchd_ui`、`build/vtouch.sh` 未覆盖。

本机重建：

```sh
GL_HEADERS=/data/data/com.termux/files/usr/tmp/opencode/glvnd-extract/data/data/com.termux/files/usr/include \
  python3 scripts/build_termux.py
sh scripts/test_runtime.sh
python3 scripts/ci_check.py --core build/optimized/vtouchd_ui --core-default build/optimized/vtouchd \
  --panel build/optimized/libtestimgui.so --su build/optimized/vtouch.sh
```

EGL/GLES 头文件来自下载后仅解压到临时目录的 Termux `libglvnd-dev` 包，未安装系统包；
新面板链接系统 EGL/GLES/Android/log 和随包 libc++。仍需在目标设备验证加载及 API/ROM 兼容。
