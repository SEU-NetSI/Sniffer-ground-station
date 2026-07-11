# Cross-platform Sniffer host

`sniffer_storage` captures the firmware's raw Sniffer byte stream without changing its wire or file format:

```text
[18-byte little-endian meta][msgLength-byte payload]
```

The actual transport in this repository is a vendor-specific **libusb bulk interface**, not a serial port. The defaults are VID:PID `0483:5740`, interface `0`, Bulk IN endpoint `0x81`. Consequently `/dev/cu.*`, `termios`, and baud rates do not apply.

## Build on macOS

Install libusb if needed:

```bash
brew install libusb pkg-config
```

Then build and test:

```bash
cd sniffer_storage
make clean
make
make test
```

The macOS Makefile uses `pkg-config` first and enables `-std=c11 -Wall -Wextra -Wpedantic`.

## Automatic USB device selection

When exactly one matching UWB USB device is connected, capture can start without
listing or entering its changing bus/address:

```bash
./sniffer_storage --output capture.bin
```

Automatic selection is intentionally refused when multiple matching devices are
connected. In that case, discover and select the exact device as follows.

## Discover the exact USB device

This lists only devices matching the selected VID/PID and reports interface endpoint descriptors; it does not claim an interface or send data:

```bash
./sniffer_storage --list-devices
```

If multiple matching devices exist, select the printed bus/address explicitly:

```bash
./sniffer_storage --device 1:7 --output capture.bin
```

Bus/address values can change after reconnecting the device, so list again before use.

## Capture

```bash
./sniffer_storage \
  --output capture.bin \
  --max-payload 4096
```

For a short hardware check, stop after a known number of valid records:

```bash
./sniffer_storage --output smoke.bin --max-records 10
```

By default, every complete meta is printed immediately in the same multi-line
format as `sniffer_storage_original.c`, before its payload is read. Disable this
high-volume output with `--no-print-records`:

```bash
./sniffer_storage \
  --output "raw_sensor_data_$(date +%Y%m%d_%H%M%S).bin" \
  --timeout-ms 1000
```

To append the payload as continuous uppercase hexadecimal on each record line:

```bash
./sniffer_storage \
  --output "raw_sensor_data_$(date +%Y%m%d_%H%M%S).bin" \
  --timeout-ms 1000 \
  --print-records \
  --print-payload
```

`--print-payload` requires record printing to remain enabled. Per-record terminal
output, especially payload output, can reduce capture performance at high data
rates. Detailed final counters remain available with `--print-stats`.

On macOS, phase analysis can run automatically after the capture file closes:

```bash
./sniffer_storage \
  --output "raw_sensor_data_$(date +%Y%m%d_%H%M%S).bin" \
  --timeout-ms 1000 \
  --plot-after-capture \
  --plot-period-ms 30
```

The default plot directory is `outputs_binary/<capture-name>/`; override it
with `--plot-output DIR`. Automatic plotting is off unless requested. The
analysis globally unwraps the 40-bit timestamp and subtracts the first captured
timestamp, so the first captured record is plotted at `y=0`.

The reader uses a large USB staging buffer so that a device's 18/64-byte USB packet is never requested into the parser's smaller 4-byte magic window. It then accumulates partial parser reads, continues across timeouts, checks the 18-byte meta magic and payload length, resynchronizes on the existing magic when bytes are misaligned, and writes only complete records. `Ctrl-C` closes the file, releases interface 0, closes the device, and exits libusb. Final counters include received/stored records and bytes, magic/length errors, truncation, timeouts, transport errors, file failures, and resynchronization.

Run `./sniffer_storage --help` for VID/PID, interface, endpoint, timeout, and output options.

## Raw Bulk OUT sending and its boundary

No application-level host-to-Sniffer command protocol or device-side handler exists in this repository. The external Crazyflie USB stack is also absent, and this repository explicitly does not contain directly flashable Sniffer firmware. For that reason the host does not invent a command type or claim that a command was executed.

For firmware that already defines an exact USB/CRTP packet, the host can transfer caller-supplied bytes through a descriptor-validated Bulk OUT endpoint:

```bash
./sniffer_storage \
  --device BUS:ADDRESS \
  --endpoint-out 0x01 \
  --send-hex "ACTUAL_FIRMWARE_SUPPORTED_FRAME" \
  --send-only
```

`--send-hex` refuses to run without an explicit `--device BUS:ADDRESS`; it never sends to the first automatic VID/PID match. Before transferring, the program prints the selected bus/address, endpoint, byte count, and normalized hex. OUT timeouts have a finite retry budget. USB transfer success proves only that the host transfer completed; it does not prove firmware application handling or acknowledgement. Do not use `--send-hex` until the installed firmware's descriptor, endpoint, and packet format have been confirmed.
