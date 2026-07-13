# `sniffer_storage` macOS 使用指南

## 1. 使用说明

`sniffer_storage` 已支持 Apple Silicon macOS。它通过 libusb 直接访问 USB Vendor Bulk 接口，不是串口程序，因此：

- 不使用 `/dev/cu.*` 或 `/dev/tty.*`；
- 不需要配置波特率；
- 默认匹配 VID:PID `0483:5740`；
- 默认使用 interface `0`；
- 默认从 Bulk IN endpoint `0x81` 接收数据；
- Bulk OUT endpoint 默认为 `0x01`，但普通采集不需要使用它。

当前 macOS 环境中的程序能够正常构建，19 项测试全部通过。

> 主机程序不会自动打开设备端的 Sniffer 功能。连接的板卡必须已经运行会从 USB 输出 Sniffer 数据流的固件。

## 2. 一次性安装、构建与测试

Mac 和 Windows 使用同一个 `sniffer_storage/makefile`，两端都直接运行普通的 `make`，不需要 `make -f`。Makefile 先检查 Windows 的 `OS=Windows_NT`，否则通过 `uname -s` 识别 macOS；Homebrew 和 pkg-config 只会在 macOS 分支中执行。

macOS 从项目根目录执行：

```bash
cd /Users/holden/Documents/lab_file/drone-communication-simulation-system

brew install libusb pkg-config

python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt

cd sniffer_storage
make clean
make
make test
```

macOS 的普通 `make` 会生成：

```text
sniffer_storage
pc_to_uwb
twoway_transport
```

Windows 仍在 `sniffer_storage/` 目录执行：

```bash
make clean
make
make test
```

Windows 分支继续使用原始的 `ground_control_windows.c`、`pc_to_uwb.c` 和 `twoway_transport.c`，不引用 macOS 专用源文件，也不要求安装 Homebrew。Windows 可执行文件在 MinGW 环境中带 `.exe` 后缀。

自动绘图固定使用项目根目录下的 `.venv/bin/python`。因此不要只在 `sniffer_storage/` 内部或其他虚拟环境中安装 Matplotlib。

可以用以下命令查看全部参数：

```bash
./sniffer_storage --help
```

## 3. 连接并枚举 USB 设备

1. 使用能够传输数据的 USB 线连接已经运行 Sniffer 固件的板卡。
2. 进入上位机目录。
3. 只读枚举匹配设备：

```bash
cd /Users/holden/Documents/lab_file/drone-communication-simulation-system/sniffer_storage
./sniffer_storage --list-devices
```

正常输出类似：

```text
bus=2 address=8 vid=0x0483 pid=0x5740
  interface=0 alt=0 endpoints=0x81/bulk/max64,0x01/bulk/max64
```

其中 `bus` 和 `address` 由当前 USB 连接状态决定，重新插拔后可能变化，不能长期写死。重新连接设备后应再次执行 `--list-devices`。

### 3.1 只有一台匹配设备

程序会自动选择它，采集命令不需要 `--device`：

```bash
./sniffer_storage --output capture.bin
```

### 3.2 有多台匹配设备

程序会拒绝自动选择。使用枚举结果中的 `BUS:ADDRESS`：

```bash
./sniffer_storage \
  --device 2:8 \
  --output capture.bin
```

### 3.3 设备使用其他 VID/PID

先用真实 VID/PID 枚举：

```bash
./sniffer_storage \
  --vid 0xXXXX \
  --pid 0xYYYY \
  --list-devices
```

采集时也必须带上相同的 `--vid` 和 `--pid` 参数。

## 4. 推荐的 10 条记录硬件测试

只有一台匹配设备时：

```bash
./sniffer_storage \
  --output smoke.bin \
  --max-records 10 \
  --timeout-ms 1000 \
  --no-print-records \
  --print-stats \
  --plot-after-capture \
  --plot-period-ms 30
```

有多台设备时，增加当前枚举得到的设备地址：

```bash
./sniffer_storage \
  --device 2:8 \
  --output smoke.bin \
  --max-records 10 \
  --timeout-ms 1000 \
  --no-print-records \
  --print-stats \
  --plot-after-capture \
  --plot-period-ms 30
```

程序收到 10 条完整有效记录后会自动结束采集、关闭文件、释放 USB interface，然后开始绘图。

如果设备一直没有发送数据，`--max-records 10` 不会因等待超时而自动退出。此时按 `Ctrl-C` 安全停止。

## 5. 长时间采集并自动绘图

```bash
./sniffer_storage \
  --output "raw_sensor_data_$(date +%Y%m%d_%H%M%S).bin" \
  --timeout-ms 1000 \
  --no-print-records \
  --print-stats \
  --plot-after-capture \
  --plot-period-ms 30
```

长时间采集没有设置 `--max-records`，按 `Ctrl-C` 结束。程序会依次完成：

1. 停止读取 Bulk IN；
2. 关闭二进制文件；
3. 释放 interface `0`；
4. 关闭 libusb 设备；
5. 如果至少保存了一条记录，则运行自动绘图。

每条 meta 默认会打印到终端。高数据率采集建议使用 `--no-print-records`，减少终端输出对采集性能的影响；最终统计仍可通过 `--print-stats` 查看。

### 5.1 完整打印记录、payload、统计并自动绘图

下面的示例指定设备 `2:6`，将每条 meta、payload 十六进制内容和最终统计全部打印到终端，并在采集结束后自动绘制 30 ms 相位图：

```bash
./sniffer_storage \
  --device 2:6 \
  --output "raw_sensor_data_$(date +%Y%m%d_%H%M%S).bin" \
  --timeout-ms 1000 \
  --print-records \
  --print-payload \
  --print-stats \
  --plot-after-capture \
  --plot-period-ms 30
```

使用时注意：

- `2:6` 必须来自本次 `./sniffer_storage --list-devices` 的输出；重新插拔后应重新枚举并替换该值；
- `--print-records` 打印每条记录的 meta；
- `--print-payload` 追加打印每条记录的完整 payload 十六进制内容；
- `--print-stats` 在结束时打印接收、保存、timeout、解析错误和重同步等统计；
- `--plot-after-capture` 和 `--plot-period-ms 30` 在文件关闭后生成 30 ms 相位图；
- 该命令没有限制记录数量，需要按 `Ctrl-C` 结束采集并触发自动绘图。

自动绘图完成后，图片位于：

```text
../outputs_binary/raw_sensor_data_YYYYMMDD_HHMMSS/phase.png
```

将目录名替换为本次输出文件的实际时间戳后，可以从 `sniffer_storage/` 目录打开图片：

```bash
open ../outputs_binary/raw_sensor_data_YYYYMMDD_HHMMSS/phase.png
```

自动绘图实际执行的等价手动命令如下。将示例时间戳替换为实际文件名：

```bash
cd /Users/holden/Documents/lab_file/drone-communication-simulation-system

.venv/bin/python analyze_sniffer_binary.py \
  sniffer_storage/raw_sensor_data_YYYYMMDD_HHMMSS.bin \
  --output outputs_binary/raw_sensor_data_YYYYMMDD_HHMMSS \
  --period-ms 30 \
  --timestamp-bits 40

open outputs_binary/raw_sensor_data_YYYYMMDD_HHMMSS/phase.png
```

`--print-payload` 只控制终端显示；二进制文件始终保存完整记录。完整打印会显著增加终端输出，在高数据率采集时可能影响性能；正式长时间采集更适合使用本节开头的 `--no-print-records` 命令。

## 6. 自动绘图行为

自动绘图不是实时绘图。它只会在以下条件全部满足后触发：

- 带有 `--plot-after-capture` 或 `--plot-output`；
- 采集已正常结束；
- 输出文件已成功关闭；
- 至少保存了一条完整记录。

默认输出目录为项目根目录下：

```text
outputs_binary/<二进制文件名去掉 .bin>/
```

例如输入文件为：

```text
sniffer_storage/raw_sensor_data_20260712_120000.bin
```

默认生成：

```text
outputs_binary/raw_sensor_data_20260712_120000/
├── phase.png
├── parsed_records.csv
├── group_source_0.csv
├── group_source_<其他源地址>.csv
├── parse_errors.csv
└── summary.txt
```

可以显式指定绘图输出目录：

```bash
./sniffer_storage \
  --output capture.bin \
  --max-records 1000 \
  --plot-output ../outputs_binary/my_capture \
  --plot-period-ms 30
```

`--plot-output` 本身会启用采集结束后的绘图，不必同时增加 `--plot-after-capture`。

## 7. 相位图含义

分析器读取每条记录的接收时间戳，保留有效低 40 位，处理时间戳回绕，以第一条捕获记录为时间零点，再按 `source_id` 分组绘制散点图。

`phase.png` 的含义是：

- 横轴：`接收时间 % period_ms`；
- 纵轴：`接收时间 / period_ms`；
- 不同颜色：不同的 `source_id`。

该图用于观察报文的公共周期、到达相位、抖动和相位漂移，不是距离曲线。

`--plot-period-ms` 只决定绘图使用的公共周期，不会修改 USB 采集速度或采集时长。例如：

- 固件周期约为 30 ms 时使用 `--plot-period-ms 30`；
- 固件周期为 60 ms 时使用 `--plot-period-ms 60`。

如果不确定周期，可以先查看 `summary.txt` 中各 source 的 `mean_delta_ms`，然后对同一个 `.bin` 使用不同周期重新绘图，无需重新采集。

## 8. 对已有 `.bin` 文件手动绘图

从项目根目录运行：

```bash
cd /Users/holden/Documents/lab_file/drone-communication-simulation-system

.venv/bin/python analyze_sniffer_binary.py \
  sniffer_storage/capture.bin \
  --output outputs_binary/capture_period30 \
  --period-ms 30 \
  --timestamp-bits 40
```

查看图片：

```bash
open outputs_binary/capture_period30/phase.png
```

若要比较 30 ms 和 60 ms：

```bash
.venv/bin/python analyze_sniffer_binary.py \
  sniffer_storage/capture.bin \
  --output outputs_binary/capture_period60 \
  --period-ms 60 \
  --timestamp-bits 40
```

## 9. 默认参数

| 参数 | 默认值 | 说明 |
|---|---:|---|
| `--vid` | `0x0483` | USB Vendor ID |
| `--pid` | `0x5740` | USB Product ID |
| `--interface` | `0` | 要 claim 的 USB interface |
| `--endpoint-in` | `0x81` | Sniffer 数据 Bulk IN endpoint |
| `--endpoint-out` | `0x01` | 可选 raw Bulk OUT endpoint |
| `--timeout-ms` | `5000` | 单次 USB transfer 超时，不是总采集时长 |
| `--max-payload` | `4096` | 接受的最大 payload 长度 |
| `--max-records` | `0` | 不限制记录数量 |
| `--plot-period-ms` | `60` | 相位图默认公共周期 |

## 10. 常见问题

### 10.1 `No matching devices found`

依次检查：

1. USB 线是否支持数据传输；
2. 板卡是否上电；
3. macOS 是否枚举到设备：

   ```bash
   system_profiler SPUSBDataType
   ```

4. 设备的 VID/PID 是否确实为 `0483:5740`；
5. 如果 VID/PID 不同，是否在枚举和采集命令中都带了正确的 `--vid`、`--pid`。

### 10.2 能枚举设备，但 `claim interface 0 failed`

关闭可能正在占用设备的 cfclient、串口监视器或其他 USB 程序，然后重新插拔设备并再次枚举。

### 10.3 `Required bulk endpoint is absent from interface`

设备固件的 USB descriptors 与当前参数不一致。根据 `--list-devices` 的输出确认 interface 和 endpoint，然后用以下参数覆盖：

```bash
--interface N --endpoint-in 0xNN
```

Bulk IN endpoint 的最高位必须为 `1`，例如 `0x81`。

### 10.4 只有 timeout，记录数始终为零

这通常说明 USB 设备可以打开，但固件没有输出 Sniffer 数据流。确认：

- 设备烧录的是正确固件；
- 固件构建中已经启用 Sniffer 模块；
- 设备端确实执行了向 USB 发送 Sniffer meta 和 payload 的路径。

主机程序不会通过 USB 自动开启 Sniffer。

### 10.5 没有生成 `phase.png`

检查：

1. 是否带了 `--plot-after-capture` 或 `--plot-output`；
2. 是否已经按 `Ctrl-C` 或达到 `--max-records`，让采集正常结束；
3. 最终统计中的有效记录数是否大于零；
4. 项目根目录是否存在可执行的 `.venv/bin/python`；
5. `.venv` 中是否安装了 `requirements.txt` 指定的 Matplotlib；
6. 是否从项目根目录或 `sniffer_storage/` 目录运行，以便程序找到 `analyze_sniffer_binary.py`。

必要时使用第 8 节命令对已有 `.bin` 手动绘图。

## 11. Bulk OUT 安全边界

普通 Sniffer 采集只需要 Bulk IN，不需要 `--send-hex`。

`sniffer_storage --send-hex` 只发送调用者提供的原始字节。即使 raw Bulk OUT transfer 成功，也只能证明 USB 字节传输完成，不能证明固件识别或执行了命令。

在没有确认已安装固件的准确命令格式之前，不要随意执行：

```bash
./sniffer_storage --send-hex ...
```

## 12. macOS 发送与双向程序

`pc_to_uwb` 和 `twoway_transport` 在 macOS 直接由原始源文件构建，没有单独的 `_macos.c` 版本。两者自动打开枚举到的第一台 `0483:5740` 设备，使用 interface `0`、Bulk OUT `0x01` 和 Bulk IN `0x81`。

这两个程序不支持 `--device BUS:ADDRESS`。如果连接了多台相同设备，先断开其他设备，避免程序自动选择到错误目标。

### 12.1 单次发送程序

准确语法是：

```bash
./pc_to_uwb <dest_addr> "<hex_payload>" [seq]
```

当前配套固件源码把这类下行固定封装为 `UWB_GETPC_MESSAGE`；接收端对此类型只打印收到的内容，不执行控制命令。以下命令使用广播地址 `0xffff`，发送零长度 payload，可用于最小链路测试：

```bash
./pc_to_uwb 0xffff "" 1
```

非空 payload 的业务语义由接收固件决定。发送非空内容前，仍应由固件负责人确认目的地址、payload 和预期行为。

程序发送 18 字节下行 meta，随后发送 payload。meta 使用 magic `0x0000CCCC`，包含目的地址、序号、payload 长度和发送时间；当前 macOS/Windows 主机均按小端布局发送。程序等待的 ACK 固定为：

```text
AC C0 00 01
```

出现 `Sent ... bytes to UWB dest ...` 只证明 meta 和 payload 的 Bulk OUT 已完整提交；随后出现 `Received downlink ACK from CF firmware` 才证明主机收到了上述 ACK。固件在本机 UWB TX 完成回调中生成该 ACK；它不携带目的地址、序号或远端业务响应，因此只能证明 PC → USB 设备 → 本机 UWB 发射这一段完成，不能单独证明远端 UWB 节点已经收到或完成应用层处理。

### 12.2 双向程序

交互模式：

```bash
./twoway_transport
```

程序打开设备并开始保存 Bulk IN 数据后，可输入：

```text
send <dest_addr> <hex_payload> [seq]
quit
```

交互命令按空白分词，因此 payload 内不要包含空格，可使用冒号分隔，例如 `01:02:03:04`。也可在启动时发送一次：

```bash
./twoway_transport <dest_addr> "<hex_payload>" [seq]
```

与单次发送程序相同的零长度最小链路测试为：

```bash
./twoway_transport 0xffff "" 2
```

启动参数发送完成后程序仍会进入交互模式，需要输入 `quit` 正常释放 interface。该程序会同时接收普通上行 Sniffer 帧并保存文件，但当前协议没有把上行帧与某次下行请求可靠关联，因此“收到任意上行帧”不等于应用层双向闭环成功。

除上述零长度 GETPC 链路测试外，当前源码没有定义 ping、echo、query 等具有业务响应的 payload。源码 usage 中的 `0xffff "01 02 03 04"` 只是格式示例，业务语义未知，不应直接当作硬件测试命令。要验证远端应用层闭环，仍需由固件负责人给出非空业务 payload 以及可与请求对应的预期响应。

## 13. 相关实现

- `sniffer_storage/sniffer_storage.c`：命令行参数、采集流程、信号退出和自动绘图触发条件；
- `sniffer_storage/libusb_transport.c`：设备枚举、选择、interface claim、endpoint 校验和 Bulk transfer；
- `sniffer_storage/sniffer_plot_macos.c`：macOS 自动调用项目根目录 Python 分析器；
- `analyze_sniffer_binary.py`：二进制解析、40 位时间戳处理、CSV/统计输出和 `phase.png` 绘制；
- `sniffer_storage/README.md`：简要构建与使用说明。
