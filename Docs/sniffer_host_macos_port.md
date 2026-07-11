# Sniffer 上位机 macOS 移植与验证

## 1. 实证结论

当前 Sniffer 上位机不是 Windows COM 或 USB CDC 虚拟串口客户端，而是直接使用 libusb 访问 VID:PID `0483:5740` 的 vendor interface 0 和 Bulk IN endpoint `0x81`。因此 macOS 移植继续使用 libusb；把它改成 `/dev/cu.*`、`termios` 和 baud 会连接到不同接口，缺乏代码证据。

移植保持已有文件协议不变：

```text
[18-byte little-endian Sniffer meta][msgLength-byte payload]
magic = 0x0000BBBB
```

## 2. 原始程序入口与职责

### 2.1 Makefile 实际入口

原 `sniffer_storage/makefile` 只编译 `sniffer_storage.c`，其 `main()` 是默认上位机入口。原职责为：

```text
libusb_init
  -> libusb_open_device_with_vid_pid(0483:5740)
  -> libusb_claim_interface(0)
  -> storeSnifferBinaryData
      -> read_exact_usb(meta, 18) from endpoint 0x81
      -> fwrite(meta)
      -> read_exact_usb(payload, msgLength)
      -> fwrite(payload)
  -> release / close / libusb_exit
```

### 2.2 `ground_control_windows.c`

这是 `README_windows.md` 用手工 GCC 命令构建的另一个独立入口，不在原 Makefile 中。它也是 libusb 程序而非 Win32 COM 程序；相较原默认入口已有滑窗 magic 重同步、4096 字节长度上限、超时继续等待、设备枚举和 WinUSB/Zadig 提示。

## 3. 固件发送调用链

当前仓库可见的设备到主机定义态链为：

```text
AdHocUWB/Src/dw3000_transceive.c::rxCallback           [94-108]
  -> registered hardware callback                      [168-170]
  -> AdHocUWB/Src/adhocuwb_transceive.c::adhocuwb_rxCallback [82-101]
  -> SNIFFER listener
  -> AdHocUWB/Src/adhocuwb_sniffer.c::snifferRxCallback [16-29]
  -> FreeRTOS queue
  -> snifferTask                                       [31-71]
      -> fill meta                                     [50-54]
      -> usbSendData(18-byte meta)                     [55]
      -> usbSendData(payload chunks)                   [57-67]
  -> external USB firmware stack
  -> host Bulk IN 0x81
```

`Sniffer_Meta_t` 位于 `AdHocUWB/Inc/adhocuwb_sniffer.h:12-21`。但本仓库没有 `usb.h`、`usbSendData` 定义、USB descriptors 或可烧录的父固件；根 `README.md:7-9` 也明确说明不含可直接烧录的 Sniffer 驱动固件。默认配置还没有启用该链：`SNIFFER_ENABLE` 被注释，CMake/Kbuild 中 `adhocuwb_sniffer.c` 也被注释。

## 4. Windows 专用依赖

源码中不存在以下 Win32 API：

- `windows.h`、`HANDLE`、`DWORD`、`OVERLAPPED`、`DCB`；
- `CreateFile`、`ReadFile`、`WriteFile`、`SetCommState`；
- COM 设备名或波特率配置。

实际 Windows 专用环境只有：MSYS2/MinGW、libusb DLL、WinUSB 驱动和 Zadig 提示。`#pragma pack(push, 1)` 可被 MinGW/GCC/Clang接受，不是串口 API。新的默认程序只使用标准 C 和跨平台 libusb，保留 Windows 构建能力。

## 5. macOS 构建失败与修复

验证环境：

```text
macOS 26.5.1 arm64
Apple clang 21.0.0
libusb 1.0.30 (Homebrew)
```

修改前执行 `make clean && make` 失败。`pkg-config` 给出：

```text
-I/opt/homebrew/Cellar/libusb/1.0.30/include/libusb-1.0
```

源码却 include `<libusb-1.0/libusb.h>`，形成重复目录语义并报 header not found。现统一为 `<libusb.h>` 配合 pkg-config，Makefile 增加 `-std=c11 -Wall -Wextra -Wpedantic`。修改后默认程序、legacy Windows程序和测试均在上述macOS环境零警告构建。Windows路径已保留并补充MSYS2 `pkgconf` 与 `/mingw64/include/libusb-1.0` fallback，但当前机器没有MinGW交叉编译器，因此Windows构建尚未在本次实跑。

## 6. 跨平台实现

- `sniffer_storage.c`：CLI、参数验证、信号退出、输出路径、设备选择、可选 raw Bulk OUT。
- `libusb_transport.[ch]`：枚举、按 VID/PID 和可选 BUS:ADDRESS 选择、claim/release、descriptor 中验证 Bulk endpoint、部分 IN/OUT transfer、timeout/EINTR/断开映射。
- `sniffer_buffered_transport.[ch]`：用16 KiB staging接收USB transfer，再按parser所需字节数消费，避免4字节magic窗口小于18/64字节USB packet导致overflow和数据丢失。
- `sniffer_capture.[ch]`：与 libusb 解耦的流读取、显式小端 meta 解码、部分读取累加、magic 重同步、长度检查、完整记录落盘和分类统计。
- `sniffer_hex.[ch]`：raw OUT 十六进制输入解析与规范化显示。

接收端只在 meta 和完整 payload 都收到后写入记录，避免正常截断输入污染输出文件。统计包括记录数、接收/存储字节、magic错误、非法长度、meta/payload截断、读超时、transport错误、文件写失败、重同步和丢弃字节数。

## 7. CLI

列出匹配设备及目标 interface 的 endpoint descriptors（只读枚举，不 claim）：

```bash
cd sniffer_storage
./sniffer_storage --list-devices
```

采集：

```bash
./sniffer_storage \
  --device BUS:ADDRESS \
  --output capture.bin \
  --max-records 10
```

可用 `--vid`、`--pid`、`--interface`、`--endpoint-in`、`--timeout-ms`、`--max-payload` 覆盖默认值。`--baud` 会明确拒绝并说明当前接口不是串口。

## 8. 主机到设备发送边界

全仓库原先没有 libusb OUT transfer、host send函数、`usbReceive`/`usbRx`/`CDC_Receive`、UART RX或 Sniffer命令handler。UWB routing中的 `UWB_DATA_MESSAGE_COMMAND` 是无线层枚举，不能冒充 USB 控制协议。

官方 Crazyflie USB 栈的 interface 0 通常包含 Bulk OUT并进入 CRTP queue，但本仓库的外部 USB 栈缺失，且这里调用的 `usbSendData(size, data)` 与官方现版 API 签名不同，不能据此假定现场固件应用协议。

本次只实现 descriptor验证后的 raw Bulk OUT：

```bash
./sniffer_storage \
  --device BUS:ADDRESS \
  --endpoint-out 0x01 \
  --send-hex "已由现场固件确认支持的实际帧" \
  --send-only
```

发送必须显式指定 `--device BUS:ADDRESS`，不会自动选择第一台VID/PID匹配设备。发送前程序显示 bus/address、endpoint、字节数和规范化 hex；实现会处理部分写、有限次数timeout和EINTR。USB transfer成功只证明传输完成，不证明固件解析或执行。由于缺少可烧录固件和真实 RX hook，本次没有发明 `{magic,type,length,payload}` 命令，也没有声称已完成应用层双向闭环。

## 9. 自动化验证

```bash
make clean
make
make ground_control_windows
make test
```

mock transport 测试覆盖：

1. 完整 meta + payload及字段偏移；
2. meta逐字节拆分；
3. payload多次拆分；
4. USB一次交付18字节meta而parser先请求4字节的staging回归；
5. USB TIMEOUT同时带部分数据时staging无丢失；
6. 多条连续记录；
7. 错误magic后重同步；
8. 无法恢复的错误magic尾部；
9. 非法长度；
10. payload截断；
11. meta截断；
12. payload中途设备断开计入截断和transport error；
13. timeout后恢复；
14. Bulk OUT部分写；
15. 发送hex解析、编码及非法帧拒绝；
16. Bulk OUT timeout预算耗尽后停止。

实际结果：`All 16 sniffer storage tests passed`。

## 10. 当前 macOS 硬件检查

系统存在 `/dev/cu.usbmodem21202` / `/dev/tty.usbmodem21202`。I/O Registry显示对应设备为 Crazyflie 2.x、VID:PID `0483:5740`。沙箱外执行本程序的只读枚举确认设备为 `bus=2 address=8`，interface 0 descriptors 为 `0x81/bulk/max64` 和 `0x01/bulk/max64`；地址只是本次连接状态，重插后可能改变。

没有选择 `/dev/cu.usbmodem21202`，因为 Sniffer程序使用的是同一复合设备上的 vendor Bulk interface 0，而不是CDC tty。使用明确枚举出的 bus/address 对最终staging实现做了只读 smoke capture：成功打开并 claim interface 0、验证 Bulk IN `0x81`，33次1秒timeout内收到0字节/0记录，随后用 `Ctrl-C` 正常退出，统计为0错误且 `/tmp/sniffer_macos_smoke_staged.bin` 为0字节。这证明macOS host transport和清理路径可运行，但现场设备当时没有输出 Sniffer流。全程未调用Bulk OUT。

用户重新确认固件已启用 Sniffer 后，应先运行：

```bash
./sniffer_storage --list-devices
```

再用该命令实际打印的新 BUS:ADDRESS 执行10记录只读测试。USB address可能在重连后改变，不能把当前8硬编码到长期脚本中。

## 11. 待完成的硬件闭环

- 释放当前外部进程的 USB interface 后执行真实10记录采集，核对统计和输出二进制。
- 取得现场可烧录父固件的 `usb.h`、descriptor、OUT callback和构建配置。
- 根据现场固件实际CRTP/命令格式验证 raw Bulk OUT；在此之前禁止随意发送。
- 如要新增应用命令协议，应在可烧录固件仓中实现有界OUT parser/handler及独立ACK通道，不能把ACK混入现有 Sniffer IN字节流。
