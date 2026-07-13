# Sniffer 输出图片与绘图链路分析

## 1. 分析范围

本报告基于当前工作区的实际文件和源码，结论状态使用以下标记：

- **已确认**：可由当前源码、产物或实际检查直接证明。
- **高可信推断**：代码行为与现存产物相互印证，但缺少完整原始输入或运行历史。
- **待确认**：当前工程没有足够证据。

用户描述中的目录名是 `output/`，但当前工程实际目录是 **`outputs/`（复数）**。递归检查范围包括：

- `outputs/sniffer_*` 的全部 51 个时间戳目录；
- 其中全部 PNG 及所有配套 CSV/TXT；
- `data/` 中仍保留的 3 个 `sniffer_*.csv` 原始输入；
- 全工程内的绘图、图片保存、目录命名及 `sniffer_` 关键词；
- 代表性目录：
  - `outputs/sniffer_20260528_151542`：仅图片，早期双源结果；
  - `outputs/sniffer_20260530_123401`：图片、双源明细 CSV、完整日志；
  - `outputs/sniffer_20260603_014612`：`rng_src=5/8`，12 ms 间隔特征；
  - `outputs/sniffer_20260608_154251`：仍有对应原始输入，可静态逐字段核对；
  - `outputs/sniffer_20260608_161230`：五种源地址，包含明显可疑源 `48008`；
  - `outputs/sniffer_20260608_160939`：仅日志的残缺目录。

图片格式检查覆盖 `*.png/*.jpg/*.jpeg/*.svg/*.pdf`。在 `sniffer_*` 目录中只发现 PNG，没有 JPG/JPEG/SVG/PDF。

运行验证方面，尝试在 `/tmp` 中用当前默认 `python3` 对保留输入重新运行 `analyze_sniffer.py`，但环境缺少 `pandas`（`ModuleNotFoundError`），因此没有改动或重生成任何工程内产物。源码、输入 CSV、分组 CSV、日志与现有图片之间仍可完成交叉验证。

## 2. output/sniffer_* 目录结构

### 2.1 命名与创建链路

典型结构为：

```text
data/sniffer_YYYYMMDD_HHMMSS.csv
    ↓ 输入文件基名
outputs/sniffer_YYYYMMDD_HHMMSS/
├── log.txt
├── phase.png
├── group_rng_src_0.csv
├── group_rng_src_5.csv
└── ...（每个实际出现的 rng_src 一份）
```

**已确认**：原始输入时间戳文件名由 `sniffer_monitor.c:69-74` 的 `generate_filename()` 创建：先 `mkdir("data", 0755)`，再以本地时间调用：

```c
strftime(buffer, buffer_size, "data/sniffer_%Y%m%d_%H%M%S.csv", t);
```

**已确认**：输出目录不是再次读取当前时间创建的。`run_all.py:15-19` 从输入 CSV 的文件基名取 `base`，再构造 `os.path.join(out_dir, base)`；默认 `out_dir` 是 `outputs`（`run_all.py:38-45`）。`run_all.sh:11-16` 使用同样规则。因此输出目录时间戳继承自输入文件名。

### 2.2 全量统计

| 项目 | 数量 | 说明 |
| --- | ---: | --- |
| `outputs/sniffer_*` 目录 | 51 | 全量递归检查 |
| 含 `phase.png` 的目录 | 42 | 其余 9 个没有图片 |
| `phase.png` 文件 | 42 | 全部为 960×960、RGBA、非隔行 PNG |
| 分组明细 CSV | 91 | 文件名均为 `group_rng_src_<整数>.csv` |
| `log.txt` | 26 | 分析器标准输出及错误输出 |
| JPG/JPEG/SVG/PDF | 0 | 未发现 |
| JSON/LOG/NPY/PKL | 0 | `.log`、JSON、NPY、PKL 均未发现；日志扩展名实际为 `.txt` |

51 个目录的内容**不一致**：有完整的 `phase.png + log.txt + group CSV`，也有只含图片、只含 CSV、只含日志甚至空目录的情况。图片种类本身一致：所有已发现图片都叫 `phase.png`，均为同一种“到达时间模公共周期”的相位散点图。

缺少 `phase.png` 的目录为：

```text
sniffer_20260603_014300
sniffer_20260603_014601
sniffer_20260603_015306
sniffer_20260603_020409
sniffer_20260607_110007
sniffer_20260608_160423
sniffer_20260608_160939
sniffer_20260608_161148
sniffer_20260608_161302
```

其中 `sniffer_20260603_020409` 是空目录，`sniffer_20260608_160939` 只有 `log.txt`；其他多为只剩若干分组 CSV。

### 2.3 目录不一致的原因

**已确认**：单个分析任务先在进程当前工作目录生成固定名 `phase.png` 和 `group_rng_src_*.csv`（`analyze_sniffer.py:61-62,127`），之后批处理器才搬入目标目录（`run_all.py:22-32`；Shell 版本见 `run_all.sh:16-18`）。

**高可信推断**：现有残缺/错配目录至少部分来自 `run_all.py --jobs > 1` 的并发竞争。`run_all.py:55-59` 允许多个 `worker()` 并行，但所有 worker 共享同一工作目录和同名临时产物；任一 worker 都会用 `glob("group_rng_src_*.csv")` 搬走当时存在的所有源分组文件，并争抢同一个 `phase.png`。这能解释：

- 某些目录有 CSV 但无图；
- 某些目录有图但无 CSV/日志；
- 某些目录出现不属于该次输入的源地址文件；
- 51 个目录中只有 42 张图、91 份 CSV、26 份日志，组合不稳定。

`run_all.sh` 是串行的，没有 Python 并行分支的竞争，但同样依赖当前目录中的固定临时文件；若上一次失败残留文件，也可能被下一次移动。仅凭现存文件无法为每个残缺目录确定具体生成方式或执行参数，故逐目录归因仍为**待确认**。

## 3. 图片类型总览

当前 `sniffer_*` 结果中一共只有 **1 类图片**：

| 图片名 | 格式/尺寸 | 图形 | 数量 | 差异来源 |
| --- | --- | --- | ---: | --- |
| `phase.png` | PNG，960×960，RGBA | 多组散点相位图 | 42 | 源节点集合、点数、捕获时间跨度、相位漂移、空洞和离群点不同 |

文件命名不包含源地址或时间戳；运行身份由父目录名表达。42 张图片的 SHA-1 均不同，说明它们不是简单复制品。

代表性差异：

- `sniffer_20260528_151542/phase.png`：`rng_src=0/5` 两组，点主要形成两条缓慢倾斜的窄带，另有孤立点。
- `sniffer_20260530_123401/phase.png`：仍是 `0/5`，但纵轴跨度较短，存在明显纵向断层与孤立点；配套日志证实源 0 最大组内间隔约 13097 ms。
- `sniffer_20260603_014612/phase.png`：`5/8` 两组；日志中源 5 的相邻间隔约 12 ms，故以 60 ms 取模后形成约 5 个重复相位位置；源 8 覆盖更密且有多个低纵坐标离群点。
- `sniffer_20260608_154251/phase.png`：`8/13` 两组，间隔均接近 60 ms 但夹杂长缺口，表现为近竖直带、断层和孤立点。
- `sniffer_20260608_161230/phase.png`：源 `0/5/8/13/48008`；`48008` 只有一个点，是协议解析错位、噪声或真实特殊地址中的哪一种无法仅凭图确认。

## 4. 图片生成入口与调用链

### 4.1 采集入口

```text
sniffer_monitor.c::main(argc, argv)                    [126]
  ├─ generate_filename()（未显式传输出路径时）         [69-74, 136]
  ├─ open_device()                                     [92-124, 143]
  ├─ libusb_bulk_transfer() 读取 meta                  [172-187]
  ├─ magic 校验 0xBB88                                 [189-193]
  ├─ libusb_bulk_transfer() 读取 body                  [195-205]
  ├─ 强制转换 Ranging_Message_Header_t                 [209-210]
  ├─ 计算并夹取 body_len                               [212-218]
  └─ fprintf() 写 data/sniffer_*.csv                   [220-227]
```

### 4.2 单文件分析入口

```text
analyze_sniffer.py::__main__                            [130-132]
  └─ main(csv_path)                                     [97]
      ├─ pandas.read_csv(csv_path)                      [100]
      ├─ sort_values("system_time_ms")                 [104]
      ├─ unwrap_timestamps(meta_rx_time)                [23-34, 105]
      ├─ DataFrame.groupby("rng_src")                  [111-113]
      ├─ process_group()                                [37-49]
      ├─ export_and_print()                             [52-68, 124-125]
      └─ plot_phase(groups, 60, "phase.png")           [71-94, 120, 127]
          └─ plt.savefig("phase.png", dpi=120)         [93]
```

命令行入口是：

```bash
python analyze_sniffer.py [csv_path]
```

未给参数时默认读取 `data.csv`（`analyze_sniffer.py:97,131-132`）。

### 4.3 批处理入口

Python 入口：`run_all.py::main()`（`run_all.py:36-62`）。它枚举 `data/*.csv`，对每个文件调用：

```text
main()
  └─ worker(fpath, out_dir, python_cmd)
      ├─ mkdir outputs/<basename>
      ├─ subprocess.run([python, analyze_sniffer.py, fpath])
      │   └─ stdout + stderr → log.txt
      ├─ move group_rng_src_*.csv → 结果目录
      └─ move phase.png → 结果目录
```

Shell 替代入口 `run_all.sh` 执行相同的串行流程（`run_all.sh:11-20`）。

## 5. 各图片详细分析

### 5.1 `phase.png`：到达时间模 60 ms 的相位图

- **示例路径**：`outputs/sniffer_20260530_123401/phase.png`、`outputs/sniffer_20260603_014612/phase.png`、`outputs/sniffer_20260608_154251/phase.png`、`outputs/sniffer_20260608_161230/phase.png`
- **生成脚本**：`analyze_sniffer.py`
- **生成函数**：`plot_phase(groups, period_ms, save_path="phase.png")`（`analyze_sniffer.py:71-94`）
- **调用入口**：`main()` 的 `plot_phase(groups, period_ms, "phase.png")`（`analyze_sniffer.py:127`），上层可由脚本直接入口、`run_all.py` 或 `run_all.sh` 调用
- **保存命令**：`plt.savefig(save_path, dpi=120)`（`analyze_sniffer.py:93`）
- **绘图库**：Matplotlib `matplotlib.pyplot`（`analyze_sniffer.py:15`）
- **核心绘图代码**：

```python
for idx, (src, g) in enumerate(sorted(groups.items())):
    t = g["rx_time_ms"]
    x = t % period_ms
    y = t / period_ms
    ax.plot(x, y, ".", label=f"rng_src={src}",
            color=plt.cm.tab10(idx % 10), markersize=6)
```

证据位置为 `analyze_sniffer.py:73-93`。

- **横坐标**：`rx_time_ms % 60 ms`，即一次 60 ms 周期内的到达相位，单位 ms；范围被固定为 `[0, 60]`（`analyze_sniffer.py:79,85,87,120`）。
- **纵坐标**：`rx_time_ms / 60 ms`，即连续 DW 接收时钟对应的“周期编号”。它是无量纲浮点数，不是取整后的周期序号；轴标签包含 `period = 60.000000 ms`（`analyze_sniffer.py:80,86`）。
- **散点**：每个成功进入 CSV 的 Ranging 报文对应一个点。`ax.plot(..., ".")` 只画点，不连线（`analyze_sniffer.py:81-83`）。
- **图例**：每种 `rng_src` 一项，形式为 `rng_src=<源地址>`；颜色来自 `tab10`，按排序后的源地址顺序分配，超过 10 组会循环用色（`analyze_sniffer.py:75,77,82-83`）。源码定义了 `markers` 变量但从未使用（`analyze_sniffer.py:74`）。
- **标题**：`Phase diagram: arrival time modulo common period`（`analyze_sniffer.py:88`）。
- **数据来源**：输入 CSV 的 `system_time_ms`、`meta_rx_time`、`rng_src`；导出明细还保留 `rng_seq`。真正决定散点坐标的是处理后的 `rx_time_ms`，决定分组/颜色的是 `rng_src`。

#### 绘图前处理

1. `pd.read_csv()` 按表头读入（`analyze_sniffer.py:100`）。没有显式 dtype、空值或错误行策略。
2. 按 `system_time_ms` 升序排列，作为全局到达顺序（`analyze_sniffer.py:103-105`）。
3. 对全体记录的 40 位 `meta_rx_time` 做统一解卷绕；仅当当前值小于前值且跌落量大于 `2^39` 时给后续记录加 `2^40`（`analyze_sniffer.py:19-20,23-34`）。
4. 按 `rng_src` 分组（`analyze_sniffer.py:110-113`）。
5. 各组按 `rx_time_cont` 排序，乘 `DW_TIME_TO_MS` 换算为 ms，再用 `.diff()` 计算组内相邻到达间隔 `delta_ms`（`analyze_sniffer.py:37-49`）。
6. 所有组的 `delta_ms` 被拼接并只用于确认“至少有一个间隔”和打印数量（`analyze_sniffer.py:115-122`）。尽管日志文字写“由全部相邻间隔平均得到”，实际 `period_ms` 在 `analyze_sniffer.py:120` **硬编码为 60**，并未求平均。
7. 绘图阶段只做取模和除法，没有滑动平均、归一化、去重、丢包率计算、序列号连续性检查或显式异常值剔除。

#### 图片含义与判读

**已确认**：图用于观察不同源报文相对于共同 60 ms 周期的到达相位、长期相位漂移、时间空洞和离群到达。

正常现象需结合协议预期定义，代码本身没有合格阈值。若系统设计确实要求每个源以 60 ms 周期稳定发送，则通常可作如下**高可信判读**：

- 同一源形成窄而近竖直的带：到达相位稳定；
- 带缓慢左右倾斜：实际发送/接收周期与 60 ms 存在微小频偏或时钟漂移；
- 固定存在多条相位带：可能是一个 60 ms 周期中有多个发送时隙，或实际周期是 60 ms 的约数；`20260603_014612` 中源 5 的约 12 ms 间隔与五相位位置相互印证；
- 纵向大空洞：该源长时间没有被记录，可能是丢包、停发、采集暂停或过滤造成；
- 单独的远离主带的点：异常到达、恢复后的首包、时钟/解卷绕问题或解析异常；
- 相位在 0/60 ms 边界两侧跳变：可能只是模运算的正常环绕，不应自动判为异常；
- 不同源相位带重叠：若系统要求 TDMA 时隙隔离，可能意味着时隙冲突；但工程内没有调度规范，是否异常为**待确认**。

#### 二进制版本所需字段

生成完全同义的图，最小输入是：

- `arrival_order` 或可排序的 `system_time_ms`；
- 40 位原始 `rx_timestamp_ticks`（当前 `meta_rx_time`），或已经全局解卷绕并换算成 ms 的 `rx_time_ms`；
- `source_id`（当前 `rng_src`）。

`rng_seq` 对当前散点图不是必需，但对丢包、去重和数据质量诊断非常重要，建议标准模型保留。协议长度、过滤器和 body 对当前图不必需。

#### 可复用与需修改代码

- **可直接复用**：`DW_TIME_TO_MS`、`WRAP`/阈值常量、`unwrap_timestamps()` 的算法、`process_group()` 的排序/单位换算/差分思路、`plot_phase()` 的样式与坐标计算。
- **最小修改**：让 `plot_phase()` 接收统一标准表或 `Mapping[source_id, DataFrame]`，用明确的 `output_path` 保存，并将 `period_ms` 作为配置而非在 `main()` 中硬编码。
- **应修正**：日志中“由平均得到”的错误描述；使用 `fig.savefig()` 并 `plt.close(fig)`，避免依赖全局 pyplot 状态；批处理每个 worker 使用独立临时目录。

### 5.2 类似名称但内容不同的说明

未发现第二种图片文件名或第二套绘图命令。不同 `phase.png` 不是不同“图片类型”，而是同一函数对不同捕获数据产生的实例。源地址组合可见 `0/5`、`5/8`、`8/13`、`0/5/8/13/48008` 等，必须按实例的图例和配套 CSV 分别解释，不能把某个固定颜色永久等同于某个地址：颜色由当次源地址排序位置决定。

## 6. 原始数据到图片的数据流

```text
UWB sniffer USB bulk 数据（meta 帧 + body 帧）
    ↓ sniffer_monitor.c::libusb_bulk_transfer()
18-byte Sniffer_Meta_t + Ranging_Message_t
    ↓ magic 校验、Ranging header 小端结构解释、body 长度夹取
data/sniffer_YYYYMMDD_HHMMSS.csv
    ↓ analyze_sniffer.py::pd.read_csv()
pandas.DataFrame
    ↓ 按 system_time_ms 排序
    ↓ unwrap_timestamps(meta_rx_time)
新增 rx_time_cont
    ↓ groupby(rng_src)
dict[int, DataFrame]
    ↓ process_group()
新增 rx_time_ms、delta_ms
    ├─ export_and_print() → group_rng_src_<src>.csv
    └─ plot_phase(period_ms=60)
         ↓ plt.savefig("phase.png", dpi=120)
当前工作目录/phase.png
    ↓ run_all.py::shutil.move() 或 run_all.sh::mv
outputs/sniffer_YYYYMMDD_HHMMSS/phase.png
```

注意：`sniffer_monitor.c` 自身已经读取二进制 USB 帧；当前 Python 分析器读取的是它转存后的文本 CSV。新程序若直接读取二进制文件，可绕过 CSV 序列化，但必须复现相同的字段语义、端序、时间戳宽度及到达顺序。

## 7. 当前数据格式解析逻辑

### 7.1 原始 USB 记录结构

`sniffer_monitor.c:40-50` 定义 18 字节 packed meta 帧：

| 字段 | C 类型 | 含义 |
| --- | --- | --- |
| `magic` | `uint32_t` | 期望 `0xBB88`，用于流同步 |
| `senderAddress` | `uint16_t` | meta 内发送者字段；当前 CSV/绘图未使用 |
| `seqNumber` | `uint16_t` | meta 内序列号；当前 CSV/绘图未使用 |
| `msgLength` | `uint16_t` | 设备未填写，代码明确禁止依赖 |
| `rxTime` | `uint64_t` | 实际有效低 40 位 DW 接收时间戳 |

紧随其后的 body 至少包含 8 字节 packed `Ranging_Message_Header_t`（`sniffer_monitor.c:52-58`）：

```text
uint16 little-endian srcAddress
uint16 little-endian msgSequence
uint16 little-endian msgLength
uint16 little-endian filter
remaining bytes: body payload
```

meta 和 body 由两次交替的 bulk transfer 获得（`sniffer_monitor.c:172-205`）。body 有效载荷长度计算为 `rng->msgLength - 8`，再夹取到 `[0, recv_len - 8]`（`sniffer_monitor.c:212-218`）。

### 7.2 CSV 格式与实例

CSV 表头由 `sniffer_monitor.c:159-161` 固定为：

```text
system_time_ms,magic,meta_rx_time,rng_src,rng_seq,rng_len,rng_filter,body_len,body_hex
```

当前保留输入 `data/sniffer_20260608_154251.csv` 的一条实际记录为：

```text
89339521,0x0000BB88,848091629395,8,5693,72,0xA5A5,64,061008003E16...
```

含义：主机单调时钟为 89339521 ms；magic 匹配；DW 接收 tick 为 848091629395；Ranging 源地址 8；序列号 5693；报文声明长度 72 字节；filter 为 `0xA5A5`；header 后有效 body 为 64 字节；最后一列是大写无分隔十六进制载荷。

### 7.3 Python 实际依赖

`analyze_sniffer.py` 不解析 `body_hex`，也不重新解释二进制协议；它依赖 pandas 按 CSV 表头生成列。实际使用情况：

| CSV 字段 | 用途 |
| --- | --- |
| `system_time_ms` | 恢复全局到达顺序 |
| `meta_rx_time` | 40 位解卷绕、单位换算、散点坐标 |
| `rng_src` | 分组、图例、颜色 |
| `rng_seq` | 仅导出到分组 CSV，没有参与绘图或丢包判断 |
| `magic` | 分析器不使用；采集器写入前已校验 |
| `rng_len` | 不使用 |
| `rng_filter` | 不使用 |
| `body_len` | 不使用 |
| `body_hex` | 不使用 |

## 8. 绘图函数依赖的数据字段

| 标准字段 | 当前字段来源 | 数据类型 | 单位 | 是否为绘图必需 | 二进制解析器应如何提供 |
| --- | --- | --- | --- | --- | --- |
| `arrival_order` | CSV 行经 `system_time_ms` 排序 | `int64` 或稳定序号 | 无/序号 | 条件必需 | 为每个成功解析记录给出单调序号；若给 `host_time_ms`，需稳定排序 |
| `host_time_ms` | `clock_gettime(CLOCK_MONOTONIC)` | `uint64` | ms | 用于排序；有 `arrival_order` 时可选 | 记录读取/接收的主机单调时间，不要用易回拨的墙钟 |
| `rx_timestamp_ticks` | meta `rxTime` / CSV `meta_rx_time` | `uint64`，有效低 40 位 | DW tick | 原始模型必需 | 按小端读取，并确认只保留 40 位有效值 |
| `rx_time_cont_ticks` | `unwrap_timestamps()` | `int64`（长文件建议更宽/任意精度） | DW tick | 可由前项计算 | 跨所有源、按全局到达顺序统一解卷绕 |
| `rx_time_ms` | `rx_time_cont * DW_TIME_TO_MS` | `float64` | ms | **直接绘图必需** | 由统一分析层换算，不建议二进制 parser 自行重复换算 |
| `source_id` | body header `srcAddress` / `rng_src` | `uint16` | 无 | **必需** | 小端读取 2 字节并标准化为整数 |
| `sequence` | body header `msgSequence` / `rng_seq` | `uint16` | 无 | 当前图非必需，诊断强烈建议 | 保留原始模 65536 序列号 |
| `message_length` | body header `msgLength` | `uint16` | byte | 否 | 用于验证/夹取，不交给 Plotter |
| `filter` | body header `filter` | `uint16` | bit field | 否 | 保留供过滤和质量检查 |
| `payload` | `body_hex` 解码前的 body | `bytes` | byte | 否 | 二进制模型直接保存 bytes，避免先转 hex |
| `period_ms` | `main()` 硬编码 | `float` | ms | **必需配置** | 默认 60.0，但应显式传给分析/绘图层 |

若二进制解析器直接输出已经标准化的 `rx_time_ms + source_id`，当前 `plot_phase()` 即可在极小适配后复用；若希望复用解卷绕与诊断，则应提供 `arrival_order + rx_timestamp_ticks + source_id + sequence`。

## 9. 可复用逻辑与格式相关逻辑

### 9.1 与输入格式强相关

- `sniffer_monitor.c` 的 libusb 设备发现、VID/PID、endpoint、两次 bulk transfer 交替模型；
- C packed 结构强制转换及小端假设；
- meta 固定 18 字节、Ranging header 固定 8 字节；
- magic `0xBB88` 校验与错误帧跳过；
- body `msgLength` 夹取和 hex 文本编码；
- CSV 的逗号分隔、表头名称及 `pd.read_csv()`；
- `system_time_ms` 的 CSV 列名；
- `run_all.py` 对 `data/*.csv` 和输入基名的约定。

### 9.2 与协议语义相关但与文件容器无关

- 40 位 DW 时间戳、`2^40` 回绕及 tick 频率；
- `rng_src`、`rng_seq` 的协议含义；
- Ranging header 的字段宽度和端序；
- 60 ms 是否真的是系统公共周期。

这些不能因为改为二进制文件就省略，但应由 BinaryParser/协议层负责，不应泄漏到 Plotter。

### 9.3 可复用的格式无关逻辑

- 按全局到达顺序解卷绕时间戳；
- 按源地址分组；
- 组内按连续接收时间排序；
- tick 到 ms 的单位换算；
- 相邻时间差 `delta_ms`；
- 每组 count/mean/std/min/max；
- 相位计算 `t % period_ms` 与周期坐标 `t / period_ms`；
- Matplotlib 标题、坐标轴、网格、图例、颜色、尺寸和 DPI；
- 标准化后的分组明细导出。

### 9.4 当前耦合结论

**结论：存在中度耦合。** 采集器与绘图器是两个文件，已经有进程级分离；`plot_phase()` 本身只依赖处理后的 `groups`，可复用性较好。但 `analyze_sniffer.py::main()` 同时承担 CSV 读取、到达排序、时间戳解卷绕、分组、统计、导出、打印、绘图和固定输出命名；`plot_phase()` 又约定 DataFrame 内必须已有 `rx_time_ms`。批处理器依赖当前目录中的固定产物名并事后搬移，造成路径和并发耦合。

当前代码没有真正的丢包率计算、异常值处理或去重。不能把 `delta_ms` 的大值直接等同为丢包，因为还未结合 `rng_seq` 回绕、发送策略和过滤规则。

## 10. 二进制解析版本的适配方案

推荐链路：

```text
raw binary file
    ↓ BinaryReader（块读取、偏移、EOF/截断处理）
Binary frame stream
    ↓ BinaryParser（端序、magic、meta/body、长度校验）
标准 PacketRecord 流
    ↓ normalize_timestamps / group / interval_stats
AnalysisResult
    ↓ PhasePlotter
phase.png（相同标题、轴、颜色、尺寸、DPI）
```

### 10.1 适配原则

1. BinaryReader 只负责字节获取与文件偏移，不解释业务字段。
2. BinaryParser 负责协议帧边界、端序、字段宽度、magic、长度与错误恢复，输出统一记录。
3. Analysis 不读取文件、不拼路径，只消费记录或 DataFrame。
4. Plotter 不接触 raw bytes、CSV 键名或二进制 offset，只接收标准列。
5. CLI/批处理层负责输入枚举、结果目录、日志和原子写入；每次运行必须有独立输出目录/临时目录。

### 10.2 当前与新解析器共用方式

```text
CsvSnifferAdapter  ─┐
                    ├─> list[PacketRecord] / canonical DataFrame
BinarySnifferParser ─┘
                              ↓
                     analyze_packets()
                              ↓
                     plot_phase()
```

CSV 适配器把 `system_time_ms/meta_rx_time/rng_src/rng_seq` 映射到标准字段；二进制解析器直接填充同一模型。这样当前格式与新格式只在适配入口不同。

### 10.3 函数处置建议

| 当前函数/逻辑 | 建议 | 原因 |
| --- | --- | --- |
| `unwrap_timestamps()` | 抽取并复用算法 | 与 CSV 无关；补充空输入、乱序和多回绕测试 |
| `process_group()` | 拆成纯计算与日志展示 | 当前同时计算并 `print()` |
| `export_and_print()` | 拆为 `export_groups()` 和 reporter | 导出/展示与分析解耦 |
| `plot_phase()` | 小改后直接复用 | 改为显式标准列、`fig.savefig(output_path)`、关闭 figure |
| `main()` | 重写为薄 orchestration | 当前耦合最多 |
| `sniffer_monitor.c` CSV 写出 | 保留作旧入口 | 新二进制文件输入不应复制此 I/O 路径 |
| USB/C 二进制解释 | 参考协议但按文件格式重写 | 文件帧边界未必等同两次 USB transfer |
| `run_all.py::worker()` | 重写临时/输出路径策略 | 消除共享文件名和并发竞争 |

## 11. 推荐的标准数据模型

对逐包数据建议优先使用 `dataclass` 作为解析器边界，再一次性转换为 pandas DataFrame 供向量化分析和原绘图函数使用。字典过于松散，难以约束单位和可选字段；只用 DataFrame 则不利于二进制流式解析和精确错误定位。

```python
from dataclasses import dataclass

@dataclass(frozen=True, slots=True)
class PacketRecord:
    arrival_order: int
    host_time_ms: int | None
    rx_timestamp_ticks: int
    source_id: int
    sequence: int | None = None
    message_length: int | None = None
    filter: int | None = None
    payload: bytes = b""
    input_offset: int | None = None

@dataclass(frozen=True)
class AnalysisConfig:
    tick_hz: float = 499.2e6 * 128.0
    timestamp_bits: int = 40
    period_ms: float = 60.0
```

标准分析表建议列：

```text
arrival_order: int64
host_time_ms: Int64 (optional)
rx_timestamp_ticks: uint64
rx_time_cont_ticks: uint64/int64
rx_time_ms: float64
source_id: uint16/int64
sequence: UInt16 (optional)
delta_ms: float64
```

长时间记录可能跨越许多次回绕。当前 NumPy `int64` 在约 9.22e18 tick 后溢出；按当前 tick 频率约对应 1671 天连续 tick 数量级。是否需要支持如此长的文件为**待确认**，但模型和测试应明确边界。

## 12. 建议的代码重构结构

建议的最小渐进结构，不要求本次直接实施：

```text
sniffer_analysis/
├── model.py          # PacketRecord, AnalysisConfig, AnalysisResult
├── csv_adapter.py    # 旧 CSV → PacketRecord/DataFrame
├── binary_reader.py  # 二进制块读取、offset、截断错误
├── binary_parser.py  # meta/body 协议解释
├── analysis.py       # unwrap、单位换算、分组、delta、统计
├── plotting.py       # plot_phase(result, output_path, config)
├── export.py         # group CSV / JSON summary（如需要）
└── cli.py            # 输入选择、输出目录与日志
```

建议接口：

```python
def analyze_packets(records: Iterable[PacketRecord],
                    config: AnalysisConfig) -> AnalysisResult: ...

def plot_phase(result: AnalysisResult,
               output_path: Path,
               period_ms: float) -> None: ...

records = BinaryParser(BinaryReader(path), protocol).records()
result = analyze_packets(records, config)
plot_phase(result, run_dir / "phase.png", config.period_ms)
```

为了最大程度复刻当前样式，保留 `figsize=(8, 8)`、`tab10`、点标记 `"."`、`markersize=6`、网格 `alpha=0.3`、相同英文标题和轴标签、`dpi=120`。若要求像素级一致，还需固定 Python、Matplotlib、字体、backend、操作系统和 metadata；当前工程没有依赖锁文件，因此只能保证语义和主要样式一致，无法承诺跨环境逐字节相同。

## 13. 风险、疑点与待确认事项

1. **已确认：目录名偏差。** 工程是 `outputs/`，不是 `output/`。
2. **已确认：period 日志与实现矛盾。** `period_ms=60` 是硬编码；“由全部相邻间隔平均得到”只是错误文案。
3. **高可信风险：并发产物竞争。** `run_all.py --jobs > 1` 共享固定临时文件名，现存目录不能全部视为可靠配套集合。
4. **待确认：51 次运行的完整原始数据。** `data/` 只保留 `sniffer_20260530_155534.csv`、`sniffer_20260603_020745.csv`、`sniffer_20260608_154251.csv`；多数图片不能重新计算逐点比对。
5. **待确认：二进制文件封装格式。** 当前源码描述的是 USB 上 meta/body 两次 transfer；新二进制文件是否保存长度前缀、transfer 边界、主机时间、校验和或丢帧标记尚未知。
6. **待确认：60 ms 的协议依据。** 当前工程没有配置或协议文档证明所有运行都应使用 60 ms。
7. **待确认：`rng_src=48008`。** 它可能是真实地址，也可能是错位/损坏；仅凭当前产物不能判定。
8. **已确认：当前无数据质量策略。** `read_csv` 没有 schema/缺失值检查；unwrap 只识别大幅向下跳变；没有重复包、序列号跳变、乱序或异常 tick 报告。
9. **高可信风险：依赖到达排序。** 多条记录若 `system_time_ms` 相同，当前没有显式稳定次序键；二进制版应保留严格 `arrival_order`。
10. **高可信风险：C 结构强转。** 当前依赖 packed、小端和允许非对齐读取的平台行为；跨平台二进制解析更适合显式逐字段解码。
11. **当前运行环境限制。** 默认 Python 缺少 pandas，未完成本次重跑验证；这不是现有图片真实性的反证，但依赖版本和可复现环境需要补齐。
12. **图片语义边界。** 图能显示空洞和离群点，却不能单独证明“丢包”；需要结合序列号、过滤规则和发送计划。

## 14. 最终结论

1. 当前 `outputs/sniffer_*` 中共有 51 个时间戳目录、42 张图片，但图片只有 **1 类**：`phase.png` 相位散点图。
2. 主要绘图入口是 `analyze_sniffer.py::main()` → `plot_phase()` → `plt.savefig()`；批量入口是 `run_all.py` 或 `run_all.sh`。
3. 数据源由 `sniffer_monitor.c` 从 USB 二进制 meta/body 帧解析后写成 CSV。Python 只使用 `system_time_ms`、`meta_rx_time`、`rng_src`，并在导出明细时保留 `rng_seq`。
4. 当前解析与绘图是中度耦合：绘图函数较独立，但主函数混合读取、解析后处理、统计、导出、打印、绘图和固定路径管理。
5. 二进制版的最小绘图字段是稳定到达顺序、DW 接收时间戳（或标准化 `rx_time_ms`）和源地址；推荐额外提供序列号、主机时间、长度、filter、payload 和输入 offset 以支持验证与诊断。
6. 解卷绕、单位换算、分组、间隔统计、相位计算及 Matplotlib 样式均可复用；CSV 读取、USB transfer 假设、packed 结构解释和批处理产物搬移应替换或重构。
7. 新二进制解析器应通过统一 `PacketRecord`/canonical DataFrame 适配层接入同一 Analysis/Plotter，从而让旧 CSV 与新二进制输入共用绘图代码。
8. 在复用前应优先修复批处理并发竞争、period 文案/配置、显式 schema 验证和独立输出路径；否则即使绘图本身正确，目录内的配套产物仍可能错配。
