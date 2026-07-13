# 二进制 Sniffer 数据解析与相位图实现

## 1. 任务目标

本实现直接读取新工程保存的二进制 Sniffer 文件，解析为标准记录，在全局到达顺序中处理 DW 时间戳，再生成与旧工程 `phase.png` 含义、计算方法和主要样式相同的图。实现入口为 `analyze_sniffer_binary.py`，解析、分析、导出和绘图虽放在一个可直接运行的文件中，但函数边界彼此独立；绘图函数不读取原始字节。

## 2. 旧工程 phase.png 的语义

旧分析文档 `Docs/sniffer_output_plot_analysis.md` 第 5.1、6、7 节确认：先按全局到达顺序解卷绕 40 位 `meta_rx_time`，再按 `rng_src` 分组；横坐标为 `rx_time_ms % 60.0`，纵坐标为 `rx_time_ms / 60.0`。60 ms 是旧程序的显式固定值，不是从间隔均值计算所得。

## 3. 当前二进制数据产生链路

```text
dwt_readrxtimestamp() 读取 DW3000 接收时间戳
    ↓ AdHocUWB/Src/adhocuwb_sniffer.c::snifferRxCallback [16-28]
Ranging message + rxTime 进入 FreeRTOS queue
    ↓ snifferTask [31-71]
Sniffer_Meta_t 从 Ranging header 与 rxTime 填充 [50-54]
    ↓ usbSendData(sizeof(Sniffer_Meta_t), raw) [55]
18-byte meta，经 USB 分块发送 body [57-67]
    ↓ sniffer_storage/ground_control_windows.c::read_next_meta [159-219]
按 magic 搜索并准确读取 18 字节 meta
    ↓ store_sniffer_binary_data [221-277]
fwrite(meta.raw, 18)，随后 fwrite(msgLength 字节 payload) [243-274]
    ↓
无文件头的重复 [18-byte meta][variable payload]
    ↓ analyze_sniffer_binary.py::parse_binary
PacketRecord
    ↓ analyze_records（全局 mask/unwrap、单位换算、组内 delta）
标准 CSV/统计
    ↓ plot_phase
phase.png
```

## 4. Sniffer_Meta_t 实际内存布局

固件定义在 `AdHocUWB/Inc/adhocuwb_sniffer.h:12-21`，匿名结构体和 union 均有 `__attribute__((packed))`。Windows 采集端定义在 `sniffer_storage/ground_control_windows.c:26-37`，受 `#pragma pack(push, 1)` / `pop` 包围。

使用当前主机 C 编译器实际编译等价定义，结果为：

```text
sizeof(Sniffer_Meta_t) = 18
sizeof(((Sniffer_Meta_t *)0)->raw) = 18
offsetof(magic) = 0
offsetof(senderAddress) = 4
offsetof(seqNumber) = 6
offsetof(msgLength) = 8
offsetof(rxTime) = 10
```

因此已确认不存在 10~15 字节 padding，也不存在“字段赋值后只发送 rxTime 前两字节”的风险。

## 5. 文件中的真实字节格式

每条记录是固定 18 字节 meta 加可变长 payload：

```text
offset +0   uint32 magic
offset +4   uint16 senderAddress
offset +6   uint16 seqNumber
offset +8   uint16 msgLength
offset +10  uint64 rxTime
offset +18  uint8 payload[msgLength]
```

`msgLength` 是 body 长度，不包含 18 字节 meta，也不包含已经被固件剥离的 8 字节 `Ranging_Message_Header_t`。证据是 `adhocuwb_sniffer.c:53` 用 header 的 `msgLength - sizeof(Ranging_Message_Header_t)` 赋值，随后从 `rangingMessage.body` 发送恰好该长度（57-67）；Windows 端又原样消费和写入该长度（253-274）。

文件没有文件头、包长度前缀、主机时间戳、校验和、padding、结束标记或 USB transfer 边界标记。多条记录只依靠 meta 自身的 magic 与长度衔接。USB 分块边界不会进入文件。

## 6. 字节序与字段偏移

线上结构由 STM32/GCC packed 原生字段发送，接收端按同样的小端字段读取；magic `0x0000BBBB` 在线字节为 `BB BB 00 00`，Windows 重同步代码也明确搜索这 4 字节（`ground_control_windows.c:159-177`）。Python 因而使用 `struct.Struct("<IHHHQ")`，其大小为 18。

## 7. 新旧字段映射

| 新字段 | 旧字段 | 状态与证据 |
| --- | --- | --- |
| `rxTime` | `meta_rx_time` | 已确认；均来自 `dwt_readrxtimestamp()`，固件在 `adhocuwb_sniffer.c:20-26,54` 直接复制 |
| `senderAddress` | `rng_src` | 已确认；直接赋值自 `rangingMessage.header.srcAddress`（51） |
| `seqNumber` | `rng_seq` | 已确认；直接赋值自 `rangingMessage.header.msgSequence`（52） |
| `msgLength` | body 长度 | 已确认；为 Ranging 整帧声明长度减 8 字节 header（53） |
| `magic` | 新协议 `0xBBBB` | 已确认；固件赋值（50）、采集端校验（14,247） |

旧文档涉及的旧采集 magic `0xBB88` 不适用于当前新格式；新格式是 `0xBBBB`。

## 8. senderAddress 与 rng_src 的关系

已确认严格等价。旧 `rng_src` 来自 Ranging body header 的 `srcAddress`；当前固件在去掉该 header 之前，把同一个字段复制到 meta 的 `senderAddress`。所以代码内部统一命名为 `source_id`，图例仍可正确使用 `rng_src=<id>`。

## 9. rxTime 的位宽和单位

`dwTime_t` 在 `support.h:56-67` 中具有 `raw[5]`，`NULL_TIMESTAMP=0xFFFFFFFFFF`、`UWB_MAX_TIMESTAMP=1099511627776=2^40` 分别见 `support.h:40,48`。固件从 DW3000 读 5 字节并存入局部变量（`adhocuwb_sniffer.c:20-21`），随后复制 `full`。因此有效业务位为低 40 位；解析器先 mask 再解卷绕。

这里还有一个已确认的数据卫生风险：`dwTime_t rxTime;` 没有清零，而 DW API 只写 `raw[5]`，读取 `full` 时高 24 位未初始化。低 40 位本身不受影响，解析器遇到非零高位会写入 `timestamp_high_bits` 诊断并保留低 40 位。最小固件修复是把第 20 行改成 `dwTime_t rxTime = {0};`；本任务没有静默改变线上格式。

换算常量由当前工程 `support.h:42` 再次确认：`DWT_TIME_UNITS = 1 / 499.2e6 / 128.0` 秒，即 `tick_hz=63,897,600,000 Hz`。

## 10. 二进制解析器设计

`parse_binary()` 消费 bytes 并输出不可变 `PacketRecord` 与 `ParseError`；`read_binary()` 只负责文件读取；`analyze_records()` 只处理标准记录；`plot_phase()` 只处理已分析记录。

安全行为包括：空文件；不足 18 字节的 meta；payload 截断；错误 magic；超过默认 4096 的长度；按 magic 重同步；非零时间戳高 24 位；完全重复记录；时间戳 mask/回绕；多源交错；单记录源；序列回绕下的跳变/重复统计。当前 `msgLength` 是 body 长度，合法最小值为 0，不存在大于 0 的协议最小值。发生损坏时错误 CSV 记录精确输入偏移。由于 payload 可以任意包含 magic，长度有效时必须信任长度，不能在 payload 中盲目切帧；仅在当前 meta 无效或截断时尝试同步。

## 11. 时间戳解卷绕

默认 `timestamp_bits=40`，`WRAP=2^40`，阈值为 `2^39`。记录依 `arrival_order` 保持文件顺序，跨全部源只执行一次解卷绕：当前低 40 位值小于前值且下降超过阈值时增加一次 wrap。之后才按源计算相邻 `delta_ms`。解析器不把大间隔命名为丢包。

## 12. phase.png 的计算方法

默认 `period_ms=60.0`：

```python
x = rx_time_ms % period_ms
y = rx_time_ms / period_ms
```

图为 8×8 英寸、120 DPI、点标记 `.`、markersize 6、`tab10`、grid alpha 0.3、xlim `(0, period_ms)`，标题和坐标轴文字与旧图一致。源地址按数值升序映射颜色。

## 13. 输出文件说明

每个输入独占一个输出目录：`phase.png`、`parsed_records.csv`、`group_source_<id>.csv`、`summary.txt`、`parse_errors.csv`。分组 CSV 包括到达序号、输入偏移、meta 字段、低位时间戳、连续时间戳、毫秒值和组内 delta。summary 给出每源记录数、首尾时间、间隔均值/总体标准差/最小/最大、序列跳变和重复序列数。

## 14. 测试和验证结果

`python3 -m unittest discover -s tests -v`：8 项全部通过，覆盖 18 字节布局、单记录、多源全局回绕、截断、magic 重同步、非法长度、空文件、重复记录、高位诊断、组内时间排序和相位坐标。

初次实现时只有 `tests/create_synthetic_sniffer.py` 生成的 6 记录双源 synthetic 样本。后续已对真实文件 `sniffer_storage/raw_sensor_data_20260711_115415.bin` 完成验证：2058 条记录、源 `{0}`、精确到 EOF、0 帧解析错误、7 条高位诊断，并成功生成非空 960×960 PNG。详细结果见第 20 节链接。

## 15. 已确认结论

- meta 是 packed 的 18 字节，`rxTime` 位于 offset 10。
- 文件为小端 `[meta][payload]` 重复流；单条总长为 `18 + msgLength`，不是固定 18。
- `msgLength` 为剥离 8 字节 Ranging header 后的 body 长度。
- `senderAddress == 旧 rng_src`，`seqNumber == 旧 rng_seq`。
- `rxTime` 是有效低 40 位 DW 接收时间戳，频率为 63,897,600,000 Hz。
- 新图与旧图的分组语义、坐标计算和主要样式一致。
- 固件当前只写 `dwTime_t.raw[5]`，但未先初始化 `full` 的高 24 位；低 40 位不受影响，原始高位可能不稳定。

## 16. 高可信推断

- 采集文件如果只由当前 `ground_control_windows.c` 创建，则不会包含额外容器字段；这是从唯一写文件路径得出的高可信结论。
- 损坏后的 magic 搜索通常能恢复下一帧，但 payload 内可能自然出现相同 4 字节；无法对任意损坏保证无假同步。

## 17. 待确认事项

- 仍没有同一次采集对应的旧 CSV，不能跨采集器逐记录比对。
- 没有同次采集的旧 CSV 与新二进制文件，无法逐记录实证比对，字段等价性目前由生产代码直接赋值链确认。
- 60 ms 是复现旧图的默认配置；当前工程没有证明它适用于每一种运行模式。
- CPU/编译器原生小端是当前固件链路事实；若未来移植到大端平台，应改成显式 wire serialization，而不能沿用原生字段发送。

## 18. 运行命令

```bash
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python analyze_sniffer_binary.py path/to/input.bin \
  --output outputs_binary/example \
  --period-ms 60 \
  --timestamp-bits 40
```

Synthetic 验证：

```bash
.venv/bin/python tests/create_synthetic_sniffer.py
.venv/bin/python analyze_sniffer_binary.py testdata/synthetic_sniffer.bin \
  --output outputs_binary/synthetic_sniffer --period-ms 60 --timestamp-bits 40
```

## 19. 最终结论

当前格式没有 padding 缺陷。新程序按已经证实的 18 字节小端 packed meta 与 `msgLength` body 解析，在混合源的文件顺序中统一处理 40 位回绕，并直接向每个输入的独立目录生成与旧图同义的相位图及诊断产物。真实文件验证进一步确认当前格式可以无歧义解析到 EOF；仍待确认的是本次现场周期、启动阶段异常和固件高位初始化问题。

## 20. 真实数据验证

后续已取得并验证真实文件 `sniffer_storage/raw_sensor_data_20260711_115415.bin`。文件大小 123,480 字节，成功解析 2058 条 source 0 记录，严格消费到 EOF；magic、长度、截断和重同步错误均为 0，发现 7 条非零时间戳高 24 位诊断和 4 次低 40 位回绕。

真实验证还发现分析层应像旧工程一样在全局 unwrap 后按连续时间排序再计算组内 delta；该行为已修复并增加回归测试，核心协议和相位坐标没有改变。完整证据、统计、图片检查和 Baseline 对比见 [真实 Sniffer 数据验证：20260711_115415](real_sniffer_validation_20260711_115415.md)。
