# vtouch 编辑台 (vtouch-editor)

这是 vtouch 的 Android 端可视化与脚本编辑台应用，源码位于 `editor/app/src/`，构建脚本为 `editor/build.sh`。

## 构建说明

- 需要 Java 17、Termux Python3 以及 `sdk/` 目录下的构建 jar。
- 编译与打包脚本：
  ```bash
  cd editor
  PATH=/data/data/com.termux/files/usr/lib/jvm/java-17-openjdk/bin:$PATH bash build.sh
  ```
- 运行离机自检套件：
  ```bash
  cd editor
  bash scripts/check.sh
  ```
