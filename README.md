# WFP Traffic Monitor Project

This project implements a Windows kernel-mode driver (`WfpMonitor.sys`) and a user-mode application (`WfpController.exe`) using the Windows Filtering Platform (WFP) to intercept and monitor network traffic (Tx/Rx bytes) for a specified process (`test.exe`) during its lifecycle.

## Architecture

*   **User-Mode Controller (`WfpController.exe`):**
    *   Takes the target executable path and optional arguments (e.g., ping).
    *   Installs and starts the `WfpMonitor` service.
    *   Retrieves the Process ID (PID) of the spawned target process.
    *   Communicates with the kernel driver via `DeviceIoControl` to register the target PID and poll for traffic statistics.
    *   Logs the final network traffic statistics when the process terminates.

*   **Kernel-Mode Driver (`WfpMonitor.sys`):**
    *   Registers WFP Callouts at two layers:
        1.  `FWPM_LAYER_ALE_FLOW_ESTABLISHED_V4`: Captures the flow establishment to associate network connections (FlowContexts) with the target PID.
        2.  `FWPM_LAYER_STREAM_V4`: Monitors the actual data transfer (TCP/UDP streams) and accumulates Tx/Rx bytes in a lock-protected shared structure.
    *   Exposes an IOCTL interface to receive the target PID from the user mode and to return the accumulated traffic statistics.

## Environment Requirements

*   **Target OS:** Windows 10/11 (x64)
*   **WDK Version:** 10.0.22621.0 (Specified explicitly for stable build environments)
*   **Build System:** CMake + MSVC Compiler

## Compilation Instructions

This project includes a fully automated GitHub Actions CI/CD pipeline (`.github/workflows/build.yml`) that guarantees a clean environment and explicitly downloads/installs the required WDK 22621 to avoid pre-installed WDK bugs on generic runners.

To compile locally:
1.  Ensure Visual Studio 2022 and WDK 10.0.22621.0 are installed.
2.  Open a Developer Command Prompt.
3.  Run CMake to configure and build:
    ```cmd
    cmake -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release
    ```

## Testing and Execution Instructions

**IMPORTANT:** Kernel drivers that are not signed with a Microsoft WHQL certificate will not be loaded by default Windows installations. **This must be executed in a dedicated testing Virtual Machine.**

1.  **Enable Test-Signing Mode:**
    On the testing VM, open an Administrator Command Prompt and run:
    ```cmd
    bcdedit /set testsigning on
    ```
    Restart the VM. You should see a "Test Mode" watermark in the bottom right corner of the desktop.

2.  **Deploy the Binaries:**
    Copy the compiled `WfpMonitor.sys` and `WfpController.exe` (from the GitHub Actions `WFP-Compiled-Binaries` artifact or your local build) into the same directory on the testing VM.

3.  **Run the Controller:**
    Open an **Administrator Command Prompt** in the directory where the binaries are located.
    Provide the path to an executable you want to monitor (e.g., the built-in `ping.exe`).

    ```cmd
    .\WfpController.exe C:\Windows\System32\PING.EXE "www.microsoft.com"
    ```

4.  **Expected Output:**
    The controller will install the driver, launch the target process, and wait for it to finish. Once `ping.exe` completes, the controller will query the driver and print/log the exact number of incoming (Rx) and outgoing (Tx) bytes that the process generated during its execution.
