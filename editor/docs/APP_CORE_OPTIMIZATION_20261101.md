# App / 核心联动优化

核心详细记录：`../vtouch-project/docs/RUNTIME_OPTIMIZATION_20261101.md`。

## App

- 找色轻量帧在 Matcher / QuickTest / DryRun / VisualPage 接线（此前已完成）。
- `PublishTransaction` + `Su.publish`：多文件暂存、备份、逐件 rename、失败回滚；保留中断恢复日志。
- `OpsV8.publish`：验证源码哈希、字节码和元数据；同时提交 live 及当前方案副本，锁内校验 current。
- `Schemes.publishConfig`：区域和步骤表同样成对提交；不再静默忽略镜像失败。
- 状态页“恢复未完成发布”：需用户确认，且核心已停止。只恢复日志中的目标文件。
- `RuntimeStatus`：校验确认所属核心 PID、ops 哈希和方案；旧核心、未确认、坏记录不显示成功。
- `OpsV8.mirrorProgs` 不再以 `; true` 吞同步错误，不先删除正在使用的目录再拷贝。

## 检查

```sh
PATH=/data/data/com.termux/files/usr/lib/jvm/java-17-openjdk/bin:$PATH bash build.sh
bash scripts/check.sh
```

六项：MatcherColorCheck / PublishCheck / OpsV8Check / VtToStepsCheck / VtFromStepsCheck / VtMigrateCheck。
发布检查覆盖最后一步失败回滚、成功提交、中断恢复、方案切换拒绝、加载状态的旧 PID/旧哈希拒绝。
外部 out-mig / out-mig-zt 缺失时对应两项真实产物扫描仍跳过。

## 兼容和限制

新版 App 与新版核心/面板配套使用。旧面板不认识 `.publish-lock` 与 `.publish-pending`，只有单文件 rename 保证，
无法保证多文件发布期间不重载。App 暂未限制旧核心编辑，旧核心只显示“加载未确认”。
App 发布不是掉电原子事务；SIGKILL 留锁是有意的，不能不检查就自动删锁。
当前确认仅是操作表加载，不代表区域、素材及真实运行结果全部正确。
原生核心、App 均未自动安装或启动，实际设备链路仍需用户确认后验证。

核心产物：`../vtouch-project/build/optimized/{vtouchd_ui,vtouchd,vtouch.sh}`，
自包含启动包经过内嵌字节对账；`SHA256SUMS` 在同目录。
详细构建/测试命令、边界和设备验收项见核心优化记录。
