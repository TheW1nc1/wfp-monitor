# WFP 流量监控组件

本项目是为统计 `test.exe` 生命周期内的 TCP 流量而设计的 WFP (Windows Filtering Platform) 组件，由驱动程序 (`WfpMonitor.sys`) 和控制程序 (`WfpController.exe`) 组成。

## 1. 技术方案实现

### 内核态驱动 (WfpMonitor)
驱动程序工作在两个 WFP 层级：
- **ALE 连接层** (`FWPM_LAYER_ALE_AUTH_CONNECT_V4`)：
  - 拦截目标 PIsD 的连接请求。
  - 使用 `FwpsFlowAssociateContext0` 为该连接关联上下文。
  - 获取目的 IP 和端口。
- **Stream 流数据层** (`FWPM_LAYER_STREAM_V4`)：
  - 通过 ALE 层关联的上下文识别目标流量。
  - 使用 `streamData->flags` 判断方向（`FWPS_STREAM_FLAG_SEND` / `FWPS_STREAM_FLAG_RECEIVE`）。
  - 对 `streamData->dataLength` 进行原子累加。

### 用户态控制程序 (WfpController)
- **生命周期管理**：以挂起状态启动 `test.exe` 以确保捕获完整生命周期流量。
- **自动逻辑**：自动创建/启动驱动服务，通过 IOCTL 通信获取结果，最后自动停止服务。

## 2. 依赖与环境

- **运行环境**：Windows 10/11 x64 (需要开启 `bcdedit /set testsigning on`)
- **编译环境**：Visual Studio 2022, WDK 10.0.22621.0, CMake 3.20+

## 3. 构建与执行步骤

### 编译
```cmd
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_SYSTEM_VERSION=10.0.22621.0
cmake --build build --config Release
```

### 运行
1. 以管理员权限运行 CMD。
2. 执行 `.\WfpController.exe .\test.exe`。
3. 执行完成后，同目录下生成的 `result.txt` 即为统计结果。

## 4. 特别说明

- **关于测试加载**：由于内核驱动未通过微软 WHQL 认证，测试前必须在虚拟机中运行 `bcdedit /set testsigning on` 并重启，否则驱动无法通过 `sc start` 启动。
- **关于 GitHub CI**：CI 仅用于验证代码在标准 WDK 环境下的可编译性。GitHub 托管的 Runner 无法开启测试模式进行驱动加载测试。
- **代码库内容**：本项目已包含完整的驱动源码、控制器源码、以及在测试环境下生成的 `result.txt`。
