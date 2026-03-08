# WFP 流量监控组件

本项目是一个基于 Windows Filtering Platform (WFP) 的流量监控组件，包含内核态的 WFP Callout 驱动 (`WfpMonitor.sys`) 和 用户态的控制程序 (`WfpController.exe`)。它用于统计指定目标进程（如 `test.exe`）在运行期间产生的 TCP 总出口流量（TX_BYTES）和总入口流量（RX_BYTES）。

**当前工程已重构为基于 CMake 的配置，专为 JetBrains CLion 环境优化。**

## 1. 依赖信息

- **操作系统要求**：Windows 10 x64 或更高版本。
- **开发工具**：
  - JetBrains **CLion** (支持 CMake 构建)。
  - 已安装 Visual Studio 的 C++ / MSVC Build Tools。
  - **Windows Driver Kit (WDK)** 10（需与 SDK 版本匹配）。
- **测试环境**：
  - 测试用的 Windows 虚拟机必须开启 **测试签名模式 (Testsigning)**。
  - 需要以**管理员权限**运行控制程序。

## 2. 架构说明

### 内核态驱动 (`WfpMonitor.sys`)
- 注册了两个 WFP Callout：
  1. `FWPM_LAYER_ALE_FLOW_ESTABLISHED_V4`：拦截新建的连接，匹配目标进程的 PID。匹配后记录 DST。
  2. `FWPM_LAYER_STREAM_V4`：拦截数据流计算流量。利用 `FWP_DIRECTION_OUTBOUND` 及方向获取流量字节。

### 用户态程序 (`WfpController.exe`)
- 启动 `test.exe`，动态注册并下达 PID，最后获取统计信息（`IOCTL_WFP_MONITOR_GET_STATS`），写出到 `result.txt`。

## 3. 构建步骤（CLion / CMake）

### 在 CLion 中构建
1. 在 CLion 中通过 `File -> Open` 选择 `CMakeLists.txt` 所在的 `WfpProject` 根目录。
2. 确保配置里的 Toolchain 使用了本地的 **MSVC** x64 AMD64 工具链。
3. 等待 CMake 刷新，此时 WfpController 已经具备编译条件。项目结构可以正常进行代码阅读和补全（IntelliSense 支持）。
4. **针对 Driver (WfpMonitor) 的构建**：
   - 默认的 CMake 能满足常规 C++ 的解析以及 WfpController 控制器的编译。
   - 若要实现针对 `.sys` 的原生完整编译，通常可以使用业界通用的第三方 WDK CMake 插件（如 `FindWDK`），或者在装有 WDK 的系统上使用 EWDK MSBuild 命令进行生成。

## 4. 运行步骤（测试指南）

### 4.1 虚拟机准备
在用于测试的 Windows 虚拟机中，请确保已打开测试模式（驱动未签名）：
1. 以管理员身份运行 CMD。
2. 执行以下命令：
   ```cmd
   bcdedit /set testsigning on
   ```
3. 重启虚拟机。

### 4.2 执行测试
1. 将编译好的 `WfpMonitor.sys` 和 `WfpController.exe` 放置在 **同一个目录下**。
2. 将你需要测试的 `test.exe` 也复制到当前环境。
3. **以管理员权限** 运行命令提示符 (CMD) 或 PowerShell。
4. 切换到程序所在目录，执行如下命令：
   ```cmd
   WfpController.exe path\to\test.exe
   ```

### 4.3 产出与验收
程序执行过程中会自动安装驱动、拉起并监控 `test.exe`，结束后写入 `result.txt`，内容格式如下：
```txt
DST=127.0.0.1:5566
TX_BYTES=1024
RX_BYTES=2048
```
