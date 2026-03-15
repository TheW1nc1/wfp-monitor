# WFP 流量监控组件

本项目实现了一个基于 Windows Filtering Platform（WFP）的流量监控组件，包含：
- **内核态 WFP Callout 驱动** (`WfpMonitor.sys`)：在 `FWPM_LAYER_ALE_AUTH_CONNECT` 层捕获 TCP 连接，通过 `FwpsFlowAssociateContext0` 将流上下文关联到 `FWPM_LAYER_STREAM` 层，使用 `FWPS_STREAM_FLAG_SEND/RECEIVE` 分别统计出口/入口字节数。
- **用户态控制程序** (`WfpController.exe`)：自动安装驱动、以挂起模式启动 `test.exe` 获取 PID、通过 IOCTL 传递 PID 给驱动、等待进程结束后获取统计结果、输出 `result.txt`。

## 1. 依赖信息与环境要求

| 依赖 | 版本 |
|------|------|
| 操作系统 | Windows 10 / 11 (x64) |
| WDK | 10.0.22621.0 |
| 编译器 | Visual Studio 2022 (MSVC) |
| 构建系统 | CMake 3.20+ |

## 2. 构建步骤

### 2.1 本地构建
1. 确保安装 Visual Studio 2022 和 WDK 10.0.22621.0
2. 打开 **Developer Command Prompt for VS 2022**
3. 在项目根目录运行：
   ```cmd
   cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_SYSTEM_VERSION=10.0.22621.0
   cmake --build build --config Release
   ```
4. 产物位于 `build/Release/`：
   - `WfpMonitor.sys`（内核驱动）
   - `WfpController.exe`（用户态控制程序）

### 2.2 CI 自动构建
本项目配置了 GitHub Actions CI，推送到 `master` 分支后自动构建。编译产物可从 Actions → Artifacts 下载。

## 3. 运行步骤

### 3.1 准备测试环境
⚠️ 由于驱动未经 WHQL 签名，需在测试虚拟机中开启测试签名模式：
```cmd
bcdedit /set testsigning on
```
**必须重启**。重启后右下角出现"测试模式"水印即可。

### 3.2 执行测试
1. 将 `WfpMonitor.sys`、`WfpController.exe`、`test.exe` 放在同一目录
2. **以管理员身份**打开 CMD
3. 运行：
   ```cmd
   .\WfpController.exe .\test.exe
   ```

### 3.3 工作流程
1. `WfpController.exe` 通过 SCM 安装并启动 `WfpMonitor` 驱动服务
2. 以 `CREATE_SUSPENDED` 模式启动 `test.exe`，获取其 PID
3. 通过 `IOCTL_WFP_MONITOR_SET_PID` 将 PID 发送给驱动
4. 驱动在 ALE 层匹配该 PID 的 TCP 连接，关联流上下文到 Stream 层
5. 唤醒 `test.exe`，驱动在 Stream 层按 `FWPS_STREAM_FLAG_SEND/RECEIVE` 统计字节
6. `test.exe` 结束后，通过 `IOCTL_WFP_MONITOR_GET_STATS` 获取统计结果
7. 写入 `result.txt` 并自动卸载驱动

### 3.4 result.txt 生成位置
执行完毕后，在**当前目录**生成 `result.txt`，格式：
```
DST=<ip:port>
TX_BYTES=<number>
RX_BYTES=<number>
```
