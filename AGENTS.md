# Agent Guide

本文件说明 `esp32-nfc-tool` 的开发环境和开发规则。硬件、引脚和使用方法见 [README.md](README.md)。

## 开发环境

- ESP-IDF v5.5.6，安装在 `~/.esp/esp-idf-v5.5.6`。项目根目录的 `.envrc`（不入库）由 direnv 自动加载 IDF 环境；没有 direnv 时执行 `. ~/.esp/esp-idf-v5.5.6/export.sh`。
- 目标芯片 esp32c3，已写在 `sdkconfig.defaults` 的 `CONFIG_IDF_TARGET`，不需要 `idf.py set-target`。
- CI（`.github/workflows/firmware-build.yml`）固定同一 IDF 版本。升级 IDF 时本地 `.envrc`、CI 和本文件同步修改。

## 常用命令

```bash
idf.py build          # 编译
idf.py flash          # 烧录，只插一块板子时自动识别串口，多块时用 -p 或 ESPPORT 指定
esp-log 15            # 复位板子，抓 15 秒串口日志并解析崩溃栈，然后退出
```

`idf.py monitor` 和 `idf.py menuconfig` 是交互式命令，不要在 agent 会话中运行，分别用 `esp-log` 和直接编辑配置文件代替。

## 开发规则

- 改动完成的标准是 `idf.py build` 通过且没有新增警告。
- 涉及运行时行为的改动，板子已连接时烧录并用 `esp-log` 确认日志；板子未连接时在结果中说明未上板验证。
- 默认配置改在 `sdkconfig.defaults`。`sdkconfig` 不入库，已存在时不会重新读取 defaults，修改 defaults 后删除 `sdkconfig` 再编译。
- 网页文件（`main/html/`）通过 `EMBED_FILES` 编译进固件，修改后需重新编译烧录。
- 组件依赖写在 `main/idf_component.yml`，`dependencies.lock` 随改动一起提交。
- 引脚、Wi-Fi、menuconfig 选项等变化时同步更新 README。

## 提交规则

- 未经用户明确要求或明确同意，不得执行 `git commit` 或 `git push`。
- 默认在 `master` 分支开发，提交前确认 `git status --short` 只包含本次任务相关文件。
- 提交信息使用中文，一句话概括改动，不加 type 前缀，与现有 git log 风格一致。
- 提交信息不包含 AI 署名、生成标识、协作者 trailer 或工具标记。
