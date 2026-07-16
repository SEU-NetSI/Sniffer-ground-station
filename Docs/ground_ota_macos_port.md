# ground_ota macOS 移植与使用说明

## 程序架构与通信方向

`ground_ota` 是 PC 侧 OTA 镜像广播工具，不是串口程序、采集文件解析器或接收端刷写程序。它通过 libusb 打开 Crazyflie vendor USB 接口，把 typed downlink metadata 和 OTA payload 发送给作为中继的 Crazyflie；中继固件再构造 `UWB_OTA_MESSAGE` 并通过 UWB 广播。

每条 OTA 消息的主机侧发送顺序如下：

1. 向 Bulk OUT 端点发送 20 字节 typed metadata。
2. 将 OTA payload 按最多 64 字节拆分，继续通过 Bulk OUT 发送。
3. 中继通过 UWB 完成本地发送后，可由 Bulk IN 返回 `AC C0 00 01`。
4. 同一个 USB handle 会用于整个 START、DATA 和 END 过程，退出时才释放 interface 并关闭设备。

`AC C0 00 01` 只表示中继侧一次 UWB TX 已完成，不表示远端接收机已收到、CRC 校验通过或镜像已经写入成功。实际升级结果仍需通过接收端日志、状态或启动结果确认。

## USB typed metadata 与 OTA 数据格式

typed metadata 使用 little-endian，固定为 20 字节：

| 偏移 | 长度 | 含义 |
| --- | ---: | --- |
| 0 | 4 | magic `0x0000CCCD` |
| 4 | 2 | UWB 目的地址，当前固定为广播地址 `0xFFFF` |
| 6 | 2 | sequence |
| 8 | 2 | 后续 OTA payload 长度 |
| 10 | 8 | txTime，当前写入 `0` |
| 18 | 1 | UWB message type，OTA 为 `11` |
| 19 | 1 | 保留字段，当前写入 `0` |

OTA payload 以 6 字节 little-endian header 开始：

| 偏移 | 长度 | 含义 |
| --- | ---: | --- |
| 0 | 1 | command type：START `0`、DATA `1`、END `2` |
| 1 | 1 | image type：bootloader `0`、firmware `1` |
| 2 | 2 | body 长度 |
| 4 | 2 | DATA chunk index；START/END 使用 `0` |

START body 为 8 字节，依次包含 32 位镜像长度和 CRC32。DATA body 最大为 224 字节，END 没有 body。CRC 使用 CRC-32/ISO-HDLC；输入必须是非空的原始 BIN，不是 ELF、ZIP 等容器，单个镜像最大为 262112 字节。

每条消息默认重复发送 3 次，作为固定广播冗余，不是收到远端 NACK 后的自适应重试。程序先广播约 10.24 秒 START announcement，为已经进入 OTA receive mode 的节点提供接收窗口，然后发送 DATA 和 END。这个 START 阶段是 OTA 协议时序，不是 USB 启动卡顿，不应通过缩短 `--timeout-ms` 消除。

## 固件兼容前提

真实发送前必须确认中继固件同时支持：

- 20 字节 typed metadata 和 magic `0x0000CCCD`；
- 从 metadata byte 18 读取 UWB message type；
- `UWB_OTA_MESSAGE` type `11`；
- 为 OTA TX 注册完成回调，并在本地 TX 完成后通过 USB 返回 `AC C0 00 01`；
- 接收端已经进入与 image type 对应的 OTA 接收模式。

只支持旧式 18 字节 metadata、magic `0x0000CCCC`，或把 UWB type 固定为其他值的固件不能直接配合本工具。不要通过修改主机端 magic、长度或 type 来猜测兼容；应先核对设备实际运行的固件协议。

## Windows 与 macOS 接口对应关系

程序在 Windows 和 macOS 上都使用 libusb。Windows 依赖绑定到 interface 0 的 WinUSB/libusbK 驱动；macOS 使用系统安装的 libusb，不需要也不应改成 `/dev/cu.*` 串口。

平台相关实现保持在很小范围内：

- Windows 使用 `Sleep`，macOS 和其他 POSIX 平台使用 `nanosleep`。
- Windows 可使用 `C:\path\firmware.bin`，macOS 使用 `/path/to/firmware.bin`；包含空格的路径应加引号。
- macOS 在 claim interface 后直接使用 Bulk OUT/IN，跳过可能等待到超时的 link-select vendor control request；退出时同样不发送 restore link-select。
- Windows/非 Apple 构建保留原 link-select 行为，OTA 数据格式和 ACK 判断不因平台改变。

## 跨平台设计与工程集成

- `sniffer_storage/ground_ota.c` 保留 Windows 原版的 CRC、typed metadata、START/DATA/END、sequence、分片、重复发送和 ACK 格式。
- 命令行支持 `--device BUS:ADDRESS`，同时兼容旧版的匹配序号。
- interface、Bulk endpoint 和 transfer timeout 均可通过参数覆盖。
- `--list-devices` 只枚举匹配设备，不会 open、claim 或发送 OTA。
- `sniffer_storage/makefile` 提供 Windows 和 macOS 共用的 `ground_ota` 构建目标。

## 构建依赖

需要 C11 编译器、GNU Make、pkg-config 和 libusb 1.0。Makefile 优先使用 `pkg-config`，并通过 `brew --prefix libusb` 兼容 Apple Silicon Homebrew 路径。

macOS 缺少依赖时可执行：

```sh
brew install libusb pkg-config
```

Windows 可在 MSYS2 UCRT64 中构建，并需要为目标 USB interface 安装兼容 libusb 的 WinUSB/libusbK 驱动。

## 编译

从仓库根目录执行：

```sh
make -C sniffer_storage ground_ota
```

需要强制重建但不删除其他构建产物时：

```sh
make -C sniffer_storage -B ground_ota
```

编译使用 C11、`-Wall -Wextra -Wpedantic`，macOS 的 libusb 参数由 pkg-config/Homebrew 提供。

## 参数与设备选择

```text
--file PATH             BIN 镜像路径；省略时交互提示输入
--image-type VALUE      firmware/1（默认）或 bootloader/0
--device BUS:ADDRESS    精确选择设备；仍接受旧版匹配序号
--vid VALUE             默认 0x0483
--pid VALUE             默认 0x5740
--interface VALUE       默认 0
--endpoint-in VALUE     默认 0x81
--endpoint-out VALUE    默认 0x01
--timeout-ms VALUE      Bulk transfer/ACK poll 单次超时，默认 1000 ms；非 Apple 也用于 link-select
--list-devices          列出匹配 VID/PID 的 BUS:ADDRESS
--yes                   跳过接收端就绪确认
--no-local-ack          不等待中继本地 UWB TX 完成 ACK
--verbose               打印 typed metadata 和收到的 ACK 字节
--dry-run               不打开 USB，构造完整 OTA 序列并跳过延时
--self-test             执行 CRC 和序列化测试后退出
--help                  显示帮助后退出
```

`--image-type` 也接受缩写 `fw` 和 `bl`。本地 ACK 每次最多轮询 100 ms，总等待窗口固定为 1500 ms；增大 `--timeout-ms` 不会扩大该总窗口。

先使用程序自身枚举设备：

```sh
./sniffer_storage/ground_ota --list-devices
```

如有多个相同 VID/PID 的设备，应使用输出中的 `BUS:ADDRESS` 精确选择。USB 重新连接后 address 可能变化，应重新枚举。

也可查看 macOS USB 树，但 `system_profiler` 不一定显示 libusb address：

```sh
system_profiler SPUSBDataType
```

## 安全验证示例

先运行不访问硬件的内置测试：

```sh
./sniffer_storage/ground_ota --self-test
```

构造 firmware OTA 全流程，但不打开 USB、不发送数据且不等待协议延时：

```sh
./sniffer_storage/ground_ota \
  --file /path/to/firmware.bin \
  --image-type firmware \
  --dry-run
```

验证 bootloader 镜像参数和分片时同样应先使用 dry-run：

```sh
./sniffer_storage/ground_ota \
  --file /path/to/bootloader.bin \
  --image-type bootloader \
  --dry-run
```

对小镜像调试 metadata 时可追加 `--verbose`。大镜像会产生大量重复输出，常规 dry-run 可以省略该选项。

## 实际运行模板

只有在中继协议已经确认、接收端处于 OTA receive mode、镜像类型和 CRC 预期明确、供电和恢复路径可靠且现场允许广播升级时，才使用非 dry-run 模式：

```sh
./sniffer_storage/ground_ota \
  --file /path/to/firmware.bin \
  --image-type firmware \
  --device BUS:ADDRESS \
  --vid 0x0483 --pid 0x5740 \
  --interface 0 --endpoint-out 0x01 --endpoint-in 0x81 \
  --timeout-ms 1000 --verbose
```

程序会显示镜像路径、image type、大小、CRC32 和 chunk 数，然后要求输入 `YES`。建议保留这一步人工确认，不要在首次集成或现场状态不明确时使用 `--yes`。

默认会为每次 UWB 发送等待本地 ACK。除非协议负责人已经确认中继不提供该 ACK 且允许继续广播，否则不要使用 `--no-local-ack`。不要同时运行 cfclient 或其他占用同一 USB interface 的程序。

目的地址固定为 `0xFFFF`，所有处于兼容 OTA 接收模式且能收到该广播的节点都可能受影响。主机显示 `OTA broadcast completed` 也只代表发送流程完成，不能单独作为远端升级成功证明。

## 常见错误

- `libusb header not found` 或链接找不到 `usb_*`：运行 `brew install libusb pkg-config`，并确认 `pkg-config --cflags --libs libusb-1.0` 有输出。
- `No matching USB device`：确认连接、VID/PID，并重新运行 `--list-devices`；USB 重连后不要沿用旧 address。
- `could not open` / `claim interface failed`：关闭 cfclient 和其他占用程序，检查 interface；Windows 还需确认 WinUSB/libusbK 驱动绑定。
- endpoint direction mismatch：Bulk IN 必须包含方向位 `0x80`，Bulk OUT 不得包含该位。
- Bulk OUT/IN timeout：核对 interface、endpoint 和线缆；`--timeout-ms` 是单次 USB transfer 超时，不会取消约 10 秒 START announcement。
- macOS 仍出现 `USB link-select request failed`：可能正在运行旧二进制，应使用 `make -C sniffer_storage -B ground_ota` 重建并确认执行路径。
- `BIN must contain at least one byte` / `BIN is too large`：使用非空原始 BIN，且大小不得超过 262112 字节。
- `Timed out waiting for relay UWB TX completion ACK`：确认中继支持 `0x0000CCCD` typed downlink、type `11` 和 `AC C0 00 01` 本地 TX ACK；该错误不能通过接收端已经刷写成功来替代判断。
- `OTA send failed: phase=... chunk=... repeat=... sequence=...`：根据日志定位失败阶段；修复连接或协议问题后，应从完整 OTA 流程重新开始。
- 接收端没有 OTA 日志：确认其已进入 OTA receive mode、监听 type `11`，且镜像类型与固件设计一致。
- 程序长时间显示 START announcement：约 10 秒为预期协议行为；如果明显更长，再检查每次本地 ACK 是否超时。

## 安全边界

`--dry-run` 和 `--self-test` 不会打开 USB。`--list-devices` 只枚举设备。任何带有效镜像的非 dry-run 执行都会在确认后至少广播 START，不能作为只打开设备或只测试 ACK 的无副作用命令。

不要把本地 `AC C0 00 01` 或主机的 `OTA broadcast completed` 当作端到端升级成功证明，也不要在飞行中、接收端模式未知或固件协议未确认时发送 OTA。
