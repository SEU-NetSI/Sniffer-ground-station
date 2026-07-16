# ground_admin macOS 移植与使用说明

## 程序架构与通信方向

`ground_admin` 是 PC 侧交互式 ADMIN 命令发送器，不是串口、网络或采集文件解析程序。它通过 libusb 打开 Crazyflie vendor USB 接口，先向 Bulk OUT 端点发送 18 字节下行元数据，再发送 4 字节 ADMIN payload；固件经 UWB 广播命令，并可由 Bulk IN 返回 `AC C0 00 01` ACK。

元数据保持原协议不变：little-endian magic `0x0000CCCC`、目的地址 `0xFFFF`、16 位 sequence、payload 长度 `4`、64 位未使用 txTime。payload 是 little-endian 的 16 位 event 加 16 位零长度/保留字段。命令包括 `arm`、`disarm`、`takeoff`、`start`、`hover`、`land`、`reboot`、`ota` 和 `timeout`。

## Windows 与 macOS 接口对应关系

程序使用 libusb，不直接依赖 `windows.h`、`HANDLE`、WinUSB API、COM、Winsock、Windows 线程、`Sleep` 或 Visual Studio 专用函数。Windows 依赖 WinUSB/libusbK 驱动，macOS 使用系统上的 libusb；应用层的设备枚举、open、claim interface、control transfer、Bulk OUT/IN 和 release/close 均使用同一套 libusb API。程序没有文件路径、线程或高精度计时平台差异。

Windows 上仍可在 MSYS2 UCRT64 中构建；接口 0 必须绑定兼容 libusb 的 WinUSB/libusbK 驱动。macOS 不需要也不应改成 `/dev/cu.*` 串口。

## 跨平台设计与工程集成

- `sniffer_storage/ground_admin.c` 保留命令表、字节序、magic、广播目的地址、sequence、payload、分包方式、link-select vendor request 和 ACK 格式。
- 命令行支持 `--device BUS:ADDRESS`（同时兼容匹配序号）、`--list-devices`、`--interface`、`--endpoint-in`、`--endpoint-out` 和 `--timeout-ms`。
- 错误信息包含 interface、endpoint、libusb 错误名和错误码；退出路径会释放 interface、device handle 和 libusb context。
- `sniffer_storage/makefile` 提供跨平台 `ground_admin` 构建目标，与工程现有构建流程保持一致。

## 构建依赖

需要 C11 编译器、GNU Make、pkg-config 和 libusb 1.0。Makefile 优先使用 `pkg-config`，并用 `brew --prefix libusb` 兼容 Apple Silicon 的 `/opt/homebrew`，没有写死 `/usr/local`。

macOS 缺少 libusb 或 pkg-config 时，可通过 Homebrew 安装：

```sh
brew install libusb pkg-config
```

## 编译

从仓库根目录执行：

```sh
make -C sniffer_storage ground_admin
make -C sniffer_storage sniffer_storage
```

需要强制完整重建而不删除其他未跟踪产物时：

```sh
make -C sniffer_storage -B ground_admin
make -C sniffer_storage -B sniffer_storage
```

编译使用 C11、`-Wall -Wextra -Wpedantic`，macOS 的 libusb 参数由 pkg-config/Homebrew 提供。

## 参数与设备选择

```text
--device BUS:ADDRESS  精确选择设备；仍接受旧版的匹配序号
--vid VALUE           默认 0x0483
--pid VALUE           默认 0x5740
--interface VALUE     默认 0
--endpoint-in VALUE   默认 0x81
--endpoint-out VALUE  默认 0x01
--timeout-ms VALUE    默认 1000
--list-devices        列出匹配 VID/PID 的 BUS:ADDRESS
--command NAME        发送一个命令后退出
--no-ack              发送后不等待 ACK
--verbose             打印传输字节
--dry-run             不打开 USB，仅构造并显示数据包
```

先用程序自身枚举（这是获得 libusb BUS:ADDRESS 最准确的方法）：

```sh
./sniffer_storage/ground_admin --list-devices
```

也可先查看 macOS USB 树，但 `system_profiler` 不一定显示 libusb address：

```sh
system_profiler SPUSBDataType
```

## 安全示例与实际运行模板

不会访问硬件的包构造验证：

```sh
./sniffer_storage/ground_admin --dry-run --command arm --verbose
```

实际硬件模板如下。ADMIN 命令会改变飞行器状态；确认固件协议、现场安全和预期命令后，把 `BUS:ADDRESS` 与 `COMMAND` 替换为真实值：

```sh
./sniffer_storage/ground_admin \
  --device BUS:ADDRESS \
  --vid 0x0483 --pid 0x5740 \
  --interface 0 --endpoint-out 0x01 --endpoint-in 0x81 \
  --timeout-ms 1000 --command COMMAND --verbose
```

不带 `--command` 会进入交互模式。不要同时运行 cfclient 或其他占用同一 USB interface 的程序。`reboot`、`ota`、飞行控制类命令都不是只读操作。

## 常见错误

- `libusb header not found` 或链接找不到 `usb_*`：运行 `brew install libusb pkg-config`，再确认 `pkg-config --cflags --libs libusb-1.0` 有输出。
- `No matching USB device found`：确认连接和 VID/PID，运行 `--list-devices`；若有多个设备，使用 `--device BUS:ADDRESS`。
- `could not open` / `claim interface failed`：关闭 cfclient 和其他占用程序，检查 interface 参数；Windows 需绑定 WinUSB/libusbK。
- `USB link-select request failed: LIBUSB_ERROR_TIMEOUT`：如果随后 Bulk OUT/IN 和 ACK 正常，该 control request 超时可能不是致命错误。`--timeout-ms` 同时控制该请求和 Bulk transfer，增大该值也会增加启动等待时间；可先使用默认值，并用交互模式避免每条命令重复启动。
- `Required endpoint`、Bulk IN/OUT 错误或 timeout：用设备描述符核实 interface 和端点，确认 IN 端点含方向位 `0x80`，OUT 端点不含该位。
- ACK timeout：确认固件支持该下行协议和 `AC C0 00 01` ACK；只有明确不需要 ACK 时才使用 `--no-ack`。
