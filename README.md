# WFP 流量监控组件笔试项目

本项目实现了一个基于 Windows Filtering Platform（WFP）的流量监控组件。包含一个 Windows 内核态驱动程序（`WfpMonitor.sys`）和一个用户态控制程序（`WfpController.exe`）。该组件用于统计指定进程（如提供的 `test.exe`）在时间窗口（其生命周期）内的网络流量（出口 TX_BYTES / 入口 RX_BYTES），并将结果输出到日志文件中。

## 1. 依赖信息与环境要求

*   **操作系统要求**：Windows 10 或 Windows 11 (x64)
*   **WDK 版本要求**：**10.0.22621.0** (由于新版及预览版 WDK 的 C 语言头文件存在潜在的宏定义隐蔽及语法兼容性已知问题，本项目在 CMake 中通过明确指定 `CMAKE_SYSTEM_VERSION=10.0.22621.0` 来保证使用兼容良好的稳定版 WDK 进行编译)。
*   **编译器套件**：Visual Studio 2022 (MSVC)
*   **构建系统**：CMake (3.20+)

## 2. 构建步骤

本项目主要由 CMake 进行驱动和用户态程序的统一构建。
1. 请确保本地已安装 Visual Studio 2022 和 WDK 10.0.22621.0。
2. 打开 **Developer Command Prompt for VS 2022**（VS的开发者命令行工具）。
3. 导航到项目根目录（即 `CMakeLists.txt` 所在的目录）。
4. 运行以下 CMake 命令进行配置与构建：
   ```cmd
   cmake -B build -G "Visual Studio 17 2022" -A x64
   cmake --build build --config Release
   ```
5. 构建成功后，生成的可执行文件和驱动文件将位于 `build/Release/` 目录下：
   *   驱动文件：`WfpMonitor.sys`
   *   用户态程序：`WfpController.exe`

*(本项目也支持基于 GitHub Actions 的 CI/CD 自动构建。)*

## 3. 运行步骤（测试与验收）

### 3.1 准备测试环境
**⚠️ 注意：** 由于本项目编译生成的内核驱动部件（`WfpMonitor.sys`）未经过微软 WHQL 的正式发版数字签名，默认的 Windows 机器会拒绝加载。**请务必使用专门的 Windows 测试虚拟机进行验收！**

在测试虚拟机中，请**以管理员身份打开 CMD**，运行以下命令开启“测试签名”模式：
```cmd
bcdedit /set testsigning on
```
运行成功后，**必须重启虚拟机**。重启后，屏幕右下角会显示“测试模式”的水印，说明环境准备就绪。

### 3.2 运行步骤
1. 将编译好的 `WfpMonitor.sys`、`WfpController.exe` 以及供测试的 `test.exe` 文件放置在同一个目录下。
2. **以管理员身份运行 CMD 或 PowerShell**。
3. 导航到上述文件所在的目录。
4. 执行用户态控制程序，并将 `test.exe` 的路径作为参数传递给它：
   ```cmd
   .\WfpController.exe .\test.exe
   ```

### 3.3 测试与结果解释
1. `WfpController.exe` 会自动请求服务管理器（SCM）创建并挂载 `WfpMonitor` 驱动服务。
2. 控制器会以挂起（SUSPENDED）模式启动 `test.exe`，进而获取其恒定的生命周期 PID。
3. 控制器通过内核 IOCTL (`IOCTL_WFP_MONITOR_SET_PID`) 将 PID 传给挂钩在 `FWPM_LAYER_ALE_FLOW_ESTABLISHED_V4` 和 `FWPM_LAYER_STREAM_V4` 层的 WFP 内核引擎。
4. 发送成功后，控制器唤醒 `test.exe`，让其发送 HTTP 请求。在这个极其短暂的生命周期内，所有产生的 TCP TX/RX 流量（基于 STREAM_V4 以完美契合 TCP 请求的设定）都将在内核中被以字节为单位统计。
5. 当 `test.exe` 进程因操作完成而自动结束时，控制器会察觉，并再次发送 IOCTL 向驱动索要最终统计的 `WFP_MONITOR_STATS_OUT` 结果。
6. 最后自动卸载驱动，回收系统资源。

**`result.txt` 生成位置：**
执行完毕后，在运行控制器的**当前目录**下，会自动生成 `result.txt`（并输出到控制台）。文件的格式与要求完全一致，记录了 `DST=IP:Port` 以及精准统计的 `TX_BYTES` 和 `RX_BYTES`（十进制非负整数）。
